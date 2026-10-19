#!/usr/bin/env python3
"""Write a boot image that runs the console's Tru64 UNIX PALcode, without a
UNIX guest.

usage: unix_pal_probe.py <image> [cflush] [irq] [noswap]

The image is 1.44 MB: a floppy for the ES40's `boot dva0`, or a disk on any
controller (the ES47's `boot dka0` with it as disk0.0 of a SCSI adapter).
Its SRM boot block loads one block, which the console enters at virtual
0x20000000 under its OpenVMS PALcode. That code changes PALcode the way
Linux's bootstrap does (arch/alpha/boot, switch_to_osf_pal): it reads VPTB
and PTBR (MFPR_VPTB, MFPR_PTBR), builds an OSF PCB in its own page (KSP =
its stack, PTBR, FEN), finds the PCB's physical address through the virtual
page table and calls SWPPAL with PAL variant 2 (OSF), the new PC, the PCB,
VPTB and KSP. Under the UNIX PALcode it then calls rdps, whami, swpipl (to
7 and back), rdusp, wrval and rdval, rdunique, imb, draina, tbi(-2) (the
fetches after it miss in the TB and run the TB-miss paths) and wrfen, and
halts.

  cflush  adds cflush(-1): on the EV7 a negative PFN flushes the whole L2
          (BBOX_CTL way by way); a PFN returns at once
  irq     adds wrent(entInt), wrkgp, swpipl(0) and a loop that waits for
          three interrupts (or 2^24 passes); the handler counts them in fp,
          keeps the a0 and a1 the PALcode passed in t10 and t11, and
          returns with rti
  noswap  leaves out the SWPPAL, so the calls run the OpenVMS PALcode,
          where most are undefined (a control: it ends in a halt with
          "kernel stack not valid")

After the halt, the console's `e r1`, `e -n 7 r9` and `e -n 2 r24` show:
  r1  0x7e57 once every call has returned (0xbad: SWPPAL returned; its
      status is in r14)
  r9  rdps (7)   r10 whami   r11 the IPL swpipl returned (7)   r12 rdusp
  r13 rdval after wrval(0x1234)   r14 rdunique   r15 interrupts taken (3)
  r24, r25 the last interrupt's a0 (type: 1 is the clock) and a1

Example (ES40): FLOPPY=<image> CMDS="boot dva0|e r1|e -n 7 r9|e -n 2 r24"
srm_probe.sh build-jit/alphabox unixpal. Run with ALPHABOX_TRACE_UNKNOWN=1
to see any register the PALcode reaches that nothing models; an IPR the
core does not implement prints %CPU-W-IPR.
"""
import struct
import sys

V0, T0, T1, T2, T3, T4, S0, S1, S2, S3, FP = 0, 1, 2, 3, 4, 5, 9, 10, 11, \
    12, 15
A0, A1, A2, A3, A4, T10, T11, SP, ZERO = 16, 17, 18, 19, 20, 24, 25, 30, 31
PCB_OFF = 0x180


def opr(op, func, ra, rb, rc):
    return (op << 26) | (ra << 21) | (rb << 16) | (func << 5) | rc


def opl(op, func, ra, lit, rc):
    return (op << 26) | (ra << 21) | (lit << 13) | (1 << 12) | (func << 5) | rc


def mem(op, ra, disp, rb):
    return (op << 26) | (ra << 21) | (rb << 16) | (disp & 0xffff)


def br(op, ra, disp_words):
    return (op << 26) | (ra << 21) | (disp_words & 0x1fffff)


def mov(ra, rc):
    return opr(0x11, 0x20, ZERO, ra, rc)


def lda(ra, disp, rb=ZERO):
    return mem(0x08, ra, disp, rb)


