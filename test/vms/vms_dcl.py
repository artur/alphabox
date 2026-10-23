#!/usr/bin/env python3
"""Boot OpenVMS 8.4 on the ES40, log in, run a timed DCL workload.

  vms_dcl.py <alphabox> <label> [--port N] [--env VAR=value ...]
             [--workload none|dcl|full] [--shutdown] [--memory-bits N]
             [--keep]

The disk is an APFS clone (cp -c) of $ALPHABOX_WORK/vms84/disk-installed.img
in $ALPHABOX_WORK/runs/vms-dcl-<label>, deleted afterwards unless --keep.
The machine is guests_check.sh's vms-es40 one: 1 CPU, 1 GB (--memory-bits
29 for 512 MB), the ALi IDE disk, the console on a telnet port. --env
passes variables to the emulator.

Timed, in seconds of wall clock (the host's, so a busy host shows):
  srm       emulator start to P00>>>
  boot      `boot dqa0` to "job terminated" (the end of startup)
  username  `boot dqa0` to Username:
  login     `boot dqa0` to the first DCL prompt
  then each workload step, from sending it to the next DCL prompt:
  dcl       SHOW SYSTEM; @LOOP (SYS$MANAGER:LOOP.COM, a DCL loop); RUN HELLO
            (a MACRO-32 program); BACKUP of SYS$BASE_IMAGE.EXE, DIFFERENCES
            of the copy and DELETE, three times; a BACKUP saveset of
            SYS$LDR, BACKUP/LIST of it, DELETE; SPAWN SHOW TIME
  full      dcl, then MACRO and LINK of HELLO.MAR, its RUN, SHOW MEMORY,
            SHOW DEVICE
--shutdown ends with @SYS$SYSTEM:SHUTDOWN (timed as `shutdown`). A step
fails the run if DIFFERENCES finds a difference or the output has a
%FAC-W/E/F message.

The decompressed console is cached in $ALPHABOX_WORK/runs/vms-dcl-cache
(so no run times the SRM decompressor). Prints one line:
  PASS|FAIL <label> srm=.. boot=.. username=.. <step>=.. ...  load=<1-min avg>
and writes times.json beside the console log. Standard library only.
"""
import argparse
import json
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import time

R = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
WORK = os.environ.get('ALPHABOX_WORK', os.path.join(R, 'lab'))
DISK = os.path.join(WORK, 'vms84', 'disk-installed.img')
SRM = os.path.join(R, 'test', 'rom', 'cl67srmrom.exe')
CACHE = os.path.join(WORK, 'runs', 'vms-dcl-cache')
PROMPT = r'\$ $'


class Chat:
    def __init__(self, port, logf, deadline_s=120):
        self.log = open(logf, 'wb', buffering=0)
        self.buf = ''
        self.replies = []
        end = time.time() + deadline_s
        s = None
        while time.time() < end:
            try:
                s = socket.create_connection(('127.0.0.1', port), timeout=2)
                break
            except OSError:
                time.sleep(0.5)
        if s is None:
            raise RuntimeError('no connection to the console')
        s.settimeout(0.5)
        self.s = s

    def rd(self):
        try:
            d = self.s.recv(65536)
        except socket.timeout:
            return
        if not d:
            raise EOFError
        d = re.sub(rb'\xff[\xfb-\xfe].', b'', d).replace(b'\x00', b'')
        self.log.write(d)
        self.buf += d.decode('latin1').replace('\r', '')
        for rx, text in self.replies:
            m = re.search(rx, self.buf, re.M)
            if m:
                self.buf = self.buf[m.end():]
                self.send(text)

    def send(self, text):
        b = text.replace('$DATE', time.strftime('%d-%b-%Y %H:%M', time.gmtime()
                                                ).upper()).encode() + b'\r'
        for i in range(0, len(b), 8):
            self.s.sendall(b[i:i + 8])
            time.sleep(0.02)

    def expect(self, rx, tmo):
        end = time.time() + tmo
        r = re.compile(rx, re.M)
        while time.time() < end:
            m = r.search(self.buf)
            if m:
                text = self.buf[:m.end()]
                self.buf = self.buf[m.end():]
                return text
            self.rd()
        return None


