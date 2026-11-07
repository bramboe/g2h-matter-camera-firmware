"""Prepared, opt-in Telnet/ISP identity probe. Default execution is offline.

--run-identification changes volatile radio/GPIO/UART/process state, but sends
only DEFAULT-session and GET_CHIPID frames. It does not flash, dump memory,
change persistent configuration, or stop the shared monitor.
"""
import argparse
import getpass
import re
import select
import shlex
import socket
import time
import uuid

from g2h_readonly_client import ReadOnlyClient, encode_command, Rejected


class Telnet:
    def __init__(self, host):
        self.sock = socket.create_connection((host, 23), timeout=5)
        self.sock.setblocking(False)
        self.mode = 'data'
        self.verb = None
        self.pending = ''

    def send(self, value):
        # Commands are short, including octal-encoded scripts (<16 KiB).
        data = value.encode().replace(b'\xff', b'\xff\xff')
        budget = getattr(self, "write_timeout", 5)
        if not isinstance(budget, (int, float)) or not 1 <= budget <= 30:
            raise ValueError("bounded Telnet write timeout required")
        deadline = time.monotonic() + budget
        while data:
            left = deadline - time.monotonic()
            if left <= 0 or not select.select([], [self.sock], [], left)[1]:
                raise TimeoutError('Telnet write deadline')
            try:
                n = self.sock.send(data)
            except BlockingIOError:
                continue
            if not n:
                raise EOFError('Telnet closed during write')
            data = data[n:]

    def until(self, pattern, timeout):
        deadline = time.monotonic() + timeout
        regex = re.compile(pattern)
        while True:
            match = regex.search(self.pending)
            if match:
                result = self.pending[:match.end()]
                self.pending = self.pending[match.end():]
                return result, match
            left = deadline - time.monotonic()
            if left <= 0 or not select.select([self.sock], [], [], left)[0]:
                raise TimeoutError('Telnet receive deadline')
            raw = self.sock.recv(8192)
            if not raw:
                raise EOFError('Telnet connection closed')
            out = bytearray()
            for c in raw:
                if self.mode == 'data':
                    if c == 255:
                        self.mode = 'iac'
                    elif c != 13:
                        out.append(c)
                elif self.mode == 'iac':
                    if c in (251,252,253,254):
                        self.verb, self.mode = c, 'option'
                    elif c == 250:
                        self.mode = 'sub'
                    else:
                        if c == 255:
                            out.append(c)
                        self.mode = 'data'
                elif self.mode == 'option':
                    if self.verb == 253:
                        self._negotiate(252, c)
                    elif self.verb == 251:
                        self._negotiate(253 if c in (1,3) else 254, c)
                    self.mode = 'data'
                elif self.mode == 'sub':
                    if c == 255:
                        self.mode = 'sub_iac'
                elif self.mode == 'sub_iac':
                    self.mode = 'data' if c == 240 else 'sub'
            self.pending += out.decode(errors='replace')
            if len(self.pending) > 262144:
                raise Rejected('unexpectedly large Telnet output')

    def _negotiate(self, verb, option):
        self.sock.setblocking(True)
        self.sock.settimeout(2)
        try:
            self.sock.sendall(bytes((255,verb,option)))
        finally:
            self.sock.setblocking(False)

    def login(self, password):
        self.until(r'login: ', 5)
        self.send('root\r\n')
        self.until(r'Password: ', 5)
        self.send(password + '\r\n')
        self.until(r'Choice: ', 20) # the camera menu can take longer than 8 s
        self.send('0\r\n')
        self.until(r'(?:^|\n)# ', 5)

    def run(self, command, timeout=8):
        token = uuid.uuid4().hex
        begin, end = 'G2H_BEGIN_' + token, 'G2H_END_' + token
        script = "printf '\\n%s\\n' " + shlex.quote(begin)
        script += '; sh -c ' + shlex.quote(command)
        script += "; g2h_rc=$?; printf '\\n%s %s\\n' " + shlex.quote(end) + ' "$g2h_rc"'
        if max(map(len, script.splitlines()), default=0) > 900:
            raise Rejected('command exceeds conservative terminal line limit')
        self.send(script + '\r\n')
        text, match = self.until(r'(?m)^' + end + r' ([0-9]+)\n', timeout)
        start = text.find('\n' + begin + '\n')
        if start < 0:
            raise Rejected('missing command boundary')
        output = text[start+len(begin)+2:match.start()]
        if int(match.group(1)):
            raise RuntimeError('remote command failed: ' + output[-1500:])
        return output

    def close(self):
        self.sock.close()


def octal(data):
    return ''.join('\\%03o' % b for b in data)


