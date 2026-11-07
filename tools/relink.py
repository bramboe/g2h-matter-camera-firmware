#!/usr/bin/env python3
"""Relink the application with replacement LGPL libraries from an extracted bundle."""
import argparse,os,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('bundle',type=Path);p.add_argument('--output',required=True,type=Path);a=p.parse_args()
b=a.bundle.resolve();objects=sorted((b/'objects').rglob('*.o'));libs=sorted((b/'libraries').rglob('*.a'))
if len(objects)<2 or len(libs)<13:p.error('Incomplete relinkable bundle')
# Match the application's recorded link order, including group rescans.
order=['G2HCameraSDK.a','libCHIP.a','libdatachannel.a','libusrsctp.a','libsrtp2.a','libjuice.a','libssl.a','libcrypto.a','libavcodec.a','libswscale.a','libswresample.a','libopus.a','libavutil.a']
rank={name:i for i,name in enumerate(order)}
libs.sort(key=lambda path:(rank.get(path.name,len(order)),str(path)))
zig=os.environ.get('ZIG','zig')
if subprocess.check_output([zig,'version'],text=True).strip()!='0.14.1':p.error('Use Zig 0.14.1')
subprocess.run([zig,'c++','-target','arm-linux-musleabihf','-mcpu=cortex_a7','-Os','-DNDEBUG','-ffunction-sections','-fdata-sections','-static','-Wl,--gc-sections','-Wl,-z,stack-size=262144']+[str(x) for x in objects]+['-Wl,--start-group']+[str(x) for x in libs]+['-Wl,--end-group','-lpthread','-lm','-o',str(a.output)],check=True)
