"""Bounded still images through standard commands on the existing Matter fabric."""
import asyncio
from contextlib import suppress
import base64
import binascii
from .protocol import MatterCameraError,field

MAX_JPEG=64000
SNAPSHOT_ALLOCATION={'imageCodec':0,'maxFrameRate':1,'minResolution':{'width':320,'height':180},'maxResolution':{'width':320,'height':180},'quality':70}

def decode_snapshot(response,expected=(320,180)):
    if expected not in ((320,180),(640,360)):raise MatterCameraError('Unsupported snapshot dimensions')
    width,height=expected
    codec=field(response,'imageCodec');resolution=field(response,'resolution');encoded=field(response,'data')
    if not isinstance(codec,int) or isinstance(codec,bool) or codec!=0 or not isinstance(resolution,dict):raise MatterCameraError('Invalid snapshot format')
    if any(isinstance(resolution.get(k),bool) or not isinstance(resolution.get(k),int) for k in ['width','height']) or resolution.get('width')!=width or resolution.get('height')!=height:raise MatterCameraError('Invalid snapshot dimensions')
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
            if found or marker!=0xc0 or length!=17 or data[offset+2]!=8 or data[offset+7]!=3 or int.from_bytes(data[offset+3:offset+5],'big')!=height or int.from_bytes(data[offset+5:offset+7],'big')!=width:raise MatterCameraError('Unsupported JPEG frame')
            found=True
        offset+=length
    raise MatterCameraError('Incomplete JPEG image')

def preferred_resolution(capabilities,prefer_large=True):
    if not prefer_large or not isinstance(capabilities,list) or len(capabilities)>8:return (320,180)
    for capability in capabilities:
        if not isinstance(capability,dict):continue
        try:
            codec=capability['2'] if '2' in capability else field(capability,'imageCodec');resolution=capability['0'] if '0' in capability else field(capability,'resolution')
        except MatterCameraError:continue
        if not isinstance(resolution,dict):continue
        # Attribute reads from matter.js use numeric TLV structure tags; typed
        # command responses and some servers use the schema field names.
        width=resolution.get('0',resolution.get('width'));height=resolution.get('1',resolution.get('height'))
        if type(codec) is int and codec==0 and type(width) is int and type(height) is int and (width,height)==(640,360):return (640,360)
    return (320,180)

async def capture_snapshot(wire,node_id,diagnostics=None,prefer_large=True):
    attributes=await wire.request('read_attribute',node_id=node_id,attribute_path=['1/1361/65532'])
    feature=attributes.get('1/1361/65532') if isinstance(attributes,dict) else None
    if not isinstance(feature,int) or isinstance(feature,bool) or not feature&4:raise MatterCameraError('Snapshot capability unavailable')
    # Only request the larger image when the actual device advertises it.
    # Existing firmware with no such capability retains its 320x180 behavior.
    size=(320,180)
    if prefer_large:
        try:
            capabilities=await wire.request('read_attribute',node_id=node_id,attribute_path=['1/1361/10'])
            size=preferred_resolution(capabilities.get('1/1361/10') if isinstance(capabilities,dict) else None)
        except MatterCameraError:
            pass
    resolution={'width':size[0],'height':size[1]}
    request=dict(SNAPSHOT_ALLOCATION,minResolution=resolution,maxResolution=resolution)
    allocation=await wire.request('device_command',node_id=node_id,endpoint_id=1,cluster_id=1361,command_name='SnapshotStreamAllocate',payload=request)
    stream=field(allocation,'snapshotStreamID')
    if not isinstance(stream,int) or isinstance(stream,bool) or not 0<=stream<=65535:raise MatterCameraError('Invalid snapshot stream')
    try:
        for attempt in range(2):
            try:
                response=await wire.request('device_command',node_id=node_id,endpoint_id=1,cluster_id=1361,command_name='CaptureSnapshot',payload={'snapshotStreamID':stream,'requestedResolution':resolution},interaction_timeout_ms=5000)
                break
            except MatterCameraError:
                if attempt:raise
                # The device keeps JPEG encoding off the live media event loop.
                # A cold capture may return before its worker has finished.
                # Reuse this allocation once, then fail closed and deallocate.
                await asyncio.sleep(0.5)
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
        result=decode_snapshot(response,size)
        if diagnostics is not None:diagnostics.update(stage='complete',jpeg_bytes=len(result))
        return result
    finally:
        with suppress(MatterCameraError,OSError,TimeoutError):
            await asyncio.wait_for(wire.request('device_command',node_id=node_id,endpoint_id=1,cluster_id=1361,command_name='SnapshotStreamDeallocate',payload={'snapshotStreamID':stream},interaction_timeout_ms=3000),3)
