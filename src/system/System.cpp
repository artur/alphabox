/* Alphabox Alpha Emulator
 * Copyright (C) 2020 Tomáš Glozar
 * Copyright (C) 2026 Artur Goulão
 * Website: https://github.com/lenticularis39/axpbox
 *          https://github.com/artur/alphabox
 *
 * Forked from: ES40 emulator
 * Copyright (C) 2007-2008 by the ES40 Emulator Project
 * Copyright (C) 2007 by Camiel Vanderhoeven
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
 *
 * Although this is not required, the author would appreciate being notified of,
 * and receiving any modifications you may make to the source code that might
 * serve the general public.
 */

#include "System.hpp"
#include "AlphaCPU.hpp"
#include "DPR.hpp"
#include "Flash.hpp"
#include "StdAfx.hpp"
#include "Tsunami.hpp"
#include "lockstep.hpp"
#if defined(__APPLE__) && defined(HAVE_SDL)
#include "gui/gui.hpp" // bx_gui->main_thread_pump()
#endif

#include <ctype.h>
#include <map>
#include <mutex>
#include <signal.h>
#include <stdlib.h>
#include <unordered_map>

#include <thread>
#ifdef ALPHABOX_HVF
#include "HvRuntime.hpp"
#endif

#define CLOCK_RATIO 10000

#if defined(LS_MASTER) || defined(LS_SLAVE)
char debug_string[10000] = "";
char *dbg_strptr = debug_string;
#endif

/**
 * Constructor.
 **/
CSystem::CSystem(CConfigurator *cfg) try {
  if (theSystem != 0)
    FAILURE(Configuration, "More than one system");
  theSystem = this;
  myCfg = cfg;

  iNumComponents = 0;
  iNumMemories = 0;
  iNumCPUs = 0;
  m_platform =
      find_platform(myCfg->get_text_value("platform", DEFAULT_PLATFORM));
  if (!m_platform)
    FAILURE_1(Configuration, "Unknown platform %s",
              myCfg->get_text_value("platform", DEFAULT_PLATFORM));
  // Decided before the processors are built, which read it (Platform.hpp).
  if (!m_platform->native_vmspal)
    request_native_pal("this board's console PALcode is not the ES40's");

  iNumMemoryBits = (int)myCfg->get_num_value("memory.bits", false, 27);
  // How much memory the board holds, from its descriptor: the ES40 takes
  // 64 MB (the smallest four-DIMM set the SPD model describes) to 32 GB
  // (four Typhoon arrays of 8 GB each).
  if (iNumMemoryBits < m_platform->min_memory_bits ||
      iNumMemoryBits > m_platform->max_memory_bits)
    FAILURE_3(Configuration, "memory.bits must be between %d and %d on the %s",
              m_platform->min_memory_bits, m_platform->max_memory_bits,
              m_platform->description);
  m_exit_on_pal_halt = myCfg->get_bool_value("exit_on_pal_halt", false);

  // The board's chipset: everything that is neither memory nor a device
  // range goes to it (Chipset.hpp).
  m_chipset = create_chipset(m_platform->chipset, this);
  m_phys_mask = m_chipset->phys_mask();

  //  iNumConfig = 0;
#if defined(IDB)
  iSingleStep = 0;
  iSSCycles = 0;
#endif
  state.cpu_lock_flags = 0;

  // A host whose size_t can't hold the memory size (32-bit) must refuse it
  // rather than allocate less.
  if (iNumMemoryBits >= sizeof(size_t) * 8)
    FAILURE(Configuration, "memory.bits is too large for this host");
  CHECK_ALLOCATION(memory = alloc_guest_memory((size_t)1 << iNumMemoryBits));
  init_code_page_map();

  printf("%s(%s): $Id: System.cpp,v 1.79 2008/06/12 07:29:44 iamcamiel Exp $\n",
         cfg->get_myName(), cfg->get_myValue());
} catch (...) {
  // A constructor that throws leaves no object behind: don't leave theSystem
  // pointing at it for main_sim's failure handler (the exception propagates).
  if (theSystem == this)
    theSystem = 0;
}

/**
 * Destructor. Calls the destructors for registered devices, and
 * frees used memory.
 **/
CSystem::~CSystem() {
  int i;

  printf("Freeing memory in use by system...\n");

  for (i = 0; i < iNumComponents; i++)
    delete acComponents[i];

  for (i = 0; i < iNumMemories; i++)
    free(asMemories[i]);

  free(memory);
  delete m_chipset;
}

/**
 * free memory, and allocate and clear new memory.
 **/
void CSystem::ResetMem(unsigned int membits) {
  free_guest_memory(memory, (size_t)1 << iNumMemoryBits);
  iNumMemoryBits = membits;
  CHECK_ALLOCATION(memory = alloc_guest_memory((size_t)1 << iNumMemoryBits));
  init_code_page_map();
}

/**
 * Guest DRAM. Under ALPHABOX_HV=1 the processor's dispatch loop runs inside
 * a VM and the device threads stay outside, so the array both of them read
 * and write has to be memory the two genuinely share: the framework's own
 * allocator, mapped into the VM at its own address. Everywhere else it is
 * an ordinary zeroed allocation.
 **/
#ifdef ALPHABOX_HVF
void *CSystem::operator new(size_t n) {
  if (hv::enabled()) {
    if (void *p = hv::alloc(n))
      return p;
  }
  return ::operator new(n);
}
void CSystem::operator delete(void *p) noexcept {
  if (hv::enabled())
    return; // the VM's allocator releases everything at exit
  ::operator delete(p);
}
#endif

void CCodePageMap::init(CSystem *sys, u64 dram_bytes, void *page_bits,
                        void *line_bits) {
  m_sys = sys;
  m_pages = dram_bytes >> kPageShift;
  m_lines = dram_bytes >> kLineShift;
  m_page_bits = (u8 *)page_bits;
  m_line_bits = (u8 *)line_bits;
  m_trace = getenv("ALPHABOX_TRACE_CODEWRITE") != nullptr;
  m_gen.store(0, std::memory_order_relaxed);
  m_marked.store(0, std::memory_order_relaxed);
}

void CCodePageMap::trace_write(u64 phys) {
  static std::mutex m;
  static std::map<u64, u64> seen; // by 256-byte line
  static u64 n = 0;
  std::lock_guard<std::mutex> g(m);
  seen[phys >> kLineShift]++;
  if ((++n % 1000000) == 0) {
    printf("[CODEWRITE] %llu writes onto compiled lines, %zu lines:",
           (unsigned long long)n, seen.size());
    int k = 0;
    for (auto it = seen.begin(); it != seen.end() && k < 10; ++it, ++k)
      printf(" %llx(%llu)", (unsigned long long)(it->first << kLineShift),
             (unsigned long long)it->second);
    printf("\n");
    fflush(stdout);
  }
}

void CCodePageMap::note_code(u64 phys, size_t bytes) {
  if (!m_page_bits)
    return;
  const u64 last = phys + (bytes ? bytes - 1 : 0);
  for (u64 ln = phys >> kLineShift; ln <= (last >> kLineShift); ++ln) {
    if (ln >= m_lines)
      return;
    __atomic_fetch_or(&m_line_bits[ln >> 3], (u8)(1u << (ln & 7)),
                      __ATOMIC_RELAXED);
  }
  for (u64 pg = phys >> kPageShift; pg <= (last >> kPageShift); ++pg) {
    if (pg >= m_pages)
      return;
    const u8 bit = (u8)(1u << (pg & 7));
    // The processor that sets the bit is the one that announces the page;
    // whoever loses the race has nothing to announce.
    const u8 was =
        __atomic_fetch_or(&m_page_bits[pg >> 3], bit, __ATOMIC_RELAXED);
    if (!(was & bit)) { // a page that holds code keeps the slower write path
      m_marked.fetch_add(1, std::memory_order_relaxed);
      // Whatever was written to this page before we looked was written
      // while it was not yet code, so make the next flush do its work
      // rather than trust a count taken before this block existed.
      m_gen.fetch_add(1, std::memory_order_release);
      if (m_sys)
        m_sys->request_code_page_flush();
    }
  }
}

