"""Config-flow transport recovery and bounded public version metadata."""
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest.mock import AsyncMock,patch
from aiohttp import ClientConnectionError
ROOT=Path(__file__).parents[1]/'home-assistant/custom_components/g2h_matter_camera'
def module(name,**values):
    r=types.ModuleType(name);r.__dict__.update(values);return r
class Flow:
    def __init_subclass__(cls,**kwargs):super().__init_subclass__()
    def async_show_form(self,**kwargs):return kwargs
    def async_abort(self,**kwargs):return kwargs
pkg=module('g2h_flow_test',DOMAIN='g2h_matter_camera');pkg.__path__=[str(ROOT)]
entries=module('homeassistant.config_entries',ConfigFlow=Flow)
stubs={
 'g2h_flow_test':pkg,
 'homeassistant':module('homeassistant',config_entries=entries),
 'homeassistant.config_entries':entries,
 'voluptuous':module('voluptuous',Schema=lambda schema:schema,Required=lambda key,**kwargs:key,In=lambda choices:choices),
 'homeassistant.const':module('homeassistant.const',CONF_URL='url'),
 'homeassistant.components.matter.helpers':module('homeassistant.components.matter.helpers',get_matter=lambda hass:hass.adapter),
 'homeassistant.helpers.aiohttp_client':module('homeassistant.helpers.aiohttp_client',async_get_clientsession=lambda hass:object()),
}
with patch.dict(sys.modules,stubs):
    spec=importlib.util.spec_from_file_location('g2h_flow_test.config_flow',ROOT/'config_flow.py');target=importlib.util.module_from_spec(spec);spec.loader.exec_module(target)
class ConfigFlow(unittest.IsolatedAsyncioTestCase):
    async def test_disconnect_returns_actionable_error_and_current_metadata(self):
        with tempfile.TemporaryDirectory(prefix='g2h-config-flow-') as d:
            node=types.SimpleNamespace(node_id=73,available=True,node_data=types.SimpleNamespace(attributes={'0/40/3':'Aqara G2H Thread & Matter Camera'}))
            client=types.SimpleNamespace(get_nodes=lambda:[node],server_info=types.SimpleNamespace(schema_version=13,sdk_version='test-sdk'))
            async def executor(fn,*args):return fn(*args)
            hass=types.SimpleNamespace(adapter=types.SimpleNamespace(matter_client=client),
                config_entries=types.SimpleNamespace(async_loaded_entries=lambda domain:[types.SimpleNamespace(entry_id='existing-matter',data={'url':'ws://existing.invalid/ws'})]),
                config=types.SimpleNamespace(path=lambda name:str(Path(d)/name)),async_add_executor_job=executor)
            wire=types.SimpleNamespace(connect=AsyncMock(side_effect=ClientConnectionError('private socket details')),close=AsyncMock())
            flow=target.ConfigFlow();flow.hass=hass
            with patch.object(target,'MatterWire',return_value=wire):result=await flow.async_step_user({'node_id':'73'})
            self.assertEqual(result['errors'],{'base':'matter_read_failed'});wire.close.assert_awaited_once()
            metadata=json.loads((Path(d)/'g2h_matter_camera_diagnostic.json').read_text())
            self.assertEqual(metadata['extension_version'],'0.4.0')
            self.assertEqual(metadata['candidate_nodes'][0]['node_id'],73)
            self.assertNotIn('private socket details',json.dumps(result))
if __name__=='__main__':unittest.main()
