"""Existing camera setup/unload contracts, no real server or HA credentials."""
import importlib.util
from pathlib import Path
import sys
import types
import unittest
from unittest.mock import AsyncMock, patch
from aiohttp import ClientConnectionError
ROOT=Path(__file__).parents[1]/'home-assistant/custom_components/g2h_matter_camera'
def module(name,**values):
    r=types.ModuleType(name);r.__dict__.update(values);return r
class NotReady(Exception):pass
class MissingNode(Exception):pass
platform=types.SimpleNamespace(CAMERA='camera',SENSOR='sensor',BINARY_SENSOR='binary_sensor')
stubs={
 'homeassistant.const':module('homeassistant.const',Platform=platform,CONF_URL='url'),
 'homeassistant.components.matter.helpers':module('homeassistant.components.matter.helpers',get_matter=lambda hass:hass.adapter),
 'homeassistant.exceptions':module('homeassistant.exceptions',ConfigEntryNotReady=NotReady),
 'homeassistant.helpers.aiohttp_client':module('homeassistant.helpers.aiohttp_client',async_get_clientsession=lambda hass:object()),
 'matter_server.common.errors':module('matter_server.common.errors',NodeNotExists=MissingNode),
}
with patch.dict(sys.modules,stubs):
    spec=importlib.util.spec_from_file_location('g2h_lifecycle_test',ROOT/'__init__.py',submodule_search_locations=[str(ROOT)])
    target=importlib.util.module_from_spec(spec);sys.modules[spec.name]=target;spec.loader.exec_module(target)
