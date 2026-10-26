"""The processor rows the oracle can test (cpu_oracle.py --cpu), and what
the cases need to know of each: the identity software reads (IMPLVER, the
AMASK feature mask), the architecture extensions that mask says the part
has, and what an opcode 18 function code with no instruction does.

The values are the manuals', with the same citations as the emulator's own
table of rows, src/cpu/CpuModels.cpp. The two must agree: check_source()
reads that file and CpuModel.hpp and lists every difference, and a
difference fails the run. A row the emulator gains is added here with it,
as the EV8's was (docs/platforms/ev8.md, S1).

Every row runs on the ES40, whose console the image replaces: the image's
PALcode is the 21264's (image.py), which is the core of every row (the EV6
family, the EV7, and the EV8 in its EV7-compatible mode). A row of another
family than the board's is started through the ALPHABOX_CPU_ANY_BOARD
test hook (docs/headless.md).

What a case needs of a row (Builder.need, image.py), and so when it is
skipped:
  - an extension ("BWX", "FIX", "CIX", "MVI", "PREFETCH"): its AMASK bit.
    Builder finds these itself from the words a case emits (word_needs);
  - Unused(word): `word` encodes no instruction of the row's own (Row.own),
    for a case that expects OPCDEC or a no-op from it;
  - Own(word): `word` is one of the row's own instructions, for a case of
    that instruction (the EV8's, cases.py group ev8);
  - UNUSED_MISC_NOP: the row completes an unused 18.xxxx code as a no-op;
  - PAL21264: the row has the 21264's PALcode interface (its PALshadow
    registers, I_CTL<SDE>, the AST registers), which the native EV8 does
    not: the image runs an EV8 PALcode of its own there (image.py,
    build_pal8) with the same services, but for those;
  - NATIVE8: the row has the 21464's own (group ev8n, ev8n_cases.py);
  - TARANTULA: the row has Tarantula's state and instructions (opcodes
    01-05, group tarantula, tarantula_cases.py). Not found from the words:
    the other groups' OPCDEC cases emit opcodes 01-07 on every row, and on
    a Tarantula row they still raise OPCDEC, its vector enable being 0
    outside group tarantula;
  - EV8 and NOT_EV8: the row has the EV8's additions (CPU_EXT_EV8), or
    lacks them, for the cases of the 21464's sets and for the cases that
    expect OPCDEC from the same words everywhere else (groups simd,
    paired): one reason for all of them, where Own(word) and Unused(word)
    would give one per word. Those groups check that the words are the
    row's own (Row.own) themselves.
"""
import os
import re

import alpha_asm

# AMASK bits [ARM 4th ed. p. 808]: the names are CpuModel.hpp's.
AMASK_BITS = {"BWX": 0x0001, "FIX": 0x0002, "CIX": 0x0004, "MVI": 0x0100,
              "TRAP": 0x0200, "SMT": 0x0400, "EV8SIMD": 0x0800,
              "PREFETCH": 0x1000}

UNUSED_MISC_NOP = "unused 18.xxxx codes are no-ops"
PAL21264 = "the 21264's PALcode interface"
NATIVE8 = "the 21464's PALcode interface"
TARANTULA = "Tarantula"
EV8 = "the EV8's additions"
NOT_EV8 = "a row without the EV8's additions"


class Unused:
    """A need: `word` is not an instruction of the row's own."""

    def __init__(self, word):
        self.word = word & 0xFFFFFFFF

    def __eq__(self, other):
        return isinstance(other, Unused) and other.word == self.word

    def __hash__(self):
        return hash(("Unused", self.word))


class Own:
    """A need: `word` is an instruction of the row's own."""

    def __init__(self, word):
        self.word = word & 0xFFFFFFFF

    def __eq__(self, other):
        return isinstance(other, Own) and other.word == self.word

    def __hash__(self):
        return hash(("Own", self.word))


def function_of(word):
    """The function field of an instruction word, as the ARM's Table C-1
    writes codes: the 16 bits of opcode 18 (oo.ffff), the 11 of the
    floating-point operate opcodes 14-17 and the EV8's 07 (oo.fff), the 7
    of the integer operate opcodes 10-13 and 1C (oo.ff); None for the other
    formats."""
    op = word >> 26
    if op == 0x18:
        return word & 0xFFFF
    if 0x14 <= op <= 0x17:
        return (word >> 5) & 0x7FF
    if 0x10 <= op <= 0x13 or op == 0x1C:
        return (word >> 5) & 0x7F
    if op == 0x07:                # the EV8's paired single, FP operate
        return (word >> 5) & 0x7FF
    return None


