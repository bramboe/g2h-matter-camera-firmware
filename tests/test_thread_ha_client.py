"""Offline conformance against the cached official Matter client/helper API."""
import asyncio
import importlib.util
from pathlib import Path
import sys
import types
import unittest
from unittest.mock import AsyncMock
ROOT=Path(__file__).parents[1]/'home-assistant/custom_components/g2h_thread_diagnostics'
package=types.ModuleType('g2h_thread_diagnostics');package.__path__=[str(ROOT)];sys.modules[package.__name__]=package
helpers=types.ModuleType('homeassistant.components.matter.helpers')
helpers.get_matter=lambda hass:hass.adapter
sys.modules[helpers.__name__]=helpers
errors=types.ModuleType('matter_server.common.errors')
class MatterError(Exception):pass
errors.MatterError=MatterError;sys.modules[errors.__name__]=errors
spec=importlib.util.spec_from_file_location('g2h_thread_diagnostics.wire',ROOT/'wire.py');wire=importlib.util.module_from_spec(spec);spec.loader.exec_module(wire)
class TestClient(unittest.IsolatedAsyncioTestCase):
    def hass(self):
        client=types.SimpleNamespace(read_attribute=AsyncMock(return_value={'ok':True}))
        return types.SimpleNamespace(adapter=types.SimpleNamespace(matter_client=client),config_entries=types.SimpleNamespace(async_loaded_entries=lambda domain:[types.SimpleNamespace(entry_id='existing')]))
    async def test_uses_only_existing_client_read(self):
        hass=self.hass();self.assertEqual(await wire.read(hass,'existing',70),{'ok':True})
        hass.adapter.matter_client.read_attribute.assert_awaited_once_with(70,'0/4294048784/*')
    async def test_missing_existing_entry(self):
        hass=self.hass()
        with self.assertRaises(ValueError):await wire.read(hass,'other',70)
        hass.adapter.matter_client.read_attribute.assert_not_awaited()
    async def test_refused_and_malformed(self):
        hass=self.hass();hass.adapter.matter_client.read_attribute.side_effect=MatterError('private server response')
        with self.assertRaisesRegex(ValueError,'Matter diagnostics read refused'):await wire.read(hass,'existing',70)
        hass.adapter.matter_client.read_attribute.side_effect=None;hass.adapter.matter_client.read_attribute.return_value=[]
        with self.assertRaisesRegex(ValueError,'Invalid Matter diagnostics response'):await wire.read(hass,'existing',70)
if __name__=='__main__':unittest.main()
