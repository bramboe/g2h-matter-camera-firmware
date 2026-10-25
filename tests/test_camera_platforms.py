"""HA coordinator/entity lifecycle fixtures; no HA credentials or sockets."""
import asyncio
import importlib.util
from pathlib import Path
import sys
import types
import unittest
from unittest.mock import AsyncMock, patch
ROOT=Path(__file__).parents[1]/'home-assistant/custom_components/g2h_matter_camera'
def module(name, **values):
    result=types.ModuleType(name);result.__dict__.update(values);return result
class UpdateFailed(Exception):pass
class Coordinator:
    def __init__(self,hass,logger,**kwargs):
        self.hass=hass;self.interval=kwargs.get('update_interval');self._update_interval_seconds=self.interval.total_seconds() if self.interval else None;self.config_entry=kwargs.get('config_entry');self._shutdown_requested=False;self._retry_after=None;self._unsub_refresh=None;self.always_update=kwargs.get('always_update',True);self.data=None;self.last_update_success=True;self.shutdowns=0;self.notifications=0
    async def async_refresh(self):
        before=self.data;was_success=self.last_update_success
        try:
            self.data=await self._async_update_data();self.last_update_success=True
            if self.always_update or not was_success or self.data!=before:self.async_update_listeners()
        except UpdateFailed:
            self.last_update_success=False
            if was_success:self.async_update_listeners()
    def async_set_updated_data(self,data):
        before=self.data;was_success=self.last_update_success;self.data=data;self.last_update_success=True
        if self.always_update or not was_success or data!=before:self.async_update_listeners()
    def _async_unsub_refresh(self):
        if self._unsub_refresh:self._unsub_refresh();self._unsub_refresh=None
    async def _handle_refresh_interval(self):
        self._unsub_refresh=None
        await self.async_refresh()
        if not self._shutdown_requested:self._schedule_refresh()
    async def async_shutdown(self):
        self.shutdowns+=1;self._shutdown_requested=True;self._async_unsub_refresh()
    def async_update_listeners(self):self.notifications+=1
class CoordinatorEntity:
    def __init__(self,coordinator):self.coordinator=coordinator
    @property
    def available(self):return self.coordinator.last_update_success
class Sensor:pass
class BinarySensor:pass
class MatterError(Exception):pass
class ClientError(Exception):pass
pkg=module('g2h_platform_test',DOMAIN='g2h_matter_camera');pkg.__path__=[str(ROOT)]
stubs={
 'g2h_platform_test':pkg,
 'homeassistant.components.matter.helpers':module('homeassistant.components.matter.helpers',
    get_matter=lambda hass:hass.adapter,get_node_device_identifier=lambda info,node:('matter',f'existing-{node}')),
 'homeassistant.components.sensor':module('homeassistant.components.sensor',SensorEntity=Sensor),
 'homeassistant.components.binary_sensor':module('homeassistant.components.binary_sensor',BinarySensorEntity=BinarySensor,
    BinarySensorDeviceClass=types.SimpleNamespace(MOTION='motion')),
 'homeassistant.helpers.device_registry':module('homeassistant.helpers.device_registry',DeviceInfo=dict),
 'homeassistant.helpers.update_coordinator':module('homeassistant.helpers.update_coordinator',DataUpdateCoordinator=Coordinator,
    CoordinatorEntity=CoordinatorEntity,UpdateFailed=UpdateFailed),
 'homeassistant.const':module('homeassistant.const',EntityCategory=types.SimpleNamespace(DIAGNOSTIC='diagnostic')),
 'matter_server.common.errors':module('matter_server.common.errors',MatterError=MatterError),
 'matter_server.client.exceptions':module('matter_server.client.exceptions',MatterClientException=ClientError),
}
def load(name):
    spec=importlib.util.spec_from_file_location('g2h_platform_test.'+name,ROOT/(name+'.py'))
    result=importlib.util.module_from_spec(spec);sys.modules[spec.name]=result;spec.loader.exec_module(result);return result
with patch.dict(sys.modules,stubs):
    diagnostics=load('diagnostics');telemetry=load('thread_telemetry');sensor=load('sensor');motion=load('binary_sensor')
