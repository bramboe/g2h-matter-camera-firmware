"""Actual synthetic JPEG fixtures, standard Matter commands, no camera access."""
import base64,importlib.util,sys,types,unittest
from pathlib import Path
ROOT=Path(__file__).parents[1];SOURCE=ROOT/'home-assistant/custom_components/g2h_matter_camera'
pkg=types.ModuleType('g2h_snapshot_capability_test');pkg.__path__=[str(SOURCE)];sys.modules[pkg.__name__]=pkg
spec=importlib.util.spec_from_file_location(pkg.__name__+'.snapshot',SOURCE/'snapshot.py');target=importlib.util.module_from_spec(spec);spec.loader.exec_module(target)
def response(size):
 width,height=size;data=(ROOT/f'tests/fixtures/synthetic-snapshot-{width}.jpg').read_bytes()
 return {'imageCodec':0,'resolution':{'width':width,'height':height},'data':base64.b64encode(data).decode()}
class Wire:
 def __init__(self,capabilities=None,unsupported=False):self.capabilities=capabilities;self.unsupported=unsupported;self.calls=[];self.size=None
 async def request(self,command,**args):
  self.calls.append((command,args))
  if command=='read_attribute':
   if args['attribute_path']==['1/1361/65532']:return {'1/1361/65532':6}
   if self.unsupported:raise target.MatterCameraError('Unsupported snapshot capability')
   return {'1/1361/10':self.capabilities}
  if args['command_name']=='SnapshotStreamAllocate':
   r=args['payload']['maxResolution'];self.size=(r['width'],r['height']);return {'snapshotStreamID':2}
  if args['command_name']=='CaptureSnapshot':return response(self.size)
  return {}
class SnapshotCapabilities(unittest.IsolatedAsyncioTestCase):
 async def test_advertised_large_image_is_requested_and_validated(self):
  wire=Wire([{'Resolution':{'width':640,'height':360},'ImageCodec':0}]);detail={}
  jpeg=await target.capture_snapshot(wire,73,diagnostics=detail)
  self.assertEqual(jpeg,base64.b64decode(response((640,360))['data']));self.assertEqual(wire.size,(640,360))
  self.assertEqual(detail['stage'],'complete');self.assertEqual(detail['width'],640)
  self.assertNotIn('data',detail)
  self.assertEqual(wire.calls[-1][1]['command_name'],'SnapshotStreamDeallocate')
 async def test_old_device_and_explicit_thumbnail_keep_small_image(self):
  for wire,prefer in ((Wire(),True),(Wire(unsupported=True),True),(Wire([{'resolution':{'width':640,'height':360},'imageCodec':0}]),False)):
   await target.capture_snapshot(wire,73,prefer_large=prefer);self.assertEqual(wire.size,(320,180))
   self.assertTrue(all(call[1]['node_id']==73 for call in wire.calls))
 async def test_actual_matter_js_numeric_attribute_structure(self):
  wire=Wire([{'0':{'0':320,'1':180},'1':1,'2':0,'3':False},{'0':{'0':640,'1':360},'1':1,'2':0,'3':False}])
  await target.capture_snapshot(wire,73);self.assertEqual(wire.size,(640,360))
 def test_untrusted_or_unbounded_capability_list_does_not_expand_request(self):
  valid={'resolution':{'width':640,'height':360},'imageCodec':0}
  for caps in (None,{},[valid]*9,[dict(valid,imageCodec=False)],[{'resolution':{'width':640,'height':True},'imageCodec':0}],['640x360']):
   self.assertEqual(target.preferred_resolution(caps),(320,180))
 def test_real_jpeg_dimensions_match_the_allocated_size(self):
  for size in ((320,180),(640,360)):
   self.assertTrue(target.decode_snapshot(response(size),size).startswith(b'\xff\xd8'))
  with self.assertRaises(target.MatterCameraError):target.decode_snapshot(response((640,360)),(320,180))
  with self.assertRaises(target.MatterCameraError):target.decode_snapshot(response((320,180)),(640,360))
 async def test_cold_capture_retries_once_without_reallocating(self):
  class Cold(Wire):
   attempts=0
   async def request(self,command,**args):
    if args.get('command_name')=='CaptureSnapshot':
     self.attempts+=1
     if self.attempts==1:raise target.MatterCameraError('Worker not ready')
    return await super().request(command,**args)
  wire=Cold([{'0':{'0':640,'1':360},'2':0}])
  await target.capture_snapshot(wire,73);self.assertEqual(wire.attempts,2)
  self.assertEqual(sum(a.get('command_name')=='SnapshotStreamAllocate' for _,a in wire.calls),1)
  self.assertEqual(wire.calls[-1][1]['command_name'],'SnapshotStreamDeallocate')
 async def test_persistent_capture_failure_stops_and_releases(self):
  class Failed(Wire):
   attempts=0
   async def request(self,command,**args):
    if args.get('command_name')=='CaptureSnapshot':
     self.attempts+=1;raise target.MatterCameraError('Not ready')
    return await super().request(command,**args)
  wire=Failed()
  with self.assertRaises(target.MatterCameraError):await target.capture_snapshot(wire,73)
  self.assertEqual(wire.attempts,2)
  self.assertEqual(wire.calls[-1][1]['command_name'],'SnapshotStreamDeallocate')
 async def test_invalid_jpeg_still_releases_stream(self):
  class Broken(Wire):
   async def request(self,command,**args):
    result=await super().request(command,**args)
    if args.get('command_name')=='CaptureSnapshot':result['data']='not-base64!'
    return result
  wire=Broken()
  with self.assertRaises(target.MatterCameraError):await target.capture_snapshot(wire,73)
  self.assertEqual(wire.calls[-1][1]['command_name'],'SnapshotStreamDeallocate')
if __name__=='__main__':unittest.main()
