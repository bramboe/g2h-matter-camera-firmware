#!/usr/bin/env python3
"""Prepare private EN/NL voice banks from owned vendor assets and local macOS TTS.
No camera access, downloads or playback. Generated audio is not a public release.
"""
import argparse,hashlib,json,shutil,subprocess,tempfile
from pathlib import Path
NAMES=('add_ok','add_sensor','bind_ok','bind_timeout','con_ok','connecting','deleted','deleted_tip','fail_connect','fail_join','leave_msg','leave_msg_fail','reset_ok','scan_success','waiting','wifi_connected')
DUTCH={
 'add_ok':'Het accessoire is toegevoegd.',
 'add_sensor':'Klaar om een accessoire toe te voegen.',
 'bind_ok':'Het koppelen is gelukt.',
 'bind_timeout':'De tijd voor het koppelen is verstreken.',
 'con_ok':'De verbinding is gemaakt.',
 'connecting':'De camera maakt verbinding.',
 'deleted':'Het apparaat is verwijderd.',
 'deleted_tip':'Het apparaat wordt verwijderd.',
 'fail_connect':'Verbinding maken is niet gelukt.',
 'fail_join':'Verbinding met het netwerk is niet gelukt.',
 'leave_msg':'Spreek uw bericht in na de toon.',
 'leave_msg_fail':'Het bericht kon niet worden opgeslagen.',
 'reset_ok':'Het herstellen van de instellingen is voltooid.',
 'scan_success':'De QR-code is gescand.',
 'waiting':'De camera wacht op een verbinding.',
 'wifi_connected':'De wifi-verbinding is gemaakt.',
 'language_test':'De camera spreekt Nederlands.',
}
def valid_adts(data):
 at=0
 while at<len(data):
  h=data[at:at+7]
  if len(h)<7 or h[0]!=255 or h[1]&0xf6!=0xf0 or (h[2]>>6)&3!=1 or (h[2]>>2)&15!=8 or ((h[2]&1)<<2)|(h[3]>>6)!=1:return False
  n=((h[3]&3)<<11)|(h[4]<<3)|(h[5]>>5)
  if not 8<=n<=1000 or at+n>len(data):return False
  at+=n
 return at>0

def main():
 p=argparse.ArgumentParser(description=__doc__)
 p.add_argument('--english-original',required=True,type=Path)
 p.add_argument('--out',required=True,type=Path)
 p.add_argument('--dutch-voice',default='Xander (Dutch (Netherlands))')
 p.add_argument('--english-test-voice',default='Daniel')
 p.add_argument('--ffmpeg',default=shutil.which('ffmpeg'))
 a=p.parse_args()
 if not a.ffmpeg or not shutil.which('say'):p.error('Existing ffmpeg and local macOS say are required; no software is installed automatically')
 if a.out.exists():p.error('Choose a new output directory')
 a.out.mkdir(mode=0o700,parents=True)
 record={'format':'g2h-voice-bank-v1','source':'owned factory English + local synthetic Dutch','files':{}}
 def encode(source,dest):
  subprocess.run([a.ffmpeg,'-hide_banner','-loglevel','error','-y','-i',str(source),'-ac','1','-ar','16000','-af','loudnorm=I=-20:TP=-3:LRA=8','-c:a','aac','-profile:a','aac_low','-b:a','32000','-f','adts',str(dest)],check=True,timeout=30)
 def speak(text,voice,dest):
  with tempfile.TemporaryDirectory(prefix='g2h-local-voice-') as tmp:
   pcm=Path(tmp)/'voice.aiff'
   subprocess.run(['say','-v',voice,'-r','165','-o',str(pcm),text],check=True,timeout=40)
   encode(pcm,dest)
 for locale in ('en','nl'):
  directory=a.out/locale;directory.mkdir(mode=0o700)
  for name in (*NAMES,'language_test'):
   dest=directory/(name+'.aac')
   if locale=='en' and name!='language_test':
    source=a.english_original/dest.name
    if source.is_symlink() or not source.is_file():raise ValueError('Regular owned English asset required: '+dest.name)
    data=source.read_bytes()
    if valid_adts(data):dest.write_bytes(data)
    else:encode(source,dest)
   else:speak(DUTCH[name] if locale=='nl' else 'The camera is speaking English.',a.dutch_voice if locale=='nl' else a.english_test_voice,dest)
   data=dest.read_bytes()
   if not 8<=len(data)<=192000 or not valid_adts(data):raise ValueError('Unsafe or unsupported ADTS output: '+dest.name)
   dest.chmod(0o600);record['files'][locale+'/'+dest.name]={'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest()}
  lines=[record['files'][locale+'/'+name+'.aac']['sha256']+'  '+name+'.aac\n' for name in sorted((*NAMES,'language_test'))]
  (directory/'SHA256SUMS').write_text(''.join(lines));(directory/'SHA256SUMS').chmod(0o600)
 (a.out/'manifest.private.json').write_text(json.dumps(record,indent=2)+'\n');(a.out/'manifest.private.json').chmod(0o600)
 print('VOICE_BANKS_READY',json.dumps({'files':len(record['files']),'bytes':sum(x['bytes'] for x in record['files'].values()),'out':str(a.out)}))
if __name__=='__main__':main()