class Platforms(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.entry=types.SimpleNamespace(entry_id='camera-entry',pref_disable_polling=False,async_create_background_task=lambda hass,coroutine,**kwargs:asyncio.create_task(coroutine),data={'matter_entry_id':'matter-entry','node_id':73,'unique_id':'persistent-camera'})
        self.client=types.SimpleNamespace(read_attribute=AsyncMock(),server_info=object())
        self.hass=types.SimpleNamespace(is_stopping=False,loop=asyncio.get_running_loop(),adapter=types.SimpleNamespace(matter_client=self.client),
            config_entries=types.SimpleNamespace(async_get_entry=lambda key:object()),
            data={'g2h_matter_camera':{'camera-entry':{'client':self.client}}})
    async def test_older_firmware_loads_camera_platforms(self):
        p=telemetry.PATH;self.client.read_attribute.return_value={p+'0':True,p+'1':'router',p+'2':25}
        entities=[];await sensor.async_setup_entry(self.hass,self.entry,entities.extend)
        role=next(e for e in entities if e.key=='role');neighbors=next(e for e in entities if e.key=='neighbors')
        self.assertTrue(role.available);self.assertEqual(role._attr_unique_id,'persistent-camera-thread-role')
        self.assertFalse(neighbors.available);self.assertIsNone(neighbors.native_value)
        self.client.read_attribute.side_effect=MatterError('unsupported cluster')
        entities=[];await motion.async_setup_entry(self.hass,self.entry,entities.extend)
        self.assertEqual(len(entities),1);self.assertFalse(entities[0].available)
        self.assertIsNone(entities[0].is_on);self.assertEqual(entities[0]._attr_device_info,role._attr_device_info)
    async def test_optional_stale_does_not_break_other_radio_fields(self):
        p=telemetry.PATH;self.client.read_attribute.return_value={p+'0':True,p+'1':'router',p+'2':25,
            p+'9':False,p+'10':7,p+'12':True,p+'13':0,p+'18':True,p+'19':19}
        entities=[];await sensor.async_setup_entry(self.hass,self.entry,entities.extend);by={e.key:e for e in entities}
        self.assertTrue(by['role'].available);self.assertFalse(by['neighbors'].available)
        self.assertTrue(by['children'].available);self.assertEqual(by['children'].native_value,0)
        self.assertEqual(by['tx_total'].native_value,19)
    async def test_unsupported_optional_read_preserves_core(self):
        p=telemetry.PATH
        async def reply(node, paths):
            if paths == [p+str(i) for i in (0,1,2)]:
                return {p+'0':True,p+'1':'router',p+'2':25}
            raise MatterError('optional attributes unsupported')
        self.client.read_attribute.side_effect=reply
        coordinator=sensor.RadioCoordinator(self.hass,self.entry);await coordinator.async_refresh()
        self.assertTrue(coordinator.last_update_success);self.assertEqual(coordinator.data['role'],'router')
        self.assertIsNone(coordinator.data['neighbors'])
        self.assertTrue(all(isinstance(call.args[1],list) for call in self.client.read_attribute.await_args_list))
    async def test_large_failed_table_does_not_hide_counts_or_other_table(self):
        p=telemetry.PATH
        async def reply(node, paths):
            if paths == [p+str(i) for i in (0,1,2)]:return {p+'0':True,p+'1':'router',p+'2':25}
            if paths == [p+'11']:raise MatterError('oversized table')
            if paths == [p+'14']:return {p+'14':'validated child table'}
            if paths == [p+'17']:raise MatterError('unsupported table')
            return {p+'9':True,p+'10':2,p+'12':True,p+'13':1,p+'15':True,p+'16':3}
        self.client.read_attribute.side_effect=reply
        coordinator=sensor.RadioCoordinator(self.hass,self.entry);await coordinator.async_refresh()
        self.assertTrue(coordinator.last_update_success)
        self.assertEqual(coordinator.data['neighbors'],2);self.assertIsNone(coordinator.data['neighbors_table'])
        self.assertEqual(coordinator.data['children_table'],'validated child table')
        self.assertEqual(coordinator.data['routers'],3);self.assertIsNone(coordinator.data['routers_table'])
    async def test_optional_timeout_preserves_core_with_bounded_delay(self):
        p=telemetry.PATH
        async def reply(node, paths):
            if paths == [p+str(i) for i in (0,1,2)]:return {p+'0':True,p+'1':'router',p+'2':25}
            await asyncio.sleep(30)
        self.client.read_attribute.side_effect=reply
        coordinator=sensor.RadioCoordinator(self.hass,self.entry);began=self.hass.loop.time();await coordinator.async_refresh()
        self.assertLess(self.hass.loop.time()-began,1.5);self.assertTrue(coordinator.last_update_success)
        self.assertEqual(coordinator.data['channel'],25)
    async def test_motion_false_real_then_locally_expires(self):
        p=diagnostics.MOTION_PATH;self.client.read_attribute.return_value={p+'0':True,p+'1':False,p+'2':True,
            p+'3':'native-held-motion',p+'5':10,p+'6':60}
        entities=[];await motion.async_setup_entry(self.hass,self.entry,entities.extend);entity=entities[0];coordinator=entity.coordinator
        self.assertTrue(entity.available);self.assertIs(entity.is_on,False)
        self.assertEqual(coordinator.interval.total_seconds(),.5)
        p=diagnostics.MOTION_PATH
        self.client.read_attribute.assert_awaited_once_with(73,[p+str(i) for i in (0,1,2,3,5,6)])
        self.assertEqual(entity._attr_device_class,'motion');self.assertFalse(hasattr(entity,'_attr_entity_category'))
        await asyncio.sleep(.08);self.assertFalse(entity.available);self.assertIsNone(entity.is_on);self.assertGreaterEqual(coordinator.notifications,1)
        await coordinator.close();self.assertIsNone(coordinator.expiry);self.assertTrue(coordinator.closed)
    async def test_disconnect_fails_cleanly_and_shutdown_prevents_reads(self):
        coordinator=motion.MotionCoordinator(self.hass,self.entry)
        self.client.read_attribute.side_effect=ClientError('transport internals')
        await coordinator.async_refresh();self.assertFalse(coordinator.last_update_success)
        await coordinator.close();self.assertEqual(coordinator.shutdowns,1)
        self.client.read_attribute.reset_mock();await coordinator.async_refresh()
        self.client.read_attribute.assert_not_awaited()
    async def test_motion_change_only_no_age_recorder_churn(self):
        p=diagnostics.MOTION_PATH
        values={p+'0':True,p+'1':False,p+'2':True,p+'3':'native-held-motion',p+'5':10,p+'6':2000}
        self.client.read_attribute.return_value=values
        entities=[];await motion.async_setup_entry(self.hass,self.entry,entities.extend);entity=entities[0];coordinator=entity.coordinator
        self.assertFalse(coordinator.always_update);initial=coordinator.notifications;attributes=entity.extra_state_attributes
        self.assertNotIn('sample_age_ms',attributes);self.assertNotIn('sample_age_ms',coordinator.data)
        for age in (20,30,40,50):
            values[p+'5']=age;await coordinator.async_refresh()
            self.assertEqual(coordinator.notifications,initial);self.assertEqual(entity.extra_state_attributes,attributes)
        values[p+'1']=True;await coordinator.async_refresh();self.assertEqual(coordinator.notifications,initial+1);self.assertTrue(entity.is_on)
        await coordinator.async_refresh();self.assertEqual(coordinator.notifications,initial+1)
        values[p+'1']=False;await coordinator.async_refresh();self.assertEqual(coordinator.notifications,initial+2);self.assertFalse(entity.is_on)
        await coordinator.close()
    async def test_expiry_during_read_republishes_same_boolean_recovery(self):
        p=diagnostics.MOTION_PATH
        values={p+'0':True,p+'1':False,p+'2':True,p+'3':'native-held-motion',p+'5':0,p+'6':60}
        self.client.read_attribute.return_value=values
        entities=[];await motion.async_setup_entry(self.hass,self.entry,entities.extend)
        entity=entities[0];coordinator=entity.coordinator
        initial=coordinator.notifications
        async def delayed(node,paths):
            await asyncio.sleep(.09)
            return dict(values,**{p+'6':200})
        self.client.read_attribute.side_effect=delayed
        # Capture the pre-read data just as HA does. Expiry runs while waiting.
        await coordinator.async_refresh()
        self.assertTrue(entity.available);self.assertIs(entity.is_on,False)
        self.assertEqual(coordinator.notifications,initial+2)
        self.assertEqual(coordinator.data['expiry_generation'],1)
        self.assertEqual(coordinator.poll_timing['expiries'],1)
        self.assertEqual(coordinator.poll_timing['expired_during_read'],1)
        self.assertEqual(coordinator.poll_timing['reads'],2)
        self.assertGreaterEqual(coordinator.poll_timing['last_read_ms'],80)
        await coordinator.close()
    async def test_slow_response_never_extends_the_sample_lease(self):
        p=diagnostics.MOTION_PATH
        async def delayed(node,paths):
            await asyncio.sleep(.09)
            return {p+'0':True,p+'1':False,p+'2':True,p+'3':'native-held-motion',p+'5':0,p+'6':60}
        self.client.read_attribute.side_effect=delayed
        coordinator=motion.MotionCoordinator(self.hass,self.entry)
        await coordinator.async_refresh()
        self.assertFalse(coordinator.fresh)
        self.assertEqual(coordinator.poll_timing['remaining_at_response_ms'],0)
        await coordinator.close()
    async def test_expiry_and_same_boolean_recovery_each_notify_once(self):
        p=diagnostics.MOTION_PATH;values={p+'0':True,p+'1':False,p+'2':True,p+'3':'native-held-motion',p+'5':10,p+'6':100}
        self.client.read_attribute.return_value=values
        entities=[];await motion.async_setup_entry(self.hass,self.entry,entities.extend);entity=entities[0];coordinator=entity.coordinator
        initial=coordinator.notifications;await asyncio.sleep(.13)
        self.assertEqual(coordinator.notifications,initial+1);self.assertFalse(entity.available);self.assertIsNone(entity.is_on)
        coordinator._expire();self.assertEqual(coordinator.notifications,initial+1)
        self.client.read_attribute.return_value={p+'0':False,p+'1':None,p+'2':None,p+'3':'stale',p+'5':200,p+'6':100}
        await coordinator.async_refresh();self.assertEqual(coordinator.notifications,initial+1)
        values[p+'5']=0;self.client.read_attribute.return_value=values
        await coordinator.async_refresh();self.assertTrue(entity.available);self.assertIs(entity.is_on,False)
        self.assertEqual(coordinator.notifications,initial+2)
        await coordinator.async_refresh();self.assertEqual(coordinator.notifications,initial+2)
        await coordinator.close()
if __name__=='__main__':unittest.main()