/// A device wrote a range. Pages are the cheap filter -- a transfer is
/// usually nowhere near code -- and only a page that holds some go on to
/// the lines.
void CCodePageMap::note_write_range(u64 phys, size_t bytes) {
  if (!m_page_bits || !bytes)
    return;
  const u64 last = phys + bytes - 1;
  for (u64 pg = phys >> kPageShift; pg <= (last >> kPageShift); ++pg) {
    if (!holds_code(pg << kPageShift))
      continue;
    const u64 from = (pg << kPageShift) > phys ? (pg << kPageShift) : phys;
    const u64 to = ((pg + 1) << kPageShift) - 1 < last
                       ? ((pg + 1) << kPageShift) - 1
                       : last;
    for (u64 ln = from >> kLineShift; ln <= (to >> kLineShift); ++ln)
      if (holds_code_line(ln << kLineShift)) {
        m_gen.fetch_add(1, std::memory_order_release);
        return;
      }
  }
}

/// One bit per page, from the same allocator as guest memory: the dispatch
/// loop reads it inside the VM and the device threads write to it outside.
void CSystem::init_code_page_map() {
  if (m_code_page_bits) { // a memory resize: the old maps sized the old DRAM
    free_guest_memory(m_code_page_bits, m_code_page_bytes);
    free_guest_memory(m_code_line_bits, m_code_line_bytes);
  }
  const size_t page_bytes =
      ((size_t)1 << (iNumMemoryBits - CCodePageMap::kPageShift)) / 8 + 1;
  const size_t line_bytes =
      ((size_t)1 << (iNumMemoryBits - CCodePageMap::kLineShift)) / 8 + 1;
  void *pb = alloc_guest_memory(page_bytes);
  void *lb = alloc_guest_memory(line_bytes);
  CHECK_ALLOCATION(pb);
  CHECK_ALLOCATION(lb);
  m_code_page_bits = pb;
  m_code_line_bits = lb;
  m_code_page_bytes = page_bytes;
  m_code_line_bytes = line_bytes;
  m_code_pages.init(this, (u64)1 << iNumMemoryBits, pb, lb);
}

void *CSystem::alloc_guest_memory(size_t bytes) {
#ifdef ALPHABOX_HVF
  if (hv::enabled()) {
    void *p = hv::alloc(bytes);
    if (p) {
      memset(p, 0, bytes);
      printf("%%SYS-I-HVMEM: %zu MB of guest memory shared with the VM\n",
             bytes >> 20);
      return p;
    }
    printf("%%SYS-W-HVMEM: the VM's allocator could not provide %zu MB; the "
           "guest's memory will not be visible inside\n",
           bytes >> 20);
  }
#endif
  return calloc(bytes, 1);
}

void CSystem::free_guest_memory(void *p, size_t bytes) {
#ifdef ALPHABOX_HVF
  if (hv::enabled())
    return; // alloc() memory is released when the VM goes away
  (void)bytes;
#else
  (void)bytes;
#endif
  free(p);
}

/**
 * Register a device.
 **/
void CSystem::RegisterComponent(CSystemComponent *component) {
  acComponents[iNumComponents] = component;
  iNumComponents++;
}

void CSystem::UnregisterComponent(CSystemComponent *component) {
  iNumComponents--;
}

/**
 * Get the number of bits that corresponds to the amount of RAM installed.
 * (e.g. 28 = 256 MB, 29 = 512 MB, 30 = 1 GB)
 **/
unsigned int CSystem::get_memory_bits() { return iNumMemoryBits; }

/**
 * Obtain a pointer to system memory.
 **/
char *CSystem::PtrToMem(u64 address) {
  if (address >> iNumMemoryBits) // Non Memory
    return 0;

  return (char *)memory + address;
}

/**
 * Register a device as being a CPU. Return the CPU number.
 **/
void CSystem::set_direct_memory(u64 base, u64 size, u8 *host) {
  // Withdraw first (end = 0: no CPU can map a page from here on), publish
  // the new triple with the end last, then have every CPU drop what it
  // cached under the old one.
  m_direct_end.store(0, std::memory_order_release);
  m_direct_base = base;
  m_direct_host = host;
  if (size)
    m_direct_end.store(base + size, std::memory_order_release);
  for (int i = 0; i < iNumCPUs; i++)
    acCPUs[i]->request_dpc_flush();
}

void CSystem::request_code_page_flush() {
  for (int i = 0; i < iNumCPUs; i++)
    acCPUs[i]->request_dpc_flush();
}

int CSystem::RegisterCPU(class CAlphaCPU *cpu) {
  // The board says how many processors it takes, and nobody checked: the
  // number this returns is used as an index straight away
  // (cpu_lock_address[n], m_ll_seq_snap[n]), so a fifth cpuN block in the
  // configuration used to write outside the arrays with -1. Refuse it the
  // way an impossible memory size is refused.
  const int board_max =
      m_platform && m_platform->max_cpus > 0 ? m_platform->max_cpus : 4;
  const int hard_max = (int)(sizeof(acCPUs) / sizeof(acCPUs[0]));
  const int limit = board_max < hard_max ? board_max : hard_max;
  if (iNumCPUs >= limit)
    FAILURE_2(Configuration, "this machine takes at most %d processors (%s)",
              limit, m_platform ? m_platform->description : "unknown board");
  acCPUs[iNumCPUs] = cpu;
  iNumCPUs++;
  return iNumCPUs - 1;
}

/**
 * Reserve a range of the 64-bit system address space for a device.
 **/
int CSystem::RegisterMemory(CSystemComponent *component, int index, u64 base,
                            u64 length) {
  struct SMemoryUser *m;
  int i;

#if defined(CHECK_MEM_RANGES)
  for (i = 0; i < iNumMemories; i++) {
    if (component == asMemories[i]->component)
      continue;

    // check for overlaps
    if (base >= asMemories[i]->base &&
        base <= (asMemories[i]->base + asMemories[i]->length - 1)) {
      printf(
          "WARNING: Start address for %s/%d (%016" PRIx64 "-%016" PRIx64 ")\n"
          "  is within memory range of %s/%d (%016" PRIx64 "-%016" PRIx64
          ").\n",
          component->devid_string, index, base, base + length - 1,
          asMemories[i]->component->devid_string, asMemories[i]->index,
          asMemories[i]->base, asMemories[i]->base + asMemories[i]->length - 1);
    }

    if (base + length - 1 >= asMemories[i]->base &&
        base + length - 1 <=
            (asMemories[i]->base + asMemories[i]->length - 1)) {
      printf("WARNING: End address for %s/%d (%016" PRIx64 "-%016" PRIx64 ")\n"
             "  is within memory range of %s/%d (%016" PRIx64 "-%016" PRIx64
             ").\n",
             component->devid_string, index, base, base + length - 1,
             asMemories[i]->component->devid_string, asMemories[i]->index,
             asMemories[i]->base,
             asMemories[i]->base + asMemories[i]->length - 1);
    }
  }
#endif // defined(CHECK_MEM_RANGES)

  for (i = 0; i < iNumMemories; i++) {
    if ((asMemories[i]->component == component) &&
        (asMemories[i]->index == index)) {
      asMemories[i]->base = base;
      asMemories[i]->length = length;
      aMemoryBounds[i].base = base;
      aMemoryBounds[i].end = base + length;
      return 0;
    }
  }

  if (iNumMemories == MAX_COMPONENTS)
    FAILURE_2(Configuration,
              "Out of memory ranges (%d) adding %s: raise MAX_COMPONENTS",
              MAX_COMPONENTS, component->devid_string);

  CHECK_ALLOCATION(
      m = (struct SMemoryUser *)malloc(sizeof(struct SMemoryUser)));
  m->component = component;
  m->base = base;
  m->length = length;
  m->index = index;

  asMemories[iNumMemories] = m;
  aMemoryBounds[iNumMemories].base = base;
  aMemoryBounds[iNumMemories].end = base + length;
  iNumMemories++;

  // If this range carries a bulk data register, record its absolute
  // address so the processor can serve the transfer without leaving the
  // VM (see SBulkPort). A range that is re-registered at a new address
  // replaces its old entry; a stale one simply never matches again.
  SBulkPort bp;
  if (component->get_bulk_port(index, &bp) && bp.data) {
    const u64 addr = base + bp.offset;
    int slot = -1;
    for (int k = 0; k < m_nbulk; k++)
      if (m_bulk[k].d.data == bp.data)
        slot = k;
    if (slot < 0 && m_nbulk < kMaxBulkPorts)
      slot = m_nbulk++;
    if (slot >= 0) {
      m_bulk[slot].addr = addr;
      m_bulk[slot].d = bp;
    }
  }
  return 0;
}