class Row:
    def __init__(self, name, part, family, implver, amask, source,
                 unused_misc_nop=True, ext=(), own=None):
        self.name = name
        self.part = part
        self.family = family          # CpuModel.hpp's cpu_family, without CPU_FAMILY_
        self.implver = implver
        self.amask = amask
        self.source = source
        # cpu_model::ext: the CPU_EXT_* flags, without CPU_EXT_.
        self.ext = frozenset(ext)
        # cpu_family_unused_misc_is_nop: the 21264 completes an 18.xxxx code
        # with no instruction as a no-op [21464 IDS p. 676]; the ARM allows
        # that or a trap.
        self.unused_misc_nop = unused_misc_nop
        # The instructions the row has beyond the 21264's, as
        # {(opcode, function): mnemonic}: the function is function_of()'s
        # (16 bits for opcode 18, 11 for opcodes 14-17, 7 for the integer
        # operates), or None for all of an opcode. Empty on the EV6 family
        # and the EV7; the EV8's are in docs/platforms/ev8-isa.md.
        self.own = own or {}

    def has(self, ext):
        return bool(self.amask & AMASK_BITS[ext])

    def own_instruction(self, word):
        op = word >> 26
        for key in ((op, None), (op, function_of(word))):
            if key in self.own:
                return self.own[key]
        return None

    def lacks(self, need):
        """Why a case with this need cannot run on the row, or None."""
        if isinstance(need, Unused):
            m = self.own_instruction(need.word)
            if m:
                return "%08x is an instruction of this row (%s)" % (need.word, m)
            return None
        if isinstance(need, Own):
            if self.own_instruction(need.word):
                return None
            return "%08x is no instruction of this row" % need.word
        if need == PAL21264:
            return None if self.family != "EV8" else (
                "the native EV8 has the 21464's PALcode interface, not the "
                "21264's (PALshadow R4-R7/R20-R23, SDE, ASTs)")
        if need == NATIVE8:
            return None if self.family == "EV8" else (
                "the 21464's own PALcode interface is the native EV8's")
        if need == TARANTULA:
            return None if "TARANTULA" in self.ext else (
                "no Tarantula (CPU_EXT_TARANTULA)")
        if need == EV8:
            return None if "EV8" in self.ext else (
                "no EV8 additions (CPU_EXT_EV8)")
        if need == NOT_EV8:
            return None if "EV8" not in self.ext else (
                "the row has the EV8's additions (CPU_EXT_EV8): these words "
                "are its instructions")
        if need == UNUSED_MISC_NOP:
            return None if self.unused_misc_nop else (
                "unused 18.xxxx codes are not no-ops on this row")
        if not self.has(need):
            return "no %s (AMASK bit %d clear)" % (
                need, AMASK_BITS[need].bit_length() - 1)
        return None

    def describe(self):
        own = sorted(set(self.own.values()))
        return "%s: %s, IMPLVER %d, AMASK %#x, unused 18.xxxx %s%s" % (
            self.name, self.part, self.implver, self.amask,
            "no-ops" if self.unused_misc_nop else "trap",
            ", and " + " ".join(own) if own else "")


# The EV8's own instructions, those the `ev8` and `ev8n` rows execute: S2's
# and S3's (ev8-isa.md 3.1, 3.2), S5's short-vector SIMD set under 1C
# (3.3, alpha_asm.SIMD1C; simd_cases.py) and S6's paired single under 07
# (3.4, alpha_asm.PAIRED; paired_cases.py).
_EV8_OWN = {(0x18, 0xEC00): "CCB", (0x11, 0x68): "CMOV2",
            (0x17, 0x068): "FCMOV2", (0x18, 0xB000): "LDL_ARM",
            (0x18, 0xB400): "LDQ_ARM", (0x18, 0xB800): "QUIESCE"}
_EV8_OWN.update({(0x1C, f): n for n, f in alpha_asm.SIMD1C.items()})
_EV8_OWN.update({(0x07, f): n for f, n in alpha_asm.paired_codes().items()})

_EV68 = AMASK_BITS["BWX"] | AMASK_BITS["FIX"] | AMASK_BITS["CIX"] | \
    AMASK_BITS["MVI"] | AMASK_BITS["TRAP"] | AMASK_BITS["PREFETCH"]

