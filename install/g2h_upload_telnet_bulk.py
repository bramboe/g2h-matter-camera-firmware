"""Bounded binary upload over the existing Telnet session, verified before use.

Only the caller's Telnet pseudo-terminal has echo temporarily disabled. Data
lines start with #, making them inert shell comments if a decoder exits early.
No serial-device settings or camera configuration are changed.
"""
import base64,hashlib,re,secrets,shlex,time

def upload(session,directory,name,image,budget_seconds=30):
    if not isinstance(budget_seconds,int) or not 5<=budget_seconds<=180:raise ValueError("bounded upload time required")
    if not re.fullmatch(r'/tmp/g2h_[A-Za-z0-9_]+',directory) or not re.fullmatch(r'[A-Za-z0-9_-]+',name):raise ValueError('temporary G2H path required')
    if len(image)>12*1024*1024:raise ValueError('bounded prototype upload only')
    nonce=secrets.token_hex(12);ready='G2H_UPLOAD_READY_'+nonce;done='G2H_UPLOAD_DONE_'+nonce
    target=shlex.quote(directory+'/'+name)
    terminal=session.run('stty -g').strip()
    if not re.fullmatch(r'[0-9a-fA-F:]{1,200}',terminal):raise ValueError('unknown Telnet terminal snapshot')
    restore='stty '+terminal
    script="stty icanon eof '^D' -echo || exit 1; trap "+shlex.quote(restore+' 2>/dev/null')+" 0 1 2 15; printf '\\n%s\\n' "+shlex.quote(ready)+"; sed 's/^#//' | base64 -d > "+target+"; rc=$?; "+restore+"; printf '\\n%s %s\\n' "+shlex.quote(done)+' "$rc"'
    command='sh -c '+shlex.quote(script)
    if len(command)>900:raise ValueError('terminal command too long')
    session.send(command+'\r\n');session.until(r'(?m)^'+ready+r'\n',5)
    deadline=time.monotonic()+budget_seconds;finished=False
    try:
        encoded=base64.b64encode(image).decode('ascii')
        for at in range(0,len(encoded),8192):
            if time.monotonic()>deadline:raise TimeoutError('bounded bulk-upload deadline')
            block=encoded[at:at+8192]
            session.send(''.join('#'+block[i:i+512]+'\r\n' for i in range(0,len(block),512)))
        session.send('\x04')
        _,match=session.until(r'(?m)^'+done+r' ([0-9]+)\n',10);finished=True
        if match.group(1)!='0':raise RuntimeError('remote base64 decoder failed')
    finally:
        if not finished:
            try:session.send('\x04');session.until(r'(?m)^'+done+r' ([0-9]+)\n',5)
            except Exception:session.close()
    expected=hashlib.md5(image).hexdigest()
    session.run('set -e\n[ "$(md5sum '+target+' | awk \'{print $1}\')" = '+expected+' ]\n[ "$(stat -c %s '+target+')" = '+str(len(image))+' ]\nchmod 700 '+target)
    if session.run('stty -g').strip()!=terminal:raise RuntimeError('own Telnet terminal restoration not confirmed')
    return {'bytes':len(image),'checksum_verified':True,'own_telnet_terminal_restored':True}