RESTORE = r'''#!/bin/sh
cd "$1" || exit 1
trap '' HUP INT TERM
sleep "$2"
[ -f done ] && exit 0
if ! mkdir restore.lock 2>/dev/null; then
    for i in 1 2 3 4 5; do
        if [ -f done ]; then exit "$(cat result)"; fi
        sleep 1
    done
    exit 1
fi
failed=0
same() {
    [ -r "/proc/$1/stat" ] || return 1
    [ "$(awk '{print $22}' "/proc/$1/stat")" = "$2" ]
}
if [ -r worker.pid ] && [ -r worker.birth ]; then
    p=$(cat worker.pid); b=$(cat worker.birth)
    if same "$p" "$b"; then kill -KILL "$p" 2>/dev/null || :; fi
fi
if [ -r reader.pid ] && [ -r reader.birth ]; then
    r=$(cat reader.pid); b=$(cat reader.birth)
    if same "$r" "$b"; then kill "$r" 2>/dev/null || :; fi
fi
if [ -r drainer.pid ] && [ -r drainer.birth ]; then
    r=$(cat drainer.pid); b=$(cat drainer.birth)
    if same "$r" "$b"; then kill "$r" 2>/dev/null || :; fi
fi
if [ -f entered ]; then
    printf high > /sys/class/gpio/gpio18/direction || failed=1
    printf low > /sys/class/gpio/gpio63/direction || failed=1
    sleep 1
    printf high > /sys/class/gpio/gpio63/direction || failed=1
    sleep 1
fi
if [ -f serial ]; then
    stty -F /dev/ttyS2 "$(cat serial)" || failed=1
fi
if [ -f entered ]; then
    printf in > /sys/class/gpio/gpio18/direction || failed=1
    printf in > /sys/class/gpio/gpio63/direction || failed=1
fi
if [ -f agent.pid ] && [ -f agent.birth ]; then
    p=$(cat agent.pid); b=$(cat agent.birth)
    if same "$p" "$b"; then kill -CONT "$p" || failed=1; else failed=1; fi
fi
printf '%s\n' "$failed" > result
: > done
echo "RESTORE_RESULT=$failed"
exit "$failed"
'''


def prepare_script(directory):
    # The first identification attempt requires the measured baseline exactly.
    return 'cd ' + shlex.quote(directory) + r''' || exit 1
set -eu
[ "$(md5sum /tmp/out/zigbee_agent | awk '{print $1}')" = d03f0eb913cbf5efd0d66e9ad9d31936 ]
[ "$(md5sum /etc/zigbeeAgent.conf | awk '{print $1}')" = 939537a7def186418cd20a24411ecce0 ]
for n in 18 63; do
    [ "$(cat /sys/class/gpio/gpio$n/direction)" = in ]
    [ "$(cat /sys/class/gpio/gpio$n/value)" = 1 ]
    [ "$(cat /sys/class/gpio/gpio$n/active_low)" = 0 ]
done
p=
for d in /proc/[0-9]*; do
    [ "$(cat "$d/comm" 2>/dev/null)" = zigbee_agent ] || continue
    [ -z "$p" ]; p=${d##*/}
done
[ -n "$p" ]
[ "$(awk '/^State:/ {print $2}' /proc/$p/status)" = S ]
parent=$(awk '/^PPid:/ {print $2}' /proc/$p/status)
[ "$(cat /proc/$parent/comm)" = monitor ]
ls -l /proc/$parent/fd | grep -q '/dev/watchdog'
printf '%s\n' "$p" > agent.pid
awk '{print $22}' /proc/$p/stat > agent.birth
ls -l /proc/$p/fd | grep -q '/dev/ttyS2'
for d in /proc/[0-9]*; do
    [ "${d##*/}" = "$p" ] && continue
    if ls -l "$d/fd" 2>/dev/null | grep -q '/dev/ttyS2'; then
        echo 'Unexpected UART holder'; exit 1
    fi
done
stty -F /dev/ttyS2 -g > serial
printf '%s\n' "$$" > worker.pid
awk '{print $22}' /proc/$$/stat > worker.birth
nohup sh ./restore.sh "$PWD" 45 > restore.log 2>&1 < /dev/null &
guard=$!
printf '%s\n' "$guard" > guard.pid
kill -0 "$guard"
kill -STOP "$p"
sleep 1
[ "$(awk '/^State:/ {print $2}' /proc/$p/status)" = T ]
stty -F /dev/ttyS2 115200 raw -echo cs8 -parenb -cstopb -ixon -ixoff -crtscts clocal -hupcl min 0 time 1
# Drain old runtime replies while stopped; VTIME bounds a no-data read.
hexdump -v -n 521 -e '1/1 "%02x\n"' /dev/ttyS2 > drain.hex &
drainer=$!
printf '%s\n' "$drainer" > drainer.pid
awk '{print $22}' /proc/$drainer/stat > drainer.birth || :
wait "$drainer"
: > entered
printf low > /sys/class/gpio/gpio18/direction
printf low > /sys/class/gpio/gpio63/direction
sleep 1
printf high > /sys/class/gpio/gpio63/direction
sleep 1
echo ISP_READY
'''


