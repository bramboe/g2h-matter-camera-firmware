"""Bounded, acknowledged binary transfer over the existing Telnet connection.

Only our Telnet PTY is temporarily changed, never a radio UART. All data and end
markers are inert shell comments if a receiver exits early. Native decoder,
size/hash verification and terminal restoration are checked before use.
"""
import base64,hashlib,re,secrets,shlex,time

def upload(session,directory,name,image,budget_seconds=30):
    if not isinstance(budget_seconds,int) or not 5<=budget_seconds<=600:raise ValueError('bounded upload time required')
    if not re.fullmatch(r'/tmp/g2h_[A-Za-z0-9_]+',directory) or not re.fullmatch(r'[A-Za-z0-9_-]+',name):raise ValueError('temporary G2H path required')
    if len(image)>12*1024*1024:raise ValueError('bounded prototype upload only')
    nonce=secrets.token_hex(12);ready='G2H_UPLOAD_READY_'+nonce;done='G2H_UPLOAD_DONE_'+nonce
    ack='G2H_A_'+nonce+'_';ending='#G2H_E_'+nonce
    target=shlex.quote(directory+'/'+name)
    terminal=session.run('stty -g').strip()
    if not re.fullmatch(r'[0-9a-fA-F:]{1,200}',terminal):raise ValueError('unknown Telnet terminal snapshot')
    restore='stty '+terminal
    # Raw input applies backpressure. Every 16 lines are acknowledged through a
    # single stderr pipe, keeping the PTY window bounded to one 8-KiB block.
    program='$0==e{exit}{print substr($0,2);if(NR%16==0){print a NR|"cat >&2";fflush("cat >&2")}}'
    decoder='awk -v e='+shlex.quote(ending)+' -v a='+shlex.quote(ack)+' '+shlex.quote(program)+' | base64 -d > '+target
    script="stty -icanon min 1 time 0 -echo || exit 1; trap "+shlex.quote(restore+' 2>/dev/null')+" 0 1 2 15; printf '\\n%s\\n' "+shlex.quote(ready)+'; '+decoder+'; rc=$?; '+restore+"; printf '\\n%s %s\\n' "+shlex.quote(done)+' "$rc"'
    command='sh -c '+shlex.quote(script)
    if len(command)>900:raise ValueError('terminal command too long')
    session.send(command+'\r\n');session.until(r'(?m)^'+ready+r'\n',5)
    deadline=time.monotonic()+budget_seconds;finished=False
    prior=getattr(session,'write_timeout',5);session.write_timeout=30
    try:
        encoded=base64.b64encode(image).decode('ascii')
        for at in range(0,len(encoded),8192):
            if time.monotonic()>deadline:raise TimeoutError('bounded bulk-upload deadline')
            block=encoded[at:at+8192]
            session.send(''.join('#'+block[i:i+512]+'\n' for i in range(0,len(block),512)))
            if (len(block)+511)//512==16:
                pattern=r'(?m)^(?:'+ack+str(at//512+16)+'|'+done+r' ([0-9]+))\n'
                _,match=session.until(pattern,min(30,max(1,deadline-time.monotonic())))
                if match.group(1) is not None:
                    finished=True;raise RuntimeError('upload receiver exited before end marker')
        session.send(ending+'\n')
        _,match=session.until(r'(?m)^'+done+r' ([0-9]+)\n',15);finished=True
        if match.group(1)!='0':raise RuntimeError('remote base64 decoder failed')
    finally:
        if not finished:
            try:session.send(ending+'\n');session.until(r'(?m)^'+done+r' ([0-9]+)\n',5)
            except Exception:session.close()
        session.write_timeout=prior
    expected=hashlib.md5(image).hexdigest()
    session.run('set -e\n[ "$(md5sum '+target+' | awk \'{print $1}\')" = '+expected+' ]\n[ "$(stat -c %s '+target+')" = '+str(len(image))+' ]\nchmod 700 '+target)
    if session.run('stty -g').strip()!=terminal:raise RuntimeError('own Telnet terminal restoration not confirmed')
    return {'bytes':len(image),'checksum_verified':True,'own_telnet_terminal_restored':True}
