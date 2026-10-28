"""Automatic attachment reuses Matter, verifies identity and avoids duplicates."""
import asyncio
import importlib.util
from pathlib import Path
import sys
import types
import unittest
from unittest.mock import AsyncMock, Mock, patch

ROOT=Path(__file__).parents[1]/'home-assistant/custom_components/g2h_matter_camera'
def module(name,**values):
    value=types.ModuleType(name);value.__dict__.update(values);return value
pkg=module('g2h_auto_test');pkg.__path__=[str(ROOT)]
stubs={'g2h_auto_test':pkg,
    'homeassistant.components.matter.helpers':module('homeassistant.components.matter.helpers',get_matter=lambda hass:hass.adapter)}
with patch.dict(sys.modules,stubs):
    spec=importlib.util.spec_from_file_location('g2h_auto_test.auto_attach',ROOT/'auto_attach.py')
    target=importlib.util.module_from_spec(spec);spec.loader.exec_module(target)

def node(node_id=73,product='Aqara G2H Thread & Matter Camera'):
    return types.SimpleNamespace(node_id=node_id,node_data=types.SimpleNamespace(attributes={'0/40/3':product}))

class AutoAttach(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.nodes=[node(3,'BILRESA'),node(68,'G2H Thread-router'),node()]
        self.entries=[]
        self.matter_entries=[types.SimpleNamespace(entry_id='existing-matter')]
        self.flow=AsyncMock(return_value={'type':'create_entry'})
        self.client=types.SimpleNamespace(get_nodes=lambda:self.nodes)
        self.hass=types.SimpleNamespace(data={},adapter=types.SimpleNamespace(matter_client=self.client),
            config_entries=types.SimpleNamespace(async_loaded_entries=lambda domain:self.matter_entries,
                async_entries=lambda domain:self.entries,flow=types.SimpleNamespace(async_init=self.flow)))
        self.scanner=target.AutoAttach(self.hass)

    async def test_only_camera_candidates_use_existing_matter_entry(self):
        await self.scanner.async_scan()
        self.flow.assert_awaited_once_with('g2h_matter_camera',context={'source':'import'},
            data={'matter_entry_id':'existing-matter','node_id':73})
        self.assertEqual(self.scanner.counters['created'],1)

    async def test_existing_or_disabled_camera_is_not_read_or_recreated(self):
        self.entries=[types.SimpleNamespace(disabled_by='user',data={'matter_entry_id':'existing-matter','node_id':73})]
        await self.scanner.async_scan();await self.scanner.async_scan()
        self.flow.assert_not_awaited()

    async def test_startup_then_new_pairing_then_repeated_scans(self):
        self.matter_entries=[];await self.scanner.async_scan();self.flow.assert_not_awaited()
        self.matter_entries=[types.SimpleNamespace(entry_id='existing-matter')]
        self.nodes=[];await self.scanner.async_scan();self.flow.assert_not_awaited()
        async def create(*args,**kwargs):
            self.entries.append(types.SimpleNamespace(data=kwargs['data']))
            return {'type':'create_entry'}
        self.flow.side_effect=create;self.nodes=[node()]
        await self.scanner.async_scan();await self.scanner.async_scan()
        self.flow.assert_awaited_once()

    async def test_overlapping_slow_scans_never_queue_another_flow(self):
        started=asyncio.Event();release=asyncio.Event()
        async def slow(*args,**kwargs):
            started.set();await release.wait();return {'type':'abort','reason':'matter_read_failed'}
        self.flow.side_effect=slow
        task=asyncio.create_task(self.scanner.async_scan());await started.wait()
        await self.scanner.async_scan();self.assertEqual(self.flow.await_count,1)
        release.set();await task;self.assertIsNone(self.scanner.task)

    async def test_read_failure_retries_without_sticky_partial_entry(self):
        self.flow.return_value={'type':'abort','reason':'matter_read_failed'}
        await self.scanner.async_scan()
        self.flow.return_value={'type':'create_entry'}
        await self.scanner.async_scan()
        self.assertEqual(self.scanner.counters['failed'],1)
        self.assertEqual(self.scanner.counters['created'],1)

    async def test_concurrent_manual_flow_duplicate_is_not_an_error(self):
        self.flow.return_value={'type':'abort','reason':'already_configured'}
        await self.scanner.async_scan()
        self.assertEqual(self.scanner.counters['already_configured'],1)
        self.assertEqual(self.scanner.counters['created'],0)

    async def test_stop_cancels_observer_and_pending_scan_not_shared_client(self):
        started=asyncio.Event()
        async def pending(*args,**kwargs):started.set();await asyncio.Event().wait()
        self.flow.side_effect=pending;self.scanner.cancel_interval=Mock();self.scanner.cancel_started=Mock()
        task=asyncio.create_task(self.scanner.async_scan());await started.wait()
        await self.scanner.async_close()
        self.assertTrue(task.cancelled());self.assertIsNone(self.scanner.task)
        self.scanner.cancel_interval.assert_called_once();self.scanner.cancel_started.assert_called_once()
        await self.scanner.async_scan();self.assertEqual(self.flow.await_count,1)

if __name__=='__main__':unittest.main()