class IdentityTransport:
    """Only the two exact first-identification packets can reach the UART."""
    allowed = (encode_command(0x4e,b'\0'), encode_command(0x32))

    def __init__(self, session, directory):
        self.session, self.directory, self.pending = session, directory, b''
        self.exchanges = []

    def _validate(self, data):
        if data not in self.allowed:
            raise Rejected('first probe permits only DEFAULT session and chip identity')

    def _reader_preamble(self):
        return ''

    def write_all(self, data, deadline):
        self._validate(data)
        if time.monotonic() >= deadline:
            raise TimeoutError('expired exchange')
        command = 'cd ' + shlex.quote(self.directory) + ' || exit 1; '
        command += r'''[ -f entered ] && [ ! -f done ] || exit 1
[ ! -d restore.lock ] || exit 1
'''
        command += self._reader_preamble()
        command += r'''printf '%s\n' "$$" > worker.pid
awk '{print $22}' /proc/$$/stat > worker.birth
hexdump -v -n 521 -e '1/1 "%02x\n"' /dev/ttyS2 &
r=$!
printf '%s\n' "$r" > reader.pid
awk '{print $22}' /proc/$r/stat > reader.birth
'''
        command += 'printf ' + shlex.quote(octal(data)) + ' > /dev/ttyS2\n'
        command += r'''(
  sleep 2
  b=$(cat reader.birth)
  if [ -n "$b" ] && [ "$(awk '{print $22}' /proc/$r/stat 2>/dev/null)" = "$b" ]; then
    kill "$r" 2>/dev/null || :
  fi
) &
g=$!
wait "$r" || :
kill "$g" 2>/dev/null || :
wait "$g" 2>/dev/null || :
exit 0
'''
        output = self.session.run(command, timeout=min(4, max(0.1, deadline-time.monotonic())))
        lines = output.splitlines()
        if any(line and not re.fullmatch('[0-9a-f]{2}',line) for line in lines):
            raise Rejected('unexpected UART bridge diagnostics')
        self.pending = bytes.fromhex(''.join(lines))
        self.exchanges.append({'tx': data.hex(), 'rx': self.pending.hex()})

    def read_some(self, maximum, deadline):
        if time.monotonic() >= deadline:
            raise TimeoutError('expired buffered read')
        part, self.pending = self.pending[:maximum], self.pending[maximum:]
        return part


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', default='192.168.1.50')
    parser.add_argument('--run-identification', action='store_true')
    args = parser.parse_args()
    if not args.run_identification:
        print('OFFLINE ONLY. First probe: volatile ISP reset, DEFAULT session, GET_CHIPID, normal reset, resume agent.')
        print('45-second independent recovery process; no flash or persistent configuration changes.')
        return
    password = getpass.getpass('Camera root Telnet password: ')
    directory = '/tmp/g2h_isp_' + uuid.uuid4().hex
    session = Telnet(args.host)
    prepared = False
    try:
        session.login(password)
        password = None
        session.run('umask 077; mkdir ' + shlex.quote(directory))
        encoded = RESTORE.encode()
        for offset in range(0, len(encoded), 128):
            operator = ' > ' if offset == 0 else ' >> '
            session.run('printf ' + shlex.quote(octal(encoded[offset:offset+128])) + operator + shlex.quote(directory+'/restore.sh'))
        prepared = True
        print(session.run(prepare_script(directory), timeout=15))
        print('IDENTITY:', ReadOnlyClient(IdentityTransport(session,directory),timeout=5).identify())
    finally:
        if prepared:
            try:
                command = 'sh ' + shlex.quote(directory+'/restore.sh') + ' ' + shlex.quote(directory) + ' 0'
                print(session.run(command, timeout=10))
                print(session.run('cat ' + shlex.quote(directory+'/result') + '; ps | grep "[z]igbee_agent"',timeout=5))
            except Exception as exc:
                print('Recovery confirmation unavailable. The scheduled guard applies only if preparation reached guard startup:',type(exc).__name__, directory)
        session.close()


if __name__ == '__main__':
    main()