def swap_part():
    """SWPPAL to the OSF PALcode; continues right after it."""
    c = [br(0x30, T0, 0)]                       # br t0, .+4: t0 = base + 4
    c += [0x29, mov(V0, S0)]                    # s0 = MFPR_VPTB
    c += [0x15, mov(V0, S1)]                    # s1 = MFPR_PTBR
    c += [lda(S2, PCB_OFF - 4, T0)]             # s2 = PCB virtual
    c += [opl(0x12, 0x34, S2, 13, T1),          # srl s2, 13, t1
          opr(0x10, 0x32, T1, S0, T1),          # s8addq t1, s0, t1
          mem(0x29, T2, 0, T1),                 # ldq t2, 0(t1): the PTE
          opl(0x12, 0x34, T2, 32, T2),          # srl t2, 32, t2
          opl(0x12, 0x39, T2, 13, T2),          # sll t2, 13, t2
          lda(T3, 0x1fff),
          opr(0x11, 0x00, S2, T3, T3),          # and s2, t3, t3
          opr(0x11, 0x20, T2, T3, S3)]          # bis: s3 = PCB physical
    c += [mem(0x2d, SP, 0, S2),                 # ksp
          mem(0x2d, ZERO, 8, S2),               # usp
          mem(0x2d, S1, 16, S2),                # ptbr
          mem(0x2d, ZERO, 24, S2),              # pcc, asn
          mem(0x2d, ZERO, 32, S2),              # unique
          lda(T4, 1), mem(0x2d, T4, 40, S2),    # flags: FEN
          mem(0x2d, ZERO, 48, S2), mem(0x2d, ZERO, 56, S2)]
    c += [lda(A0, 2)]
    newpc = (len(c) + 8) * 4
    c += [lda(A1, newpc - 4, T0), mov(S3, A2), mov(S0, A3), mov(SP, A4),
          0x0a,                                 # swppal
          mov(V0, 14), lda(1, 0xbad), 0x00]     # returned: failed
    assert len(c) * 4 == newpc
    return c


def calls_part(cflush):
    c = [0x36, mov(V0, 9)]                      # rdps
    c += [0x3c, mov(V0, 10)]                    # whami
    c += [lda(A0, 7), 0x35, mov(V0, 11)]        # swpipl(7)
    c += [mov(11, A0), 0x35]                    # swpipl(old)
    c += [0x3a, mov(V0, 12)]                    # rdusp
    c += [lda(A0, 0x1234), 0x31]                # wrval
    c += [0x32, mov(V0, 13)]                    # rdval
    c += [0x9e, mov(V0, 14)]                    # rdunique
    c += [0x86, 0x02]                           # imb, draina
    c += [lda(A0, -2), 0x33]                    # tbi(-2)
    c += [lda(A0, 1), 0x2b]                     # wrfen(1)
    if cflush:
        c += [lda(A0, -1), 0x01]                # cflush(-1)
    return c


def irq_part():
    c = [br(0x30, T0, 0)]                       # br t0, .+4
    h = len(c)
    c += [None,                                 # lda a0, handler(t0)
          lda(A1, 0), 0x34,                     # wrent(handler, entInt)
          lda(A0, 0), 0x37,                     # wrkgp(0)
          mov(ZERO, FP),
          lda(A0, 0), 0x35,                     # swpipl(0)
          mem(0x09, T2, 0x100, ZERO)]           # ldah t2, 0x100
    loop = len(c)
    c += [opl(0x10, 0x29, T2, 1, T2),           # subq t2, 1, t2
          br(0x39, T2, 2),                      # beq t2, done
          opl(0x10, 0x1d, FP, 3, T3)]           # cmpult fp, 3, t3
    c += [br(0x3d, T3, loop - len(c) - 1)]      # bne t3, loop
    c += [lda(A0, 7), 0x35]                     # done: swpipl(7)
    halt = len(c)
    c += [lda(1, 0x7e57), 0x00]                 # halt
    handler = len(c)
    c += [opl(0x10, 0x20, FP, 1, FP),           # addq fp, 1, fp
          mov(A0, T10), mov(A1, T11), 0x3f]     # rti
    c[h] = lda(A0, (handler - 1) * 4, T0)
    return c, halt


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    opts = sys.argv[2:]
    code = [] if 'noswap' in opts else swap_part()
    code += calls_part('cflush' in opts)
    if 'irq' in opts:
        code += irq_part()[0]
    else:
        code += [lda(1, 0x7e57), 0x00]          # halt
    assert len(code) * 4 <= PCB_OFF
    img = bytearray(1474560)  # a 1.44 MB floppy, or any disk
    b0 = bytearray(512)
    struct.pack_into('<QQQ', b0, 0x1E0, 1, 1, 0)  # 1 block from LBA 1
    csum = sum(struct.unpack_from('<63Q', b0, 0)) & (2**64 - 1)
    struct.pack_into('<Q', b0, 0x1F8, csum)
    img[0:512] = b0
    for n, w in enumerate(code):
        struct.pack_into('<I', img, 512 + 4 * n, w)
    with open(sys.argv[1], 'wb') as f:
        f.write(img)


if __name__ == '__main__':
    main()
