#!/usr/bin/env python3
"""OpenVMS on one binary, the native vmspal routines against the PALcode.

  vms_ab.py <alphabox> <label> [--rounds N] [--port-base P] [--sequential]
            [--workload dcl|full|none]

Runs test/vms/vms_dcl.py N rounds; a round is one boot with the native
OpenVMS PALcode routines (the interpreter's default, arm "vmspal") and one
with palcode.vms.nohle = true (the console's PALcode, arm "nohle"), on the
same binary. By default the two of a round run at the same time (ports P
and P+1, so both see the same host); --sequential runs them one after the
other, the order alternating by round. Then prints, per timed step, each
arm's median, min-max and the ratio of the medians.

This is not perf_ab.py (which has no OpenVMS workload): wall-clock times
on whatever the host is doing, so the result is an estimate. Each run's
line, with the host load at its start and end, goes to
$ALPHABOX_WORK/runs/vms-ab-<label>.txt, and the table to stdout.
"""
import argparse
import json
import os
import statistics
import subprocess
import sys
import time

T = os.path.dirname(os.path.abspath(__file__))
R = os.path.abspath(os.path.join(T, '..', '..'))
WORK = os.environ.get('ALPHABOX_WORK', os.path.join(R, 'lab'))


def pressure():
    """The host's memory pressure (macOS: 1 normal, 2 warning, 4 critical)."""
    r = subprocess.run(['sysctl', '-n', 'kern.memorystatus_vm_pressure_level'],
                       capture_output=True, text=True)
    return int(r.stdout.strip() or 1)


def run(binp, label, port, nohle, workload, bits):
    cmd = [sys.executable, '-I', os.path.join(T, 'vms_dcl.py'), binp, label,
           '--port', str(port), '--workload', workload, '--memory-bits', str(bits)]
    if nohle:
        cmd.append('--nohle')
    return subprocess.Popen(cmd, stdout=subprocess.PIPE, text=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('alphabox')
    ap.add_argument('label')
    ap.add_argument('--rounds', type=int, default=3)
    ap.add_argument('--port-base', type=int, default=22610)
    ap.add_argument('--sequential', action='store_true')
    ap.add_argument('--workload', default='dcl')
    ap.add_argument('--memory-bits', type=int, default=30)
    a = ap.parse_args()
    binp = os.path.abspath(a.alphabox)
    log = open(os.path.join(WORK, 'runs', 'vms-ab-%s.txt' % a.label), 'a')
    res = {'vmspal': [], 'nohle': []}
    for r in range(a.rounds):
        arms = ['vmspal', 'nohle'] if r % 2 == 0 else ['nohle', 'vmspal']
        procs = []
        for i, arm in enumerate(arms):
            lab = '%s-r%d-%s' % (a.label, r, arm)
            while pressure() >= 4:  # a guest paged out measures the host
                time.sleep(10)
            p = run(binp, lab, a.port_base + i, arm == 'nohle', a.workload,
                    a.memory_bits)
            if a.sequential:
                out = p.communicate()[0]
                procs.append((arm, lab, out))
            else:
                procs.append((arm, lab, p))
        for arm, lab, p in procs:
            out = p if isinstance(p, str) else p.communicate()[0]
            line = out.strip().splitlines()[-1] if out.strip() else 'FAIL %s no output' % lab
            line += ' pressure=%d' % pressure()
            print(line, flush=True)
            log.write(line + '\n')
            log.flush()
            tj = os.path.join(WORK, 'runs', 'vms-dcl-' + lab, 'times.json')
            if line.startswith('PASS') and os.path.exists(tj):
                res[arm].append(json.load(open(tj)))
    steps = []
    for arm in res:
        for t in res[arm]:
            for k, v in t.items():
                if isinstance(v, float) and not k.startswith('load') and k not in steps:
                    steps.append(k)
    groups = {'backup': ('backup', 'diff', 'delete', 'saveset')}
    print('\n%-16s %22s %22s %8s' % ('step (s)', 'vmspal median [min-max]',
                                    'nohle median [min-max]', 'nohle/vmspal'))

    def col(vals):
        if not vals:
            return '%22s' % '-'
        return '%7.1f [%5.1f-%5.1f] n%d' % (statistics.median(vals), min(vals), max(vals), len(vals))

    def vals(arm, keys):
        out = []
        for t in res[arm]:
            if all(k in t for k in keys):
                out.append(sum(t[k] for k in keys))
        return out

    rows = [(s, [s]) for s in steps]
    io = [s for s in steps if s.startswith(groups['backup'])]
    if io:
        rows.append(('backup+diff all', io))
    wl = [s for s in steps if s not in ('srm', 'boot', 'username', 'login', 'shutdown')]
    if wl:
        rows.append(('workload total', wl))
    for name, keys in rows:
        va, vb = vals('vmspal', keys), vals('nohle', keys)
        ratio = (statistics.median(vb) / statistics.median(va)) if va and vb and statistics.median(va) else 0
        print('%-16s %22s %22s %8.2f' % (name, col(va), col(vb), ratio))
    return 0 if all(len(v) == a.rounds for v in res.values()) else 1


if __name__ == '__main__':
    sys.exit(main())
