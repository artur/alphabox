#!/usr/bin/env python3
"""List every internal-register access in the PALcode images of a console.

  ev7_pal_iprs.py <image> [--base HEX ...] [--detail] [--whole] [--csv FILE]
  ev7_pal_iprs.py <image> --dis START END

<image> is a console image loaded at address 0: the decompressed cache
alphabox writes (rom.decompressed: a 16-byte header, the image and the
'ALPHDC01' tag; an older cache without the tag also works), or a raw
image. The PALcode images in it are found by their header at 32 KB
boundaries (a version quadword at +8 whose third byte is the PALcode type:
1 OpenVMS, 2 Tru64 UNIX; the build at +0x21), or named with --base.

For each image it follows the branches from every PALcode entry point
(reset, the exception vectors 0x100-0x780, the CALL_PAL slots at 0x2000
and 0x3000) and lists:

  - every HW_MFPR and HW_MTPR: the IPR index and its 21264 name, the
    address, the entry points that reach it, and the bits the code around
    it depends on: for a read, the fields the next instructions isolate
    (shifts, masks, bit tests) or write back; for a write, the bits of the
    value known from the code before it;
  - every HW_LD and HW_ST with its type (physical, locked, virtual...),
    and for physical ones the address where the code fixes it: the EV7
    register (PA<34:22> all ones: the 4 MB register window) or I/O
    (PA<43>). Known bits are propagated over the control flow, so a
    register base built before a branch is still seen after it;
  - the HW_RET forms (hint, stall bit, displacement);
  - IMPLVER and AMASK, with the bits tested after them. --whole also scans
    the rest of the image (the console's own code) for these two.

Code that no branch reaches but that decodes cleanly around an access (a
dispatch table entered by hw_ret, say) is counted as reached 'indirect'.
What is left is counted and, with --detail, listed apart.

Run from the source tree, it reads what the emulator implements: the IPR
indexes of cpu/cpu_pal.hpp (core: impl, ign for a write accepted and
dropped, warn for the unknown-IPR path) and the register table of
chipsets/marvel/Ev7Csr.cpp (a register missing from it is marked NOT
MODELLED). The IPR numbering is the 21264's (EV6/EV67/EV68 Hardware
Reference Manual, chapter 5). Uses only the standard library.
"""
import argparse
import os
import re
import struct
import sys

M64 = (1 << 64) - 1

REG = ['v0', 't0', 't1', 't2', 't3', 't4', 't5', 't6', 't7', 's0', 's1', 's2',
       's3', 's4', 's5', 'fp', 'a0', 'a1', 'a2', 'a3', 'a4', 'a5', 't8', 't9',
       't10', 't11', 'ra', 'pv', 'at', 'gp', 'sp', 'zero']

# 21264 IPRs: index -> (name, access). PCTX is 0x40-0x7f (bits select the
# fields written); the IER_CM group is 0x08-0x0b. 0x2d is not in the HRM's
# table; the original es40 code calls it M_FIX, and the ES40's own PALcode
# writes it at reset as the EV7's does.
IPRS = {
    0x2d: ('M_FIX', 'W'),
    0x00: ('ITB_TAG', 'W'), 0x01: ('ITB_PTE', 'W'), 0x02: ('ITB_IAP', 'W'),
    0x03: ('ITB_IA', 'W'), 0x04: ('ITB_IS', 'W'), 0x05: ('PMPC', 'R'),
    0x06: ('EXC_ADDR', 'R'), 0x07: ('IVA_FORM', 'R'),
    0x08: ('IER_CM(none)', 'RW'), 0x09: ('CM', 'RW'), 0x0a: ('IER', 'RW'),
    0x0b: ('IER_CM', 'RW'), 0x0c: ('SIRR', 'RW'), 0x0d: ('ISUM', 'R'),
    0x0e: ('HW_INT_CLR', 'W'), 0x0f: ('EXC_SUM', 'R'),
    0x10: ('PAL_BASE', 'RW'), 0x11: ('I_CTL', 'RW'),
    0x12: ('IC_FLUSH_ASM', 'W'), 0x13: ('IC_FLUSH', 'W'),
    0x14: ('PCTR_CTL', 'RW'), 0x15: ('CLR_MAP', 'W'), 0x16: ('I_STAT', 'RW'),
    0x17: ('SLEEP', 'W'),
    0x20: ('DTB_TAG0', 'W'), 0x21: ('DTB_PTE0', 'W'), 0x24: ('DTB_IS0', 'W'),
    0x25: ('DTB_ASN0', 'W'), 0x26: ('DTB_ALTMODE', 'W'),
    0x27: ('MM_STAT', 'R'), 0x28: ('M_CTL', 'W'), 0x29: ('DC_CTL', 'W'),
    0x2a: ('DC_STAT', 'RW'), 0x2b: ('C_DATA', 'RW'), 0x2c: ('C_SHFT', 'W'),
    0xa0: ('DTB_TAG1', 'W'), 0xa1: ('DTB_PTE1', 'W'), 0xa2: ('DTB_IAP', 'W'),
    0xa3: ('DTB_IA', 'W'), 0xa4: ('DTB_IS1', 'W'), 0xa5: ('DTB_ASN1', 'W'),
    0xc0: ('CC', 'RW'), 0xc1: ('CC_CTL', 'W'), 0xc2: ('VA', 'R'),
    0xc3: ('VA_FORM', 'R'), 0xc4: ('VA_CTL', 'W'),
}


def ipr_name(i):
    if (i & 0xc0) == 0x40:
        f = [n for b, n in ((0, 'ASN'), (1, 'ASTER'), (2, 'ASTRR'),
                            (3, 'PPCE'), (4, 'FPE')) if i & (1 << b)]
        return 'PCTX[%s]' % ','.join(f) if f else 'PCTX'
    return IPRS.get(i, ('?', ''))[0]


EXC_ENTRIES = {0x000: 'RESET(console)', 0x100: 'DTBM_DOUBLE_3',
               0x180: 'DTBM_DOUBLE_4', 0x200: 'FEN', 0x280: 'UNALIGN',
               0x300: 'DTBM_SINGLE', 0x380: 'DFAULT', 0x400: 'OPCDEC',
               0x480: 'IACV', 0x500: 'MCHK', 0x580: 'ITB_MISS',
               0x600: 'ARITH', 0x680: 'INTERRUPT', 0x700: 'MT_FPCR',
               0x780: 'RESET/WAKEUP'}