CTsunami *CSystem::tsunami() const {
  if (m_chipset->kind() != CHIPSET_TSUNAMI)
    FAILURE_1(Logic, "this machine's chipset is the %s, not the Tsunami",
              m_chipset->name());
  return static_cast<CTsunami *>(m_chipset);
}

void CSystem::reset_pci_devices() {
  for (int i = 0; i < iNumComponents; i++)
    acComponents[i]->ResetPCI();
}

/**
 * Processors other than the first, on a machine whose console expects them
 * to be running already.
 *
 * The ES40's console starts them itself, through the management processor,
 * and they wait until it does. A board without one -- the DS20E -- instead
 * asserts a processor's halt line and waits for it to answer: its console
 * writes the halt register for processor 1 and gives up when nothing
 * replies. Such a processor is released here at the PALcode reset entry,
 * which is where the ES40's management processor puts one too.
 *
 * (Found by tracing the console's own attempt: docs/platforms/ds20e.md.)
 *
 * They are not released at once. Every processor runs the same PALcode
 * reset, and the first to get there clears the Cchip's arbitration
 * (MISC<ACL>) and becomes the console's primary; releasing all of them
 * together left that to the order in which the host started their threads,
 * and one of the first seven two-processor DS20E boots came up on
 * processor 1 (`P01>>>`). So the others wait until processor 0 has cleared
 * the arbitration itself (arbitration_cleared), which makes it the primary, as
 * on a machine whose processor 0 leaves reset first. That ordering is a
 * modelling choice: what a real DS20E's reset logic does is not known.
 */
void CSystem::start_secondaries() {
  if (m_platform->console_starts_secondaries || iNumCPUs < 2)
    return;
  m_secondaries_pending = true;
}

void CSystem::release_secondaries() {
  if (!m_secondaries_pending.exchange(false))
    return;
  for (int i = 1; i < iNumCPUs; i++) {
    if (!acCPUs[i]->get_waiting())
      continue;
    // The reset entry is PAL_BASE, with the PALmode bit. It was written as
    // 0x8001 because the ES40's decompressed firmware puts PALcode at
    // 0x8000, which is true of that board and not a rule.
    const u64 entry = acCPUs[i]->get_pal_base() | U64(1);
    printf("%%SYS-I-SECONDARY: releasing CPU %d at the PALcode reset entry "
           "(%016llx).\n",
           i, (unsigned long long)entry);
    acCPUs[i]->set_pc(entry);
    acCPUs[i]->stop_waiting();
  }
}

void CSystem::arbitration_cleared(int cpu) {
  // On a board whose processors all run from reset, the others start now
  // that processor 0 is the one that cleared it.
  if (cpu == 0 && m_secondaries_pending)
    release_secondaries();
}

bool CSystem::trace_mp_on() {
  static const bool on = getenv("ALPHABOX_TRACE_MP") != nullptr;
  return on;
}

void CSystem::trace_mp(const char *what, u32 reg, u64 value) {
  if (!trace_mp_on())
    return;
  printf("%%SYS-T-MP: %s %08x = %016" PRIx64 "%s\n", what, reg, value,
         t_running_cpu ? "" : " (device)");
  if (t_running_cpu)
    printf("            from cpu%d pc=%016" PRIx64 "\n",
           t_running_cpu->get_cpuid(), t_running_cpu->get_pc());
}

bool CSystem::trace_unknown_on() {
  static const bool on = getenv("ALPHABOX_TRACE_UNKNOWN") != nullptr;
  return on;
}

void CSystem::trace_unknown(const char *space, u64 address, int dsize,
                            bool write, u64 data, CSystemComponent *source) {
  if (!trace_unknown_on())
    return;

  // A poll repeats one access millions of times (Marvel's console waits on
  // an EV7 register for 2^28 reads), and printing each one makes the wait
  // last ten minutes. The first three are printed, then every power of two
  // with its count -- a repeated read still shows the firmware is waiting.
  u64 repeats = 1;
  {
    static std::mutex m;
    static std::unordered_map<u64, u64> seen;
    const u64 pc = t_running_cpu ? t_running_cpu->get_pc() : 0;
    const u64 key = (address * U64(0x9e3779b97f4a7c15)) ^ (pc << 8) ^
                    ((u64)dsize << 1) ^ (write ? 1 : 0);
    std::lock_guard<std::mutex> lock(m);
    if (seen.size() > 1000000)
      seen.clear();
    repeats = ++seen[key];
  }
  if (repeats > 3 && (repeats & (repeats - 1)) != 0)
    return;

  // A processor's own access says which instruction made it; a device's
  // says which device.
  char from[128] = "";
  if (repeats > 3)
    snprintf(from, sizeof(from), " [%" PRIu64 " times]", repeats);
  if (t_running_cpu)
    // The return address too: firmware reaches hardware through access
    // helpers, so the instruction is rarely the interesting caller.
    snprintf(from + strlen(from), sizeof(from) - strlen(from),
             " from cpu%d pc=%016" PRIx64 " ra=%016" PRIx64,
             t_running_cpu->get_cpuid(), t_running_cpu->get_pc(),
             t_running_cpu->get_r(26, true));
  else if (source)
    snprintf(from + strlen(from), sizeof(from) - strlen(from), " from %s",
             source->devid_string);

  if (write)
    printf("%%SYS-T-UNKNOWN: write %2d bits to %011" PRIx64
           " (%s) = %016" PRIx64 "%s\n",
           dsize, address, space, data, from);
  else
    printf("%%SYS-T-UNKNOWN: read  %2d bits at %011" PRIx64 " (%s)%s\n", dsize,
           address, space, from);
}

volatile sig_atomic_t got_sigint = 0;
// SIGUSR1: take a snapshot of the whole machine at the next safe point in
// Run() -- threads stopped, exactly as the serial console's <BREAK> menu does
// it. ALPHABOX_SNAPSHOT names the file (default autosave.axp), and
// ALPHABOX_SNAPSHOT_EXIT=1 exits right after saving, so that the disk images
// on exit are the disks the snapshot saw: no guest instruction runs between
// the two. That pair -- snapshot plus disks -- is what ALPHABOX_RESTORE
// resumes from, and it is what lets a benchmark skip the guest's two-minute
// boot.
volatile sig_atomic_t got_sigusr1 = 0;

/**
 * Handle SIGINT (Ctrl-C) or SIGTERM by setting a flag that makes the main loop
 * exit gracefully (threads stopped, flash and DPR saved).
 **/
void sigint_handler(int signum) { got_sigint = 1; }
void sigusr1_handler(int signum) { got_sigusr1 = 1; }

/**
 * Run the system by clocking the CPU(s) and devices.
 **/