ROWS = {r.name: r for r in (
    Row("ev68cb", "21264CB (EV68CB)", "EV6", 2, _EV68,
        "21264CB HRM 2.15: AMASK feature mask 0x1307, IMPLVER 2"),
    Row("ev7", "21364 (EV7)", "EV7", 3, _EV68,
        "IMPLVER 3: ARM 4th ed., Table D-4 (21364/EV7); AMASK: the EV68 "
        "core's, an inference (src/cpu/CpuModels.cpp)"),
    Row("ev7z", "21364 (EV7z)", "EV7", 3, _EV68,
        "the EV7's core and values (src/cpu/CpuModels.cpp)"),
    # The 21464 in EV7-compatible mode (docs/platforms/ev8.md, D1): the
    # EV7's core and PALcode interface, the EV8's identity and extensions.
    # Of the IDS's AMASK 0x1F07 only what the row implements: bit 10 with
    # LDx_ARM and QUIESCE (S3), bit 11 with the 1C SIMD set and paired
    # single (S5 and S6), so 0x1F07; bit 3 (Tarantula) with S7 and S8. Its own instructions are those it executes so far
    # (S2, S3, S5: _EV8_OWN); Tarantula's (S7) are found by the need
    # TARANTULA instead.
    Row("ev8", "21464 (EV8)", "EV7", 4,
        _EV68 | AMASK_BITS["SMT"] | AMASK_BITS["EV8SIMD"],
        "IMPLVER 4: 21464 IDS Rev 1.1k, PDF p. 76; AMASK: of the IDS's "
        "0x1F07 (p. 76), the bits whose instructions the row has",
        ext={"EV8", "TARANTULA"}, own=_EV8_OWN),
    # The 21464 on its own privileged interface (docs/platforms/ev8-native.md,
    # S11, N16): a family of its own, the `ev8` row's user instructions and
    # AMASK. The image runs an EV8 PALcode of its own on it (image.py,
    # build_pal8), and the cases of the 21264's PALcode interface are
    # skipped (PAL21264); group ev8n has the 21464's.
    Row("ev8n", "21464 (EV8), native interface", "EV8", 4,
        _EV68 | AMASK_BITS["SMT"] | AMASK_BITS["EV8SIMD"],
        "IMPLVER 4: 21464 IDS Rev 1.1k, PDF p. 76; AMASK: the ev8 row's "
        "(ev8-native.md, N16)",
        ext={"EV8", "TARANTULA"}, own=_EV8_OWN),
)}

# The ES40's processor (src/platforms/Platforms.cpp): a row of another
# family needs the test hook to run on its board.
BOARD_ROW = "ev68cb"


def word_needs(word):
    """The extension an instruction word belongs to [ARM 4th ed., Appendix
    C and 4.2, 4.6.5, 4.10, 4.13, 4.4.7-9, 4.11], or None."""
    op = word >> 26
    if op in (0x0A, 0x0C, 0x0D, 0x0E):        # LDBU LDWU STW STB
        return "BWX"
    if op == 0x14:                            # ITOFx, SQRTx
        return "FIX"
    if op == 0x1C:
        fn = (word >> 5) & 0x7F
        if fn in (0x00, 0x01):                # SEXTB SEXTW
            return "BWX"
        if fn in (0x30, 0x32, 0x33):          # CTPOP CTLZ CTTZ
            return "CIX"
        if fn == 0x31 or 0x34 <= fn <= 0x3F:  # PERR, UNPK/PK, MIN/MAX
            return "MVI"
        if fn in (0x70, 0x78):                # FTOIT FTOIS
            return "FIX"
    if op == 0x22 and (word >> 21) & 31 == 31:  # LDS F31: prefetch, modify intent
        return "PREFETCH"
    return None


# --- the emulator's table -------------------------------------------------------

_SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..",
                    "src", "cpu")


def _strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)


