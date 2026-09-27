#!/usr/bin/env python3
"""Map a host sampling profile of compiled code back to guest blocks.

usage: jit_profile.py <sample-output> <jit_disasm_cpu0.txt> <t_end_ms> [top_n] [annotate_n]

Inputs come from jit_profile.sh: macOS `sample` output for the emulator, and
the JIT_DISASM lane's listing, whose block headers carry the host address,
size and compile time ("host <hex> at <ms>") and whose lines carry each
instruction's bytes. A sampled address belongs to the block covering it that
was compiled last before the sampling ended: a reclaim frees code, and later
blocks can land at the same address. Prints the CPU thread's split between
compiled code and the rest, the hottest blocks, and the hottest few annotated
per host instruction with the guest instruction each belongs to. Sampling has
skid: a stall shows up on the instruction after the one that caused it.
"""
import bisect, collections, os, re, sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'lab'))
try:
    import alphadis
except ImportError:
    alphadis = None

def cpu_thread_leaves(sample_txt):
    leaf, other = collections.Counter(), collections.Counter()
    for t in re.split(r'\n(?=\s{4}\d+ Thread_)', sample_txt):
        if 'jit_run' not in t:
            continue
        nodes = []
        for l in t.split('\n'):
            m = re.match(r'^(\s+[+!:| ]*)(\d+) (.+?)\s+\[0x([0-9a-f]+)\]', l)
            if m:
                nodes.append((len(m.group(1)), int(m.group(2)), m.group(3), int(m.group(4), 16)))
        for k, (ind, c, name, addr) in enumerate(nodes):
            if (nodes[k + 1][0] if k + 1 < len(nodes) else -1) <= ind:
                if '???' in name:
                    leaf[addr] += c
                else:
                    other[name.split('  (in')[0].strip()[:70]] += c
        break  # the first thread running jit_run: CPU 0
    return leaf, other

def main():
    samp, dis, t_end = sys.argv[1], sys.argv[2], int(sys.argv[3])
    top_n = int(sys.argv[4]) if len(sys.argv) > 4 else 15
    ann_n = int(sys.argv[5]) if len(sys.argv) > 5 else 2
    leaf, other = cpu_thread_leaves(open(samp).read())
    hdr = re.compile(rb'^\[JIT\]\[CPU0\] block @ ([0-9a-f]+)( PAL)?  \((\d+) instr, (\d+) bytes\) host ([0-9a-f]+) at (\d+)')
    blocks = []
    with open(dis, 'rb') as f:
        off = 0
        for l in f:
            if l.startswith(b'[JIT][CPU0] block @'):
                m = hdr.match(l)
                if m and int(m.group(6)) <= t_end:
                    blocks.append(dict(host=int(m.group(5), 16), size=int(m.group(4)), off=off,
                                       g=int(m.group(1), 16), n=int(m.group(3)), t=int(m.group(6)),
                                       pal=bool(m.group(2))))
            off += len(l)
    blocks.sort(key=lambda b: b['host'])
    starts = [b['host'] for b in blocks]
    per, peroff, unk = collections.Counter(), collections.defaultdict(collections.Counter), 0
    for a, c in leaf.items():
        j, best = bisect.bisect_right(starts, a) - 1, None
        while j >= 0 and blocks[j]['host'] > a - 65536:
            b = blocks[j]
            if b['host'] <= a < b['host'] + b['size'] and (best is None or b['t'] > best['t']):
                best = b
            j -= 1
        if best is None:
            unk += c
            continue
        k = (best['host'], best['t'])
        per[k] += c
        peroff[k][a - best['host']] += c
    jit, rest = sum(leaf.values()), sum(other.values())
    print(f"CPU thread: {jit + rest} samples, compiled code {jit} ({100 * jit / max(1, jit + rest):.0f}%), "
          f"unmapped {unk}")
    for n, c in other.most_common(5):
        print(f"   {c:5}  {n}")
    bk = {(b['host'], b['t']): b for b in blocks}
    acc = 0
    print("hottest blocks (share of compiled-code samples):")
    for k, c in per.most_common(top_n):
        b = bk[k]
        acc += c
        print(f"  {c:5} {100 * c / max(1, jit):5.1f}% (cum {100 * acc / max(1, jit):5.1f}%)  "
              f"guest {b['g']:x}{' PAL' if b['pal'] else ''}  {b['n']} instr, {b['size']} B")
    f = open(dis, 'rb')
    for k, c in per.most_common(ann_n):
        b = bk[k]
        f.seek(b['off'])
        print(f"\n== guest {b['g']:x}: {c} samples")
        f.readline()
        pc, gi, prof = 0, 0, peroff[k]
        for raw in f:
            l = raw.decode(errors='replace').rstrip()
            if l.startswith('[JIT][CPU0] block @') or l == '':
                break
            if l.startswith('alpha '):
                w = int(l.split()[1], 16)
                d = alphadis.dis(b['g'] + 4 * gi, w) if alphadis else f'{w:08x}'
                print(f"      --- {b['g'] + 4 * gi:x}: {d}")
                gi += 1
                continue
            m = re.match(r'^(.*?)\s*;\s*([0-9A-F]+)\s*$', l)
            if not m:
                if re.match(r'^L\d+:', l):
                    print('      ' + l)
                continue
            n = len(m.group(2)) // 2
            s = sum(prof.get(pc + o, 0) for o in range(0, n, 4))
            if s or pc >= 0x60:
                print(f"  {s:5} {pc:5x}  {m.group(1).strip()}")
            pc += n

if __name__ == '__main__':
    main()
