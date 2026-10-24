"""Use the existing Matter server/fabric. No pairing, credentials or media URLs."""
import asyncio
from contextlib import suppress
import uuid
from aiohttp import WSMsgType

class MatterCameraError(Exception):
    """A bounded protocol or device error; excludes raw payloads/secrets."""

class MatterCameraCompatibilityError(MatterCameraError):
    """An explicit server capability mismatch, not a connection/read error."""
    def __init__(self,schema):
        self.schema=schema
        super().__init__('Matter server camera signalling unavailable (schema '+str(schema)+')')

class MatterWire:
    def __init__(self, session, url, event=None):
        self.session=session; self.url=url; self.event=event
        self.ws=None; self.reader=None; self.pending={}; self.info={}
    async def connect(self):
        self.ws=await self.session.ws_connect(self.url,heartbeat=30,max_msg_size=8*1024*1024)
        try:
            self.info=await asyncio.wait_for(self.ws.receive_json(),10)
            schema=self.info.get('schema_version')
            if not isinstance(schema,int) or isinstance(schema,bool) or schema<1:
                raise MatterCameraError('Matter server returned no valid schema version')
            if self.event is not None:self.require_camera_api()
            self.reader=asyncio.create_task(self._read())
            if self.event is not None:
                await self.request('start_listening')
        except BaseException:
            await self.close(); raise
    def require_camera_api(self):
        schema=self.info.get('schema_version',0)
        if schema<12:raise MatterCameraCompatibilityError(schema)
    async def _read(self):
        try:
            async for msg in self.ws:
                if msg.type!=WSMsgType.TEXT: continue
                data=msg.json()
                if 'message_id' in data:
                    future=self.pending.get(str(data['message_id']))
                    if future is None or future.done(): continue
                    if 'error_code' in data:
                        future.set_exception(MatterCameraError('Matter command refused (code '+str(data['error_code'])+')'))
                    else: future.set_result(data.get('result'))
                elif self.event and data.get('event')=='webrtc_callback':
                    self.event(data.get('data',{}))
        finally:
            for future in list(self.pending.values()):
                if not future.done():future.set_exception(MatterCameraError('Matter connection closed'))
    async def request(self, command, **args):
        if self.ws is None or self.ws.closed:raise MatterCameraError('Matter connection unavailable')
        key=uuid.uuid4().hex;future=asyncio.get_running_loop().create_future();self.pending[key]=future
        try:
            await self.ws.send_json({'message_id':key,'command':command,'args':args})
            return await asyncio.wait_for(future,40)
        finally:self.pending.pop(key,None)
    async def close(self):
        if self.ws:await self.ws.close()
        if self.reader:
            self.reader.cancel()
            with suppress(asyncio.CancelledError):await self.reader

# Fixed source constraints are factual, not generic camera capabilities.
VIDEO_ALLOCATION={'streamUsage':3,'videoCodec':0,'minFrameRate':1,'maxFrameRate':20,
 'minResolution':{'width':1280,'height':720},'maxResolution':{'width':1280,'height':720},
 'minBitRate':1,'maxBitRate':4294967295,'keyFrameInterval':0}

def field(value, name):
    if not isinstance(value,dict):raise MatterCameraError('Invalid Matter response')
    wanted=name.replace('_','').lower()
    for key,item in value.items():
        if str(key).replace('_','').lower()==wanted:return item
    raise MatterCameraError('Missing Matter response field '+name)
