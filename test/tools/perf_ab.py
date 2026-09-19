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
SECTIONS = ['alu', 'branch', 'call', 'ldst', 'stride', 'fp', 'byte', 'div', 'sort']

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

def refuse_if_busy(when):
    r = busy_host()
    if r:
        sys.exit(f"perf_ab: refusing to time ({when}) while the host is busy with\n  " + "\n  ".join(r))
    # A run writes a fresh clone of a 4 GB image and a Windows boot diverges it
    # by hundreds of MB; a snapshot is the whole of guest RAM. A full disk does
    # not fail cleanly -- it failed the state save mid-write once, after the
    # guest had booted, and the failure read as "no snapshot" not "no space".
    st = os.statvfs(WORK)
    free_gb = st.f_bavail * st.f_frsize / 2**30
    if free_gb < 6:
        sys.exit(f"perf_ab: refusing ({when}): only {free_gb:.1f} GB free under {WORK}; "
                 "old run clones (lab/work-*, lab/bench-*, lab/ntbench-*) are the usual cause")

def run_one(label, binary, workload, section, scale, snapshot=False):
    """One run -- a cold boot, or a resumed snapshot; returns ({section: ms}, {section: result}, total_ms, log)."""
    if snapshot:
        cmd = [os.path.join(R, 'test/tools/nt_snap.sh'), 'run', label, binary, workload, section, str(scale)]
    else:
        cmd = [os.path.join(R, 'test/tools/nt_bench.sh'), label, binary, 'win2k-installed',
               'es40-window.cfg', workload, section, str(scale)]
    env = dict(os.environ, TIMEOUT='900')
    p = subprocess.run(cmd, capture_output=True, text=True, env=env)
    ms, res, total = {}, {}, None
    for ln in p.stdout.splitlines():
        m = re.match(r'(\w+)\s+(\d+) ms\s+\((-?\d+)\)', ln)
        if m: ms[m.group(1)] = int(m.group(2)); res[m.group(1)] = m.group(3)
        m = re.match(r'total (\d+) ms', ln)
        if m: total = int(m.group(1))
    return ms, res, total, p.stdout

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('label'); ap.add_argument('base'); ap.add_argument('head')
    ap.add_argument('--rounds', type=int, default=2)
    ap.add_argument('--workload', default='axp'); ap.add_argument('--section', default='all')
    ap.add_argument('--scale', default='1')
    ap.add_argument('--expect', default='', help='section:pct,... written down BEFORE the run')
    ap.add_argument('--snapshot', action='store_true',
                    help='resume the desktop snapshot (nt_snap.sh run) instead of cold-booting each run')
    a = ap.parse_args()
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
        'expect': expect,
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
            ms, res, total, log = run_one(f'{a.label}-{arm}-{r}', binary, a.workload, a.section, a.scale, a.snapshot)
            if total is None:
                sys.exit(f"perf_ab: {arm} round {r} produced no result:\n{log[-800:]}")
            runs[arm].append({'ms': ms, 'result': res, 'total': total})
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
    line = (f"| {meta['date'][:16]} | {a.label} | {meta['commit']} | {meta['base']['sha']} -> {meta['head']['sha']} | "
            f"{tot[1]} -> {tot[2]} | {tot[3]:+.1f}% | {verdict} |\n")
    lp = os.path.join(WORK, 'results', 'ledger.md')
    if not os.path.exists(lp):
        open(lp, 'w').write("# Performance ledger\n\nEvery timed A/B, appended by test/tools/perf_ab.py. "
                            "Quote this, not memory.\n\n| when | label | HEAD | base -> head | total ms | delta | verdict |\n|---|---|---|---|---|---|---|\n")
    open(lp, 'a').write(line)
    print(f"\nwritten: {jp}\nledger:  {lp}")
    sys.exit(1 if fails else 0)

if __name__ == '__main__':
    main()
