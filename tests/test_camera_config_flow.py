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
    async def async_set_unique_id(self,unique):self.unique=unique
    def _abort_if_unique_id_configured(self):
        if self.unique in getattr(self,'existing_ids',()):raise AlreadyConfigured()
    def async_create_entry(self,**kwargs):return {'type':'create_entry',**kwargs}
class AlreadyConfigured(Exception):pass
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
    def auto_flow(self,attributes=None,existing=()):
        node=types.SimpleNamespace(node_id=73,node_data=types.SimpleNamespace(attributes=attributes or {'0/40/3':'Aqara G2H Thread & Matter Camera'}))
        entries=types.SimpleNamespace(async_loaded_entries=lambda domain:[types.SimpleNamespace(entry_id='existing-matter',data={'url':'ws://existing.invalid/ws'})],async_entries=lambda domain:list(existing))
        flow=target.ConfigFlow();flow.hass=types.SimpleNamespace(config_entries=entries,
            adapter=types.SimpleNamespace(matter_client=types.SimpleNamespace(get_nodes=lambda:[node])))
        return flow
    def verified_wire(self):
        return types.SimpleNamespace(connect=AsyncMock(),close=AsyncMock(),require_camera_api=lambda:None,
            request=AsyncMock(return_value={'0/40/3':'Aqara G2H Thread & Matter Camera',
                '0/40/15':'g2hcam-test','0/40/18':'verified-camera','1/29/1':[1361,1363]}))
    async def test_import_verifies_live_identity_before_creating_same_node(self):
        flow=self.auto_flow();wire=self.verified_wire()
        with patch.object(target,'MatterWire',return_value=wire):
            result=await flow.async_step_import({'matter_entry_id':'existing-matter','node_id':73})
        self.assertEqual(result['data'],{'matter_entry_id':'existing-matter','node_id':73,'unique_id':'verified-camera'})
        self.assertEqual(flow.unique,'verified-camera');wire.close.assert_awaited_once()
    async def test_import_rejects_forged_cache_without_live_camera_clusters(self):
        flow=self.auto_flow();wire=self.verified_wire();wire.request.return_value['1/29/1']=[]
        with patch.object(target,'MatterWire',return_value=wire):
            result=await flow.async_step_import({'matter_entry_id':'existing-matter','node_id':73})
        self.assertEqual(result['reason'],'not_g2h_camera');self.assertFalse(hasattr(flow,'unique'))
        wire.close.assert_awaited_once()
    async def test_import_ignores_radio_and_unrelated_matter_nodes(self):
        flow=self.auto_flow({'0/40/3':'G2H Thread-router'})
        with patch.object(target,'MatterWire') as wire:
            result=await flow.async_step_import({'matter_entry_id':'existing-matter','node_id':73})
        self.assertEqual(result['reason'],'no_camera');wire.assert_not_called()
    async def test_import_existing_disabled_entry_skips_even_verification(self):
        flow=self.auto_flow(existing=[types.SimpleNamespace(disabled_by='user',data={'matter_entry_id':'existing-matter','node_id':73})])
        with patch.object(target,'MatterWire') as wire:
            result=await flow.async_step_import({'matter_entry_id':'existing-matter','node_id':73})
        self.assertEqual(result['reason'],'already_configured');wire.assert_not_called()
    async def test_import_unique_id_prevents_second_entry_for_same_camera(self):
        flow=self.auto_flow();flow.existing_ids={'verified-camera'};wire=self.verified_wire()
        with patch.object(target,'MatterWire',return_value=wire):
            with self.assertRaises(AlreadyConfigured):
                await flow.async_step_import({'matter_entry_id':'existing-matter','node_id':73})
        wire.close.assert_awaited_once()
    async def test_import_connection_failure_creates_nothing_and_cleans_up(self):
        flow=self.auto_flow();wire=self.verified_wire();wire.connect.side_effect=ClientConnectionError('private details')
        with patch.object(target,'MatterWire',return_value=wire):
            result=await flow.async_step_import({'matter_entry_id':'existing-matter','node_id':73})
        self.assertEqual(result,{'reason':'matter_read_failed'});wire.close.assert_awaited_once()
    async def test_import_refuses_old_server_and_different_matter_entry(self):
        flow=self.auto_flow();wire=self.verified_wire()
        def old():raise target.MatterCameraCompatibilityError(11)
        wire.require_camera_api=old
        with patch.object(target,'MatterWire',return_value=wire):
            result=await flow.async_step_import({'matter_entry_id':'existing-matter','node_id':73})
        self.assertEqual(result,{'reason':'camera_api_missing'});wire.request.assert_not_awaited()
        with patch.object(target,'MatterWire') as constructor:
            result=await flow.async_step_import({'matter_entry_id':'other-matter','node_id':73})
        self.assertEqual(result['reason'],'matter_not_ready');constructor.assert_not_called()

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
            self.assertEqual(metadata['extension_version'],'0.5.0')
            self.assertEqual(metadata['candidate_nodes'][0]['node_id'],73)
            self.assertNotIn('private socket details',json.dumps(result))
if __name__=='__main__':unittest.main()
