#!/usr/bin/env python3
"""Configuration corpus test: the parser and the configuration rules.

usage: cfg_corpus.py [--update] [--only SUBSTRING] <alphabox-binary>

Every configuration in the repository (es40.cfg, test/**/*.cfg), the
hand-written cases in test/config/valid/ and the test machines srm_cfg.py
generates for each board are parsed with `alphabox run --dump-config`,
which applies every rule but starts nothing. The resolved tree it prints,
and its exit status, must match test/config/golden/<name>.dump. A change
to a golden file is a change of behaviour: make it on purpose, with
--update, and say why in the commit.

Then each file in test/config/malformed/, and the cases this script
generates (deep nesting, long names and values, numbers out of range),
must be refused with exit status 1 and an error that names the place
(file:line:col) and says what is wrong (malformed/expect.txt). A crash, a
hang, or an accepted file is a failure.

The exit statuses in the golden files are those of a build with SDL
(`alphabox --version` lists SDL3): without it, the configurations with a
GUI are refused, and only the trees are compared.

Generated files go to $ALPHABOX_WORK/cfg-corpus (default <repo>/lab).
Prints PASS or FAIL per case and a summary; exit status 0 when all pass.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
CFGDIR = os.path.join(REPO, "test", "config")
GOLDEN = os.path.join(CFGDIR, "golden")
MALFORMED = os.path.join(CFGDIR, "malformed")
TIMEOUT = 20

# The srm_cfg.py test machines: (name, arguments).
SRM_CFG = [
    ("srm-es40", []),
    ("srm-es40-cpus4", ["--cpus", "4"]),
    ("srm-es40-membits30", ["--membits", "30"]),
    ("srm-es40-cpuopt", ["--cpus", "2", "--cpu-opt", "palcode.vms.nohle=true",
                         "--cpu1-opt", "speed=500M"]),
    ("srm-es40-gui", ["--gui"]),
    ("srm-es40-exit-on-halt", ["--exit-on-halt"]),
    ("srm-es40-rom", ["--rom", "OTHER.EXE"]),
    ("srm-es40-floppy", ["--floppy", "floppy.img"]),
    ("srm-es40-ide", ["--ide-cfg", "@ide"]),
    ("srm-es40-extra", ["--extra-cfg", "@extra"]),
    ("srm-es40-nic-udp", ["--nic", "de600", "--nic-udp", "5555:5556"]),
] + [("srm-es40-scsi-" + c, ["--scsi", c]) for c in (
    "sym53c810", "sym53c825", "sym53c875", "sym53c895", "sym53c896",
    "isp1020", "isp1040", "isp1080", "isp1240")] + [
    ("srm-es40-nic-" + c, ["--nic", c]) for c in (
        "dec21040", "dec21041", "dec21140", "dec21143", "de600", "i82557",
        "i82558", "i82559")] + [
    ("srm-" + p, ["--platform", p]) for p in (
        "ds20e", "ds10", "ds20l", "es45", "es45m1", "ds25", "ds15", "es47",
        "es80", "gs1280")] + [
    ("srm-ds20e-devices", ["--platform", "ds20e", "--cpus", "2", "--scsi",
                           "sym53c875", "--nic", "dec21143"]),
    ("srm-ds10-devices", ["--platform", "ds10", "--scsi", "isp1040",
                          "--nic", "i82559"]),
]

# Inputs for --ide-cfg and --extra-cfg ("@name" in SRM_CFG).
SRM_FILES = {
    "ide": ("    disk0.0 = file\n    {\n      file = \"ide.img\";\n    }\n"
            "    disk1.0 = file\n    {\n      file = \"cd.iso\";\n"
            "      cdrom = true;\n    }\n"),
    "extra": ("  pci0.2 = dec21152\n  {\n    pci.0 = de600 { type = \"null\"; }\n"
              "    pci.1 = sym53c875 { disk0.0 = ramdisk { size = 1M; } }\n  }\n"
              "  pci1.1 = de602 { }\n"),
}


def generated_malformed():
    """Malformed cases too large or too odd to keep as files:
    (name, bytes, line, col, expected text[, "run"]). "run": a value only
    the device reads, so `alphabox run` must refuse it, not --check."""
    machine = b'sys0 = tsunami\n{\n  rom.srm = "x";\n  cpu0 = ev68cb\n  {\n  }\n'
    n = 100000
    return [
        # The 17th section: 16 "a = b {" before it, 7 bytes each.
        ("gen-deep-nesting.cfg", b"a = b {" * n + b"}" * n + b"\n", 1, 113,
         "nested more than 16 deep"),
        ("gen-long-name.cfg", machine + b"  " + b"x" * 9000 + b" = 1;\n}\n",
         7, 3, "longer than"),
        ("gen-long-class.cfg", machine + b"  pci0.1 = " + b"y" * 9000 +
         b"\n  {\n  }\n}\n", 7, 12, "longer than"),
        ("gen-nul-byte.cfg", machine + b"  rom.dpr = \"a\x00b\";\n}\n", 7, 15,
         "Illegal character NUL"),
        ("gen-nul-outside.cfg", machine + b"  \x00\n}\n", 7, 3,
         "Illegal character 0x00"),
        ("gen-high-byte.cfg", machine + b"  caf\xc3\xa9 = 1;\n}\n", 7, 6,
         "Illegal character 0xc3"),
        # 9000 characters into an error message (FAILURE's 8000-byte buffer).
        ("gen-long-value.cfg", b'sys0 = tsunami\n{\n  memory.bits = "' +
         b"x" * 9000 + b'";\n  cpu0 = ev68cb\n  {\n  }\n}\n', 1, 1,
         "Illegal numeric value", "run"),
    ]


def features(binary):
    out = subprocess.run([binary, "--version"], capture_output=True,
                         text=True).stdout
    return out


def run_check(binary, path, dump=True):
    """(exit status or 'signal N'/'timeout', stdout, stderr)."""
    args = [binary, "run", "--dump-config" if dump else "--check", path]
    try:
        p = subprocess.run(args, capture_output=True, timeout=TIMEOUT,
                           cwd=os.path.dirname(path))
    except subprocess.TimeoutExpired:
        return "timeout", "", ""
    rc = p.returncode
    status = "exit %d" % rc if rc >= 0 else "signal %d" % -rc
    err = p.stderr.decode("latin-1")
    # A build with -fsanitize=address,undefined: any report is a failure.
    if "ERROR: AddressSanitizer" in err or "runtime error:" in err:
        status = "sanitizer report"
    return (status, p.stdout.decode("latin-1"), err)


def run_emulator(binary, path, work):
    """`alphabox run` on a configuration that must stop before the machine
    starts: (status, stdout, stderr), in a directory of its own."""
    d = os.path.join(work, "run-" + os.path.basename(path))
    os.makedirs(d, exist_ok=True)
    try:
        p = subprocess.run([binary, "run", path], capture_output=True,
                           timeout=TIMEOUT, cwd=d, stdin=subprocess.DEVNULL)
    except subprocess.TimeoutExpired:
        return "timeout", "", ""
    rc = p.returncode
    status = "exit %d" % rc if rc >= 0 else "signal %d" % -rc
    err = p.stderr.decode("latin-1")
    if "ERROR: AddressSanitizer" in err or "runtime error:" in err:
        status = "sanitizer report"
    return status, p.stdout.decode("latin-1"), err


def corpus(work):
    """[(name, path)] of the configurations that must parse as before."""
    files = []
    for top, dirs, names in os.walk(REPO):
        dirs[:] = [d for d in dirs if not d.startswith(".") and
                   not d.startswith("build") and d not in
                   ("third_party", "lab", "roms", "malformed")]
        for nm in sorted(names):
            if nm.endswith(".cfg"):
                p = os.path.join(top, nm)
                rel = os.path.relpath(p, REPO)
                files.append((rel.replace(os.sep, "-")[:-4], p))
    gen = os.path.join(work, "srm")
    os.makedirs(gen, exist_ok=True)
    for key, text in SRM_FILES.items():
        with open(os.path.join(gen, key + ".inc"), "w") as f:
            f.write(text)
    for name, args in SRM_CFG:
        args = [os.path.join(gen, a[1:] + ".inc") if a.startswith("@") else a
                for a in args]
        out = os.path.join(gen, name + ".cfg")
        subprocess.run([sys.executable, os.path.join(HERE, "srm_cfg.py"),
                        "--out", out, "--dir", gen] + args, check=True)
        files.append((name, out))
    return sorted(files)


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("binary")
    ap.add_argument("--update", action="store_true",
                    help="rewrite the golden files from this binary")
    ap.add_argument("--only", help="run only the cases whose name has this")
    args = ap.parse_args()
    binary = os.path.abspath(args.binary)
    work = os.path.join(os.environ.get("ALPHABOX_WORK",
                                       os.path.join(REPO, "lab")), "cfg-corpus")
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    feats = features(binary)
    with_sdl = "SDL3" in feats
    print("binary: %s" % binary)
    print("  " + feats.strip().replace("\n", "\n  "))
    if not with_sdl:
        print("  (no SDL: exit statuses are not compared)")

    fails = 0
    count = 0
    os.makedirs(GOLDEN, exist_ok=True)
    for name, path in corpus(work):
        if args.only and args.only not in name:
            continue
        count += 1
        status, out, err = run_check(binary, path)
        got = "# %s\n%s" % (status, out)
        gfile = os.path.join(GOLDEN, name + ".dump")
        if args.update:
            if status.startswith("signal") or status == "timeout":
                print("FAIL %s: %s, not recorded" % (name, status))
                fails += 1
                continue
            with open(gfile, "w") as f:
                f.write(got)
            print("UPDATED %s (%s)" % (name, status))
            continue
        try:
            want = open(gfile).read()
        except FileNotFoundError:
            print("FAIL %s: no golden file (run with --update)" % name)
            fails += 1
            continue
        if not with_sdl:
            got = re.sub(r"\A# exit \d+", "# exit ?", got)
            want = re.sub(r"\A# exit \d+", "# exit ?", want)
        if got == want:
            print("PASS %s" % name)
        else:
            fails += 1
            print("FAIL %s: the dump differs from %s" %
                  (name, os.path.relpath(gfile, REPO)))
            g, w = got.splitlines(), want.splitlines()
            shown = 0
            for i in range(max(len(g), len(w))):
                a = w[i] if i < len(w) else "<end>"
                b = g[i] if i < len(g) else "<end>"
                if a != b and shown < 6:
                    print("    line %d: want %r" % (i + 1, a))
                    print("    line %d: got  %r" % (i + 1, b))
                    shown += 1
            for line in err.splitlines()[:5]:
                print("    stderr: " + line)

    # Malformed input: refused cleanly, with the place.
    cases = []
    expect = os.path.join(MALFORMED, "expect.txt")
    if os.path.exists(expect):
        for line in open(expect):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            m = re.match(r"(?:(run)\s+)?(\S+)\s+(\d+):(\d+)\s+(.*)$", line)
            if not m:
                sys.exit("cfg_corpus: bad line in expect.txt: " + line)
            cases.append((m.group(2), os.path.join(MALFORMED, m.group(2)),
                          int(m.group(3)), int(m.group(4)), m.group(5),
                          m.group(1) or "check"))
        gdir = os.path.join(work, "malformed")
        os.makedirs(gdir)
        for case in generated_malformed():
            name, data = case[0], case[1]
            p = os.path.join(gdir, name)
            with open(p, "wb") as f:
                f.write(data)
            cases.append((name, p) + case[2:] + (("check",) if len(case) < 6
                                                  else ()))
    for name, path, line, col, text, mode in cases:
        if args.only and args.only not in name:
            continue
        count += 1
        where = "%s:%d:" % (os.path.basename(path), line)
        if col:
            where += "%d:" % col
        if mode == "run":
            # What only a device reads: the emulator itself must refuse it.
            status, out, err = run_emulator(binary, path, work)
            errs = [l for l in out.splitlines()
                    if l.startswith("Emulator Failure")]
        else:
            status, out, err = run_check(binary, path, dump=False)
            errs = [l for l in err.splitlines() if l.startswith("%SYS-E")]
        ok = (status == "exit 1" and errs and where in errs[0] and
              text.lower() in errs[0].lower())
        if ok:
            print("PASS %s: %s" % (name, errs[0][:150]))
        else:
            fails += 1
            print("FAIL %s: %s; want an error at %s saying \"%s\"" %
                  (name, status, where, text))
            for l in (errs or err.splitlines())[:3]:
                print("    stderr: " + l[:200])

    print("%s: %d cases, %d failed" % ("PASS" if not fails else "FAIL", count,
                                       fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
