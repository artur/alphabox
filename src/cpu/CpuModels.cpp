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
 * The Alpha processors of the EV6 family this emulator implements, the
 * EV7 and EV8 parts built on its core, and the EV8 on its own interface.
 *
 * The EV68CB is here because its values are established: the ES40 console
 * prints "Alpha EV68CB pass 4.0" from them, and the regression logs would
 * show any change. The other parts of the
 * family (EV6 21264, EV67 21264A, EV68AL, EV68DC) are rows waiting for
 * their values; a machine that ships with one of them -- the DS20E, for
 * instance -- adds its row with a source, and the console naming it
 * correctly is the check (docs/platforms/ds20e.md).
 **/
#include "StdAfx.hpp"

#include "CpuModel.hpp"

static const cpu_model models[] = {
    // 21264CB: chip identification 0x21, processor type 12 revision 6,
    // EV6-family IMPLVER, and every extension the part implements
    // [21264CB HRM 5-16, 2-38; ARM D-1..5].
    {"ev68cb", "21264CB (EV68CB)", 0x21, 12, 6, 2,
     AMASK_BWX | AMASK_FIX | AMASK_CIX | AMASK_MVI | AMASK_TRAP |
         AMASK_PREFETCH,
     CPU_FAMILY_EV6, CPU_EXT_NONE, 64, 64, 0, 1},
    // 21364 (EV7): the EV68 core, so the same extensions [inference: no
    // document lists the 21364's AMASK, and neither the console nor its two
    // PALcodes execute AMASK (E1, docs/platforms/marvel.md)], IMPLVER 3, the
    // value the Alpha Architecture Reference Manual assigns the 21364/EV7
    // and EV79 [ARM 4th ed., Table D-4], and 64 KB L1 caches. The chip ID
    // is what the console's PALcode turns into the processor type and
    // revision it reports (0x3ea94 in the
    // decompressed SRM V7.3-1): I_CTL<29:24>, plus bit 19 of CSR 0x28020 when
    // the chip ID is 2, indexes a table of (minor, 15) pairs, minor = index
    // + 1, and the console names minor 1-5 "EV7 rev 1.0", "2.0", "2.1",
    // "2.2" and "3.0". Chip ID 2 is "EV7 rev 2.1", type 15 minor 3 (Linux
    // hwrpb.h EV7_CPU is 15). The L2 is 1.75 MB, 7-way (Linux setup.c), but
    // console listings print 1.50 MB for revision 2 parts
    // (docs/platforms/marvel.md): six ways enabled, which the console reads
    // back from BBOX_CTL.
    {"ev7", "21364 (EV7)", 2, 15, 3, 3,
     AMASK_BWX | AMASK_FIX | AMASK_CIX | AMASK_MVI | AMASK_TRAP |
         AMASK_PREFETCH,
     CPU_FAMILY_EV7, CPU_EXT_NONE, 64, 64, 1536, 1},
    // EV7z, the same core at 1.15-1.3 GHz. A real ES47 7/1300's show
    // config calls it "EV7 rev 3.0" with 1.75 MB of cache: minor 5, which
    // only chip ID 4 reaches in the console's table [inference: no document
    // gives the chip ID]. The console reports it as type 15 (show cpu:
    // "Type Major 15, Minor 5"): Linux's EV79_CPU, 16, is not what this
    // console hands on, and the rows's type is for messages.
    {"ev7z", "21364 (EV7z)", 4, 15, 5, 3,
     AMASK_BWX | AMASK_FIX | AMASK_CIX | AMASK_MVI | AMASK_TRAP |
         AMASK_PREFETCH,
     CPU_FAMILY_EV7, CPU_EXT_NONE, 64, 64, 1792, 1},
    // 21464 (EV8) in EV7-compatible mode (docs/platforms/ev8.md, decision D1
    // and option A): the EV7's privileged interface, so the EV7 console and
    // its PALcodes run unchanged, with the 21464's identity and, as the
    // stages add them, its instructions (CPU_EXT_EV8). IMPLVER 4 [21464 IDS
    // Rev 1.1k, PDF p. 76]. AMASK: the IDS's mask is 0x1F07 [IDS p. 76], and
    // the row reports only what it implements, so that software testing
    // AMASK never uses a missing feature: the EV68 core's 0x1307 and bit 10
    // (SMT), whose LDx_ARM and QUIESCE it has since S3 [IDS pp. 668, 674],
    // 0x1707; bit 11 with the SIMD and paired-single sets (S5 and S6), bit 3
    // with Tarantula (S7, S8) [decision; docs/cpu-fidelity.md]. Chip ID 4,
    // the EV7z's, which the console names "EV7 rev 3.0", type 15 minor 5
    // [decision D2: the IDS gives CPU_CNFG<CHIP_ID> no value, p. 487, and
    // the console turns I_CTL<29:24> into a name through its EV7 table]. L1
    // 64 KB each and a 3 MB L2 [IDS pp. 42-43], of which BBOX_CTL, seven
    // 256 KB ways, shows the console 1.75 MB (chipsets/marvel/Ev7Csr.cpp).
    // Four TPUs [IDS p. 46]. One processor per chip by default, as the EV7
    // console would see the chip; `tpus = 2..4` lists TPUs 1-3 to the
    // Marvel console as processors of their own, an Alphabox construction
    // (S4; Configurator.cpp, processor_tpus).
    {"ev8", "21464 (EV8)", 4, 15, 5, 4,
     AMASK_BWX | AMASK_FIX | AMASK_CIX | AMASK_MVI | AMASK_TRAP | AMASK_SMT |
         AMASK_PREFETCH,
     CPU_FAMILY_EV7, CPU_EXT_EV8, 64, 64, 3072, 4},
    // 21464 (EV8) on its own privileged interface, the IDS's
    // (docs/platforms/ev8-native.md, S11, decision N16): a family of its
    // own, CPU_FAMILY_EV8, with the IDS's IPRs, PALcode instructions,
    // entries, modes, shadow registers and TB, and four TPUs per chip by
    // default. IMPLVER 4 [IDS p. 76]; the `ev8` row's AMASK and user
    // instructions, stage by stage, by the same rule (report what
    // executes). Type 18 minor 0: the HWRPB's processor type is written by
    // our console, and 18 is the next free value [decision D3; ARM pp.
    // 805-807]. Chip ID 0: CPU_CNFG<CHIP_ID> has no value [IDS p. 487], and
    // nothing reads it but our PALcode [decision N16]. The caches are the
    // `ev8` row's [IDS pp. 42-43]. The ES47, ES80 and GS1280 take it, in
    // 21464 mode (S11b, their rows' other_cpu_model); the CPU oracle runs it
    // on the ES40 through ALPHABOX_CPU_ANY_BOARD.
    {"ev8n", "21464 (EV8), native interface", 0, 18, 0, 4,
     AMASK_BWX | AMASK_FIX | AMASK_CIX | AMASK_MVI | AMASK_TRAP | AMASK_SMT |
         AMASK_PREFETCH,
     CPU_FAMILY_EV8, CPU_EXT_EV8, 64, 64, 3072, 4},
};

const cpu_model *find_cpu_model(const char *name) {
  for (const cpu_model &m : models)
    if (!strcmp(m.name, name))
      return &m;
  return nullptr;
}

bool cpu_any_board() {
  static const bool on = [] {
    const char *e = getenv("ALPHABOX_CPU_ANY_BOARD");
    return e && *e && strcmp(e, "0") != 0;
  }();
  return on;
}
