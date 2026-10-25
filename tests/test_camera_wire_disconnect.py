"""Unexpected transport loss must release a live camera, no real sockets."""
import asyncio,importlib.util
from pathlib import Path
import types,unittest
from unittest.mock import AsyncMock
from aiohttp import WSMsgType
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('g2h_wire_disconnect_test',ROOT/'home-assistant/custom_components/g2h_matter_camera/protocol.py')
target=importlib.util.module_from_spec(spec);spec.loader.exec_module(target)
class Socket:
 def __init__(self,messages=(),hold=False):self.messages=messages;self.hold=hold;self.closed=False;self.gate=asyncio.Event()
 async def __aiter__(self):
  for message in self.messages:yield message
  if self.hold:await self.gate.wait()
 async def close(self):self.closed=True;self.gate.set()
class WireDisconnect(unittest.IsolatedAsyncioTestCase):
 async def test_remote_eof_notifies_and_fails_pending_request_without_payloads(self):
  events=[];wire=target.MatterWire(None,'ws://unused',events.append);wire.ws=Socket()
  pending=asyncio.get_running_loop().create_future();wire.pending['request']=pending
  await wire._read()
  self.assertEqual(events,[{'event_type':'connection_closed'}])
  self.assertIsInstance(pending.exception(),target.MatterCameraError)
  with self.assertRaises(target.MatterCameraError):await wire.request('device_command')
  await wire.close();self.assertEqual(len(events),1)
 async def test_explicit_teardown_does_not_report_a_connection_failure(self):
  events=[];wire=target.MatterWire(None,'ws://unused',events.append);wire.ws=Socket(hold=True)
  wire.reader=asyncio.create_task(wire._read());await asyncio.sleep(0)
  await wire.close();self.assertTrue(wire.reader.done());self.assertEqual(events,[])
 async def test_invalid_server_frame_closes_without_exporting_raw_details(self):
  def invalid():raise ValueError('PRIVATE_SDP_MUST_NOT_LEAK')
  events=[];wire=target.MatterWire(None,'ws://unused',events.append)
  wire.ws=Socket([types.SimpleNamespace(type=WSMsgType.TEXT,json=invalid)])
  await wire._read();self.assertEqual(events,[{'event_type':'connection_closed'}])
  self.assertNotIn('PRIVATE',str(events));await wire.close()
if __name__=='__main__':unittest.main()