class Lifecycle(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        diagnostic_stub=module('g2h_lifecycle_test.diagnostics',_exception_classes=lambda err:{
            'error_class':type(err).__name__ if err is not None else None,
            'cause_class':type(err.__cause__).__name__ if err is not None and err.__cause__ is not None else None})
        self.modules=patch.dict(sys.modules,{'g2h_lifecycle_test':target,'g2h_lifecycle_test.diagnostics':diagnostic_stub})
        self.modules.start();self.addCleanup(self.modules.stop)
        self.entry=types.SimpleNamespace(entry_id='existing-camera',data={'matter_entry_id':'matter','node_id':73,'unique_id':'identity'})
        self.radio=types.SimpleNamespace(close=AsyncMock());self.motion=types.SimpleNamespace(close=AsyncMock())
        self.client=types.SimpleNamespace(close=AsyncMock(),get_node=lambda node:object())
        self.entries=types.SimpleNamespace(async_unload_platforms=AsyncMock(return_value=True),async_forward_entry_setups=AsyncMock(),
             async_get_entry=lambda key:types.SimpleNamespace(data={'url':'ws://existing-matter.invalid/ws'}))
        self.hass=types.SimpleNamespace(config_entries=self.entries,adapter=types.SimpleNamespace(matter_client=self.client),
            data={'g2h_matter_camera':{'existing-camera':{'radio':self.radio,'motion':self.motion,'client':self.client}}})
    async def test_success_closes_own_coordinators_not_shared_client(self):
        self.assertTrue(await target.async_unload_entry(self.hass,self.entry))
        self.entries.async_unload_platforms.assert_awaited_once_with(self.entry,['camera','sensor','binary_sensor'])
        self.radio.close.assert_awaited_once();self.motion.close.assert_awaited_once();self.client.close.assert_not_awaited()
        self.assertNotIn('existing-camera',self.hass.data['g2h_matter_camera'])
    async def test_failed_platform_unload_keeps_existing_runtime(self):
        self.entries.async_unload_platforms.return_value=False
        self.assertFalse(await target.async_unload_entry(self.hass,self.entry))
        self.assertIn('existing-camera',self.hass.data['g2h_matter_camera'])
        self.radio.close.assert_not_awaited();self.motion.close.assert_not_awaited()
    async def test_setup_client_error_becomes_retryable_not_unexpected(self):
        wire=types.SimpleNamespace(connect=AsyncMock(side_effect=ClientConnectionError('private detail')),close=AsyncMock())
        with patch.object(target,'MatterWire',return_value=wire):
            with self.assertRaisesRegex(NotReady,'Cannot verify existing Matter camera'):
                await target.async_setup_entry(self.hass,self.entry)
        wire.close.assert_awaited_once();self.entries.async_forward_entry_setups.assert_not_awaited()
    async def test_verified_setup_reuses_identity_and_loads_all_platforms(self):
        wire=types.SimpleNamespace(connect=AsyncMock(),request=AsyncMock(return_value={}),close=AsyncMock())
        with patch.object(target,'MatterWire',return_value=wire),patch.object(target,'camera_identity',return_value='identity'):
            self.assertTrue(await target.async_setup_entry(self.hass,self.entry))
        self.entries.async_forward_entry_setups.assert_awaited_once_with(self.entry,['camera','sensor','binary_sensor'])
        self.assertIs(self.hass.data['g2h_matter_camera']['existing-camera']['client'],self.client)
        self.client.close.assert_not_awaited()
    async def test_offline_verified_identity_refreshes_once(self):
        node=types.SimpleNamespace(available=False);self.client.get_node=lambda node_id:node
        async def interview(node_id):node.available=True
        self.client.interview_node=AsyncMock(side_effect=interview)
        wire=types.SimpleNamespace(connect=AsyncMock(),request=AsyncMock(return_value={}),close=AsyncMock())
        with patch.object(target,'MatterWire',return_value=wire),patch.object(target,'camera_identity',return_value='identity'),patch.object(target.asyncio,'timeout',wraps=target.asyncio.timeout) as deadline:
            self.assertTrue(await target.async_setup_entry(self.hass,self.entry))
        self.client.interview_node.assert_awaited_once_with(73);deadline.assert_called_once_with(15)
        refresh=self.hass.data['g2h_matter_camera']['existing-camera']['cache_refresh']
        self.assertTrue(refresh['attempted']);self.assertTrue(refresh['succeeded']);self.assertTrue(refresh['cached_node_available_after'])
    async def test_online_verified_identity_does_not_interview(self):
        self.client.get_node=lambda node_id:types.SimpleNamespace(available=True);self.client.interview_node=AsyncMock()
        wire=types.SimpleNamespace(connect=AsyncMock(),request=AsyncMock(return_value={}),close=AsyncMock())
        with patch.object(target,'MatterWire',return_value=wire),patch.object(target,'camera_identity',return_value='identity'):
            self.assertTrue(await target.async_setup_entry(self.hass,self.entry))
        self.client.interview_node.assert_not_awaited()
        self.assertFalse(self.hass.data['g2h_matter_camera']['existing-camera']['cache_refresh']['attempted'])
    async def test_offline_interview_failure_nonfatal_and_sanitized(self):
        self.client.get_node=lambda node_id:types.SimpleNamespace(available=False)
        self.client.interview_node=AsyncMock(side_effect=TimeoutError('private server details'))
        wire=types.SimpleNamespace(connect=AsyncMock(),request=AsyncMock(return_value={}),close=AsyncMock())
        with patch.object(target,'MatterWire',return_value=wire),patch.object(target,'camera_identity',return_value='identity'):
            self.assertTrue(await target.async_setup_entry(self.hass,self.entry))
        refresh=self.hass.data['g2h_matter_camera']['existing-camera']['cache_refresh']
        self.assertTrue(refresh['attempted']);self.assertFalse(refresh['succeeded']);self.assertEqual(refresh['error_class'],'TimeoutError')
        self.assertNotIn('private server details',str(refresh))
        self.entries.async_forward_entry_setups.assert_awaited_once()
    async def test_unverified_offline_identity_never_interviews(self):
        self.client.get_node=lambda node_id:types.SimpleNamespace(available=False);self.client.interview_node=AsyncMock()
        wire=types.SimpleNamespace(connect=AsyncMock(),request=AsyncMock(return_value={}),close=AsyncMock())
        with patch.object(target,'MatterWire',return_value=wire),patch.object(target,'camera_identity',return_value='different-camera'):
            with self.assertRaises(NotReady):await target.async_setup_entry(self.hass,self.entry)
        self.client.interview_node.assert_not_awaited()
if __name__=='__main__':unittest.main()
