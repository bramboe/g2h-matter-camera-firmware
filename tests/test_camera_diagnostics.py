"""Synthetic Matter responses and existing HA client binding, no HA credentials."""
import importlib.util
from pathlib import Path
import re,sys,types,unittest
from unittest.mock import AsyncMock,patch
from aiohttp import ClientConnectionError
ROOT=Path(__file__).resolve().parents[1]
def module(name,**values):
 result=types.ModuleType(name);result.__dict__.update(values);return result
class FakeMatterError(Exception):pass
class FakeClientError(Exception):pass
stubs={'g2h_test_camera':module('g2h_test_camera',DOMAIN='g2h_matter_camera'),
       'homeassistant.components.matter.helpers':module('homeassistant.components.matter.helpers',get_matter=lambda hass:hass.adapter),
       'matter_server.common.errors':module('matter_server.common.errors',MatterError=FakeMatterError),
       'matter_server.client.exceptions':module('matter_server.client.exceptions',MatterClientException=FakeClientError)}
with patch.dict(sys.modules,stubs):
 spec=importlib.util.spec_from_file_location('g2h_test_camera.diagnostics',ROOT/'home-assistant/custom_components/g2h_matter_camera/diagnostics.py')
 target=importlib.util.module_from_spec(spec);spec.loader.exec_module(target)
P=target.MOTION_PATH
class MotionValues(unittest.TestCase):
 def response(self,**overrides):
  values={'0':True,'1':False,'2':True,'3':'native-held-motion','5':400,'6':2000};values.update(overrides)
  return {P+k:v for k,v in values.items()}
 def test_clear_is_real_false_not_unknown(self):
  self.assertIs(target.motion_values(self.response())['motion'],False)
  self.assertIs(target.motion_values(self.response(**{'1':True}))['motion'],True)
 def test_unavailable_never_fabricates_clear(self):
  for changes in ({'0':False},{'1':None},{'1':0},{'2':False},{'2':None},{'5':2001},{'5':None},{'5':-1},{'5':True},{'6':None},{'6':0}):
   with self.subTest(changes=changes):self.assertIsNone(target.motion_values(self.response(**changes))['motion'])
  self.assertIsNone(target.motion_values({})['motion'])
 def test_untrusted_reason_sanitized(self):
  self.assertEqual(target.motion_values(self.response(**{'3':'arbitrary/private output'}))['reason'],'unavailable')
 def test_non_mapping_rejected(self):
  with self.assertRaises(ValueError):target.motion_values([])
class ExistingClient(unittest.IsolatedAsyncioTestCase):
 def setUp(self):
  self.client=types.SimpleNamespace(read_attribute=AsyncMock(return_value={P+'0':False}))
  self.entry=types.SimpleNamespace(entry_id='camera-config',data={'matter_entry_id':'existing-matter','node_id':68})
  self.hass=types.SimpleNamespace(adapter=types.SimpleNamespace(matter_client=self.client),
    config_entries=types.SimpleNamespace(async_get_entry=lambda key:types.SimpleNamespace(entry_id=key)),
    data={'g2h_matter_camera':{'camera-config':{'client':self.client}}})
 async def test_reads_configured_node_using_existing_client_only(self):
  result=await target.read_cluster(self.hass,self.entry,target.MOTION_CLUSTER)
  self.client.read_attribute.assert_awaited_once_with(68,f'0/{target.MOTION_CLUSTER}/*')
  self.assertEqual(result,{P+'0':False})
 async def test_replaced_client_refused(self):
  self.hass.adapter.matter_client=object()
  with self.assertRaises(ValueError):await target.read_cluster(self.hass,self.entry,target.MOTION_CLUSTER)
  self.client.read_attribute.assert_not_awaited()
 async def test_server_failure_and_invalid_shape(self):
  self.client.read_attribute.side_effect=FakeMatterError('synthetic')
  with self.assertRaises(ValueError):await target.read_cluster(self.hass,self.entry,target.MOTION_CLUSTER)
  self.client.read_attribute.side_effect=None;self.client.read_attribute.return_value=[]
  with self.assertRaises(ValueError):await target.read_cluster(self.hass,self.entry,target.MOTION_CLUSTER)
 async def test_client_disconnect_is_normalized(self):
  self.client.read_attribute.side_effect=FakeClientError('private transport internals')
  with self.assertRaisesRegex(ValueError,'Cannot read existing Matter diagnostics'):
   await target.read_cluster(self.hass,self.entry,target.MOTION_CLUSTER)
 async def test_low_level_websocket_failure_is_normalized(self):
  self.client.read_attribute.side_effect=ClientConnectionError('private transport internals')
  with self.assertRaisesRegex(ValueError,'Cannot read existing Matter diagnostics'):
   await target.read_cluster(self.hass,self.entry,target.MOTION_CLUSTER)
 async def test_timeout_propagates_as_failure(self):
  self.client.read_attribute.side_effect=TimeoutError()
  with self.assertRaises(TimeoutError):await target.read_cluster(self.hass,self.entry,target.MOTION_CLUSTER)
if __name__=='__main__':unittest.main()
