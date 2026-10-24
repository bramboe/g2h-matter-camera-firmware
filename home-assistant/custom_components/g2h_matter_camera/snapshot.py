"""Bounded still images through standard commands on the existing Matter fabric."""
import asyncio
from contextlib import suppress
import base64
import binascii
from .protocol import MatterCameraError,field

MAX_JPEG=64000
SNAPSHOT_ALLOCATION={'imageCodec':0,'maxFrameRate':1,'minResolution':{'width':320,'height':180},'maxResolution':{'width':320,'height':180},'quality':70}

def decode_snapshot(response):
    codec=field(response,'imageCodec');resolution=field(response,'resolution');encoded=field(response,'data')
    if not isinstance(codec,int) or isinstance(codec,bool) or codec!=0 or not isinstance(resolution,dict):raise MatterCameraError('Invalid snapshot format')
    if any(isinstance(resolution.get(k),bool) or not isinstance(resolution.get(k),int) for k in ['width','height']) or resolution.get('width')!=320 or resolution.get('height')!=180:raise MatterCameraError('Invalid snapshot dimensions')
    if not isinstance(encoded,str) or not encoded or len(encoded)>4*((MAX_JPEG+2)//3):raise MatterCameraError('Snapshot exceeds bounds')
    try:data=base64.b64decode(encoded,validate=True)
    except (binascii.Error,ValueError):raise MatterCameraError('Invalid snapshot encoding') from None
    if len(data)<16 or len(data)>MAX_JPEG or data[:2]!=b'\xff\xd8' or data[-2:]!=b'\xff\xd9':raise MatterCameraError('Invalid JPEG envelope')
    offset=2;found=False
    while offset+4<=len(data):
        if data[offset]!=255:raise MatterCameraError('Invalid JPEG marker')
        marker=data[offset+1];offset+=2
        if marker==0xda:
            if not found:raise MatterCameraError('Missing JPEG dimensions')
            if offset+14>=len(data)-2 or data[offset:offset+3]!=b'\x00\x0c\x03' or data[offset+9:offset+12]!=b'\x00\x3f\x00':raise MatterCameraError('Invalid JPEG scan')
            return data
        if marker in (0xd8,0xd9,0,255) or 0xd0<=marker<=0xd7:raise MatterCameraError('Unexpected JPEG marker')
        length=int.from_bytes(data[offset:offset+2],'big')
        if length<2 or offset+length>len(data)-2:raise MatterCameraError('Invalid JPEG segment')
        if marker in (0xc0,0xc1,0xc2,0xc3,0xc5,0xc6,0xc7,0xc9,0xca,0xcb,0xcd,0xce,0xcf):
            if found or marker!=0xc0 or length!=17 or data[offset+2]!=8 or data[offset+7]!=3 or int.from_bytes(data[offset+3:offset+5],'big')!=180 or int.from_bytes(data[offset+5:offset+7],'big')!=320:raise MatterCameraError('Unsupported JPEG frame')
            found=True
        offset+=length
    raise MatterCameraError('Incomplete JPEG image')

async def capture_snapshot(wire,node_id,diagnostics=None):
    attributes=await wire.request('read_attribute',node_id=node_id,attribute_path=['1/1361/65532'])
    feature=attributes.get('1/1361/65532') if isinstance(attributes,dict) else None
    if not isinstance(feature,int) or isinstance(feature,bool) or not feature&4:raise MatterCameraError('Snapshot capability unavailable')
    allocation=await wire.request('device_command',node_id=node_id,endpoint_id=1,cluster_id=1361,command_name='SnapshotStreamAllocate',payload=SNAPSHOT_ALLOCATION)
    stream=field(allocation,'snapshotStreamID')
    if not isinstance(stream,int) or isinstance(stream,bool) or not 0<=stream<=65535:raise MatterCameraError('Invalid snapshot stream')
    try:
        response=await wire.request('device_command',node_id=node_id,endpoint_id=1,cluster_id=1361,command_name='CaptureSnapshot',payload={'snapshotStreamID':stream,'requestedResolution':{'width':320,'height':180}},interaction_timeout_ms=5000)
        if diagnostics is not None:
            # Shape only. Never retain bytes, encoded media or arbitrary errors.
            diagnostics['stage']='decode'
            for name in ('data','resolution','imageCodec'):
                value=field(response,name)
                diagnostics[name+'_type']=type(value).__name__[:32]
                if name=='data' and isinstance(value,(str,list,dict)):
                    diagnostics['encoded_length']=len(value)
                elif name=='resolution' and isinstance(value,dict):
                    diagnostics['width']=value.get('width') if type(value.get('width')) is int else None
                    diagnostics['height']=value.get('height') if type(value.get('height')) is int else None
        result=decode_snapshot(response)
        if diagnostics is not None:diagnostics.update(stage='complete',jpeg_bytes=len(result))
        return result
    finally:
        with suppress(MatterCameraError,OSError,TimeoutError):
            await asyncio.wait_for(wire.request('device_command',node_id=node_id,endpoint_id=1,cluster_id=1361,command_name='SnapshotStreamDeallocate',payload={'snapshotStreamID':stream},interaction_timeout_ms=3000),3)