VMS_CALLS = {
    0x00: 'HALT', 0x01: 'CFLUSH', 0x02: 'DRAINA', 0x03: 'LDQP', 0x04: 'STQP',
    0x05: 'SWPCTX', 0x06: 'MFPR_ASN', 0x07: 'MTPR_ASTEN', 0x08: 'MTPR_ASTSR',
    0x09: 'CSERVE', 0x0a: 'SWPPAL', 0x0b: 'MFPR_FEN', 0x0c: 'MTPR_FEN',
    0x0d: 'MTPR_IPIR', 0x0e: 'MFPR_IPL', 0x0f: 'MTPR_IPL', 0x10: 'MFPR_MCES',
    0x11: 'MTPR_MCES', 0x12: 'MFPR_PCBB', 0x13: 'MFPR_PRBR',
    0x14: 'MTPR_PRBR', 0x15: 'MFPR_PTBR', 0x16: 'MFPR_SCBB',
    0x17: 'MTPR_SCBB', 0x18: 'MTPR_SIRR', 0x19: 'MFPR_SISR',
    0x1a: 'MFPR_TBCHK', 0x1b: 'MTPR_TBIA', 0x1c: 'MTPR_TBIAP',
    0x1d: 'MTPR_TBIS', 0x1e: 'MFPR_ESP', 0x1f: 'MTPR_ESP', 0x20: 'MFPR_SSP',
    0x21: 'MTPR_SSP', 0x22: 'MFPR_USP', 0x23: 'MTPR_USP', 0x24: 'MTPR_TBISD',
    0x25: 'MTPR_TBISI', 0x26: 'MFPR_ASTEN', 0x27: 'MFPR_ASTSR',
    0x29: 'MFPR_VPTB', 0x2a: 'MTPR_VPTB', 0x2b: 'MTPR_PERFMON',
    0x2e: 'MTPR_DATFX', 0x3e: 'WTINT', 0x3f: 'MFPR_WHAMI',
    0x80: 'BPT', 0x81: 'BUGCHK', 0x82: 'CHME', 0x83: 'CHMK', 0x84: 'CHMS',
    0x85: 'CHMU', 0x86: 'IMB', 0x87: 'INSQHIL', 0x88: 'INSQTIL',
    0x89: 'INSQHIQ', 0x8a: 'INSQTIQ', 0x8b: 'INSQUEL', 0x8c: 'INSQUEQ',
    0x8d: 'INSQUEL/D', 0x8e: 'INSQUEQ/D', 0x8f: 'PROBER', 0x90: 'PROBEW',
    0x91: 'RD_PS', 0x92: 'REI', 0x93: 'REMQHIL', 0x94: 'REMQTIL',
    0x95: 'REMQHIQ', 0x96: 'REMQTIQ', 0x97: 'REMQUEL', 0x98: 'REMQUEQ',
    0x99: 'REMQUEL/D', 0x9a: 'REMQUEQ/D', 0x9b: 'SWASTEN', 0x9c: 'WR_PS_SW',
    0x9d: 'RSCC', 0x9e: 'READ_UNQ', 0x9f: 'WRITE_UNQ', 0xa0: 'AMOVRR',
    0xa1: 'AMOVRM', 0xa2: 'INSQHILR', 0xa3: 'INSQTILR', 0xa4: 'INSQHIQR',
    0xa5: 'INSQTIQR', 0xa6: 'REMQHILR', 0xa7: 'REMQTILR', 0xa8: 'REMQHIQR',
    0xa9: 'REMQTIQR', 0xaa: 'GENTRAP', 0xae: 'CLRFEN'}
UNIX_CALLS = {
    0x00: 'halt', 0x01: 'cflush', 0x02: 'draina', 0x09: 'cserve',
    0x0a: 'swppal', 0x0d: 'wripir', 0x10: 'rdmces', 0x11: 'wrmces',
    0x2b: 'wrfen', 0x2d: 'wrvptptr', 0x30: 'swpctx', 0x31: 'wrval',
    0x32: 'rdval', 0x33: 'tbi', 0x34: 'wrent', 0x35: 'swpipl', 0x36: 'rdps',
    0x37: 'wrkgp', 0x38: 'wrusp', 0x39: 'wrperfmon', 0x3a: 'rdusp',
    0x3c: 'whami', 0x3d: 'retsys', 0x3e: 'wtint', 0x3f: 'rti',
    0x80: 'bpt', 0x81: 'bugchk', 0x83: 'callsys', 0x86: 'imb', 0x92: 'urti',
    0x9e: 'rdunique', 0x9f: 'wrunique', 0xaa: 'gentrap', 0xae: 'clrfen'}

# EV7 CSR offsets: read from the model's table (chipsets/marvel/Ev7Csr.cpp)
# when the script runs from the source tree, so a name means the Marvel
# chipset decodes the register; otherwise only the few named here.
CSR_NAMES = {0x0: 'RBOX_CFG', 0x50: 'RBOX_INT', 0x60: 'RBOX_IMASK',
             0x70: 'RBOX_IREQ', 0xa0: 'RBOX_IT', 0xb0: 'RBOX_SCRATCH1',
             0x100: 'GIO_CFG', 0x110: 'GIO_DAT', 0x120: 'GIO_CTL'}
CSR_MODELLED = False
SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..',
                   'src')


def load_csr_table():
    global CSR_NAMES, CSR_MODELLED
    d = os.path.join(SRC, 'chipsets', 'marvel')
    cpp, hpp = os.path.join(d, 'Ev7Csr.cpp'), os.path.join(d, 'Ev7Csr.hpp')
    if not (os.path.exists(cpp) and os.path.exists(hpp)):
        return
    consts = {}
    for m in re.finditer(r'constexpr u32 (\w+) = (0x[0-9a-fA-F]+);',
                         open(hpp).read()):
        consts[m.group(1)] = int(m.group(2), 16)
    t = {}
    for m in re.finditer(r'\{(0x[0-9a-fA-F]+|[A-Z_0-9]+), "([^"]+)"\}',
                         open(cpp).read()):
        k = m.group(1)
        t[int(k, 16) if k.startswith('0x') else consts.get(k, -1)] = \
            m.group(2)
    if 'GIO_LOCK' in consts:
        t[consts['GIO_LOCK']] = 'GIO lock'
    CSR_NAMES, CSR_MODELLED = t, True


