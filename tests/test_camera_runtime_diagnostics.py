"""Public-only runtime export and two read-only probe paths; no real sockets."""
import asyncio
import importlib.util
import json
from pathlib import Path
import sys
import types
import unittest
from unittest.mock import AsyncMock,patch
ROOT=Path(__file__).parents[1]/'home-assistant/custom_components/g2h_matter_camera'
def module(name,**values):
    m=types.ModuleType(name);m.__dict__.update(values);return m
class CommonError(Exception):pass
class ClientError(Exception):pass
class Wire:
    response={};failure=None;slow_close=False;requests=[]
    def __init__(self,session,url):self.reader=None
    async def connect(self):
        if self.failure is not None:raise self.failure
    async def request(self,command,**kwargs):
        self.requests.append((command,kwargs));return self.response
    async def close(self):
        if self.slow_close:await asyncio.sleep(30)
pkg=module('g2h_runtime_test',DOMAIN='g2h_matter_camera');pkg.__path__=[str(ROOT)]
stubs={'g2h_runtime_test':pkg,
 'homeassistant.components.matter.helpers':module('homeassistant.components.matter.helpers',get_matter=lambda hass:hass.adapter),
 'matter_server.common.errors':module('matter_server.common.errors',MatterError=CommonError),
 'matter_server.client.exceptions':module('matter_server.client.exceptions',MatterClientException=ClientError),
 'homeassistant.const':module('homeassistant.const',CONF_URL='url'),
 'homeassistant.helpers.aiohttp_client':module('homeassistant.helpers.aiohttp_client',async_get_clientsession=lambda hass:object()),
 'g2h_runtime_test.protocol':module('g2h_runtime_test.protocol',MatterWire=Wire)}
with patch.dict(sys.modules,stubs):
    spec=importlib.util.spec_from_file_location('g2h_runtime_test.diagnostics',ROOT/'diagnostics.py');target=importlib.util.module_from_spec(spec);spec.loader.exec_module(target)
class RuntimeExport(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        p=f'0/{0xFFF1FC10}/';self.values={p+'0':True,p+'1':'router',p+'2':25,'private-key':'DO_NOT_EXPORT'}
        self.entry=types.SimpleNamespace(entry_id='camera',data={'node_id':73,'matter_entry_id':'matter','dataset':'DO_NOT_EXPORT'})
        self.client=types.SimpleNamespace(read_attribute=AsyncMock(return_value=self.values))
        cause=ClientError('DO_NOT_EXPORT');error=ValueError('DO_NOT_EXPORT');error.__cause__=cause
        radio=types.SimpleNamespace(last_update_success=False,last_exception=error,closed=False,
            data={'role':'router','channel':25,'children':0,'neighbors_table':'DO_NOT_EXPORT'})
        motion=types.SimpleNamespace(poll_timing={'reads':12,'expiries':1,'last_read_ms':25,'private':'DO_NOT_EXPORT','errors':False},last_update_success=True,last_exception=None,closed=False,fresh=False,
            data={'motion':False,'producer_enabled':False,'reason':'native-producer-disabled','sample_age_ms':100,'freshness_window_ms':2000})
        self.hass=types.SimpleNamespace(adapter=types.SimpleNamespace(matter_client=self.client),
            config_entries=types.SimpleNamespace(async_get_entry=lambda key:types.SimpleNamespace(data={'url':'ws://DO_NOT_EXPORT.invalid/?token=DO_NOT_EXPORT'})),
            data={'g2h_matter_camera':{'camera':{'client':self.client,'node':types.SimpleNamespace(available=False),'radio':radio,'motion':motion,'cache_refresh':{'attempted':True,'succeeded':False,'cached_node_available_after':False,'error_class':'TimeoutError','cause_class':None,'private_error':'DO_NOT_EXPORT'}}}})
        Wire.response=self.values;Wire.failure=None;Wire.slow_close=False;Wire.requests=[]
    async def test_probes_compare_stale_cache_without_private_values(self):
        with patch.dict(sys.modules,stubs):result=await target.async_get_config_entry_diagnostics(self.hass,self.entry)
        self.assertFalse(result['cached_node_available'])
        self.assertTrue(result['cache_refresh']['attempted']);self.assertEqual(result['cache_refresh']['error_class'],'TimeoutError')
        self.assertEqual(result['official_client_core']['role'],'router');self.assertEqual(result['independent_wire_core']['role'],'router')
        self.assertEqual(result['radio']['error_class'],'ValueError');self.assertEqual(result['radio']['cause_class'],'ClientError')
        self.assertEqual(result['radio']['table_lengths']['neighbors'],13);self.assertIsNone(result['motion']['motion'])
        self.assertNotIn('DO_NOT_EXPORT',json.dumps(result))
        self.assertEqual(result['motion']['poll_timing'],{'reads':12,'expiries':1,'last_read_ms':25})
        paths=[f'0/{0xFFF1FC10}/{i}' for i in (0,1,2)]
        self.client.read_attribute.assert_awaited_once_with(73,paths)
        self.assertEqual(Wire.requests,[('read_attribute',{'node_id':73,'attribute_path':paths})])
    async def test_independent_failures_and_cleanup_bounded(self):
        self.client.read_attribute.side_effect=ClientError('DO_NOT_EXPORT');Wire.failure=CommonError('DO_NOT_EXPORT');Wire.slow_close=True
        began=asyncio.get_running_loop().time()
        with patch.dict(sys.modules,stubs):result=await target.async_get_config_entry_diagnostics(self.hass,self.entry)
        self.assertLess(asyncio.get_running_loop().time()-began,1)
        self.assertEqual(result['official_client_core']['error_class'],'ValueError')
        self.assertEqual(result['official_client_core']['cause_class'],'ClientError')
        self.assertEqual(result['independent_wire_core']['error_class'],'CommonError')
        self.assertNotIn('DO_NOT_EXPORT',json.dumps(result))
if __name__=='__main__':unittest.main()
