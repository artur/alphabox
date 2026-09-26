#!/usr/bin/env python3
"""A/B two emulator binaries on the NT benchmark, and refuse to be misread.

usage: perf_ab.py <label> <base-binary> <head-binary> [--rounds N] [--workload axp]
                  [--section all] [--scale 1] [--expect stride:-20,alu:0,...]

Every performance claim in this project goes through here, because the ways
it has gone wrong are all mechanical and a script can refuse each of them:

  - a build or another guest running during a timed round (refused up front,
    and re-checked before every boot);
  - a delta quoted from a single pair (fewer than 2 interleaved rounds is a
    hard error, not a warning);
  - phases mixed up (the guest times each section itself; nothing here reads
    a stats window);
  - the host clock guessed (it is read, printed, and used for cycles/instr);
  - the wrong build measured (both binaries are hashed, and HEAD is recorded);
  - between-sitting drift mistaken for a change (the ledger keeps every run of
    every binary, so the same binary measured twice has a visible spread);
  - a mechanism assumed (--expect records the prediction BEFORE the run and
    marks each section hit or miss afterwards);
  - a speedup that changed the answers (every section's computed result must
    be identical across arms, or the run is a FAIL regardless of speed).

Output: a table, lab/results/<label>.json (everything), and one line appended
to lab/results/ledger.md. Quote the ledger, not memory.
"""
import argparse, hashlib, json, os, re, statistics, subprocess, sys, time, datetime

R = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
WORK = os.environ.get('ALPHABOX_WORK', os.path.join(R, 'lab'))
SECTIONS = ['alu', 'branch', 'call', 'ldst', 'stride', 'fp', 'byte', 'div', 'sort', 'cab']

def sh(cmd):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True).stdout.strip()

