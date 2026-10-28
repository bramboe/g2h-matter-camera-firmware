"""Install owned EN/NL voice banks, retaining the exact factory directory.

No native process restart or executable patch. A detached timer restores the
factory directory unless integrity and the same host lifetime are confirmed.
The supplied validator is built from this repository's VoiceBanks implementation.
"""
import argparse,base64,getpass,hashlib,io,json,os,re,shlex,sys,tarfile,time,uuid
from pathlib import Path
from g2h_identification_probe import Telnet
from g2h_upload_telnet_bulk import upload
from g2h_backup_live import identity,safe_status,private_json
from g2h_camera_upgrade_live import image_bytes,free_kb

HERE=Path(__file__).resolve().parent
sys.path.insert(0,str(HERE.parent/'tools'))
from prepare_voice_bank import NAMES,valid_adts
Q=shlex.quote
BANK='/mnt/sdcard/g2h-matter-camera/voice'
CURRENT='/mnt/config/g2h-matter-camera/voice-current'
ALIAS='/etc/ch/en'
FILES=tuple(sorted((*NAMES,'language_test')))

def payload(directory):
    """Pack only the exact validated files; never accept archive input."""
    packed=io.BytesIO();digests={}
    with tarfile.open(fileobj=packed,mode='w:gz') as archive:
        for language in ('en','nl'):
            bank=directory/language
            if bank.is_symlink() or not bank.is_dir():raise ValueError('regular bank directory required')
            manifest='';files={}
            for name in FILES:
                path=bank/(name+'.aac')
                if path.is_symlink() or not path.is_file() or not 8<=path.stat().st_size<=192000:
                    raise ValueError('bounded regular voice asset required')
                data=path.read_bytes()
                if not valid_adts(data):raise ValueError('invalid guarded ADTS voice asset')
                files[path.name]=data;manifest+=hashlib.sha256(data).hexdigest()+'  '+path.name+'\n'
                digests[language+'/'+path.name]=hashlib.md5(data).hexdigest()
            path=bank/'SHA256SUMS'
            if path.is_symlink() or not path.is_file() or path.read_bytes()!=manifest.encode():
                raise ValueError('exact sorted voice manifest required')
            files['SHA256SUMS']=manifest.encode()
            digests[language+'/SHA256SUMS']=hashlib.md5(manifest.encode()).hexdigest()
            for name,data in files.items():
                info=tarfile.TarInfo(language+'/'+name);info.size=len(data);info.mode=0o600;info.mtime=0
                archive.addfile(info,io.BytesIO(data))
    return packed.getvalue(),digests

def rollback_script(transaction,original):
    # All substituted paths are installer-owned literals, never user shell text.
    return f'''#!/bin/sh
set -eu
transaction={Q(transaction)}
original={Q(original)}
alias={Q(ALIAS)}
current={Q(CURRENT)}
bank={Q(BANK)}
[ "$1" = force ] || [ ! -f "$transaction/committed" ] || exit 0
mkdir "$transaction/action.lock"
trap 'rmdir "$transaction/action.lock" 2>/dev/null || :' EXIT
if [ -L "$alias" ]; then
    [ "$(readlink "$alias")" = "$current" ]
    [ -d "$original" ] && [ ! -L "$original" ]
    rm "$alias"
    mv "$original" "$alias"
elif [ -d "$alias" ] && [ ! -L "$alias" ]; then
    [ ! -e "$original" ] && [ ! -L "$original" ]
elif [ ! -e "$alias" ] && [ ! -L "$alias" ]; then
    [ -d "$original" ] && [ ! -L "$original" ]
    mv "$original" "$alias"
else
    exit 1
fi
if [ -L "$current" ]; then
    target=$(readlink "$current")
    [ "$target" = "$bank/en" ] || [ "$target" = "$bank/nl" ]
    rm "$current"
elif [ -e "$current" ]; then
    exit 1
fi
sync
cd "$alias"
while read expected name; do
    actual=$(md5sum "$name") || exit 1
    [ "${{actual%% *}}" = "$expected" ] || exit 1
done < "$transaction/original-md5"
printf '%s\\n' restored > "$transaction/restored"
'''

