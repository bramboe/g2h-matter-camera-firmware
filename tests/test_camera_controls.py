"""Optional controls use existing Matter identity and verified native state."""
import asyncio
import importlib.util
from pathlib import Path
import sys
import types
import unittest
from unittest.mock import AsyncMock, patch

from test_camera_platforms import Coordinator, CoordinatorEntity, UpdateFailed, module

class HomeAssistantError(Exception): pass
class MatterError(Exception): pass
class MatterClientException(Exception): pass
class ClientError(Exception): pass

SOURCE = Path(__file__).parents[1] / 'home-assistant/custom_components/g2h_matter_camera'
package = module('g2h_controls_test', DOMAIN='g2h_matter_camera'); package.__path__ = [str(SOURCE)]
stubs = {
    package.__name__: package,
    'aiohttp': module('aiohttp', ClientError=ClientError),
    'homeassistant.components.matter.helpers': module('homeassistant.components.matter.helpers',
        get_matter=lambda hass: hass.adapter,
        get_node_device_identifier=lambda info, node: ('matter', f'existing-{node}')),
    'homeassistant.components.switch': module('homeassistant.components.switch', SwitchEntity=type('Switch', (), {})),
    'homeassistant.exceptions': module('homeassistant.exceptions', HomeAssistantError=HomeAssistantError),
    'homeassistant.helpers.device_registry': module('homeassistant.helpers.device_registry', DeviceInfo=dict),
    'homeassistant.helpers.update_coordinator': module('homeassistant.helpers.update_coordinator',
        DataUpdateCoordinator=Coordinator, CoordinatorEntity=CoordinatorEntity, UpdateFailed=UpdateFailed),
    'matter_server.common.errors': module('matter_server.common.errors', MatterError=MatterError),
    'matter_server.client.exceptions': module('matter_server.client.exceptions', MatterClientException=MatterClientException),
}
with patch.dict(sys.modules, stubs):
    spec = importlib.util.spec_from_file_location(package.__name__ + '.switch', SOURCE / 'switch.py')
    target = importlib.util.module_from_spec(spec); spec.loader.exec_module(target)


def values(audio=True, requested=True, observed=True, pending=False, error=''):
    return {target.AUDIO_PATH: audio, **{target.THREAD_PATH + str(i): value
        for i, value in ((24, requested), (25, observed), (26, pending), (27, error))}}