def workload(name):
    """(step name, DCL command, timeout) in order."""
    if name == 'none':
        return []
    w = [('show_system', 'SHOW SYSTEM', 120),
         ('loop', '@SYS$MANAGER:LOOP', 300),
         ('hello', 'RUN SYS$MANAGER:HELLO', 120)]
    # SYS$COMMON, not SYS$LOADABLE_IMAGES: BACKUP takes the first directory
    # of a search list only, and finds nothing in SYS$SPECIFIC
    img = 'SYS$COMMON:[SYS$LDR]SYS$BASE_IMAGE.EXE'
    for i in range(3):
        w += [('backup%d' % i, 'BACKUP %s SYS$SCRATCH:GC_COPY.EXE' % img, 300),
              ('diff%d' % i, 'DIFFERENCES %s SYS$SCRATCH:GC_COPY.EXE' % img, 300),
              ('delete%d' % i, 'DELETE SYS$SCRATCH:GC_COPY.EXE;*', 120)]
    w += [('saveset', 'BACKUP SYS$COMMON:[SYS$LDR]*.EXE SYS$SCRATCH:GC.BCK/SAVE_SET', 600),
          ('saveset_list', 'BACKUP/LIST=SYS$SCRATCH:GC.LIS SYS$SCRATCH:GC.BCK/SAVE_SET', 600),
          ('saveset_delete', 'DELETE SYS$SCRATCH:GC.BCK;*,GC.LIS;*', 120),
          ('spawn', 'SPAWN SHOW TIME', 300)]
    if name == 'full':
        w += [('macro', 'MACRO/OBJECT=SYS$SCRATCH:H2.OBJ SYS$MANAGER:HELLO.MAR', 600),
              ('link', 'LINK/EXECUTABLE=SYS$SCRATCH:H2.EXE SYS$SCRATCH:H2.OBJ', 600),
              ('run2', 'RUN SYS$SCRATCH:H2.EXE', 120),
              ('show_memory', 'SHOW MEMORY', 120),
              ('show_device', 'SHOW DEVICE', 120)]
    return w


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('alphabox')
    ap.add_argument('label')
    ap.add_argument('--port', type=int, default=22600)
    ap.add_argument('--env', action='append', default=[])
    ap.add_argument('--workload', default='dcl', choices=['none', 'dcl', 'full'])
    ap.add_argument('--shutdown', action='store_true')
    ap.add_argument('--keep', action='store_true')
    ap.add_argument('--boot-timeout', type=float, default=900)
    ap.add_argument('--memory-bits', type=int, default=30,
                    help='guest memory, 2^N bytes (30: 1 GB, as guests_check)')
    a = ap.parse_args()

    binp = os.path.abspath(a.alphabox)
    d = os.path.join(WORK, 'runs', 'vms-dcl-' + a.label)
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    os.makedirs(CACHE, exist_ok=True)
    if subprocess.run(['cp', '-c', DISK, os.path.join(d, 'disk.img')]).returncode:
        print('FAIL %s no disk clone of %s' % (a.label, DISK))
        return 2
    shutil.copy(SRM, os.path.join(d, 'cl67srmrom.exe'))
    cached = os.path.join(CACHE, 'decompressed.rom')
    if os.path.exists(cached):
        shutil.copy(cached, os.path.join(d, 'decompressed.rom'))
    with open(os.path.join(d, 'es40.cfg'), 'w') as f:
        f.write('''sys0 = tsunami
{
  memory.bits = %d;
  rom.srm = "cl67srmrom.exe";
  rom.decompressed = "decompressed.rom";
  rom.flash = "flash.rom";
  rom.dpr = "dpr.rom";
  cpu0 = ev68cb { speed = 833M; }
  serial0 = serial { address = "127.0.0.1"; port = %d; }
  serial1 = serial { null_attach = true; }
  pci0.7 = ali { vga_console = false; }
  pci0.15 = ali_ide
  {
    disk0.0 = file { file = "disk.img"; }
  }
  pci0.19 = ali_usb { }
}
''' % (a.memory_bits, a.port))
    env = dict(os.environ)
    env['SDL_VIDEO_DRIVER'] = 'dummy'
    for kv in a.env:
        k, _, v = kv.partition('=')
        env[k] = v
    out = open(os.path.join(d, 'alphabox.out'), 'wb')
    t0 = time.time()
    p = subprocess.Popen([binp, 'run', 'es40.cfg'], cwd=d, env=env,
                         stdout=out, stderr=subprocess.STDOUT)
    times = {}
    ok = True
    why = ''
    load0 = os.getloadavg()[0]

    def stop():
        if p.poll() is None:
            p.send_signal(signal.SIGTERM)
            try:
                p.wait(60)
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait()

    try:
        c = Chat(a.port, os.path.join(d, 'console.log'))
        if c.expect(r'P00>>>', 600) is None:
            raise RuntimeError('no P00>>>')
        times['srm'] = time.time() - t0
        if not os.path.exists(cached) and os.path.exists(os.path.join(d, 'decompressed.rom')):
            shutil.copy(os.path.join(d, 'decompressed.rom'), cached + '.tmp')
            os.replace(cached + '.tmp', cached)
        c.replies.append((r'enter date and time[^\n]*\n?', '$DATE'))
        c.send('boot dqa0')
        tb = time.time()
        if c.expect(r'job terminated at', a.boot_timeout) is None:
            raise RuntimeError('startup did not finish')
        times['boot'] = time.time() - tb
        end = time.time() + 3
        while time.time() < end:
            c.rd()
        c.send('')
        if c.expect(r'Username: ', 300) is None:
            raise RuntimeError('no Username:')
        times['username'] = time.time() - tb
        c.send('SYSTEM')
        if c.expect(r'Password: ', 120) is None:
            raise RuntimeError('no Password:')
        c.send('Alphabox84')
        if c.expect(PROMPT, 600) is None:
            raise RuntimeError('no DCL prompt')
        times['login'] = time.time() - tb
        c.send('SET TERMINAL/WIDTH=132/PAGE=0')
        c.expect(PROMPT, 120)
        texts = {}
        for name, cmd, tmo in workload(a.workload):
            ts = time.time()
            c.send(cmd)
            t = c.expect(PROMPT, tmo)
            if t is None:
                raise RuntimeError('%s: no prompt within %ds' % (name, tmo))
            times[name] = time.time() - ts
            texts[name] = t
        for name, t in texts.items():
            if name.startswith('diff') and not re.search(
                    r'difference sections found: *0|No differences', t, re.I):
                ok = False
                why += ' %s found differences;' % name
            if re.search(r'%[A-Z]+-[FEW]-', t):
                ok = False
                why += ' %s: %s;' % (name, re.search(r'%[A-Z]+-[FEW]-[^\n]*', t).group(0))
        if a.shutdown:
            ts = time.time()
            c.replies = [(r'(\]\??:?|\?) ?$', '')]
            c.send('@SYS$SYSTEM:SHUTDOWN')
            if c.expect(r'SHUTDOWN COMPLETE|halted CPU|P00>>>', 600) is None:
                raise RuntimeError('shutdown did not complete')
            times['shutdown'] = time.time() - ts
    except (RuntimeError, EOFError) as e:
        ok = False
        why += ' %s;' % e
    finally:
        stop()
    con = open(os.path.join(d, 'console.log'), 'rb').read().decode('latin1')
    if re.search(r'BUGCHECK|CPUSPINWAIT|INVEXCEPTN', con):
        ok = False
        why += ' bugcheck;'
    times['load_start'] = load0
    times['load_end'] = os.getloadavg()[0]
    times['ok'] = ok
    times['memory_bits'] = a.memory_bits
    times['binary'] = binp
    with open(os.path.join(d, 'times.json'), 'w') as f:
        json.dump(times, f, indent=1)
    line = ' '.join('%s=%.1f' % (k, v) for k, v in times.items()
                    if isinstance(v, float) and not k.startswith('load'))
    print('%s %s %s load=%.1f/%.1f%s' % ('PASS' if ok else 'FAIL', a.label, line,
                                       load0, times['load_end'],
                                       (' --' + why) if why else ''))
    if not a.keep:
        for f in ('disk.img',):
            try:
                os.remove(os.path.join(d, f))
            except OSError:
                pass
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