void CSystem::Run() {
  int i;

  int k;

#if defined(DUMP_MEMMAP)
  printf("Alphabox Memory Map\n");
  printf("Physical Address Size     Device/Index\n");
  printf("---------------- -------- -------------------------\n");
  for (i = 0; i < iNumMemories; i++) {
    printf("%016" PRIx64 " %8x %s/%d\n", asMemories[i]->base,
           asMemories[i]->length, asMemories[i]->component->devid_string,
           asMemories[i]->index);
  }
#endif // defined(DUMP_MEMMAP)

  /* catch CTRL-C and SIGTERM and shut down gracefully */
  signal(SIGINT, &sigint_handler);
  signal(SIGTERM, &sigint_handler);
  signal(SIGUSR1, &sigusr1_handler);

  // ALPHABOX_RESTORE=<file>: resume a saved machine instead of cold-booting.
  // Every component has been constructed and initialised by now and no
  // thread runs yet, which is the one moment the whole state can be replaced
  // consistently. The disk images in the working directory must be the ones
  // the snapshot was taken against (see ALPHABOX_SNAPSHOT_EXIT).
  if (const char *snap = getenv("ALPHABOX_RESTORE")) {
    printf("%%SYS-I-RESTORE: resuming from %s\n", snap);
    RestoreState(snap);
  }

  start_threads();

  for (k = 0;; k++) {
    if (got_sigint) {
      // A snapshot of guest memory for inspection: where the firmware put
      // something is often the only way to find what it expects
      // (docs/platforms.md). Off unless asked for.
      if (getenv("ALPHABOX_DUMP_MEMORY")) {
        printf("%%SYS-I-MEMDUMP: writing memory_000000000000.dmp.\n");
        DumpMemory(0);
      }
      FAILURE(Graceful, "CTRL-C or SIGTERM detected");
    }

    if (m_pal_halt_exit.load(std::memory_order_relaxed))
      FAILURE(Graceful, "HALT invoked, exit_on_pal_halt configured");

    if (got_sigusr1) {
      got_sigusr1 = 0;
      const char *snap = getenv("ALPHABOX_SNAPSHOT");
      if (!snap)
        snap = "autosave.axp";
      stop_threads();
      printf("%%SYS-I-SNAPSHOT: saving machine state to %s\n", snap);
      SaveState(snap);
      if (getenv("ALPHABOX_SNAPSHOT_EXIT"))
        FAILURE(Graceful, "snapshot taken, exiting as asked");
      start_threads();
    }

    if (ProcessPendingReset())
      continue;

#if defined(__APPLE__) && defined(HAVE_SDL)
    // macOS: SDL windowing only works on the main thread, so pump the GUI
    // here (~100 Hz) between the device state checks (see gui/sdl.cpp).
    for (int pump = 0; pump < 10; pump++) {
      if (bx_gui)
        bx_gui->main_thread_pump();
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
#else
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
#endif
    for (i = 0; i < iNumComponents; i++)
      acComponents[i]->check_state();
#if !defined(HIDE_COUNTER)
#if defined(PROFILE)
    printf("%d | %016" PRIx64 " | %" PRId64 " profiled instructions.  \r", k,
           acCPUs[0]->get_pc(), profiled_insts);
#else  // defined(PROFILE)
    printf("%d | %016" PRIx64 "\r", k, acCPUs[0]->get_pc());
#endif // defined(PROFILE)
#endif // defined(HIDE_COUNTER)
  }

  //  printf ("%%SYS-W-SHUTDOWN: CTRL-C or Device Failed\n");
  //  return 1;
}

/**
 * Do one clock tick. The cpu(s) will execute one single instruction, and
 * some devices may be clocked.
 **/
int CSystem::SingleStep() {
  for (int i = 0; i < iNumCPUs; i++)
    if (!acCPUs[i]->get_waiting())
      acCPUs[i]->execute();

      //  iSingleStep++;
#if defined(LS_MASTER) || defined(LS_SLAVE)
  if (!(iSingleStep % 50)) {
    lockstep_sync_m2s("sync1");
    *dbg_strptr = '\0';
    lockstep_compare(debug_string);
    dbg_strptr = debug_string;
    *dbg_strptr = '\0';
  }
#endif // defined(LS_MASTER) || defined(LS_SLAVE)

  //  if (iSingleStep >= CLOCK_RATIO)
  //  {
  //     iSingleStep = 0;
  //     for(i=0;i<iNumSlowClocks;i++)
  //     {
  //        result = acSlowClocks[i]->DoClock();
  //      if (result)
  //        return result;
  //     }
  //#ifdef IDB
  //     iSSCycles++;
  //#if !defined(LS_SLAVE)
  //     if (bHashing)
  //#endif
  //       printf("%d | %016" PRIx64 "\r",iSSCycles,acCPUs[0]->get_pc());
  //#endif
  //  }
  return 0;
}

#if defined(DEBUG_PORTACCESS)
u64 lastport;
#endif // defined(DEBUG_PORTACCESS)
// EV6/EV68 Dcache blocks are 64 bytes; LDx_L/STx_C monitor that cache line.
#define CPU_LOCK_MATCH_MASK U64(0x00000807ffffffc0)
#define CPU_LOCK_IO_MASK U64(0x0000080000000000)

static inline bool cpu_lock_matches(u64 locked_address, u64 address) {
  return !((locked_address ^ address) & CPU_LOCK_MATCH_MASK);
}

static constexpr u32 CPU_LLSC_DMA_WRITER = 0x80000000U;
static constexpr u32 CPU_LLSC_DMA_READERS = 0x7fffffffU;

// EV68 HRM 4.6 requires an external writer to invalidate the locked cache
// line. The gate makes the invalidating probe and RAM write atomic with
// respect to the load/publish and test/store portions of LDx_L/STx_C.
CSystem::CLLSCDRAMGuard::CLLSCDRAMGuard(CSystem *sys, bool active)
    : system(active ? sys : nullptr) {
  if (system)
    system->cpu_llsc_enter();
}

CSystem::CLLSCDRAMGuard::~CLLSCDRAMGuard() {
  if (system)
    system->cpu_llsc_leave();
}

CSystem::CPCIDMAWriteGuard::CPCIDMAWriteGuard(CSystem *sys, bool active)
    : system(active ? sys : nullptr) {
  if (system)
    system->pci_dma_write_enter();
}

CSystem::CPCIDMAWriteGuard::~CPCIDMAWriteGuard() {
  if (system) {
    // Again on the way out: invalidate() runs before the transfer, so a
    // processor that flushed its instruction cache while the bytes were
    // still arriving would have counted a write that had not happened yet.
    if (touched_code)
      system->m_code_pages.note_write_all();
    system->pci_dma_write_leave();
  }
}

void CSystem::CPCIDMAWriteGuard::invalidate(u64 address, size_t bytes) {
  if (system) {
    system->cpu_clear_external_locks(address, bytes);
    // A device writing into a page some block was compiled from changes code
    // just as a store does -- a driver paged in over an old one is exactly
    // that -- and the processor never sees it.
    const u64 before = system->m_code_pages.write_gen();
    system->m_code_pages.note_write_range(address, bytes);
    if (system->m_code_pages.write_gen() != before)
      touched_code = true;
  }
}

void CSystem::cpu_llsc_enter() {
  u32 gate = cpu_llsc_dma_gate.load(std::memory_order_acquire);
  for (;;) {
    if (!(gate & CPU_LLSC_DMA_WRITER) &&
        (gate & CPU_LLSC_DMA_READERS) != CPU_LLSC_DMA_READERS &&
        cpu_llsc_dma_gate.compare_exchange_weak(gate, gate + 1,
                                                std::memory_order_acquire,
                                                std::memory_order_relaxed))
      return;
    std::this_thread::yield();
    gate = cpu_llsc_dma_gate.load(std::memory_order_acquire);
  }
}

void CSystem::cpu_llsc_leave() {
  cpu_llsc_dma_gate.fetch_sub(1, std::memory_order_release);
}

void CSystem::pci_dma_write_enter() {
  u32 gate = cpu_llsc_dma_gate.load(std::memory_order_acquire);
  for (;;) {
    if (!(gate & CPU_LLSC_DMA_WRITER) &&
        cpu_llsc_dma_gate.compare_exchange_weak(
            gate, gate | CPU_LLSC_DMA_WRITER, std::memory_order_acquire,
            std::memory_order_relaxed))
      break;
    std::this_thread::yield();
    gate = cpu_llsc_dma_gate.load(std::memory_order_acquire);
  }

  while (cpu_llsc_dma_gate.load(std::memory_order_acquire) &
         CPU_LLSC_DMA_READERS)
    std::this_thread::yield();
}

void CSystem::pci_dma_write_leave() {
  cpu_llsc_dma_gate.store(0, std::memory_order_release);
}

// --- Load-locked / store-conditional (HRM 4.2) -----------------------------
// Keep the original CAS-backed model for same-address LL/SC sequences, because
// it provides the emulator's MP atomicity. Some Alpha code stores
// conditionally to a different quadword in the same locked cache line; those
// must not compare against the value loaded from the LDx_L address.

void CSystem::cpu_lock(int cpuid, u64 address, u64 value) {
  state.cpu_lock_address[cpuid] = address;
  cpu_lock_value[cpuid] = value;
  // ABA guard: remember the line's STx_C sequence as of this LDx_L.
  m_ll_seq_snap[cpuid] =
      m_ll_seq[(u32)((address >> 6) & (kLLBuckets - 1))].load(
          std::memory_order_acquire);
  state.cpu_lock_flags |= (1 << cpuid); // atomic fetch_or
}

/**
 * STx_C: consume this CPU's lock and perform the conditional store.
 *
 * Returns 1 when the store happened. The caller holds the LL/SC reader guard
 * (CLLSCDRAMGuard), which keeps a DMA write from slipping between the checks
 * and the store.
 **/
u64 CSystem::cpu_stx_c(int cpuid, u64 phys, int size_bits, u64 value,
                       char *dram, u64 dram_sz, CSystemComponent *source) {
  u64 expected = 0;
  bool same_address = false;
  if (!cpu_take_lock(cpuid, phys, &expected, &same_address))
    return 0; // reservation lost -> SC fails

  if (phys >= dram_sz) {
    WriteMem(phys, size_bits, value, source); // I/O-space conditional store
    return 1;
  }

  const u32 b = (u32)((phys >> 6) & (kLLBuckets - 1));
  for (int spins = 0; m_ll_lock[b].exchange(1, std::memory_order_acquire);
       spins++)
    if ((spins & 0x3f) == 0x3f)
      std::this_thread::yield();
  u64 ok;
  if (m_ll_seq[b].load(std::memory_order_relaxed) != m_ll_seq_snap[cpuid])
    ok = 0; // another STx_C wrote this line since our LDx_L (ABA)
  else if (same_address)
  {
    ok = dram_cas(dram, phys, expected, value, size_bits) ? 1 : 0;
    if (ok)
      m_code_pages.note_write(phys); // the ordinary LDx_L/STx_C pair writes
                                     // here, and a store is a store
  }
  else {
    // STx_C to another quadword of the locked line: no value to compare.
    dram_write(dram, phys, size_bits, value);
    m_code_pages.note_write(phys); // a conditional store can change code too
    ok = 1;
  }
  if (ok)
    m_ll_seq[b].fetch_add(1, std::memory_order_relaxed);
  m_ll_lock[b].store(0, std::memory_order_release);
  return ok;
}

bool CSystem::cpu_take_lock(int cpuid, u64 address, u64 *expected,
                            bool *same_address) {
  // I/O-space conditional stores have no cache line to watch; treat as held.
  bool held = (address & CPU_LOCK_IO_MASK) ||
              ((state.cpu_lock_flags.load() & (1 << cpuid)) &&
               cpu_lock_matches(state.cpu_lock_address[cpuid], address));

  // STx_C always consumes this CPU's lock, success or fail.
  state.cpu_lock_flags &= ~(1 << cpuid); // atomic fetch_and
  if (held) {
    *expected = cpu_lock_value[cpuid];
    *same_address = (state.cpu_lock_address[cpuid] == address);
  }
  return held;
}

/**
 * Drop one CPU's load lock. Called when that CPU takes an exception or
 * interrupt (HRM 4.2.4: a pending STx_C must fail if an exception/interrupt
 * intervened).
 **/
void CSystem::cpu_clear_lock(int cpuid) {
  state.cpu_lock_flags &= ~(1 << cpuid); // atomic fetch_and
}

/** Model the EV68 invalidating probe for every reservation line touched by
 * DMA. **/
void CSystem::cpu_clear_external_locks(u64 address, size_t bytes) {
  if (!bytes)
    return;

  const u64 first_line = (address & U64(0x00000807ffffffff)) & ~U64(63);
  const u64 last_line =
      ((address & U64(0x00000807ffffffff)) + bytes - 1) & ~U64(63);
  int clear_mask = 0;
  const int flags = state.cpu_lock_flags.load(std::memory_order_relaxed);
  for (int i = 0; i < iNumCPUs; i++) {
    if (!(flags & (1 << i)))
      continue;
    const u64 locked_line = state.cpu_lock_address[i] & CPU_LOCK_MATCH_MASK;
    if (locked_line >= first_line && locked_line <= last_line)
      clear_mask |= 1 << i;
  }
  if (clear_mask)
    state.cpu_lock_flags.fetch_and(~clear_mask, std::memory_order_relaxed);
}

/**
 * \brief Write 8, 4, 2 or 1 byte(s) to a 64-bit system address. This could be
 *memory, a registered device range, or what the chipset decodes itself
 *(CChipset::write_io: its registers, PCI and I/O space nothing answers in).
 **/
void CSystem::WriteMem(u64 address, int dsize, u64 data,
                       CSystemComponent *source) {
  u64 a;
  int i;
  u8 *p;
#if defined(ALIGN_MEM_ACCESS)
  u64 t64;
  u32 t32;
  u16 t16;
#endif // defined(ALIGN_MEM_ACCESS)
  // No device-write lock breaking here: the CAS-backed STx_C model detects
  // any intervening store to the locked location by value comparison.
  a = address & m_phys_mask;

  if (a >> iNumMemoryBits) // non-memory
  {

    // check registered device memory ranges, the one that answered last
    // before the rest
    i = iLastMemory;
    if (i < 0 || i >= iNumMemories || a < aMemoryBounds[i].base ||
        a >= aMemoryBounds[i].end) {
      for (i = 0; i < iNumMemories; i++)
        if (a >= aMemoryBounds[i].base && a < aMemoryBounds[i].end)
          break;
      if (i < iNumMemories)
        iLastMemory = i;
    }
    if (i < iNumMemories) {
      asMemories[i]->component->WriteMem(
          asMemories[i]->index, a - aMemoryBounds[i].base, dsize, data);
      return;
    }

    // Not memory and no device's: the chipset's registers, or space
    // nothing answers in.
    m_chipset->write_io(a, address, dsize, data, source);
    return;
  }

  p = (u8 *)memory + a;

  switch (dsize) {
  case 8:
    *((u8 *)p) = (u8)data;
    break;
  case 16:
    *((u16 *)p) = endian_16((u16)data);
    break;
  case 32:
    *((u32 *)p) = endian_32((u32)data);
    break;
  default:
    *((u64 *)p) = endian_64((u64)data);
  }
  // Whoever wrote it -- the native PALcode's stores come through here, and
  // so does anything a device writes one word at a time.
  m_code_pages.note_write(a);
}

/**
 * \brief Read 8, 4, 2 or 1 byte(s) from a 64-bit system address. This could be
 *memory, a registered device range, or what the chipset decodes itself
 *(CChipset::read_io: its registers, PCI and I/O space nothing answers in).
 **/
u64 CSystem::ReadMem(u64 address, int dsize, CSystemComponent *source) {
  u64 a;
  int i;
  u8 *p;

  a = address & m_phys_mask;
  if (a >> iNumMemoryBits) // Non Memory
  {

    // check registered device memory ranges, the one that answered last
    // before the rest
    i = iLastMemory;
    if (i < 0 || i >= iNumMemories || a < aMemoryBounds[i].base ||
        a >= aMemoryBounds[i].end) {
      for (i = 0; i < iNumMemories; i++)
        if (a >= aMemoryBounds[i].base && a < aMemoryBounds[i].end)
          break;
      if (i < iNumMemories)
        iLastMemory = i;
    }
    if (i < iNumMemories)
      return asMemories[i]->component->ReadMem(
          asMemories[i]->index, a - aMemoryBounds[i].base, dsize);

    // Not memory and no device's: the chipset's registers, or space
    // nothing answers in.
    return m_chipset->read_io(a, address, dsize, source);
  }

  p = (u8 *)memory + a;

  switch (dsize) {
  case 8:
    return *((u8 *)p);
  case 16:
    return endian_16(*((u16 *)p));
  case 32:
    return endian_32(*((u32 *)p));
  default:
    return endian_64(*((u64 *)p));
  }
}

/**
 * Load ROM contents from file. Try if the decompressed ROM image
 * is available, otherwise create it first.
 **/
/**
 * Run one progress chunk of the SRM self-decompressor on CPU 0 (up to 180
 * million instructions). Returns true once the decompressor has jumped below
 * 0x200000 (into the inflated console), stopped at the first instruction
 * there: the PC and memory it leaves are what LoadROM saves as the
 * decompressed image.
 **/
static bool srm_decomp_chunk(CAlphaCPU *cpu) {
  for (int i = 0; i < 90000; i++)
    if (cpu->run_until_below(U64(0x200000), 2000))
      return true;
  return false;
}

/**
 * decompressed.rom is the entry PC and PAL_BASE (8 bytes each), the low 2 MB
 * of memory, and this tag. The image is where it always was, 16 bytes in;
 * the tag marks a file saved at the decompressor's jump into the console.
 * Earlier builds saved the PC some way past that jump -- inside the
 * console's PALcode reset code, without the registers and IPRs that code
 * had set, the cycle counter enable among them -- so a file without the
 * tag is decompressed again rather than trusted. (From the JIT lane such a
 * file does not boot; from the interpreter the console divides by zero
 * timing the processor, an unexpected exception through vector 440, and
 * restarts.)
 **/
/**
 * Hand every processor to the console at `entry`, the PALcode reset entry
 * the decompressor jumps to (PALmode bit set). PAL_BASE is that entry's
 * page: CPU 0's still names the decompressor's own PALcode, which the
 * console reuses as memory, and a secondary is released at PAL_BASE + 1 --
 * copied from CPU 0, it ran the decompressor's leftovers (OPCDEC at 0x6022xx
 * with two processors). Earlier builds stopped past the console's first
 * instructions, which had already moved PAL_BASE, so it never showed.
 **/
static void hand_to_console(CAlphaCPU **cpus, int n, u64 entry) {
  for (int i = 0; i < n; i++) {
    cpus[i]->set_pc(entry);
    cpus[i]->set_PAL_BASE(entry & ~U64(1));
  }
}

static const char kDecompTag[8] = {'A', 'L', 'P', 'H', 'D', 'C', '0', '1'};
static const long kDecompSize = 2 * sizeof(u64) + 0x200000;

int CSystem::LoadROM() {
  // The firmware image lands in guest memory behind every processor's back
  // (a reset reloads it while compiled code exists).
  m_code_pages.note_write_all();
  FILE *f;
  char *buffer;
  int i;
  int j;
  u64 temp;
  u32 scratch;
  bool loadedFromFlash = false;

  // If flash.rom contains a partitioned ES40 image (CPQ header at the SRM
  // partition), execute its embedded self-decompressor to inflate the console
  // into low RAM just like the cl67srmrom.exe path would.
  u32 fl_off = 0;
  u32 fl_hdr = 0;
  u32 fl_size = 0;
  u64 fl_base = 0;

  if (theSROM && theSROM->HasBootFirmware()) {
    printf("%%SYS-I-READFLASH: Reading boot ROM image from %s.\n",
           myCfg->get_text_value("rom.flash", "flash.rom"));

    const u8 *flash = theSROM->GetFlashBytes();
    const u32 srm_off = 0x00010000;
    const u32 srm_len = 0x000E0000;

    printf("%%SYS-I-DECOMP: Decompressing SRM image from flash.\n0%%");
    fflush(stdout);

    // The SRM partition is wrapped in a 0x40-byte CPQ header. The
    // self-decompressing payload is not position independent and expects
    // to be loaded exactly like the cl67srmrom.exe path: payload at
    // 0x900000, PC=0x900001, PAL_BASE=0x900000.
    const u64 load_base = U64(0x0000000000900000);
    const u32 cpq_hdr_len = 0x40;

    memcpy(PtrToMem(load_base), flash + srm_off + cpq_hdr_len,
           srm_len - cpq_hdr_len);

    acCPUs[0]->set_pc(load_base | 1);
    acCPUs[0]->set_PAL_BASE(load_base);
    acCPUs[0]->enable_icache();

    bool decomp_ok = true;
    j = 0;
    while (acCPUs[0]->get_clean_pc() > U64(0x200000)) {
      srm_decomp_chunk(acCPUs[0]);
      j++;
      if (j < 50) {
        printf("%d%%", j * 2);
        fflush(stdout);
      } else {
        printf(".");
        fflush(stdout);
      }
      if (j > 500) {
        printf("\n%%SYS-F-DECOMPFAIL: SRM decompressor did not return to low "
               "memory.\n");
        decomp_ok = false;
        break;
      }
    }
    printf("100%%\n");

    acCPUs[0]->restore_icache();

    if (decomp_ok) {
      hand_to_console(acCPUs, iNumCPUs, acCPUs[0]->get_pc());
      start_secondaries();

      loadedFromFlash = true;
    }
  }

  // A machine whose own update utility installed its console leaves it in
  // the flash behind a standard ROM header, wherever that machine keeps it.
  if (!loadedFromFlash && theSROM &&
      theSROM->FindConsoleImage(&fl_off, &fl_hdr, &fl_size, &fl_base) &&
      PtrToMem(fl_base)) {
    printf("%%SYS-I-READFLASH: Console image in flash at %x, %u bytes, "
           "loaded at %" PRIx64 ".\n",
           fl_off, fl_size, fl_base);
    printf("%%SYS-I-DECOMP: Decompressing SRM image from flash.\n0%%");
    fflush(stdout);

    memcpy(PtrToMem(fl_base), theSROM->GetFlashBytes() + fl_off + fl_hdr,
           fl_size);
    acCPUs[0]->set_pc(fl_base | 1);
    acCPUs[0]->set_PAL_BASE(fl_base);
    acCPUs[0]->enable_icache();

    bool decomp_ok = true;
    j = 0;
    while (acCPUs[0]->get_clean_pc() > U64(0x200000)) {
      srm_decomp_chunk(acCPUs[0]);
      if (++j > 500) {
        printf("\n%%SYS-F-DECOMPFAIL: SRM decompressor did not return to low "
               "memory.\n");
        decomp_ok = false;
        break;
      }
      printf(".");
      fflush(stdout);
    }
    printf("100%%\n");
    acCPUs[0]->restore_icache();

    if (decomp_ok) {
      hand_to_console(acCPUs, iNumCPUs, acCPUs[0]->get_pc());
      start_secondaries();
      loadedFromFlash = true;
    }
  }

  if (!loadedFromFlash) {
    f = fopen(myCfg->get_text_value("rom.decompressed", "decompressed.rom"),
              "rb");
    // A decompressed image is the entry PC, PAL_BASE, 2 MB of memory and the
    // tag (kDecompTag). One of any other size is a write that never finished
    // (the emulator stopped during it): decompress again rather than boot
    // from half an image. One without the tag was saved past the console's
    // entry by an older Alphabox: decompress that again too.
    if (f) {
      const char *dec =
          myCfg->get_text_value("rom.decompressed", "decompressed.rom");
      fseek(f, 0, SEEK_END);
      const long have = ftell(f);
      char tag[sizeof(kDecompTag)] = {};
      if (have == kDecompSize + (long)sizeof(kDecompTag)) {
        fseek(f, kDecompSize, SEEK_SET);
        (void)!fread(tag, 1, sizeof(tag), f);
      }
      fseek(f, 0, SEEK_SET);
      if (have == kDecompSize) {
        printf("%%SYS-I-ROMOLD: %s was saved by an older version, past the "
               "console's entry; decompressing again.\n",
               dec);
        fclose(f);
        f = nullptr;
      } else if (have != kDecompSize + (long)sizeof(kDecompTag) ||
                 memcmp(tag, kDecompTag, sizeof(tag)) != 0) {
        printf("%%SYS-W-ROMSIZE: %s (%ld bytes) is not a whole image; "
               "decompressing again.\n",
               dec, have);
        fclose(f);
        f = nullptr;
      }
    }
    if (!f) {
      const char *srm =
          myCfg->get_text_value("rom.srm", m_platform->firmware_file);
      f = fopen(srm, "rb");
      if (!f)
        FAILURE(Runtime, "No original or decompressed SRM ROM image found");
      printf("%%SYS-I-READROM: Reading original ROM image from %s.\n", srm);

      // Where the console's own code starts in the file, and where it runs.
      // An update bundle carries the console behind a fixed wrapper; a raw
      // image carries the standard Alpha ROM header, which says where the
      // machine loads it (docs/platforms.md).
      size_t skip = 0x240;
      u64 rom_base = U64(0x900000);
      if (m_platform->firmware == FW_RAW_IMAGE) {
        skip = 0; // the console's own first instruction
      } else if (m_platform->firmware == FW_ROM_HEADER) {
        u32 header[14];
        if (fread(header, sizeof(u32), 14, f) != 14)
          FAILURE(Runtime, "File is too short to be a SRM ROM image");
        for (i = 0; i < 14; i++)
          header[i] = endian_32(header[i]);
        if (header[0] != 0x5a5ac3c3 || header[1] != 0xa5a53c3c)
          FAILURE_1(Runtime, "%s does not carry an Alpha ROM header", srm);
        skip = header[2];
        rom_base = header[6];
        printf("%%SYS-I-ROMHEADER: %s: %u bytes, loaded at %" PRIx64 ".\n", srm,
               header[4], rom_base);
        if (!PtrToMem(rom_base))
          FAILURE_1(Runtime, "ROM load address %" PRIx64 " is outside memory",
                    rom_base);
      }

      fseek(f, 0, SEEK_END);
      const long file_size = ftell(f);
      if (file_size <= (long)skip)
        FAILURE(Runtime, "File is too short to be a SRM ROM image");
      fseek(f, (long)skip, SEEK_SET);
      buffer = PtrToMem(rom_base);
      while (!feof(f))
        (void)!fread(buffer++, 1, 1, f);
      fclose(f);

      printf("%%SYS-I-DECOMP: Decompressing ROM image.\n0%%");
      // PALmode entry at the image's first instruction.
      acCPUs[0]->set_pc(rom_base + 1);
      acCPUs[0]->set_PAL_BASE(rom_base);
      acCPUs[0]->enable_icache();

      j = 0;
      while (acCPUs[0]->get_clean_pc() > 0x200000) {
        srm_decomp_chunk(acCPUs[0]);
        j++;
        if (((j % 5) == 0) && (j < 50))
          printf("%d%%", j * 2);
        else
          printf(".");
        fflush(stdout);
      }

      printf("100%%\n");
      acCPUs[0]->restore_icache();
      hand_to_console(acCPUs, iNumCPUs, acCPUs[0]->get_pc());
      start_secondaries();

      // Written beside the final name and renamed into place, so an image
      // under that name is always whole.
      const std::string dec =
          myCfg->get_text_value("rom.decompressed", "decompressed.rom");
      const std::string tmp = dec + ".tmp";
      f = fopen(tmp.c_str(), "wb");
      if (!f) {
        printf("%%SYS-W-NOWRITE: Couldn't write decompressed rom to %s.\n",
               dec.c_str());
      } else {
        printf("%%SYS-I-ROMWRT: Writing decompressed rom to %s.\n",
               dec.c_str());
        temp = endian_64(acCPUs[0]->get_pc());
        bool ok = fwrite(&temp, 1, sizeof(u64), f) == sizeof(u64);
        temp = endian_64(acCPUs[0]->get_pal_base());
        ok = ok && fwrite(&temp, 1, sizeof(u64), f) == sizeof(u64);
        buffer = PtrToMem(0);
        ok = ok && fwrite(buffer, 1, 0x200000, f) == 0x200000;
        ok = ok &&
             fwrite(kDecompTag, 1, sizeof(kDecompTag), f) == sizeof(kDecompTag);
        ok = (fclose(f) == 0) && ok;
        if (!ok || rename(tmp.c_str(), dec.c_str()) != 0) {
          printf("%%SYS-W-NOWRITE: Couldn't write decompressed rom to %s.\n",
                 dec.c_str());
          remove(tmp.c_str());
        }
      }
    } else {
      printf("%%SYS-I-READROM: Reading decompressed ROM image from %s.\n",
             myCfg->get_text_value("rom.decompressed", "decompressed.rom"));
      (void)!fread(&temp, 1, sizeof(u64), f);
      const u64 entry = endian_64(temp);
      // The saved PAL_BASE is not used: see hand_to_console().
      (void)!fread(&temp, 1, sizeof(u64), f);
      hand_to_console(acCPUs, iNumCPUs, entry);
      buffer = PtrToMem(0);
      (void)!fread(buffer, 1, 0x200000, f);
      fclose(f);
      // The three paths that decompress the firmware release the processors
      // a console does not start itself; this one, which reads the same
      // image back from cache, did not -- so on such a board the first boot
      // of a firmware worked and every later one left every secondary
      // parked. (The ES40's console starts its own, so it never showed.)
      start_secondaries();
    }
  } // !loadedFromFlash

#if !defined(SRM_NO_SPEEDUPS) || !defined(SRM_NO_IDE)
  printf("%%SYM-I-PATCHROM: Patching ROM for speed.\n");
#endif
#if !defined(SRM_NO_SPEEDUPS)
  WriteMem(U64(0x14248), 32, 0xe7e00000, 0); // e7e00000 = BEQ r31, +0
  WriteMem(U64(0x14288), 32, 0xe7e00000, 0);
  WriteMem(U64(0x142c8), 32, 0xe7e00000, 0);
  WriteMem(U64(0x68320), 32, 0xe7e00000, 0);
  WriteMem(U64(0x8bb78), 32, 0xe7e00000, 0); // memory test (aa)
  WriteMem(U64(0x8bc0c), 32, 0xe7e00000, 0); // memory test (bb)
  WriteMem(U64(0x8bc94), 32, 0xe7e00000, 0); // memory test (00)

  // WriteMem(U64(0xb1158),32,0xe7e00000,0);   // CPU sync?
#endif
#ifdef ES40_JIT
  // Blocks compiled over the patch sites above -- by the console run before
  // a reset -- must not outlive the patch: drop them so the patched bytes
  // take effect.
  acCPUs[0]->flush_icache();
#endif
  printf("%%SYS-I-ROMLOADED: ROM Image loaded successfully!\n");
  return 0;
}

/**
 * Initialize all devices.
 **/
void CSystem::init() {
  if (!m_native_pal)
    printf("%%SYS-I-VMSPAL: vmspal PALcode replacement routines on all CPUs "
           "(palcode.vms.nohle = true on any CPU selects native PALcode).\n");
  for (int i = 0; i < iNumComponents; i++)
    acComponents[i]->init();
}

void CSystem::start_threads() {
  int i;

  printf("Start threads:");
  for (i = 0; i < iNumComponents; i++) {
#ifdef IDB
    // When running with IDB, the trace engine takes care of managing the CPU,
    // so its thread shouldn't be started.
    if (dynamic_cast<CAlphaCPU *>(acComponents[i]))
      continue;
#endif
    acComponents[i]->start_threads();
  }
  printf("\n");

  for (i = 0; i < iNumCPUs; i++)
    acCPUs[i]->release_threads();
}

void CSystem::stop_threads() {
  printf("Stop threads:");
  for (int i = 0; i < iNumComponents; i++)
    acComponents[i]->stop_threads();
  printf("\n");
}

// --- Firmware-triggered system reset support ------------------------------

void CSystem::RequestSystemReset() {
  m_reset_requested.store(true, std::memory_order_release);
}

bool CSystem::IsSystemResetRequested() const {
  return m_reset_requested.load(std::memory_order_acquire);
}

bool CSystem::ProcessPendingReset() {
  if (!m_reset_requested.exchange(false, std::memory_order_acq_rel))
    return false;

  struct ResetInProgressGuard {
    CSystem *sys;
    explicit ResetInProgressGuard(CSystem *s) : sys(s) {
      sys->SetResetInProgress(true);
    }
    ~ResetInProgressGuard() { sys->SetResetInProgress(false); }
  };

  printf("\n%%SYS-I-RESET: System reset requested by firmware.\n");
  if (theSROM)
    theSROM->FlushIfDirty();
  if (theDPR)
    theDPR->FlushIfDirty();

  ResetInProgressGuard rip(this);
  stop_threads();
  ResetChipsetState();
  for (int dev = 0; dev < iNumComponents; dev++)
    acComponents[dev]->ResetPCI();
  for (int cpu = 0; cpu < iNumCPUs; cpu++)
    acCPUs[cpu]->ResetForSystemReset();
  LoadROM();
  start_threads();
  return true;
}

void CSystem::ResetChipsetState() {
  // Re-establish the same power-on defaults used in the constructor.
  state.cpu_lock_flags = 0;
  memset(state.cpu_lock_address, 0, sizeof(state.cpu_lock_address));
  memset(cpu_lock_value, 0, sizeof(cpu_lock_value));

  m_chipset->reset();
}

/**
 * Save system state to a state file.
 **/
void CSystem::SaveState(const char *fn) {
  FILE *f;
  int i;
  u64 m;
  unsigned int j;
  int *mem = (int *)memory;
  int int0 = 0;
  const u64 memints = (U64(1) << iNumMemoryBits) / sizeof(int);
  u32 temp_32;

  // Write to a temporary file and rename it into place. A save that fails
  // part-way -- out of space, or the emulator dying during the memory image,
  // which is by far the largest part -- used to leave a truncated file where
  // the last good state file had been. Now the previous one survives intact.
  // The byte format is unchanged, so existing state files still restore.
  char tmpname[1024];
  snprintf(tmpname, sizeof(tmpname), "%s.tmp", fn);

  f = fopen(tmpname, "wb");
  if (!f) {
    printf("%%SYS-F-SAVESTATE: cannot create %s: %s\n", tmpname,
           strerror(errno));
    return;
  }

  temp_32 = 0xa1fae540; // MAGIC NUMBER (ALFAES40 ==> A1FAE540 )
  fwrite(&temp_32, sizeof(u32), 1, f);
  temp_32 = 0x00020002; // File Format Version 2.2 (2.2: TIG IPCRs)
  fwrite(&temp_32, sizeof(u32), 1, f);

  // memory: a non-zero int is written as is; a run of zero ints as one 0
  // followed by the number of further zero ints (at most 0xffffffff, so
  // longer runs continue in the next record).
  for (m = 0; m < memints; m++) {
    if (mem[m]) {
      fwrite(&(mem[m]), 1, sizeof(int), f);
      continue;
    }
    j = 0;
    while (m + 1 < memints && !mem[m + 1] && j != 0xffffffff) {
      m++;
      j++;
    }
    fwrite(&int0, 1, sizeof(int), f);
    fwrite(&j, 1, sizeof(int), f);
  }

  fwrite(&state, sizeof(state), 1, f);
  m_chipset->save_state(f);

  // components
  //
  //  Components should also save any non-initial memory-registrations and
  //  re-register upon restore!
  //
  for (i = 0; i < iNumComponents; i++)
    acComponents[i]->SaveState(f);

  // stdio latches its error flag, so this one check covers every write above,
  // the components' writes included.
  const bool write_failed = (ferror(f) != 0);
  const bool close_failed = (fclose(f) != 0);
  if (write_failed || close_failed) {
    printf("%%SYS-F-SAVESTATE: writing %s failed: %s. %s is left unchanged.\n",
           tmpname, strerror(errno), fn);
    remove(tmpname);
    return;
  }

  if (rename(tmpname, fn) != 0) {
    printf("%%SYS-F-SAVESTATE: cannot rename %s to %s: %s\n", tmpname, fn,
           strerror(errno));
    remove(tmpname);
    return;
  }
}

/**
 * Restore system state from a state file.
 **/
void CSystem::RestoreState(const char *fn) {
  m_code_pages.note_write_all(); // the whole of memory is about to be replaced
  FILE *f;
  int i;
  u64 m;
  unsigned int j;
  int *mem = (int *)memory;
  const u64 memints = (U64(1) << iNumMemoryBits) / sizeof(int);
  u32 temp_32;

  f = fopen(fn, "rb");
  if (!f) {
    printf("%%SYS-F-NOFILE: Can't open restore file %s\n", fn);
    return;
  }

  if (fread(&temp_32, sizeof(u32), 1, f) != 1 ||
      temp_32 != 0xa1fae540) // MAGIC NUMBER (ALFAES40 ==> A1FAE540 )
  {
    printf("%%SYS-F-FORMAT: %s does not appear to be a state file.\n", fn);
    fclose(f);
    return;
  }

  if (fread(&temp_32, sizeof(u32), 1, f) != 1 ||
      temp_32 != 0x00020002) // File Format Version 2.2 (2.2: TIG IPCRs)
  {
    printf("%%SYS-I-VERSION: State file %s is a different version.\n", fn);
    fclose(f);
    return;
  }

  // memory (see SaveState for the zero-run encoding)
  for (m = 0; m < memints; m++) {
    if (fread(&(mem[m]), sizeof(int), 1, f) != 1)
      FAILURE(Runtime, "State file is truncated in the memory image");
    if (!mem[m]) {
      if (fread(&j, sizeof(int), 1, f) != 1)
        FAILURE(Runtime, "State file is truncated in the memory image");
      if (j >= memints - m)
        FAILURE(Runtime, "State file holds more memory than memory.bits");
      while (j--)
        mem[++m] = 0;
    }
  }

  if (fread(&state, sizeof(state), 1, f) != 1 || !m_chipset->restore_state(f))
    FAILURE(Runtime, "State file is truncated in the system state");

  // components
  //
  //  Components should also save any non-initial memory-registrations and
  //  re-register upon restore!
  //
  for (i = 0; i < iNumComponents; i++) {
    if (acComponents[i]->RestoreState(f))
      FAILURE(Runtime, "Unable to restore system state");
  }

  fclose(f);
}

/**
 * Dump memory contents to a file.
 **/
void CSystem::DumpMemory(unsigned int filenum) {
  char file[100];
  u64 x;
  int *mem = (int *)memory;
  FILE *f;

  sprintf(file, "memory_%012d.dmp", filenum);
  f = fopen(file, "wb");
  if (!f)
    return;

  x = (U64(1) << iNumMemoryBits) / sizeof(int) / 2;

  while (x > 0 && !mem[x - 1])
    x--;

  fwrite(mem, 1, (size_t)(x * sizeof(int)), f);
  fclose(f);
}

/**
 *  Dump system state to stdout for debugging purposes.
 **/
void CSystem::panic(char *message, int flags) {
  int cpunum;

  int i;
  CAlphaCPU *cpu;
  printf("\n******** SYSTEM PANIC *********\n");
  printf("* %s\n", message);
  printf("*******************************\n");
  for (cpunum = 0; cpunum < iNumCPUs; cpunum++) {
    cpu = acCPUs[cpunum];
    printf("\n==================== STATE OF CPU %d ====================\n",
           cpunum);

    printf("PC: %016" PRIx64 "\n", cpu->get_pc());
#ifdef IDB
    printf("Physical PC: %016" PRIx64 "\n", cpu->get_current_pc_physical());
    printf("Instruction Count: %" PRId64 "\n", cpu->get_instruction_count());
#endif
    printf("\n");

    for (i = 0; i < 32; i++) {
      if (i < 10)
        printf("R");
      printf("%d:%016" PRIx64, i, cpu->get_r(i, false));
      if (i % 4 == 3)
        printf("\n");
      else
        printf(" ");
    }

    printf("\n");
    for (i = 4; i < 8; i++) {
      if (i < 10)
        printf("S");
      printf("%d:%016" PRIx64, i, cpu->get_r(i + 32, false));
      if (i % 4 == 3)
        printf("\n");
      else
        printf(" ");
    }

    for (i = 20; i < 24; i++) {
      if (i < 10)
        printf("S");
      printf("%d:%016" PRIx64, i, cpu->get_r(i + 32, false));
      if (i % 4 == 3)
        printf("\n");
      else
        printf(" ");
    }

    printf("\n");
    for (i = 0; i < 32; i++) {
      if (i < 10)
        printf("F");
      printf("%d:%016" PRIx64, i, cpu->get_f(i));
      if (i % 4 == 3)
        printf("\n");
      else
        printf(" ");
    }
  }

  printf("\n");
#ifdef IDB
  if (flags & PANIC_LISTING) {
    u64 start;

    u64 end;
    start = cpu->get_pc() - 64;
    end = start + 128;
    cpu->listing(start, end, cpu->get_pc());
  }
#endif
  if (flags & PANIC_ASKSHUTDOWN) {
    printf("Stop Emulation? ");

    int c = getc(stdin);
    if (c == 'y' || c == 'Y')
      flags |= PANIC_SHUTDOWN;
  }

  if (flags & PANIC_SHUTDOWN) {
    FAILURE(Abort, "Panic shutdown");
  }

  return;
}

#if defined(PROFILE)
u64 profile_buckets[PROFILE_BUCKETS];
u64 profiled_insts;
bool profile_started = false;
#endif
CSystem *theSystem = 0;