class Controls(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.entry = types.SimpleNamespace(entry_id='camera', data={'node_id':73, 'unique_id':'persistent-camera'})
        self.client = types.SimpleNamespace(read_attribute=AsyncMock(return_value=values()),
            write_attribute=AsyncMock(), server_info=object(), disconnect=AsyncMock())
        self.hass = types.SimpleNamespace(loop=asyncio.get_running_loop(),
            adapter=types.SimpleNamespace(matter_client=self.client),
            data={'g2h_matter_camera':{'camera':{'client':self.client}}})
        self.coordinator = target.ControlsCoordinator(self.hass, self.entry)
        self.coordinator.local_timeout = .02
        self.coordinator.verification_timeout = .02
        self.coordinator.verification_interval = .001

    async def asyncTearDown(self):
        await self.coordinator.close()
        self.client.disconnect.assert_not_awaited()

    async def test_older_firmware_missing_controls_creates_no_fake_switches(self):
        self.client.read_attribute.return_value = {}
        entities = []; await target.async_setup_entry(self.hass, self.entry, entities.extend)
        self.assertEqual(entities, [])
        self.assertIn('controls', self.hass.data['g2h_matter_camera']['camera'])
        await self.hass.data['g2h_matter_camera']['camera']['controls'].close()
        self.client.read_attribute.side_effect = MatterError('unsupported')
        entities = []; await target.async_setup_entry(self.hass, self.entry, entities.extend)
        self.assertEqual(entities, [])
        await self.hass.data['g2h_matter_camera']['camera']['controls'].close()

    async def test_known_null_false_and_thread_first_use_have_distinct_states(self):
        for audio, observed, requested, pending, error, known in (
            (False,False,False,False,'',True), (True,True,None,False,'',True),
            (None,None,None,False,'',False), (True,False,True,False,'',True),
            (True,True,True,True,'',False), (True,True,True,False,'verification-failed',True),
            (True,1,True,False,'',False)):
            self.client.read_attribute.return_value = values(audio,requested,observed,pending,error)
            await self.coordinator.async_refresh()
            thread = target.CameraControlSwitch(self.coordinator,self.entry,'thread')
            sound = target.CameraControlSwitch(self.coordinator,self.entry,'audio')
            self.assertEqual(thread.available,known)
            self.assertIs(sound.is_on,audio)
            self.assertEqual(sound.available,type(audio) is bool)
        self.assertEqual(thread._attr_device_info,{'identifiers':{('matter','existing-73')}})
        self.assertEqual(sound._attr_unique_id,'persistent-camera-control-audio')

    async def test_success_waits_for_real_readback_and_only_writes_existing_node(self):
        await self.coordinator.async_refresh()
        reads = iter((values(requested=False,pending=True), values(requested=False,observed=False)))
        async def read(node, paths):
            self.assertEqual(node,73); self.assertEqual(paths,target.PATHS)
            self.assertIn('thread',self.coordinator.writing)
            entity = target.CameraControlSwitch(self.coordinator,self.entry,'thread')
            self.assertFalse(entity.available)
            return next(reads)
        self.client.read_attribute.side_effect = read
        await self.coordinator.set_enabled('thread',False)
        self.client.write_attribute.assert_awaited_once_with(73,target.THREAD_PATH+'24',False)
        thread = target.CameraControlSwitch(self.coordinator,self.entry,'thread')
        self.assertTrue(thread.available);self.assertIs(thread.is_on,False)
        self.client.read_attribute.side_effect = None
        self.client.read_attribute.return_value = values(audio=False)
        await self.coordinator.set_enabled('audio',False)
        self.assertEqual(self.client.write_attribute.await_args.args,(73,target.AUDIO_PATH,False))

    async def test_failed_transition_is_unavailable_without_optimistic_success(self):
        await self.coordinator.async_refresh()
        self.client.read_attribute.return_value = values(audio=None,requested=False,observed=True,error='command-failed')
        with self.assertRaisesRegex(HomeAssistantError,'niet bevestigd'):
            await self.coordinator.set_enabled('thread',False)
        self.assertFalse(target.CameraControlSwitch(self.coordinator,self.entry,'thread').available)
        self.assertIs(self.coordinator.data['thread']['state'],True)
        # The next real observation restores the actual on state, preserving a
        # retry path even when the radio remembers the failed disable request.
        await self.coordinator.async_refresh()
        self.assertTrue(target.CameraControlSwitch(self.coordinator,self.entry,'thread').available)
        self.assertIs(target.CameraControlSwitch(self.coordinator,self.entry,'thread').is_on,True)

    async def test_abandoned_read_blocks_later_reads_and_writes_and_close_cancels(self):
        await self.coordinator.async_refresh()
        gate=asyncio.Event()
        async def slow(*args): await gate.wait(); return values()
        self.client.read_attribute.side_effect=slow
        await self.coordinator.async_refresh()
        pending=self.coordinator._pending_rpc;self.assertFalse(pending.done())
        before=self.client.read_attribute.await_count
        await self.coordinator.async_refresh()
        self.assertEqual(self.client.read_attribute.await_count,before)
        with self.assertRaises(HomeAssistantError):await self.coordinator.set_enabled('audio',False)
        self.client.write_attribute.assert_not_awaited()
        await self.coordinator.close();self.assertTrue(pending.cancelled())
        self.assertIsNone(self.coordinator._pending_rpc)

    async def test_shared_client_replacement_refuses_write(self):
        await self.coordinator.async_refresh()
        self.hass.adapter.matter_client=object()
        with self.assertRaises(HomeAssistantError):await self.coordinator.set_enabled('audio',False)
        self.client.write_attribute.assert_not_awaited()

    async def test_audio_only_capability_does_not_create_thread_or_video_switch(self):
        self.client.read_attribute.return_value={target.AUDIO_PATH:None}
        entities=[];await target.async_setup_entry(self.hass,self.entry,entities.extend)
        self.assertEqual([entity._attr_name for entity in entities],['Geluid'])
        self.assertFalse(entities[0].available);self.assertIsNone(entities[0].is_on)
        await entities[0].coordinator.close()