def csr_name(off):
    if 0x2000 <= off < 0x2000 + 0x114 * 0x10 and not off & 0xf:
        return 'RBOX_ROUTE[%#x]' % ((off - 0x2000) >> 4)
    if off in CSR_NAMES:
        return CSR_NAMES[off]
    return '+%#x%s' % (off, ' NOT MODELLED' if CSR_MODELLED else '')


# What the core does with each index, read from cpu/cpu_pal.hpp when the
# script runs from the source tree: 'impl' (read or written), 'ign' (a
# write accepted and dropped), 'warn' (the unknown-IPR path: a warning
# once, reads give 0).
CORE_R, CORE_W, CORE_WIGN = set(), set(), set()


def load_core_iprs():
    p = os.path.join(SRC, 'cpu', 'cpu_pal.hpp')
    if not os.path.exists(p):
        return
    s = open(p).read()
    mf = s[s.index('#define DO_HW_MFPR'):s.index('#define DO_HW_MTPR')]
    mt = s[s.index('#define DO_HW_MTPR'):s.index('#define DO_HW_RET')]
    for m in re.finditer(r'case (0x[0-9a-fA-F]+):', mf):
        CORE_R.add(int(m.group(1), 16))
    # In MTPR, empty cases fall into the next one; a body that is only
    # `break;` accepts the write and drops it.
    pending = []
    for blk in re.split(r'(?=case 0x)', mt):
        m = re.match(r'case (0x[0-9a-fA-F]+):', blk)
        if not m:
            continue
        pending.append(int(m.group(1), 16))
        body = re.sub(r'/\*.*?\*/', '', blk[m.end():], flags=re.S)
        body = body.replace('\\', ' ').split()
        if not body:
            continue
        (CORE_WIGN if body[0] == 'break;' else CORE_W).update(pending)
        pending = []


def core_status(idx, rd):
    if not (CORE_R or CORE_W):
        return '?'
    if (idx & 0xc0) == 0x40:
        return 'impl'
    if rd:
        return 'impl' if idx in CORE_R else 'warn'
    if idx in CORE_W:
        return 'impl'
    return 'ign' if idx in CORE_WIGN else 'warn'


