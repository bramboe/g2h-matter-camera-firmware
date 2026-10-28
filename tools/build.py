#!/usr/bin/env python3
"""Pinned source recipe. No camera access, publishing, or personal configuration."""
import argparse, hashlib, json, os, shutil, subprocess, tarfile, urllib.request
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
LOCK=json.loads((ROOT/'build/inputs.lock.json').read_text())
def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()
def verify_sdk_project_inputs(source,staged):
 for f in source.rglob('*'):
  if f.is_file() and '__pycache__' not in f.parts:
   copied=staged/f.relative_to(source)
   if not copied.is_file() or sha(copied)!=sha(f):raise ValueError('SDK project inputs are stale; rerun sdk stage')
def run(args,cwd=None,env=None): subprocess.run([str(x) for x in args],cwd=cwd,env=env,check=True,stdin=subprocess.DEVNULL)
def verified_archive(path,item):
 if not path.is_file() or sha(path)!=item['sha256']: raise ValueError('Missing or incorrect archive: '+path.name)
def extract(archive,dest):
 dest.mkdir(parents=True,exist_ok=True)
 with tarfile.open(archive) as t:
  # data filter rejects traversal and escaping links on Python 3.12+.
  for m in t.getmembers():
   if m.name.startswith('/') or '..' in Path(m.name).parts or m.issym() or m.islnk():
    raise ValueError('Unsafe archive member: '+m.name)
  t.extractall(dest,filter='data')
def check_checkout(matter):
 actual=subprocess.check_output(['git','-C',str(matter),'rev-parse','HEAD'],text=True).strip()
 if actual!=LOCK['matter']['commit']: raise ValueError('Matter revision differs from lock')
 changed=subprocess.check_output(['git','-C',str(matter),'diff','--name-only','HEAD'],text=True).splitlines()
 allowed='src/app/clusters/camera-av-stream-management-server/CameraAVStreamManagementCluster.cpp'
 if any(name!=allowed for name in changed):raise ValueError('Matter tree has unrecorded tracked changes')
 submodules=subprocess.check_output(['git','-C',str(matter),'submodule','status','--recursive'],text=True).splitlines()
 if any(line.startswith(('+','U')) for line in submodules):raise ValueError('A Matter submodule is not at its pinned revision')
 patch=ROOT/LOCK['matter']['patch']
 actual_diff=subprocess.check_output(['git','-C',str(matter),'diff','HEAD','--','src/app/clusters/camera-av-stream-management-server/CameraAVStreamManagementCluster.cpp'])
 if actual_diff and actual_diff!=patch.read_bytes():raise ValueError('Matter snapshot source has an unrecorded patch')
 rtc=matter/LOCK['libdatachannel']['path']
 actual=subprocess.check_output(['git','-C',str(rtc),'rev-parse','HEAD'],text=True).strip()
 if actual!=LOCK['libdatachannel']['commit']: raise ValueError('libdatachannel revision differs from lock')
 return rtc
