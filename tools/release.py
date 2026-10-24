#!/usr/bin/env python3
"""Create/validate an offline source-build manifest; never publishes or installs."""
import argparse,hashlib,importlib.util,json,subprocess,tarfile
from pathlib import Path,PurePosixPath
ROOT=Path(__file__).resolve().parents[1]
# Load the sibling recipe by exact file path (not a third-party module named
# "build"). Its CLI is protected by __main__, so importing has no build effects.
_BUILD_SPEC=importlib.util.spec_from_file_location('g2h_release_build_recipe',ROOT/'tools/build.py')
BUILD=importlib.util.module_from_spec(_BUILD_SPEC)
_BUILD_SPEC.loader.exec_module(BUILD)
def check_source_checkout(matter):
 rtc=BUILD.check_checkout(matter)
 patch=ROOT/BUILD.LOCK['matter']['patch']
 if digest(patch)!=BUILD.LOCK['matter']['patch_sha256']:raise ValueError('SDK patch differs from lock')
 name='src/app/clusters/camera-av-stream-management-server/CameraAVStreamManagementCluster.cpp'
 actual=subprocess.check_output(['git','-C',str(matter),'diff','HEAD','--',name])
 # Build preparation may accept an unpatched tree, but a release of the patched
 # snapshot implementation must include the exact applied modification.
 if actual!=patch.read_bytes():raise ValueError('Release SDK must have the exact applied snapshot patch')
 dirty=subprocess.check_output(['git','-C',str(rtc),'diff','--name-only','HEAD'],text=True)
 if dirty.strip():raise ValueError('libdatachannel has unrecorded tracked source changes')
 result=subprocess.run(['git','-C',str(matter),'submodule','foreach','--quiet','--recursive','git diff --quiet HEAD --'],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
 if result.returncode:raise ValueError('A pinned SDK submodule has tracked source changes')
 return rtc
def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def safe_relative(s):
 p=PurePosixPath(s)
 if p.is_absolute() or not p.parts or '..' in p.parts or '\\' in s:raise ValueError('Unsafe artifact path')
 return p
def validate_manifest(m,base):
 if m.get('format')!='g2h-source-build-v1':raise ValueError('Unsupported manifest')
 if m.get('vendor_firmware_included') is not False:raise ValueError('Vendor redistribution prohibited')
 if m.get('binary_matches_source') is not True:raise ValueError('Source/binary correspondence unverified')
 if not isinstance(m.get('inputs'),dict) or not isinstance(m.get('artifacts'),list) or not m['artifacts']:raise ValueError('Incomplete manifest')
 names=set()
 for a in m['artifacts']:
  name=a['path'];safe_relative(name)
  if name not in {'g2h-matter-camera','g2h-media-broker','relinkable.tar.gz','corresponding-sources.tar.gz'}:raise ValueError('Artifact is outside the public release allowlist')
  if name in names:raise ValueError('Duplicate artifact')
  names.add(name);p=base/name
  if p.is_symlink() or not p.is_file() or not p.resolve().is_relative_to(base.resolve()):raise ValueError('Missing/escaping artifact')
  if a['bytes']!=p.stat().st_size or a['sha256']!=digest(p):raise ValueError('Artifact digest mismatch: '+name)
 required={'g2h-matter-camera','relinkable.tar.gz','corresponding-sources.tar.gz'}
 if not required.issubset(names):raise ValueError('Missing executable, relinkable objects, or corresponding sources')
 if not isinstance(m.get('test_evidence'),dict):raise ValueError('Missing test evidence')
 # Offline validity is distinct from hardware qualification.
 if m.get('hardware_qualified') and not m['test_evidence'].get('device_report'):raise ValueError('No hardware report')
def archive(out,items):
 with tarfile.open(out,'w:gz') as t:
  for p,name in items:
   if p.is_symlink():raise ValueError('Archive symlink rejected')
   t.add(p,arcname=str(name),recursive=False)
def main():
 p=argparse.ArgumentParser(description=__doc__);sub=p.add_subparsers(dest='action',required=True)
 v=sub.add_parser('verify');v.add_argument('manifest',type=Path)
 s=sub.add_parser('stage');s.add_argument('--build-out',required=True,type=Path);s.add_argument('--matter-root',required=True,type=Path);s.add_argument('--dest',required=True,type=Path)
 a=p.parse_args()
 if a.action=='verify':
  validate_manifest(json.loads(a.manifest.read_text()),a.manifest.parent);print('Offline manifest valid; see hardware_qualified separately.');return
 matter=a.matter_root.resolve();check_source_checkout(matter)
 out=a.build_out.resolve();dest=a.dest.resolve()
 record=json.loads((out/'build-record.json').read_text())
 if record.get('format')!='g2h-build-record-v1' or record.get('inputs_lock_sha256')!=digest(ROOT/'build/inputs.lock.json'):raise ValueError('Missing or stale build record')
 for name,value in record['source_sha256'].items():
  safe_relative(name)
  if digest(ROOT/name)!=value:raise ValueError('Source changed after build: '+name)
 for name,value in record['link_input_sha256'].items():
  safe_relative(name)
  if digest(out/name)!=value:raise ValueError('Relink input changed after build: '+name)
 if record['binary_sha256']!=digest(out/'app/g2h-matter-camera'):raise ValueError('Binary changed after build')
 if dest.exists():raise ValueError('Destination must be new')
 objects=sorted((out/'app/CMakeFiles/g2h-app-objects.dir').rglob('*.o'))
 libs=[out/'sdk-out/lib/G2HCameraSDK.a',out/'sdk-out/lib/libCHIP.a',out/'rtc/libdatachannel.a',out/'rtc/deps/usrsctp/usrsctplib/libusrsctp.a',out/'rtc/deps/libsrtp/libsrtp2.a',out/'rtc/deps/libjuice/libjuice.a']
 libs+=sorted((out/'crypto-prefix/lib').glob('*.a'))+sorted((out/'media-prefix/lib').glob('*.a'))
 if len(objects)<2 or len(libs)<13 or any(not x.is_file() for x in libs):raise ValueError('Missing relinkable inputs')
 archives=[out/'downloads'/x['file'] for x in json.loads((ROOT/'build/inputs.lock.json').read_text())['archives']]
 if any(not x.is_file() for x in archives):raise ValueError('Missing corresponding third-party source archives')
 for entry,path in zip(json.loads((ROOT/'build/inputs.lock.json').read_text())['archives'],archives):
  if digest(path)!=entry['sha256']:raise ValueError('Corresponding source archive changed: '+path.name)
 # git archive pins all SDK files; submodule sources are each included separately.
 sources=[]
 for tree in [matter]+[x.parent for x in matter.rglob('.git') if x.is_file()]:
  # Only tracked files, never untracked credentials, caches, vendor binaries, or device state.
  paths=subprocess.check_output(['git','-C',str(tree),'ls-files','-z']).decode().split('\0')
  for name in paths:
   f=tree/name
   if f.is_file() and not f.is_symlink():sources.append((f,Path('matter')/tree.relative_to(matter)/name))
 dest.mkdir(parents=True)
 for name in ['g2h-matter-camera','g2h-media-broker']:
  f=out/'app'/name
  if f.is_file():(dest/name).write_bytes(f.read_bytes())
 relink=[(x,Path('objects')/x.relative_to(out/'app')) for x in objects]
 relink += [(x,Path('libraries')/x.relative_to(out)) for x in libs]
 relink.append((ROOT/'tools/relink.py',Path('recipe/relink.py')))
 for x in [out/'build-record.json']:
  if x.is_file():relink.append((x,Path('recipe')/x.name))
 archive(dest/'relinkable.tar.gz',relink)
 sources += [(x,Path('archives')/x.name) for x in archives]
 for folder in ['app/src','build','tools','licenses']:
  sources += [(x,Path('project')/x.relative_to(ROOT)) for x in (ROOT/folder).rglob('*') if x.is_file() and not x.is_symlink() and '__pycache__' not in x.parts]
 archive(dest/'corresponding-sources.tar.gz',sources)
 check_source_checkout(matter)
 manifest={'format':'g2h-source-build-v1','vendor_firmware_included':False,'binary_matches_source':True,'hardware_qualified':False,'inputs':json.loads((ROOT/'build/inputs.lock.json').read_text()),'test_evidence':{'device_report':None,'note':'Locally staged build; hardware tests and independent rebuild pending.'},'artifacts':[{'path':x.name,'bytes':x.stat().st_size,'sha256':digest(x)} for x in sorted(dest.iterdir())]}
 (dest/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n');validate_manifest(manifest,dest)
 print('Staged local source candidate. Not hardware-qualified or independently reproduced.')
if __name__=='__main__':main()
