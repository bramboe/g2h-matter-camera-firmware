"""Cached Matter availability events update HA state without new subscriptions."""
import asyncio
import importlib.util
from pathlib import Path
import sys
import types
import unittest
from unittest.mock import Mock,AsyncMock,patch
ROOT=Path(__file__).parents[1]/'home-assistant/custom_components/g2h_matter_camera'
def module(name,**values):
    m=types.ModuleType(name);m.__dict__.update(values);return m
class Camera:
    def __init__(self):self.removers=[];self.state_writes=0
    async def async_added_to_hass(self):pass
    def async_on_remove(self,callback):self.removers.append(callback)
    def async_write_ha_state(self):self.state_writes+=1
    async def async_will_remove_from_hass(self):pass
class CameraError(Exception):pass
pkg=module('g2h_camera_event_test',DOMAIN='g2h_matter_camera');pkg.__path__=[str(ROOT)]
events=types.SimpleNamespace(NODE_UPDATED='node_updated')
stubs={'g2h_camera_event_test':pkg,
 'homeassistant.components.camera':module('homeassistant.components.camera',Camera=Camera,CameraEntityFeature=types.SimpleNamespace(STREAM=1),WebRTCAnswer=object,WebRTCCandidate=object,WebRTCError=object),
 'homeassistant.components.matter.helpers':module('homeassistant.components.matter.helpers',get_node_device_identifier=lambda info,node:('matter',f'existing-{node}')),
 'homeassistant.const':module('homeassistant.const',CONF_URL='url'),
 'homeassistant.core':module('homeassistant.core',callback=lambda func:func),
 'homeassistant.helpers.aiohttp_client':module('homeassistant.helpers.aiohttp_client',async_get_clientsession=lambda hass:object()),
 'homeassistant.helpers.device_registry':module('homeassistant.helpers.device_registry',DeviceInfo=dict),
 'matter_server.common.models':module('matter_server.common.models',EventType=events),
 'webrtc_models':module('webrtc_models',RTCIceCandidateInit=object),
 'g2h_camera_event_test.protocol':module('g2h_camera_event_test.protocol',MatterWire=object,MatterCameraError=CameraError,VIDEO_ALLOCATION={},field=lambda *args:None),
 'g2h_camera_event_test.snapshot':module('g2h_camera_event_test.snapshot',capture_snapshot=AsyncMock())}
with patch.dict(sys.modules,stubs):
    spec=importlib.util.spec_from_file_location('g2h_camera_event_test.camera',ROOT/'camera.py');target=importlib.util.module_from_spec(spec);spec.loader.exec_module(target)
class AvailabilityEvents(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.unsubscribe=Mock();self.client=types.SimpleNamespace(server_info=object(),subscribe_events=Mock(return_value=self.unsubscribe),send_command=AsyncMock())
        self.entry=types.SimpleNamespace(entry_id='camera',data={'node_id':73,'unique_id':'existing-camera'})
        self.runtime={'client':self.client,'node':types.SimpleNamespace(node_id=73,available=False)}
        self.hass=types.SimpleNamespace(loop=asyncio.get_running_loop(),data={'g2h_matter_camera':{'camera':self.runtime}})
        self.entity=target.G2HMatterCamera(self.hass,self.entry)
    async def test_real_node_update_republishes_availability(self):
        self.assertFalse(self.entity.available);await self.entity.async_added_to_hass()
        args=self.client.subscribe_events.call_args;self.assertEqual(args.kwargs,{'event_filter':'node_updated','node_filter':73})
        callback=args.args[0];node=types.SimpleNamespace(node_id=73,available=True);callback(events.NODE_UPDATED,node)
        self.assertIs(self.runtime['node'],node);self.assertTrue(self.entity.available);self.assertEqual(self.entity.state_writes,1)
        node.available=False;callback(events.NODE_UPDATED,node);self.assertFalse(self.entity.available);self.assertEqual(self.entity.state_writes,2)
        self.client.send_command.assert_not_awaited()
    async def test_wrong_node_ignored_and_listener_removed(self):
        await self.entity.async_added_to_hass();callback=self.client.subscribe_events.call_args.args[0]
        callback(events.NODE_UPDATED,types.SimpleNamespace(node_id=74,available=True))
        self.assertFalse(self.entity.available);self.assertEqual(self.entity.state_writes,0)
        self.assertEqual(len(self.entity.removers),1)
        await self.entity.async_will_remove_from_hass()
        # HA Entity's normal removal lifecycle invokes async_on_remove hooks.
        self.entity.removers[0]();self.unsubscribe.assert_called_once()
if __name__=='__main__':unittest.main()