def sha(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()[:12]

def busy_host():
    """Anything that would perturb a timed round: a compiler actually running,
    or another guest. Judged by the EXECUTABLE, not by command text -- a shell
    whose script mentions `cmake --build` is not a build, and the wrapper this
    tool is launched from usually mentions it. Our own ancestry is excluded."""
    procs = sh("ps -Ao pid,ppid,args")
    table = {}
    for ln in procs.splitlines()[1:]:
        parts = ln.split(None, 2)
        if len(parts) == 3: table[parts[0]] = (parts[1], parts[2])
    mine = set(); p = str(os.getpid())
    while p in table and p not in mine:
        mine.add(p); p = table[p][0]
    reasons = []
    for pid, (ppid, args) in table.items():
        if pid in mine or ppid in mine: continue
        exe = os.path.basename(args.split()[0]) if args.split() else ''
        if exe in ('clang', 'clang++', 'cc1plus', 'cc', 'c++', 'ld', 'ld64.lld', 'ninja', 'make', 'gmake') \
           or (exe == 'cmake' and '--build' in args):
            reasons.append('a build: ' + args[:70])
        elif exe.startswith('qemu-system') or re.search(r'(lab/bin/\S+|alphabox|axpbox) run\b', args):
            reasons.append('a guest: ' + args[:70])
    return reasons

BUSY_OK = False    # --busy-ok: time anyway, and say so in the ledger
BUSY_SEEN = []     # what was running when a round started under --busy-ok

def refuse_if_busy(when, wait_s=4 * 3600):
    # Never time while a build or another guest runs -- but do not throw away
    # the rounds already done because another session's guest came and went
    # between two of ours: wait for a quiet host (polling), and say so.
    r = busy_host()
    if r and BUSY_OK:
        print(f"perf_ab: host busy ({when}), timing anyway (--busy-ok):\n  " + "\n  ".join(r), flush=True)
        BUSY_SEEN.extend(x for x in r if x not in BUSY_SEEN)
        r = []
    waited = 0
    while r and waited < wait_s:
        if waited == 0:
            print(f"perf_ab: host busy ({when}); waiting for it to be free:\n  " + "\n  ".join(r), flush=True)
        time.sleep(10); waited += 10
        r = busy_host()
    if r:
        sys.exit(f"perf_ab: gave up ({when}) after {waited // 60} min; the host is still busy with\n  " + "\n  ".join(r))
    if waited:
        print(f"perf_ab: host free after {waited // 60} min {waited % 60} s; a 30 s settle, then {when}", flush=True)
        time.sleep(30)
    # A run writes a fresh clone of a 4 GB image and a Windows boot diverges it
    # by hundreds of MB; a snapshot is the whole of guest RAM. A full disk does
    # not fail cleanly -- it failed the state save mid-write once, after the
    # guest had booted, and the failure read as "no snapshot" not "no space".
    st = os.statvfs(WORK)
    free_gb = st.f_bavail * st.f_frsize / 2**30
    if free_gb < 6:
        sys.exit(f"perf_ab: refusing ({when}): only {free_gb:.1f} GB free under {WORK}; "
                 "old run clones (lab/work-*, lab/bench-*, lab/ntbench-*) are the usual cause")

RATE_S = 0.1  # ALPHABOX_RATE window in snapshot runs: the MIPS column's resolution

def section_mips(logpath, ms, workload):
    """The guest's MIPS per section, from the run's ALPHABOX_RATE windows.

    A section runs at a steady rate, so the sections are the plateaus: runs of
    consecutive windows each within 25% of the run's mean, at least three
    long. The benchmark sleeps before each section, which usually leaves an
    idle window between them; when the console's work fills that gap, the
    change of rate still splits them. Plateaus are matched to the sections in
    order, each to the next one whose length fits the section's reported
    time; a warm-up blip or a console burst is too short to match. makecab is
    one plateau, the longest. The first and last window of a plateau are left
    out, and sections under half a second get no figure."""
    try:
        txt = open(logpath, errors='replace').read()
    except OSError:
        return {}
    wins = [(float(a), int(b), float(c)) for a, b, c in re.findall(
        r'CPU0-I-RATE: ([\d.]+) MIPS \((\d+) instructions in ([\d.]+) s\)', txt)]
    runs, cur = [], []
    for w in wins:
        if w[0] > 100 and cur and 0.75 <= w[0] / (sum(x[0] for x in cur) / len(cur)) <= 1.33:
            cur.append(w)
        else:
            if len(cur) >= 3:
                runs.append(cur)
            cur = [w] if w[0] > 100 else []
    if len(cur) >= 3:
        runs.append(cur)
    def rate(run):
        core = run[1:-1] if len(run) >= 4 else run
        t = sum(x[2] for x in core)
        return sum(x[1] for x in core) / t / 1e6 if t else None
    out = {}
    if workload == 'cab':
        if runs:
            out['cab'] = rate(max(runs, key=lambda r: sum(x[2] for x in r)))
        return out
    i = 0
    for s in SECTIONS:
        if s not in ms or ms[s] < 500:
            continue
        want = ms[s] / 1000.0
        for j in range(i, len(runs)):
            if abs(sum(x[2] for x in runs[j]) - want) <= 0.3 * want + 0.3:
                out[s] = rate(runs[j])
                i = j + 1
                break
    return out

def run_one(label, binary, workload, section, scale, snapshot=False, env_extra=None):
    """One run -- a cold boot, or a resumed snapshot; returns ({section: ms}, {section: result}, total_ms, log, {section: MIPS})."""
    logpath = None
    if snapshot:
        cmd = [os.path.join(R, 'test/tools/nt_snap.sh'), 'run', label, binary, workload, section, str(scale)]
        os.makedirs(os.path.join(WORK, 'results', 'runlogs'), exist_ok=True)
        logpath = os.path.join(WORK, 'results', 'runlogs', f'{label}.log')
        env_extra = dict(env_extra or {}, ALPHABOX_RATE=str(RATE_S), RUNLOG=logpath)
    else:
        cmd = [os.path.join(R, 'test/tools/nt_bench.sh'), label, binary, 'win2k-installed',
               'es40-window.cfg', workload, section, str(scale)]
    env = dict(os.environ, TIMEOUT='900', **(env_extra or {}))
    p = subprocess.run(cmd, capture_output=True, text=True, env=env)
    ms, res, total = {}, {}, None
    for ln in p.stdout.splitlines():
        m = re.match(r'(\w+)\s+(\d+) ms\s+\((-?\d+)\)', ln)
        if m: ms[m.group(1)] = int(m.group(2)); res[m.group(1)] = m.group(3)
        m = re.match(r'total (\d+) ms', ln)
        if m: total = int(m.group(1))
    if workload == 'cab':
        # makecab times nothing itself: the section is RUN.BAT's START..END,
        # both read from the guest's clock like every other section, and the
        # result is the hash of the cabinet it wrote (nt_snap prints it).
        t = {k: v for k, v in re.findall(r'\b(START|END)\s+(\d+:\d+:\d+\.\d+)', p.stdout)}
        h = re.search(r'^cab-sha (\w+)', p.stdout, re.M)
        if 'START' in t and 'END' in t:
            def sec(x):
                hh, mm, ss = x.split(':'); return int(hh) * 3600 + int(mm) * 60 + float(ss)
            d = sec(t['END']) - sec(t['START'])
            if d < 0: d += 86400
            ms['cab'] = total = int(round(d * 1000))
            res['cab'] = h.group(1)[:16] if h else 'none'
    mips = section_mips(logpath, ms, workload) if logpath else {}
    return ms, res, total, p.stdout, mips

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('label'); ap.add_argument('base'); ap.add_argument('head')
    ap.add_argument('--rounds', type=int, default=2)
    ap.add_argument('--workload', default='axp'); ap.add_argument('--section', default='all')
    ap.add_argument('--scale', default='1')
    ap.add_argument('--expect', default='', help='section:pct,... written down BEFORE the run')
    ap.add_argument('--busy-ok', action='store_true',
                    help='time even while other builds or guests run; the ledger row says "busy host". '
                         'A last resort: the verdict is weaker, and says so')
    ap.add_argument('--snapshot', action='store_true',
                    help='resume the desktop snapshot (nt_snap.sh run) instead of cold-booting each run')
    ap.add_argument('--env-base', action='append', default=[], metavar='K=V',
                    help='environment for the base arm only (repeatable): the same binary with a '
                         'runtime switch off measures a change free of code-layout effects')
    ap.add_argument('--env-head', action='append', default=[], metavar='K=V',
                    help='environment for the head arm only (repeatable)')
    a = ap.parse_args()
    global BUSY_OK
    BUSY_OK = a.busy_ok
    env_arm = {'base': dict(kv.split('=', 1) for kv in a.env_base),
               'head': dict(kv.split('=', 1) for kv in a.env_head)}
    if a.rounds < 2:
        sys.exit("perf_ab: fewer than 2 interleaved rounds cannot distinguish a change from drift; refusing")
    base, head = (os.path.abspath(a.base), os.path.abspath(a.head))
    for b in (base, head):
        if not os.access(b, os.X_OK): sys.exit(f"perf_ab: {b} is not executable")
    expect = {}
    for tok in filter(None, a.expect.split(',')):
        k, v = tok.split(':'); expect[k] = float(v)

    refuse_if_busy('before starting')
    meta = {
        'label': a.label, 'date': datetime.datetime.now().isoformat(timespec='seconds'),
        'host': sh('sysctl -n machdep.cpu.brand_string'),
        'pcore_ghz': None,  # Apple Silicon does not expose the clock; recorded when known
        'commit': sh(f'git -C {R} rev-parse --short HEAD'),
        'dirty': bool(sh(f'git -C {R} status --porcelain -- src')),
        'base': {'path': base, 'sha': sha(base)}, 'head': {'path': head, 'sha': sha(head)},
        'rounds': a.rounds, 'workload': a.workload, 'section': a.section, 'scale': a.scale,
        'mode': 'snapshot' if a.snapshot else 'cold-boot',
        'expect': expect, 'env': env_arm,
    }
    known = {'Apple M3 Max': 4.05, 'Apple M3 Pro': 4.05, 'Apple M3': 4.05, 'Apple M2': 3.49, 'Apple M1': 3.2, 'Apple M4': 4.4}
    for k, v in known.items():
        if meta['host'].startswith(k): meta['pcore_ghz'] = v
    if meta['base']['sha'] == meta['head']['sha']:
        print("perf_ab: NOTE base and head are the same binary -- this measures drift, which is a valid thing to measure")

    runs = {'base': [], 'head': []}
    for r in range(1, a.rounds + 1):
        for arm, binary in (('base', base), ('head', head)):
            refuse_if_busy(f'{arm} round {r}')
            print(f"== {arm} round {r}: {os.path.basename(binary)}", flush=True)
            ms, res, total, log, mips = run_one(f'{a.label}-{arm}-{r}', binary, a.workload, a.section, a.scale, a.snapshot, env_arm[arm])
            if total is None:
                sys.exit(f"perf_ab: {arm} round {r} produced no result:\n{log[-800:]}")
            runs[arm].append({'ms': ms, 'result': res, 'total': total, 'mips': mips})
            print(f"   total {total} ms", flush=True)

    # analysis
    secs = [s for s in SECTIONS if all(s in x['ms'] for x in runs['base'] + runs['head'])]
    rows, fails, overlap_total = [], [], False
    def stats(arm, s):
        v = [x['ms'][s] for x in runs[arm]] if s != 'TOTAL' else [x['total'] for x in runs[arm]]
        return v, statistics.median(v), min(v), max(v)
    for s in secs + ['TOTAL']:
        bv, bm, blo, bhi = stats('base', s); hv, hm, hlo, hhi = stats('head', s)
        delta = 100.0 * (hm - bm) / bm
        overlap = not (hhi < blo or hlo > bhi)
        if s == 'TOTAL': overlap_total = overlap
        exp = expect.get(s)
        hit = None
        if exp is not None:
            hit = abs(delta - exp) <= max(1.0, abs(exp) * 0.5)  # within 1 point or half the predicted size
        rows.append((s, bv, hv, delta, overlap, exp, hit))
    for s in secs:
        rb = {x['result'][s] for x in runs['base']}; rh = {x['result'][s] for x in runs['head']}
        if len(rb) != 1 or rb != rh:
            fails.append(f"{s}: computed results differ (base {sorted(rb)}, head {sorted(rh)})")

    print(f"\n{a.label}: {meta['host']} @ {meta['pcore_ghz'] or '?'} GHz | HEAD {meta['commit']}{' (dirty src)' if meta['dirty'] else ''}")
    print(f"base {meta['base']['sha']}  head {meta['head']['sha']}  rounds {a.rounds}\n")
    print(f"{'section':8s} {'base ms':>24s} {'head ms':>24s} {'delta':>8s}  {'overlap':7s} {'expected':>9s} {'':4s}")
    for s, bv, hv, d, ov, exp, hit in rows:
        e = f"{exp:+.1f}%" if exp is not None else ''
        h = '' if hit is None else ('HIT' if hit else 'MISS')
        print(f"{s:8s} {str(bv):>24s} {str(hv):>24s} {d:+7.1f}%  {'yes' if ov else 'no':7s} {e:>9s} {h:4s}")
    # MIPS, measured inside the emulator on the same runs (median per arm).
    if any(x.get('mips') for x in runs['base'] + runs['head']):
        print(f"\n{'section':8s} {'base MIPS':>12s} {'head MIPS':>12s}")
        for s in secs:
            bm = [x['mips'][s] for x in runs['base'] if x.get('mips', {}).get(s)]
            hm = [x['mips'][s] for x in runs['head'] if x.get('mips', {}).get(s)]
            f = lambda v: f"{statistics.median(v):12.0f}" if v else f"{'-':>12s}"
            print(f"{s:8s} {f(bm)} {f(hm)}")
    verdict = 'FAIL' if fails else ('inconclusive (ranges overlap)' if overlap_total else 'resolved')
    print(f"\nresults identical: {'no -- ' + '; '.join(fails) if fails else 'yes, all sections'}")
    print(f"verdict: {verdict}")

    out = dict(meta, runs=runs, rows=[dict(section=s, base=bv, head=hv, delta_pct=round(d, 2), overlap=ov,
                                          expected=exp, hit=hit) for s, bv, hv, d, ov, exp, hit in rows],
               fails=fails, verdict=verdict)
    os.makedirs(os.path.join(WORK, 'results'), exist_ok=True)
    jp = os.path.join(WORK, 'results', f'{a.label}.json')
    json.dump(out, open(jp, 'w'), indent=1)
    tot = next(r for r in rows if r[0] == 'TOTAL')
    # Two builds of the CPU file differ by 5-10% per section from code layout
    # alone (ledger: shadow-knob vs tb-shadow); one binary with a runtime
    # switch does not. The method is part of what a row claims.
    method = 'same-binary' if meta['base']['sha'] == meta['head']['sha'] else 'two-build'
    if BUSY_SEEN:
        method += ', busy host'
        out['busy_host'] = BUSY_SEEN
        json.dump(out, open(jp, 'w'), indent=1)
        print(f"NOTE: timed on a busy host (--busy-ok): {len(BUSY_SEEN)} other processes ran during it")
    line = (f"| {meta['date'][:16]} | {a.label} | {meta['commit']} | {meta['base']['sha']} -> {meta['head']['sha']} | "
            f"{tot[1]} -> {tot[2]} | {tot[3]:+.1f}% | {verdict} | {method} |\n")
    lp = os.path.join(WORK, 'results', 'ledger.md')
    if not os.path.exists(lp):
        open(lp, 'w').write("# Performance ledger\n\nEvery timed A/B, appended by test/tools/perf_ab.py. "
                            "Quote this, not memory.\n\n| when | label | HEAD | base -> head | total ms | delta | verdict | method |\n|---|---|---|---|---|---|---|---|\n")
    open(lp, 'a').write(line)
    print(f"\nwritten: {jp}\nledger:  {lp}")
    sys.exit(1 if fails else 0)

if __name__ == '__main__':
    main()
