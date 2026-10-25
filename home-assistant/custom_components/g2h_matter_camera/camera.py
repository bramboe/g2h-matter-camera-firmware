"""Native Matter signalling and encrypted WebRTC, using the existing HA fabric."""
import asyncio
from contextlib import suppress
from homeassistant.components.camera import Camera,CameraEntityFeature,WebRTCAnswer,WebRTCCandidate,WebRTCError
from homeassistant.components.matter.helpers import get_node_device_identifier
from homeassistant.const import CONF_URL
from homeassistant.core import callback
from homeassistant.helpers.aiohttp_client import async_get_clientsession
from homeassistant.helpers.device_registry import DeviceInfo
from webrtc_models import RTCIceCandidateInit
from matter_server.common.models import EventType
from . import DOMAIN
from .protocol import MatterWire,MatterCameraError,VIDEO_ALLOCATION,field
from .snapshot import capture_snapshot

async def async_setup_entry(hass,entry,async_add_entities):
    async_add_entities([G2HMatterCamera(hass,entry)])

class G2HMatterCamera(Camera):
    _attr_has_entity_name=True
    _attr_name='Camera'
    _attr_supported_features=CameraEntityFeature.STREAM
    _attr_should_poll=False
    def __init__(self,hass,entry):
        super().__init__();self.hass=hass;self.entry=entry;self.sessions={};self._snapshot_lock=asyncio.Lock();self._snapshot_jpeg=None;self._snapshot_at=0;self._snapshot_preference=None
        self.runtime=hass.data[DOMAIN][entry.entry_id]
        self._attr_unique_id=entry.data['unique_id']+'-camera'
        self._attr_device_info=DeviceInfo(identifiers={get_node_device_identifier(self.runtime['client'].server_info,entry.data['node_id'])})
    async def async_added_to_hass(self):
        await super().async_added_to_hass()
        @callback
        def node_updated(event, node):
            if node.node_id != self.entry.data['node_id']:
                return
            # A real cached-node update, not fabricated availability. This is
            # an in-memory HA-client listener, not a device subscription.
            self.runtime['node'] = node
            self.async_write_ha_state()
        self.async_on_remove(self.runtime['client'].subscribe_events(
            node_updated, event_filter=EventType.NODE_UPDATED,
            node_filter=self.entry.data['node_id']))
    @property
    def available(self):return self.runtime['node'].available
    async def async_camera_image(self,width=None,height=None):
        if not self.available:return None
        prefer_large=(width is None and height is None) or (width is not None and width>320) or (height is not None and height>180)
        async with self._snapshot_lock:
            if self._snapshot_jpeg is not None and self._snapshot_preference==prefer_large and self.hass.loop.time()-self._snapshot_at<3:return self._snapshot_jpeg
            entry=self.hass.config_entries.async_get_entry(self.entry.data['matter_entry_id'])
            if entry is None:return None
            wire=MatterWire(async_get_clientsession(self.hass),entry.data[CONF_URL])
            try:
                async with asyncio.timeout(12):
                    await wire.connect()
                    detail={'stage':'request'};self.runtime['snapshot']=detail
                    image=await capture_snapshot(wire,self.entry.data['node_id'],diagnostics=detail,prefer_large=prefer_large)
                self._snapshot_jpeg=image;self._snapshot_preference=prefer_large;self._snapshot_at=self.hass.loop.time();return image
            except (MatterCameraError,OSError,TimeoutError,KeyError,ValueError,TypeError) as err:
                self.runtime.setdefault('snapshot',{})['error_class']=type(err).__name__
                return None
            finally:await wire.close()
    async def async_handle_async_webrtc_offer(self,offer_sdp,session_id,send_message):
        if self.sessions:
            send_message(WebRTCError('busy','The G2H currently supports one video session'));return
        if not offer_sdp or len(offer_sdp)>16384:
            send_message(WebRTCError('invalid_offer','Video offer exceeds device limits'));return
        matter_entry=self.hass.config_entries.async_get_entry(self.entry.data['matter_entry_id'])
        if matter_entry is None:
            send_message(WebRTCError('matter_unavailable','Existing Matter integration is unavailable'));return
        record={'id':None,'events':[],'send':send_message,'closed':False,'candidates':[]}
        def event(data):
            if data.get('node_id')!=self.entry.data['node_id'] or data.get('endpoint_id')!=1:return
            if record['id'] is None:
                if len(record['events'])<16:record['events'].append(data)
                return
            if data.get('webrtc_session_id')!=record['id']:return
            payload=data.get('data') or {}
            if data.get('event_type')=='answer':send_message(WebRTCAnswer(payload['sdp']))
            elif data.get('event_type')=='ice_candidates':
                for candidate in payload.get('ice_candidates',[]):
                    send_message(WebRTCCandidate(RTCIceCandidateInit.from_dict(candidate)))
            elif data.get('event_type')=='end':
                send_message(WebRTCError('session_ended','Matter camera ended the video session'))
                self.close_webrtc_session(session_id)
        wire=MatterWire(async_get_clientsession(self.hass),matter_entry.data[CONF_URL],event)
        record['wire']=wire;self.sessions[session_id]=record
        try:
            await wire.connect()
            response=await wire.request('device_command',node_id=self.entry.data['node_id'],endpoint_id=1,cluster_id=1361,command_name='VideoStreamAllocate',payload=VIDEO_ALLOCATION)
            stream=field(response,'videoStreamID')
            if not isinstance(stream,int) or isinstance(stream,bool) or not 0<=stream<=65535:raise MatterCameraError('Invalid video stream identifier')
            response=await wire.request('send_webrtc_provider_command',node_id=self.entry.data['node_id'],endpoint_id=1,command_name='ProvideOffer',payload={'webRtcSessionId':None,'sdp':offer_sdp,'streamUsage':3,'videoStreams':[stream]})
            record['id']=field(response,'webRTCSessionID')
            if record['closed']:
                await self._stop(record);return
            for data in record['events']:event(data)
            record['events'].clear()
            for candidate in record['candidates']:await self._candidate(record,candidate)
            record['candidates'].clear()
        except (MatterCameraError,OSError,TimeoutError,KeyError,ValueError,TypeError):
            send_message(WebRTCError('matter_video_failed','Matter video negotiation failed; check the server camera API and H.264 support'))
            self.sessions.pop(session_id,None);record['closed']=True;await self._stop(record)
    async def _candidate(self,record,candidate):
        # Older schema servers do not expose the provider convenience form for
        # candidates. The normal typed command works for an established session.
        payload=candidate.to_dict()
        if not payload.get('candidate'):return
        await record['wire'].request('device_command',node_id=self.entry.data['node_id'],endpoint_id=1,cluster_id=1363,command_name='ProvideICECandidates',payload={'webRTCSessionID':record['id'],'ICECandidates':[{'candidate':payload['candidate'],'SDPMid':payload.get('sdpMid'),'SDPMLineIndex':payload.get('sdpMLineIndex')}]})
    async def async_on_webrtc_candidate(self,session_id,candidate):
        record=self.sessions.get(session_id)
        if not record or record['closed']:return
        if record['id'] is None:
            if len(record['candidates'])>=16:raise MatterCameraError('Too many pending ICE candidates')
            record['candidates'].append(candidate);return
        await self._candidate(record,candidate)
    async def _stop(self,record):
        try:
            if record['id'] is not None:
                with suppress(MatterCameraError,OSError,TimeoutError):
                    await record['wire'].request('device_command',node_id=self.entry.data['node_id'],endpoint_id=1,cluster_id=1363,command_name='EndSession',payload={'webRTCSessionID':record['id'],'reason':2})
        finally:await record['wire'].close()
    @callback
    def close_webrtc_session(self,session_id):
        record=self.sessions.pop(session_id,None)
        if record is None:return
        record['closed']=True
        # While ProvideOffer is in flight its coroutine owns eventual teardown.
        if record['id'] is not None:self.hass.async_create_task(self._stop(record))
    async def async_will_remove_from_hass(self):
        for session_id in list(self.sessions):self.close_webrtc_session(session_id)
        await super().async_will_remove_from_hass()