def main():
 p=argparse.ArgumentParser(description=__doc__)
 p.add_argument('--matter-root',type=Path,required=True,help='Initialized pinned connectedhomeip checkout')
 p.add_argument('--out',type=Path,default=ROOT/'builds/source')
 p.add_argument('--stage',choices=['check','prepare','media','crypto','rtc','sdk','app','all'],default='check')
 p.add_argument('--fetch',action='store_true',help='Explicitly download missing hash-pinned archives')
 p.add_argument('--jobs',type=int,default=4);a=p.parse_args()
 if a.jobs<1 or a.jobs>64:p.error('--jobs must be 1..64')
 matter=a.matter_root.resolve();out=a.out.resolve();out.mkdir(parents=True,exist_ok=True)
 rtc=check_checkout(matter)
 zig=os.environ.get('ZIG','zig')
 if subprocess.check_output([zig,'version'],text=True).strip()!=LOCK['tools']['zig']:raise ValueError('Zig version differs from lock')
 env=os.environ.copy();env['SOURCE_DATE_EPOCH']=str(subprocess.check_output(['git','-C',str(matter),'show','-s','--format=%ct','HEAD'],text=True).strip())
 env['ZIG_GLOBAL_CACHE_DIR']=str(out/'zig-cache');env['ZIG_LOCAL_CACHE_DIR']=str(out/'zig-local-cache')
 tool=ROOT/'build/toolchain';tc=tool/'arm-linux.cmake';crypto=out/'crypto-prefix';media=out/'media-prefix'
 def cmake(source,dest,extra):
  run(['cmake','-S',source,'-B',dest,'-G','Ninja','-DCMAKE_BUILD_TYPE=Release','-DCMAKE_TOOLCHAIN_FILE='+str(tc)]+extra,env=env)
  run(['cmake','--build',dest,'-j',a.jobs],env=env)
 def selected(stage): return a.stage in (stage,'all')
 if a.stage=='check':
  print('Pinned Matter, libdatachannel and Zig versions verified. No compilation performed.');return
 if selected('prepare') or a.stage=='all':
  dl=out/'downloads';dl.mkdir(exist_ok=True)
  for item in LOCK['archives']:
   archive=dl/item['file']
   if not archive.exists() and a.fetch:
    temp=archive.with_suffix(archive.suffix+'.part')
    with urllib.request.urlopen(item['url'],timeout=120) as src,temp.open('wb') as dst:shutil.copyfileobj(src,dst)
    verified_archive(temp,item);temp.replace(archive)
   verified_archive(archive,item)
   extract(archive,out/'src')
  patch=ROOT/LOCK['matter']['patch']
  if sha(patch)!=LOCK['matter']['patch_sha256']:raise ValueError('SDK patch differs from lock')
  forward=subprocess.run(['git','-C',str(matter),'apply','--check',str(patch)],capture_output=True)
  if forward.returncode==0:run(['git','-C',matter,'apply',patch])
  else:run(['git','-C',matter,'apply','--reverse','--check',patch])
  sdk=out/'sdk';shutil.copytree(ROOT/'build/sdk',sdk,dirs_exist_ok=True)
  (sdk/'third_party').mkdir(exist_ok=True)
  link=sdk/'third_party/connectedhomeip'
  if link.is_symlink() and link.resolve()!=matter:raise ValueError('SDK link points at another checkout')
  if not link.exists():link.symlink_to(matter,target_is_directory=True)
 if selected('media'):
  ff=out/'media-ffmpeg';ff.mkdir(exist_ok=True)
  args=[out/'src/ffmpeg-8.1.3/configure','--prefix='+str(media),'--enable-cross-compile','--target-os=linux','--arch=arm','--cpu=cortex-a7']
  args += ['--'+key+'='+str(tool/exe) for key,exe in [('cc','g2h-cc'),('cxx','g2h-cxx'),('ar','g2h-ar'),('ranlib','g2h-ranlib')]]
  args += ['--disable-shared','--enable-static','--disable-everything','--disable-autodetect','--disable-programs','--disable-doc','--disable-avformat','--disable-avfilter','--disable-avdevice','--disable-network','--enable-avcodec','--enable-avutil','--enable-swscale','--enable-swresample','--enable-decoder=h264,aac','--enable-encoder=mjpeg,aac','--enable-parser=h264,aac','--disable-debug','--disable-runtime-cpudetect','--enable-small','--disable-swscale-alpha','--extra-cflags=-Os -ffunction-sections -fdata-sections']
  run(args,ff,env);run(['make','-j'+str(a.jobs)],ff,env);run(['make','install'],ff,env)
  cmake(out/'src/opus-1.6.1',out/'media-opus',['-DCMAKE_INSTALL_PREFIX='+str(media),'-DOPUS_BUILD_TESTING=OFF','-DOPUS_BUILD_PROGRAMS=OFF','-DOPUS_BUILD_SHARED_LIBRARY=OFF','-DOPUS_DRED=OFF','-DOPUS_OSCE=OFF','-DOPUS_DEEP_PLC=OFF','-DOPUS_DISABLE_INTRINSICS=ON','-DOPUS_CUSTOM_MODES=OFF'])
  run(['cmake','--install',out/'media-opus'],env=env)
 if selected('crypto'):
  d=out/'crypto';d.mkdir(exist_ok=True);ce=env.copy()
  for k,v in [('CC','g2h-cc'),('AR','g2h-ar'),('RANLIB','g2h-ranlib')]:ce[k]=str(tool/v)
  run(['perl',out/'src/openssl-3.5.9/Configure','linux-armv4','no-shared','no-tests','no-apps','no-docs','no-module','no-asm','no-quic','no-ml-kem','no-ml-dsa','no-slh-dsa','--prefix='+str(crypto)],d,ce)
  run(['make','-j'+str(a.jobs),'build_libs','install_dev'],d,ce)
 if selected('rtc'):
  cmake(rtc,out/'rtc',['-DCMAKE_INSTALL_PREFIX='+str(crypto),'-DOPENSSL_ROOT_DIR='+str(crypto),'-DOPENSSL_USE_STATIC_LIBS=TRUE','-DBUILD_SHARED_LIBS=OFF','-DBUILD_SHARED_DEPS_LIBS=OFF','-DNO_EXAMPLES=ON','-DNO_TESTS=ON','-DNO_MEDIA=OFF','-DNO_WEBSOCKET=ON','-DPREFER_SYSTEM_LIB=OFF','-DUSE_GNUTLS=OFF','-DUSE_NICE=OFF'])
 if selected('sdk'):
  # Refresh project headers when rebuilding an existing output directory too.
  # Otherwise a new app can silently link an SDK with old version/endpoint limits.
  shutil.copytree(ROOT/'build/sdk',out/'sdk',dirs_exist_ok=True)
  gn=['target_os="linux"','target_cpu="arm"','is_clang=true','is_debug=false','custom_toolchain="custom"','chip_device_platform="linux"','chip_enable_wifi=false','chip_enable_openthread=false','chip_enable_ble=false','chip_config_network_layer_ble=false','chip_mdns="minimal"','chip_crypto="mbedtls"','chip_with_nlfaultinjection=false','chip_build_controller=false','chip_build_tests=false','chip_build_tools=false','chip_build_libshell=false','enable_pie=false','enable_pic=false','symbol_level=0','treat_warnings_as_errors=false','runtime_library_ldflags=["-static","-pthread"]','chip_device_project_config_include="<G2HProjectConfig.h>"','chip_project_config_include_dirs=["//include","//third_party/connectedhomeip/config/standalone"]','chip_app_data_model_target="//model:model"']
  for key,exe in [('target_cc','g2h-cc'),('target_cxx','g2h-cxx'),('target_ar','g2h-ar')]:gn += [key+'='+json.dumps(str(tool/exe))]
  gn += ['target_defines=["FATCONFDIR=\\\"/tmp/g2h-matter-camera-state\\\"","SYSCONFDIR=\\\"/tmp/g2h-matter-camera-state\\\"","LOCALSTATEDIR=\\\"/tmp/g2h-matter-camera-state\\\"","CHIP_CONFIG_KVS_PATH=\\\"/tmp/g2h-matter-camera-state/kvs\\\""]']
  run(['gn','gen',out/'sdk-out','--args='+' '.join(gn)],out/'sdk',env)
  run(['ninja','-C',out/'sdk-out','-j',a.jobs],env=env)
 def project_sources():return {str(f.relative_to(ROOT)):sha(f) for folder in ['app/src','build','tools'] for f in (ROOT/folder).rglob('*') if f.is_file() and '__pycache__' not in f.parts}
 if selected('app'):
  verify_sdk_project_inputs(ROOT/'build/sdk',out/'sdk')
  source_before=project_sources()
  cmake(ROOT/'build',out/'app',['-DMATTER_ROOT='+str(matter),'-DSDK_SOURCE='+str(out/'sdk'),'-DSDK_OUTPUT='+str(out/'sdk-out'),'-DRTC_OUTPUT='+str(out/'rtc'),'-DCRYPTO_PREFIX='+str(crypto),'-DMEDIA_PREFIX='+str(media)])
 if selected('app'):
  if project_sources()!=source_before:raise ValueError('Project source changed during application build; rerun app stage')
  link_inputs=sorted((out/'app/CMakeFiles/g2h-app-objects.dir').rglob('*.o'))
  for folder in ['sdk-out/lib','rtc','crypto-prefix/lib','media-prefix/lib']:link_inputs+=sorted((out/folder).rglob('*.a'))
  record={'format':'g2h-build-record-v1','inputs_lock_sha256':sha(ROOT/'build/inputs.lock.json'),
          'source_sha256':source_before,'link_input_sha256':{str(f.relative_to(out)):sha(f) for f in link_inputs},
          'binary_sha256':sha(out/'app/g2h-matter-camera')}
  (out/'build-record.json').write_text(json.dumps(record,indent=2)+'\n')
 print('Completed '+a.stage+'. Outputs are candidates, not verified device releases.')
if __name__=='__main__':main()