def sext(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def bits_str(mask):
    """0b0111_0000 -> '<6:4>'; several ranges comma-separated."""
    out, b = [], 0
    while b < 64:
        if mask >> b & 1:
            e = b
            while e + 1 < 64 and mask >> (e + 1) & 1:
                e += 1
            out.append('<%d>' % b if e == b else '<%d:%d>' % (e, b))
            b = e + 1
        else:
            b += 1
    return ','.join(reversed(out)) if out else '-'


# ---------------------------------------------------------------------------
# Decoding


class Ins:
    __slots__ = ('pc', 'w', 'op', 'ra', 'rb', 'rc', 'lit', 'func', 'disp',
                 'target', 'kind')

    def __init__(self, pc, w):
        self.pc, self.w = pc, w
        self.op = w >> 26
        self.ra = (w >> 21) & 31
        self.rb = (w >> 16) & 31
        self.rc = w & 31
        self.lit = (w >> 13) & 0xff if w & 0x1000 else None
        self.func = (w >> 5) & 0x7f
        self.disp = None
        self.target = None
        self.kind = 'op'
        op = self.op
        if op in (0x08, 0x09) or 0x0a <= op <= 0x0f or 0x20 <= op <= 0x2f:
            self.disp = sext(w & 0xffff, 16)
            self.kind = 'mem'
        elif op >= 0x30:
            self.target = (pc + 4 + 4 * sext(w & 0x1fffff, 21)) & M64
            self.kind = 'br' if op in (0x30, 0x34) else 'cbr'
        elif op == 0x1a:
            self.kind = ['jmp', 'jsr', 'ret', 'jsr'][(w >> 14) & 3]
        elif op == 0x1e:
            self.kind = ['hw_jmp', 'hw_jsr', 'hw_ret', 'hw_jsr'][(w >> 14) & 3]
        elif op == 0x00:
            self.kind = 'call_pal'
        elif op in (0x1b, 0x1f):
            self.disp = sext(w & 0xfff, 12)
            self.kind = 'hwmem'
        elif op in (0x19, 0x1d):
            self.kind = 'mxpr'

    @property
    def ends(self):
        """No fall-through."""
        return self.kind in ('br', 'jmp', 'ret', 'hw_jmp', 'hw_ret') and \
            not (self.op == 0x34)

    def text(self):
        w, op = self.w, self.op
        b = REG[self.rb] if self.lit is None else '%#x' % self.lit
        if op == 0x19:
            i = (w >> 8) & 0xff
            return 'hw_mfpr %s, %s (%#04x)' % (REG[self.ra], ipr_name(i), i)
        if op == 0x1d:
            i = (w >> 8) & 0xff
            return 'hw_mtpr %s, %s (%#04x)' % (REG[self.rb], ipr_name(i), i)
        if op in (0x1b, 0x1f):
            return '%s%s/%s %s, %d(%s)' % (
                'hw_ld' if op == 0x1b else 'hw_st', 'q' if w >> 12 & 1 else 'l',
                hwmem_type(w), REG[self.ra], self.disp, REG[self.rb])
        if op == 0x1e:
            return '%s %s, (%s)%s' % (self.kind, REG[self.ra], REG[self.rb],
                                     '/stall' if w >> 13 & 1 else '')
        if self.kind in ('br', 'cbr'):
            return '%s %s, %#x' % (BRN.get(op, 'b%02x' % op), REG[self.ra],
                                  self.target)
        if self.kind == 'mem':
            return '%s %s, %d(%s)' % (MEMN.get(op, 'm%02x' % op), REG[self.ra],
                                     self.disp, REG[self.rb])
        if op in OPN:
            n = OPN[op].get(self.func, '%02x.%02x' % (op, self.func))
            return '%s %s, %s, %s' % (n, REG[self.ra], b, REG[self.rc])
        if op == 0x18:
            return 'misc.%04x %s' % (w & 0xffff, REG[self.ra])
        if op == 0x00:
            return 'call_pal %#x' % (w & 0x3ffffff)
        if op == 0x1a:
            return '%s %s, (%s)' % (self.kind, REG[self.ra], REG[self.rb])
        return '.long %#010x' % w


def hwmem_type(w):
    t = (w >> 13) & 7
    if w >> 26 == 0x1b:
        return {0: 'phys', 1: 'phys/lock', 2: 'vpte', 4: 'virt',
                5: 'virt/wrchk', 6: 'virt/alt',
                7: 'virt/wrchk/alt'}.get(t, 'type%d?' % t)
    return {0: 'phys', 1: 'phys/cond', 2: 'virt', 6: 'virt/alt'}.get(
        t, 'type%d?' % t)


BRN = {0x30: 'br', 0x34: 'bsr', 0x38: 'blbc', 0x39: 'beq', 0x3a: 'blt',
       0x3b: 'ble', 0x3c: 'blbs', 0x3d: 'bne', 0x3e: 'bge', 0x3f: 'bgt',
       0x31: 'fbeq', 0x32: 'fblt', 0x33: 'fble', 0x35: 'fbne', 0x36: 'fbge',
       0x37: 'fbgt'}
MEMN = {0x08: 'lda', 0x09: 'ldah', 0x0a: 'ldbu', 0x0b: 'ldq_u', 0x0c: 'ldwu',
        0x0d: 'stw', 0x0e: 'stb', 0x0f: 'stq_u', 0x28: 'ldl', 0x29: 'ldq',
        0x2a: 'ldl_l', 0x2b: 'ldq_l', 0x2c: 'stl', 0x2d: 'stq', 0x2e: 'stl_c',
        0x2f: 'stq_c', 0x20: 'ldf', 0x21: 'ldg', 0x22: 'lds', 0x23: 'ldt',
        0x24: 'stf', 0x25: 'stg', 0x26: 'sts', 0x27: 'stt'}
OPN = {
    0x10: {0x00: 'addl', 0x02: 's4addl', 0x09: 'subl', 0x0b: 's4subl',
           0x0f: 'cmpbge', 0x12: 's8addl', 0x1b: 's8subl', 0x1d: 'cmpult',
           0x20: 'addq', 0x22: 's4addq', 0x29: 'subq', 0x2b: 's4subq',
           0x2d: 'cmpeq', 0x32: 's8addq', 0x3b: 's8subq', 0x3d: 'cmpule',
           0x4d: 'cmplt', 0x6d: 'cmple'},
    0x11: {0x00: 'and', 0x08: 'bic', 0x14: 'cmovlbs', 0x16: 'cmovlbc',
           0x20: 'bis', 0x24: 'cmoveq', 0x26: 'cmovne', 0x28: 'ornot',
           0x40: 'xor', 0x44: 'cmovlt', 0x46: 'cmovge', 0x48: 'eqv',
           0x61: 'amask', 0x64: 'cmovle', 0x66: 'cmovgt', 0x6c: 'implver'},
    0x12: {0x02: 'mskbl', 0x06: 'extbl', 0x0b: 'insbl', 0x12: 'mskwl',
           0x16: 'extwl', 0x1b: 'inswl', 0x22: 'mskll', 0x26: 'extll',
           0x2b: 'insll', 0x30: 'zap', 0x31: 'zapnot', 0x32: 'mskql',
           0x34: 'srl', 0x36: 'extql', 0x39: 'sll', 0x3b: 'insql', 0x3c: 'sra',
           0x52: 'mskwh', 0x57: 'inswh', 0x5a: 'extwh', 0x62: 'msklh',
           0x67: 'inslh', 0x6a: 'extlh', 0x72: 'mskqh', 0x77: 'insqh',
           0x7a: 'extqh'},
    0x13: {0x00: 'mull', 0x20: 'mulq', 0x30: 'umulh'},
    0x1c: {0x00: 'sextb', 0x01: 'sextw', 0x30: 'ctpop', 0x32: 'ctlz',
           0x33: 'cttz', 0x70: 'ftoit', 0x78: 'ftois'},
}


def valid(i):
    if i.w == 0:
        return False
    t = i.text()
    return not (t.startswith('.long') or re.match(r'^[0-9a-f]{2}\.', t) or
                '?' in t)


def is_amask(i):
    return i.op == 0x11 and i.func == 0x61 and i.ra == 31


def is_implver(i):
    return i.op == 0x11 and i.func == 0x6c and i.ra == 31


def writes_reg(i):
    """The integer register an instruction writes, or None."""
    k = i.kind
    if k == 'op' and i.op in OPN:
        return i.rc
    if k == 'mem' and i.op in (0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x28, 0x29, 0x2a,
                               0x2b, 0x2e, 0x2f):
        return i.ra
    if k in ('br', 'cbr') and i.op in (0x30, 0x34):
        return i.ra
    if k in ('jmp', 'jsr', 'ret', 'hw_jmp', 'hw_jsr', 'hw_ret'):
        return i.ra
    if i.op == 0x19 or (i.op == 0x1b):
        return i.ra
    if i.op == 0x1f and (i.w >> 13) & 7 == 1:
        return i.ra  # store-conditional status
    if i.op == 0x18 and (i.w & 0xffff) in (0xc000, 0xe000, 0xf000):
        return i.ra  # rpcc, rc, rs
    return None


# ---------------------------------------------------------------------------
# Known-bits lattice: (mask of known bits, their values)

UNK = (0, 0)


def kconst(v):
    return (M64, v & M64)


def k_or(a, b):
    ones = (a[0] & a[1]) | (b[0] & b[1])
    zeros = (a[0] & ~a[1]) & (b[0] & ~b[1])
    return ((ones | zeros) & M64, ones & M64)


def k_and(a, b):
    zeros = (a[0] & ~a[1]) | (b[0] & ~b[1])
    ones = (a[0] & a[1]) & (b[0] & b[1])
    return ((ones | zeros) & M64, ones & M64)


def k_not(a):
    return (a[0], ~a[1] & a[0] & M64)


def k_xor(a, b):
    m = a[0] & b[0]
    return (m, (a[1] ^ b[1]) & m)


def k_shl(a, n):
    return (((a[0] << n) | ((1 << n) - 1)) & M64, (a[1] << n) & M64)


def k_shr(a, n, arith=False):
    m = a[0] >> n
    v = a[1] >> n
    if n:
        top = ((1 << n) - 1) << (64 - n)
        if not arith:
            m |= top
        elif a[0] >> 63 & 1:
            m |= top
            if a[1] >> 63 & 1:
                v |= top
    return (m & M64, v & M64)


def k_add(a, b, sub=False):
    """Ternary addition: each bit 0, 1 or unknown, carries included."""
    if sub:
        b = k_add(k_not(b), kconst(1))
    if a[0] == M64 and b[0] == M64:
        return kconst(a[1] + b[1])
    m = v = 0
    c = (1, 0)  # (known, value) of the carry in
    for p in range(64):
        bits = [((x[0] >> p) & 1, (x[1] >> p) & 1) for x in (a, b)] + [c]
        if all(k for k, _ in bits):
            s = sum(val for _, val in bits)
            m |= 1 << p
            v |= (s & 1) << p
            c = (1, s >> 1)
        else:
            ones = sum(1 for k, val in bits if k and val)
            zeros = sum(1 for k, val in bits if k and not val)
            c = (1, 1) if ones >= 2 else (1, 0) if zeros >= 2 else (0, 0)
    return (m, v)


def k_full(a):
    return a[0] == M64


def bytemask(m):
    r = 0
    for k in range(8):
        if m >> k & 1:
            r |= 0xff << (8 * k)
    return r


def k_eval(i, regs):
    """Result of an operate or address instruction in the lattice."""
    ra = regs[i.ra] if i.ra != 31 else kconst(0)
    if i.kind == 'mem':
        rb = regs[i.rb] if i.rb != 31 else kconst(0)
        if i.op == 0x08:
            return k_add(rb, kconst(i.disp))
        if i.op == 0x09:
            return k_add(rb, kconst(i.disp << 16))
        return UNK
    b = kconst(i.lit) if i.lit is not None else (
        regs[i.rb] if i.rb != 31 else kconst(0))
    n = OPN.get(i.op, {}).get(i.func)
    if n == 'bis':
        return k_or(ra, b)
    if n == 'and':
        return k_and(ra, b)
    if n == 'bic':
        return k_and(ra, k_not(b))
    if n == 'ornot':
        return k_or(ra, k_not(b))
    if n == 'xor':
        return k_xor(ra, b)
    if n == 'eqv':
        return k_not(k_xor(ra, b))
    if n in ('sll', 'srl', 'sra'):
        if not k_full(b):
            return UNK
        s = b[1] & 63
        return k_shl(ra, s) if n == 'sll' else k_shr(ra, s, n == 'sra')
    if n in ('addq', 'subq'):
        return k_add(ra, b, n == 'subq')
    if n in ('s4addq', 's8addq'):
        return k_add(k_shl(ra, 2 if n[1] == '4' else 3), b)
    if n in ('addl', 'subl'):
        r = k_add(ra, b, n == 'subl')
        if r[0] & 0x80000000:
            hi = M64 ^ 0xffffffff
            return (r[0] | hi, (r[1] | hi) if r[1] & 0x80000000 else r[1])
        return (r[0] & 0x7fffffff, r[1] & 0x7fffffff)
    if n in ('zapnot', 'zap'):
        if not k_full(b):
            return UNK
        m = bytemask(b[1] & 0xff)
        if n == 'zap':
            m = ~m & M64
        return k_and(ra, kconst(m))
    if n in ('extbl', 'extwl', 'extll', 'extql'):
        if not k_full(b):
            return UNK
        r = k_shr(ra, (b[1] & 7) * 8)
        w = {'extbl': 0xff, 'extwl': 0xffff, 'extll': 0xffffffff,
             'extql': M64}[n]
        return k_and(r, kconst(w))
    if n in ('insbl', 'inswl', 'insll', 'insql'):
        if not k_full(b):
            return UNK
        w = {'insbl': 0xff, 'inswl': 0xffff, 'insll': 0xffffffff,
             'insql': M64}[n]
        return k_shl(k_and(ra, kconst(w)), (b[1] & 7) * 8)
    if n in ('cmpeq', 'cmpult', 'cmpule', 'cmplt', 'cmple', 'cmpbge'):
        return (M64 ^ 1, 0) if n != 'cmpbge' else (M64 ^ 0xff, 0)
    if n == 'sextb':
        return UNK
    return UNK


# ---------------------------------------------------------------------------
# Symbolic tracking of a read value: register -> list(64) of source bit
# numbers (None where the bit is no longer the source's).


def sym_ident():
    return list(range(64))


def sym_mask(s, m):
    return [s[k] if m >> k & 1 else None for k in range(64)]


def sym_src_mask(s):
    r = 0
    for v in s:
        if v is not None:
            r |= 1 << v
    return r


def sym_shift(s, n, left):
    if left:
        return [None] * n + s[:64 - n]
    return s[n:] + [None] * n


class Uses:
    """What the code does with a value: isolated fields, tests, writes."""

    def __init__(self):
        self.fields = 0       # source bits isolated by a mask/shift
        self.tested = 0       # source bits a branch or compare decides on
        self.set = 0          # bits set before being written back
        self.cleared = 0      # bits cleared before being written back
        self.notes = []

    def text(self):
        p = []
        if self.tested:
            p.append('tests %s' % bits_str(self.tested))
        if self.fields and self.fields != self.tested:
            p.append('isolates %s' % bits_str(self.fields))
        if self.set:
            p.append('sets %s' % bits_str(self.set))
        if self.cleared:
            p.append('clears %s' % bits_str(self.cleared))
        p.extend(self.notes)
        return '; '.join(p) if p else 'whole value'


def follow_value(code, start, dst, limit=16):
    """Track a value from register dst after the instruction at start along
    the fall-through path (and unconditional branches) for `limit`
    instructions."""
    u = Uses()
    syms = {dst: sym_ident()}
    regs = [UNK] * 32
    pc = start + 4
    seen = 0
    while seen < limit and syms:
        i = code.get(pc)
        if i is None:
            break
        seen += 1
        n = OPN.get(i.op, {}).get(i.func) if i.kind == 'op' else None
        src_a = syms.get(i.ra) if i.ra != 31 else None
        src_b = syms.get(i.rb) if i.rb != 31 and i.lit is None else None
        bval = kconst(i.lit) if i.lit is not None else (
            regs[i.rb] if i.rb != 31 else kconst(0))
        new = None
        if i.kind == 'cbr' and src_a is not None:
            if i.op in (0x38, 0x3c):
                if src_a[0] is not None:
                    u.tested |= 1 << src_a[0]
            elif i.op in (0x3a, 0x3e):
                if src_a[63] is not None:
                    u.tested |= 1 << src_a[63]
            else:
                u.tested |= sym_src_mask(src_a)
        elif n is not None and (src_a is not None or src_b is not None):
            s = src_a if src_a is not None else src_b
            other = bval if src_a is not None else (
                regs[i.ra] if i.ra != 31 else kconst(0))
            if n in ('and', 'bic', 'zapnot', 'zap') and k_full(other):
                m = other[1]
                if n == 'bic':
                    m = ~m & M64
                elif n == 'zapnot':
                    m = bytemask(m & 0xff)
                elif n == 'zap':
                    m = ~bytemask(m & 0xff) & M64
                new = sym_mask(s, m)
                if n == 'bic' and s == sym_ident():
                    u.cleared |= ~m & M64
                else:
                    u.fields |= sym_src_mask(new)
            elif n in ('srl', 'sll', 'sra') and src_a is not None and \
                    k_full(bval):
                new = sym_shift(s, bval[1] & 63, n == 'sll')
                if n == 'sra':
                    new = s[bval[1] & 63:] + [s[63]] * (bval[1] & 63)
            elif n in ('extbl', 'extwl', 'extll', 'extql') and \
                    src_a is not None and k_full(bval):
                w = {'extbl': 8, 'extwl': 16, 'extll': 32, 'extql': 64}[n]
                new = sym_shift(s, (bval[1] & 7) * 8, False)
                new = sym_mask(new, (1 << w) - 1)
                u.fields |= sym_src_mask(new)
            elif n in ('bis', 'xor') and k_full(other):
                new = list(s)
                if s == sym_ident():
                    if n == 'bis':
                        u.set |= other[1]
                    else:
                        u.notes.append('xor %#x' % other[1])
                for k in range(64):
                    if other[1] >> k & 1:
                        new[k] = None
            elif n in ('cmpeq', 'cmpult', 'cmpule', 'cmplt', 'cmple'):
                u.tested |= sym_src_mask(s)
                if k_full(other):
                    u.notes.append('%s %#x' % (n, other[1]))
            elif n in ('cmoveq', 'cmovne', 'cmovlbs', 'cmovlbc', 'cmovlt',
                       'cmovge', 'cmovle', 'cmovgt') and src_a is not None:
                u.tested |= (1 << s[0]) if n in ('cmovlbs', 'cmovlbc') and \
                    s[0] is not None else sym_src_mask(s)
            elif n == 'bis' and i.ra == 31 and src_b is not None:
                new = list(src_b)  # mov
            else:
                u.notes.append('%s' % n)
        elif i.op == 0x1d and syms.get(i.rb) is not None:
            idx = (i.w >> 8) & 0xff
            u.notes.append('written to %s' % ipr_name(idx))
        elif i.kind == 'hwmem' and i.op == 0x1f and syms.get(i.ra) is not None:
            u.notes.append('stored (hw_st)')
        elif i.kind == 'mem' and i.op in (0x2c, 0x2d, 0x0d, 0x0e) and \
                syms.get(i.ra) is not None:
            u.notes.append('stored')
        w = writes_reg(i)
        if w is not None and w != 31:
            if new is not None:
                syms[w] = new
            else:
                syms.pop(w, None)
            if i.kind in ('op', 'mem'):
                regs[w] = k_eval(i, regs)
            else:
                regs[w] = UNK
        if i.ends:
            if i.kind == 'br' and i.op == 0x30:
                pc = i.target
                continue
            break
        pc += 4
    return u


def value_before(code, labels, at, reg, limit=24):
    """The known bits of `reg` at instruction `at`, from straight-line code
    before it (back to a branch target, an unconditional transfer or
    `limit` instructions)."""
    start = at
    for _ in range(limit):
        p = start - 4
        i = code.get(p)
        if i is None or i.ends or i.kind == 'cbr':
            break
        start = p
        if start in labels:
            break
    regs = [UNK] * 32
    origin = [None] * 32
    pc = start
    while pc < at:
        i = code[pc]
        w = writes_reg(i)
        if w is not None and w != 31:
            if i.kind in ('op', 'mem'):
                regs[w] = k_eval(i, regs)
                origin[w] = origin[i.ra] if (
                    i.kind == 'op' and OPN.get(i.op, {}).get(i.func) in
                    ('bis', 'bic', 'and', 'xor') and origin[i.ra]) else None
            else:
                regs[w] = UNK
                origin[w] = None
            if i.op == 0x19:
                origin[w] = (i.w >> 8) & 0xff
        pc += 4
    if reg == 31:
        return kconst(0), None
    return regs[reg], origin[reg]


def k_meet(a, b):
    m = a[0] & b[0] & ~(a[1] ^ b[1]) & M64
    return (m, a[1] & m)


def k_step(i, regs):
    """Apply one instruction to a register state in place."""
    w = writes_reg(i)
    if w is None or w == 31:
        return
    if i.kind in ('op', 'mem'):
        regs[w] = k_eval(i, regs)
    elif i.kind in ('br', 'jmp', 'jsr', 'ret', 'hw_jmp', 'hw_jsr', 'hw_ret'):
        regs[w] = kconst(i.pc + 4)
    else:
        regs[w] = UNK


def propagate(code, roots, base, end):
    """Known bits of every register before every instruction, from the
    entry points through branches (a forward dataflow; at a join only the
    bits all paths agree on stay known). After a call (bsr, jsr, hw_jsr)
    nothing is assumed to survive."""
    starts = set(roots)
    for i in code.values():
        if i.target is not None and base <= i.target < end:
            starts.add(i.target)
        if i.kind != 'op' and i.kind != 'mem' and i.kind != 'hwmem' and \
                i.kind != 'mxpr':
            starts.add(i.pc + 4)
    state_in = {r: [UNK] * 32 for r in roots}
    work = list(roots)
    before = {}
    while work:
        pc0 = work.pop()
        regs = list(state_in[pc0])
        pc = pc0
        while pc in code:
            before[pc] = list(regs)
            i = code[pc]
            k_step(i, regs)
            succ = []
            if i.target is not None and base <= i.target < end:
                succ.append((i.target, regs))
            if not i.ends:
                if i.kind in ('jsr', 'hw_jsr') or i.op == 0x34:
                    succ.append((pc + 4, [UNK] * 32))
                else:
                    succ.append((pc + 4, regs))
            nxt = None
            for s, st in succ:
                if s in starts or s != pc + 4:
                    old = state_in.get(s)
                    new = list(st) if old is None else [
                        k_meet(x, y) for x, y in zip(old, st)]
                    if old is None or new != old:
                        state_in[s] = new
                        work.append(s)
                else:
                    nxt = s
            if nxt is None:
                break
            pc = nxt
    return before


def known_text(k):
    m, v = k
    if m == M64:
        return '= %#x' % v
    if m == 0:
        return 'unknown'
    ones = v & m
    zeros = m & ~v & M64
    p = []
    if ones:
        p.append('ones %s' % bits_str(ones))
    if zeros and zeros != M64:
        # Only interesting zeros: inside the span of known ones or low bits.
        p.append('zeros %s' % bits_str(zeros & ((1 << 48) - 1)))
    return ', '.join(p) if p else 'unknown'


# ---------------------------------------------------------------------------


def load_image(path):
    d = open(path, 'rb').read()
    if d.endswith(b'ALPHDC01'):
        entry, palb = struct.unpack_from('<QQ', d, 0)
        return d[16:-8], 'decompressed cache (entry %#x, PAL_BASE %#x)' % (
            entry, palb)
    if len(d) % 0x10000 == 16:  # an older cache, without the tag
        return d[16:], 'decompressed cache (untagged)'
    return d, 'raw image'


def find_pals(img):
    out = []
    for a in range(0, len(img) - 0x28, 0x8000):
        lo, hi = struct.unpack_from('<II', img, a + 8)
        t = (lo >> 16) & 0xff
        if t in (1, 2) and (lo >> 24) == 0 and 1 <= (lo >> 8) & 0xff <= 9 \
                and (lo & 0xff) < 100 and hi & 0xffff == 1:
            out.append(a)
    return out


def pal_header(img, base):
    lo = struct.unpack_from('<I', img, base + 8)[0]
    b = img[base + 0x21]
    kind = {1: 'OpenVMS', 2: 'Tru64 UNIX'}.get((lo >> 16) & 0xff, '?')
    return kind, 'V%d.%02d-%d' % ((lo >> 8) & 0xff, lo & 0xff, b)


def pal_extent(img, base, nxt):
    end = nxt if nxt else len(img)
    # Stop at 4 KB of zeros once past the CALL_PAL slots.
    a = base + 0x4000
    while a < end:
        if not any(img[a:a + 0x1000]):
            return a
        a += 0x100
    return end


def entries(kind):
    names = VMS_CALLS if kind == 'OpenVMS' else UNIX_CALLS
    e = dict(EXC_ENTRIES)
    for f in range(0x40):
        e[0x2000 + f * 0x40] = 'CALL_PAL %#04x %s' % (f, names.get(f, ''))
        e[0x3000 + f * 0x40] = 'CALL_PAL %#04x %s' % (
            0x80 + f, names.get(0x80 + f, ''))
    return e


def analyse(img, base, end, kind, detail, rows):
    code = {}
    for a in range(base, end, 4):
        code[a] = Ins(a, struct.unpack_from('<I', img, a)[0])
    labels = set(i.target for i in code.values() if i.target is not None)
    reach = {}
    ents = entries(kind)
    for off, nm in sorted(ents.items()):
        root = base + off
        if code[root].w == 0:
            continue
        stack, seen = [root], set()
        while stack:
            pc = stack.pop()
            while pc in code and pc not in seen:
                seen.add(pc)
                reach.setdefault(pc, []).append(off)
                i = code[pc]
                if i.target is not None and base <= i.target < end:
                    stack.append(i.target)
                if i.ends:
                    break
                pc += 4

    # Code no entry reaches through branches, but which decodes cleanly
    # around a site: entered through a computed jump (a dispatch table, a
    # hw_ret to a built address). Counted with the rest, marked 'indirect'.
    def plausible(pc):
        # The site and the four instructions after it decode; before it,
        # instructions or the zero padding of an aligned dispatch slot.
        for p in range(pc - 16, pc + 20, 4):
            i = code.get(p)
            if i is None or not (valid(i) or (p < pc and i.w == 0)):
                return False
        return True

    def owner(pc):
        offs = reach.get(pc)
        if not offs:
            return 'indirect' if plausible(pc) else 'unreached'
        ns = sorted(set(offs))
        if len(ns) == 1:
            return ents[ns[0]].strip()
        if len(ns) <= 3:
            return ' | '.join(ents[o].strip() for o in ns)
        return '%s +%d more' % (ents[ns[0]].strip(), len(ns) - 1)

    roots = [base + o for o in ents if code[base + o].w]
    before = propagate(code, roots, base, end)

    def known_at(pc, reg):
        k, org = value_before(code, labels, pc, reg)
        if pc in before and reg != 31:
            d = before[pc][reg]
            if bin(d[0]).count('1') > bin(k[0]).count('1'):
                k = d
        return k, org

    name = '%s PALcode %s' % pal_header(img, base)
    print('=' * 78)
    print('%s at %#x-%#x (%d KB), %d of %d instructions reached from %d '
          'entries' % (name, base, end, (end - base) // 1024, len(reach),
                       len(code), len([o for o in ents
                                       if code[base + o].w])))
    print('=' * 78)
    per = {}
    unreached = []
    hwret = {}
    for pc in sorted(code):
        i = code[pc]
        r = pc in reach or (i.op in (0x19, 0x1b, 0x1d, 0x1e, 0x1f) and
                            plausible(pc))
        if i.op in (0x19, 0x1d):
            idx = (i.w >> 8) & 0xff
            rd = i.op == 0x19
            if rd:
                u = follow_value(code, pc, i.ra)
                info = u.text()
            else:
                k, org = known_at(pc, i.rb)
                info = known_text(k)
                if org is not None:
                    info += ' (from %s)' % ipr_name(org)
            row = (name, 'R' if rd else 'W', idx, ipr_name(idx), pc, owner(pc),
                   info, r)
            if r:
                per.setdefault((idx, 'R' if rd else 'W'), []).append(row)
                rows.append(row)
            else:
                unreached.append(row)
        elif i.op in (0x1b, 0x1f):
            t = hwmem_type(i.w)
            info = ''
            if t.startswith('phys'):
                k, _ = known_at(pc, i.rb)
                a = k_add(k, kconst(i.disp))
                if (a[0] >> 22) & 0x1fff == 0x1fff and \
                        (a[1] >> 22) & 0x1fff == 0x1fff:
                    if a[0] & 0x3fffff == 0x3fffff:
                        info = 'EV7 CSR %s' % csr_name(a[1] & 0x3fffff)
                    else:
                        info = 'EV7 CSR, offset unknown (disp %d)' % i.disp
                elif a[0] >> 43 & 1 and a[1] >> 43 & 1:
                    info = 'I/O (PA<43>)'
                elif k[0]:
                    info = 'base %s %s' % (REG[i.rb], known_text(k))
                else:
                    info = 'base %s unknown' % REG[i.rb]
            row = (name, 'LD' if i.op == 0x1b else 'ST', -1,
                   '%s%s/%s' % ('hw_ld' if i.op == 0x1b else 'hw_st',
                                'q' if i.w >> 12 & 1 else 'l', t),
                   pc, owner(pc), info, r)
            if r:
                rows.append(row)
                per.setdefault(('mem', row[3]), []).append(row)
            else:
                unreached.append(row)
        elif i.op == 0x1e and r:
            key = (i.kind, i.w >> 13 & 1, i.w & 0x1fff)
            hwret.setdefault(key, []).append(pc)
        elif (is_amask(i) or is_implver(i)) and r:
            u = follow_value(code, pc, i.rc)
            row = (name, 'X', -1, 'AMASK' if is_amask(i) else 'IMPLVER', pc,
                   owner(pc), ('operand %s; ' % (
                       '%#x' % i.lit if i.lit is not None else REG[i.rb])) +
                   u.text(), r)
            rows.append(row)
            per.setdefault(('x', row[3]), []).append(row)

    print('\nInternal processor registers (core: impl = implemented, ign = '
          'write dropped, warn = unknown-IPR path):')
    for key in sorted(k for k in per if isinstance(k[0], int)):
        idx, d = key
        lst = per[key]
        nind = len([r for r in lst if r[5] == 'indirect'])
        print('  %-4s %#04x %-16s %3d site(s)%s  core: %s' % (
            d, idx, ipr_name(idx), len(lst),
            ' (%d indirect)' % nind if nind else '',
            core_status(idx, d == 'R')))
        if detail:
            for row in lst:
                print('        %#07x  %-34s %s' % (row[4], row[5][:34], row[6]))
        else:
            infos = sorted(set(row[6] for row in lst))
            for s in infos[:6]:
                print('        %s' % s)
            if len(infos) > 6:
                print('        ... %d more' % (len(infos) - 6))
    print('\nHW_LD / HW_ST (reached code):')
    for key in sorted(k for k in per if k[0] == 'mem'):
        lst = per[key]
        print('  %-20s %4d site(s)' % (key[1], len(lst)))
        tgt = {}
        for row in lst:
            tgt.setdefault(row[6], []).append(row[4])
        for s in sorted(tgt):
            if s.startswith('EV7') or s.startswith('I/O') or detail:
                print('        %-44s %s' % (s, ' '.join(
                    '%#x' % p for p in tgt[s][:8]) + (
                        ' ...' if len(tgt[s]) > 8 else '')))
        other = sum(len(v) for s, v in tgt.items()
                    if not (s.startswith('EV7') or s.startswith('I/O')))
        if other and not detail:
            print('        %d to memory or an address not worked out here' %
                  other)
    print('\nHW_RET forms (reached code):')
    for key in sorted(hwret):
        print('  %-7s stall=%d disp=%#06x  %4d site(s)' % (
            key[0], key[1], key[2], len(hwret[key])))
    print('\nIMPLVER / AMASK (reached code):')
    xs = [r for k in per if k[0] == 'x' for r in per[k]]
    if not xs:
        print('  none')
    for row in xs:
        print('  %#07x %-8s %-30s %s' % (row[4], row[3], row[5][:30], row[6]))
    print('\nNot reached from an entry point: %d HW_MFPR/HW_MTPR, '
          '%d HW_LD/HW_ST' % (
              len([r for r in unreached if r[1] in 'RW']),
              len([r for r in unreached if r[1] in ('LD', 'ST')])))
    if detail:
        for row in unreached:
            print('  %#07x %-2s %-22s %s' % (row[4], row[1], row[3], row[6]))
    print()


def whole_scan(img, pals):
    print('=' * 78)
    print('IMPLVER and AMASK in the whole image (outside the PALcode)')
    print('=' * 78)
    code = {}
    found = 0
    for a in range(0, len(img) - 3, 4):
        w = struct.unpack_from('<I', img, a)[0]
        # AMASK and IMPLVER as compilers emit them: Ra = zero, and
        # IMPLVER's literal 1.
        if w >> 26 == 0x11 and ((w >> 5) & 0x7f) in (0x61, 0x6c) and \
                (w >> 21) & 31 == 31:
            if any(b <= a < e for b, e in pals):
                continue
            for p in range(a - 64, a + 68, 4):
                if 0 <= p < len(img) - 3:
                    code[p] = Ins(p, struct.unpack_from('<I', img, p)[0])
            i = code[a]
            if is_implver(i) and i.lit != 1:
                continue
            found += 1
            u = follow_value(code, a, i.rc)
            print('  %#08x %-8s operand %-6s %s' % (
                a, 'AMASK' if is_amask(i) else 'IMPLVER',
                '%#x' % i.lit if i.lit is not None else REG[i.rb], u.text()))
    if not found:
        print('  none')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[1])
    ap.add_argument('image')
    ap.add_argument('--base', action='append', default=[],
                    help='PALcode base (hex); default: found by header')
    ap.add_argument('--detail', action='store_true',
                    help='every site, and the unreached ones')
    ap.add_argument('--whole', action='store_true',
                    help='also scan the whole image for IMPLVER/AMASK')
    ap.add_argument('--csv', help='write every reached site to this file')
    ap.add_argument('--dis', nargs=2, metavar=('START', 'END'),
                    help='only disassemble START-END (hex addresses)')
    a = ap.parse_args()
    load_csr_table()
    load_core_iprs()
    img, what = load_image(a.image)
    if a.dis:
        for pc in range(int(a.dis[0], 16), int(a.dis[1], 16), 4):
            i = Ins(pc, struct.unpack_from('<I', img, pc)[0])
            print('%08x: %08x  %s' % (pc, i.w, i.text()))
        return
    print('%s: %s, %d bytes' % (a.image, what, len(img)))
    bases = [int(b, 16) for b in a.base] or find_pals(img)
    if not bases:
        sys.exit('no PALcode header found; name one with --base')
    rows = []
    exts = []
    for n, b in enumerate(bases):
        nxt = bases[n + 1] if n + 1 < len(bases) else None
        end = pal_extent(img, b, nxt)
        exts.append((b, end))
        kind, ver = pal_header(img, b)
        analyse(img, b, end, kind, a.detail, rows)
    if a.whole:
        whole_scan(img, exts)
    if a.csv:
        with open(a.csv, 'w') as f:
            f.write('pal,dir,index,name,pc,entry,info\n')
            for r in rows:
                f.write('%s,%s,%s,%s,%#x,"%s","%s"\n' % (
                    r[0], r[1], '%#04x' % r[2] if r[2] >= 0 else '', r[3],
                    r[4], r[5], r[6]))


if __name__ == '__main__':
    main()