def _split_top(s):
    """Split on commas outside string literals."""
    out, cur, q = [], "", False
    for ch in s:
        if ch == '"':
            q = not q
        if ch == "," and not q:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def read_source(src=_SRC):
    """The emulator's rows as {name: {field: value}} and the families
    whose unused 18.xxxx codes are no-ops, from CpuModels.cpp and
    CpuModel.hpp. Raises ValueError when they cannot be read."""
    hpp = _strip_comments(open(os.path.join(src, "CpuModel.hpp")).read())
    cpp = _strip_comments(open(os.path.join(src, "CpuModels.cpp")).read())
    bits = {m.group(1): int(m.group(2), 16) for m in re.finditer(
        r"#define\s+AMASK_(\w+)\s+U64\((0x[0-9a-fA-F]+)\)", hpp)}
    st = re.search(r"struct\s+cpu_model\s*{(.*?)};", hpp, re.S)
    if not st:
        raise ValueError("no struct cpu_model in CpuModel.hpp")
    fields = []
    for decl in st.group(1).split(";"):
        decl = decl.strip()
        if not decl:
            continue
        m = re.match(r"(?:const\s+)?\w+\s*\*?\s*(.*)$", decl, re.S)
        fields += [n.strip().lstrip("*").strip() for n in m.group(1).split(",")]
    rule = re.search(r"cpu_family_unused_misc_is_nop\s*\(\s*cpu_family\s+(\w+)\s*\)"
                     r"\s*{\s*return\s+([^;]*);\s*}", hpp)
    if not rule:
        raise ValueError("cannot read cpu_family_unused_misc_is_nop")
    var, expr = rule.group(1), rule.group(2).strip()
    term = r"%s\s*==\s*CPU_FAMILY_(\w+)" % var
    if not re.fullmatch(r"%s(\s*\|\|\s*%s)*" % (term, term), expr):
        raise ValueError("cpu_family_unused_misc_is_nop is not a list of "
                         "families; teach rows.read_source its new form")
    nop_families = set(re.findall(term, expr))
    tab = re.search(r"cpu_model\s+models\[\]\s*=\s*{(.*?)};", cpp, re.S)
    if not tab:
        raise ValueError("no models[] table in CpuModels.cpp")
    out = {}
    for ent in re.findall(r"{([^{}]*)}", tab.group(1)):
        vals = _split_top(ent)
        if len(vals) != len(fields):
            raise ValueError("a row of %d values for %d fields: %s" % (
                len(vals), len(fields), ent.strip()))
        row = dict(zip(fields, vals))
        row["name"] = row["name"].strip('"')
        row["part"] = row["part"].strip('"')
        row["implver"] = int(row["implver"], 0)
        mask = 0
        for t in row["amask"].split("|"):
            t = t.strip()
            mask |= bits[t[len("AMASK_"):]] if t.startswith("AMASK_") else int(t, 0)
        row["amask"] = mask
        row["family"] = row["family"].replace("CPU_FAMILY_", "")
        row["ext"] = frozenset(
            t.strip()[len("CPU_EXT_"):] for t in row["ext"].split("|")
            if t.strip() not in ("CPU_EXT_NONE", "0"))
        out[row["name"]] = row
    return out, nop_families, bits


def check_source(src=_SRC):
    """Lines that say where this table and the emulator's disagree (empty
    when they agree), or None when the emulator's source is not here."""
    if not os.path.exists(os.path.join(src, "CpuModels.cpp")):
        return None
    try:
        theirs, nop_families, bits = read_source(src)
    except (ValueError, KeyError, OSError) as e:
        return ["src/cpu/CpuModels.cpp cannot be read: %s" % e]
    diffs = []
    for name, value in sorted(AMASK_BITS.items()):
        if bits.get(name) != value:
            diffs.append("AMASK_%s: %s here, %s in CpuModel.hpp" % (
                name, hex(value), hex(bits[name]) if name in bits else "none"))
    for name in sorted(set(ROWS) | set(theirs)):
        if name not in theirs:
            diffs.append("%s: a row here, not in CpuModels.cpp" % name)
            continue
        if name not in ROWS:
            diffs.append("%s: a row of CpuModels.cpp with no model here" % name)
            continue
        mine, t = ROWS[name], theirs[name]
        for fld in ("part", "family", "implver", "amask", "ext"):
            a, b = getattr(mine, fld), t[fld]
            if a != b:
                if fld == "amask":
                    a, b = hex(a), hex(b)
                elif fld == "ext":
                    a, b = (" | ".join(sorted(x)) or "none" for x in (a, b))
                diffs.append("%s: %s %s here, %s in CpuModels.cpp" % (
                    name, fld, a, b))
        nop = t["family"] in nop_families
        if mine.unused_misc_nop != nop:
            diffs.append("%s: unused 18.xxxx codes %s here, %s by "
                         "cpu_family_unused_misc_is_nop" % (
                             name, "no-ops" if mine.unused_misc_nop else "trap",
                             "no-ops" if nop else "trap"))
    return diffs
