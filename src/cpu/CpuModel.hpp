/* Alphabox Alpha Emulator
 * Copyright (C) 2026 Artur Goulão
 * Website: https://github.com/artur/alphabox
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301,
 * USA.
 */

/**
 * \file
 * What tells one Alpha processor from another of the same core family.
 *
 * The core (`CAlphaCPU`) implements the EV6 family: the 21264 instruction
 * set, its internal processor registers and its PALcode interface. Within
 * that family a part is a table row (CpuModels.cpp) -- the values software
 * reads to identify the processor. A part with a different core (EV4, EV5)
 * is not a row: it needs its own core.
 *
 * The EV7 (21364) is the exception that proves the rule: its core is the
 * EV68's, with the same IPRs and PALcode instructions, and the system logic
 * moved onto the chip. So it is a row with family CPU_FAMILY_EV7; what the
 * family changes in the core is in cpu/ev7/, and its on-chip registers
 * (router, memory controllers, the management port) are the Marvel
 * chipset's (chipsets/marvel/).
 *
 * The EV8 (21464) is a row of the EV7 family too, in what
 * docs/platforms/ev8.md calls EV7-compatible mode: the EV7's privileged
 * interface (IPRs, HW_RET, the 44-bit physical address, the CSR window), so
 * the EV7 console and its PALcodes run unchanged, with the 21464's
 * user-visible instructions on top. Those are the row's extensions
 * (cpu_model::ext, CPU_EXT_*), which the decoder tests; the family stays
 * what the board and the PALcode see. A native EV8 (the IDS's own IPRs and
 * PALcode interface, plan S11) would be a family of its own.
 *
 * Every value here is read by the firmware or the operating system, so a
 * wrong one is visible: the console prints the processor's name from the
 * chip identification, and guests choose code paths from the architecture
 * mask. Add a row only with a source for its values -- see
 * docs/platforms.md.
 **/
#if !defined(INCLUDED_CPUMODEL_H_)
#define INCLUDED_CPUMODEL_H_

#include "StdAfx.hpp"

/// The core a part is built on: what decides the physical address map, how
/// the processor is identified and started, and where its interrupts come
/// from.
enum cpu_family {
  CPU_FAMILY_EV6, ///< 21264 and its shrinks (EV6, EV67, EV68)
  CPU_FAMILY_EV7, ///< 21364: an EV68 core with Rbox, Zbox and Cbox on chip;
                  ///< and the 21464 in EV7-compatible mode
};

/// What an opcode 18 function code with no instruction does. The Alpha
/// Architecture Reference Manual allows a trap or a no-op; the 21264 (and
/// so every EV6 and EV7 row) completes it as a no-op, the 21164 trapped
/// (docs/cpu-fidelity.md). An instruction a row adds in that space (the
/// EV8's, CPU_EXT_EV8) is decoded before this rule applies, so the rule
/// covers only the codes that are no instruction of the row; a native EV8
/// family would answer for itself.
inline bool cpu_family_unused_misc_is_nop(cpu_family f) {
  return f == CPU_FAMILY_EV6 || f == CPU_FAMILY_EV7;
}

/// Instructions a row has beyond its family's core (cpu_model::ext).
enum : u32 {
  CPU_EXT_NONE = 0,
  /// The 21464's (EV8's) user-visible additions [21464 IDS Rev 1.1k; the
  /// encodings and results Alphabox implements are docs/platforms/ev8-isa.md].
  /// Each stage of the plan (docs/platforms/ev8.md, section 6) decodes more
  /// of them under this flag, and the row's AMASK reports a feature only
  /// once every instruction it names executes. S2: CCB (18.EC00), CMOV2
  /// (11.68) and FCMOV2 (17.068); WH64EN and MAP_ALIGN are the core's
  /// already. S3: LDL_ARM (18.B000), LDQ_ARM (18.B400) and QUIESCE
  /// (18.B800), with one TPU per chip, and AMASK bit 10.
  CPU_EXT_EV8 = 0x0001,
};

struct cpu_model {
  const char *name; ///< configuration class, e.g. "ev68cb"
  const char *part; ///< the part, for messages, e.g. "21264CB (EV68CB)"
  u8 chip_id;       ///< I_CTL<29:24>, which the console turns into a name
  u8 type_major;    ///< processor type reported to software [ARM D-1..3]
  u8 type_minor;    ///< revision within the type
  u8 implver;       ///< IMPLVER: 2 EV6 family, 3 EV7 [ARM D-4], 4 EV8 [IDS]
  u64 amask;        ///< architecture extensions this part implements
  cpu_family family;
  u32 ext;                  ///< CPU_EXT_*: instructions beyond the family's
  u16 icache_kb, dcache_kb; ///< on-chip L1 caches
  u16 l2_kb;                ///< on-chip L2 (0: off chip, as on the EV6)
  /// Thread processing units per chip: 1, or the 21464's 4 [IDS p. 46]. A
  /// row with more than one takes the `tpus` key, 1 by default; with more,
  /// each TPU is a processor of its own (docs/platforms/ev8.md, S4).
  u8 max_tpus;
};

/// Architecture extension bits, as AMASK reports them [ARM D-4]. The EV8's
/// bits 3 (Tarantula) and 11 (its SIMD and paired-single sets) get their
/// names with the stage that implements them (docs/platforms/ev8-isa.md,
/// section 2); bit 10 has its with S3.
#define AMASK_BWX U64(0x0001)      ///< byte and word access
#define AMASK_FIX U64(0x0002)      ///< square root and FP conversions
#define AMASK_CIX U64(0x0004)      ///< count extension
#define AMASK_MVI U64(0x0100)      ///< multimedia instructions
#define AMASK_TRAP U64(0x0200)     ///< precise trap reporting
#define AMASK_PREFETCH U64(0x1000) ///< prefetch with modify intent
/// SMT: LDL_ARM, LDQ_ARM and QUIESCE [21464 IDS Rev 1.1k, PDF p. 668]
#define AMASK_SMT U64(0x0400)

/// The part named `name`, or nullptr.
const cpu_model *find_cpu_model(const char *name);

/// A test hook, ALPHABOX_CPU_ANY_BOARD=1: a board takes a processor of
/// another core family than its own. The CPU oracle (test/cpu) uses it to
/// run any row's instructions on the ES40, whose console it replaces; no
/// firmware or guest can run that way. Off by default, as every check that
/// refuses such a processor stays.
bool cpu_any_board();

#endif // !defined(INCLUDED_CPUMODEL_H_)