def install(session,directory,validator,backup_root):
    archive,checks=payload(directory);validator=image_bytes(validator)
    before=identity(session);status=safe_status(session)
    if status['fabrics']!=3:raise RuntimeError('expected existing three fabrics')
    session.run("mount | grep -q ' on /mnt/sdcard '")
    session.run('set -e\n[ -d '+ALIAS+' ] && [ ! -L '+ALIAS+' ]\n[ ! -e '+CURRENT+' ] && [ ! -L '+CURRENT+' ]')
    present=session.run('if [ -e '+BANK+' ] || [ -L '+BANK+' ]; then echo yes; fi').strip()
    if present:
        session.run('[ -d '+BANK+' ] && [ ! -L '+BANK+' ]')
        for name,expected in checks.items():
            path=BANK+'/'+name
            session.run('[ -f '+Q(path)+' ] && [ ! -L '+Q(path)+' ]')
            if session.run('md5sum '+Q(path)).split()[0]!=expected:raise RuntimeError('existing bank is not this exact validated pack')
    if free_kb(session,'/mnt/sdcard')<2048+(len(archive)+1023)//1024:raise RuntimeError('voice bank space unavailable')
    if free_kb(session,'/tmp')<4096+(len(archive)+len(validator)+1023)//1024:raise RuntimeError('staging space unavailable')
    nonce=uuid.uuid4().hex
    stage='/tmp/g2h_voice_'+nonce;transaction='/mnt/config/g2h-matter-camera/voice-install-'+nonce
    original='/etc/ch/.en-g2h-original-'+nonce;staged=BANK+'-stage-'+nonce
    report_dir=backup_root/('voice-install-'+time.strftime('%Y%m%dT%H%M%S')+'-'+nonce[:8]);report_dir.mkdir(parents=True,mode=0o700)
    report={'complete':False,'before':before,'transaction':transaction,'factory_directory':original,'bank':BANK,'mutated':False}
    armed=False
    try:
        # A fresh verified private copy of all files that the alias will replace.
        original_md5=''
        for name in NAMES:
            path=ALIAS+'/'+name+'.aac';expected=session.run('md5sum '+Q(path)).split()[0]
            if expected!=checks['en/'+name+'.aac']:raise RuntimeError('English bank differs from this camera')
            data=base64.b64decode(''.join(session.run('base64 '+Q(path),timeout=15).split()),validate=True)
            if hashlib.md5(data).hexdigest()!=expected:raise RuntimeError('factory backup mismatch')
            dest=report_dir/(name+'.aac');dest.write_bytes(data);dest.chmod(0o600)
            original_md5+=expected+'  '+name+'.aac\n'
        session.run('set -e\numask 077\nmkdir '+Q(stage)+'\nmkdir '+Q(transaction))
        if not present:
            session.run('mkdir '+Q(staged))
            upload(session,stage,'voice-archive',archive,budget_seconds=90)
            session.run('set -e\ntar -xzf '+Q(stage+'/voice-archive')+' -C '+Q(staged)+'\nsync\nmv '+Q(staged)+' '+Q(BANK),timeout=45)
        upload(session,stage,'validate',validator,budget_seconds=60)
        upload(session,stage,'original-md5',original_md5.encode(),budget_seconds=20)
        session.run('cp '+Q(stage+'/original-md5')+' '+Q(transaction+'/original-md5'))
        restore=rollback_script(transaction,original).encode()
        upload(session,stage,'rollback',restore,budget_seconds=20)
        session.run('cp '+Q(stage+'/rollback')+' '+Q(transaction+'/rollback.sh')+'\nchmod 700 '+Q(transaction+'/rollback.sh'))
        guard=f'''#!/bin/sh
trap '' HUP
printf '%s\\n' "$$" > {Q(transaction+'/guard.pid')}
awk '{{print $22}}' /proc/$$/stat > {Q(transaction+'/guard.birth')}
: > {Q(transaction+'/guard.ready')}
sleep 120
sh {Q(transaction+'/rollback.sh')} timed
'''.encode()
        upload(session,stage,'guard',guard,budget_seconds=20)
        session.run('cp '+Q(stage+'/guard')+' '+Q(transaction+'/guard.sh')+'\nsh '+Q(transaction+'/guard.sh')+' </dev/null >'+Q(transaction+'/guard.log')+' 2>&1 &')
        armed=True;time.sleep(1)
        session.run('[ -f '+Q(transaction+'/guard.ready')+' ]\np=$(cat '+Q(transaction+'/guard.pid')+')\nb=$(cat '+Q(transaction+'/guard.birth')+')\n[ "$(awk \'{print $22}\' /proc/$p/stat)" = "$b" ]')
        if identity(session)!=before:raise RuntimeError('host lifetime changed before voice remap')
        for name in NAMES:
            if session.run('md5sum '+Q(ALIAS+'/'+name+'.aac')).split()[0]!=checks['en/'+name+'.aac']:
                raise RuntimeError('original changed before remap')
        # Prepare both links before preserving the original directory.
        session.run('set -e\n[ ! -e '+CURRENT+' ] && [ ! -L '+CURRENT+' ]\nln -s '+Q(BANK+'/en')+' '+CURRENT+'\nln -s '+CURRENT+' '+Q('/etc/ch/.en-g2h-next-'+nonce)+'\nsync')
        session.run('set -e\n[ ! -e '+Q(original)+' ]\nmv '+ALIAS+' '+Q(original)+'\nmv '+Q('/etc/ch/.en-g2h-next-'+nonce)+' '+ALIAS+'\nsync')
        report['mutated']=True
        result=session.run(Q(stage+'/validate'),timeout=30)
        if 'VOICE_BANKS_VALID_EN' not in result:raise RuntimeError('production voice-bank validation failed')
        for name in NAMES:
            if session.run('md5sum '+Q(ALIAS+'/'+name+'.aac')).split()[0]!=checks['en/'+name+'.aac']:
                raise RuntimeError('English alias does not preserve original audio')
        report['after']=identity(session);report['status_after']=safe_status(session)
        if report['after']!=before or report['status_after']['fabrics']!=3:raise RuntimeError('camera identity changed')
        session.run('set -e\nmkdir '+Q(transaction+'/action.lock')+'\ntrap '+Q('rmdir '+Q(transaction+'/action.lock')+' 2>/dev/null || :')+' EXIT\n[ ! -f '+Q(transaction+'/restored')+' ]\n: > '+Q(transaction+'/committed')+'\nsync')
        armed=False;report['complete']=True
        recovery='Restore the previous own Matter app first, then run on the camera:\n\nsh '+transaction+'/rollback.sh force\n\nThis restores and verifies the original English directory; staged SD banks are retained.\n'
        (report_dir/'RECOVERY.txt').write_text(recovery)
        return report_dir
    finally:
        if armed:
            try:session.run('sh '+Q(transaction+'/rollback.sh')+' force',timeout=30);report['rollback_verified']=True
            except Exception:report['rollback_waiting_for_timer']=True
        private_json(report_dir/'result.private.json',report)
        # Exact temporary names only; persistent recovery and original stay.
        try:
            for name in ('validate','voice-archive','original-md5','rollback','guard'):session.run('rm -f '+Q(stage+'/'+name))
            session.run('rmdir '+Q(stage))
        except Exception:pass

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--host',required=True);p.add_argument('--banks',required=True,type=Path)
    p.add_argument('--validator',required=True,type=Path)
    p.add_argument('--backup-root',required=True,type=Path);a=p.parse_args()
    payload(a.banks);image_bytes(a.validator)
    password=getpass.getpass('Camera root Telnet password: ');session=Telnet(a.host)
    try:
        session.login(password);password=None
        print('VOICE_BANKS_INSTALLED_WITH_FACTORY_RESTORE',install(session,a.banks,a.validator,a.backup_root),flush=True)
    finally:password=None;session.close()
if __name__=='__main__':main()
