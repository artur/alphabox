/* Alphabox Alpha Emulator
 * Copyright (C) 2020 Tomáš Glozar
 * Website: https://github.com/lenticularis39/axpbox
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

#include "Platform.hpp"
#include "SystemComponent.hpp"
#include "TraceEngine.hpp"
#include "i2c_spd.hpp"
#include <atomic>
#include <mutex>

#if !defined(INCLUDED_SYSTEM_H)
#define INCLUDED_SYSTEM_H

#define MAX_COMPONENTS 100

// Interrupt-rate counters: bumped where interrupts are raised and taken,
// printed and reset every 5 s by the Ali thread under ALPHABOX_IRQSTATS=1.
struct SIrqStats {
  std::atomic<u64> cpu_int{0};      // CPU interrupt entries (PAL INTERRUPT)
  std::atomic<u64> cpu_eir[6]{};    // ...by EIR & EIEN bit pending at entry
  std::atomic<u64> cpu_sw{0};       // ...with a software interrupt pending
  std::atomic<u64> cpu_ast{0};      // ...with an AST pending
  std::atomic<u64> cchip_timer{0};  // Cchip interval-timer ticks
  std::atomic<u64> drir_rise[64]{}; // Cchip DRIR bits going 0 -> 1
  std::atomic<u64> isa_edge[16]{};  // 8259 input edges (PIC0 0-7, PIC1 8-15)
  std::atomic<u64> isa_ack[16]{};   // 8259 IRQs acknowledged by the guest
};
inline SIrqStats g_irqstats;

#if defined(PROFILE)
#define PROFILE_FROM U64(0x8000)
#define PROFILE_TO U64(0x1a81c0)
#define PROFILE_AFTER U64(0x200000)
#define PROFILE_BUCKSIZE 16
#define PROFILE_LENGTH (PROFILE_TO - PROFILE_FROM)
#define PROFILE_INSTS (PROFILE_LENGTH / 4)
#define PROFILE_BUCKETS (PROFILE_INSTS / PROFILE_BUCKSIZE)
#define PROFILE_YN(a)                                                          \
  ((a >= PROFILE_FROM) && (a < PROFILE_TO) && profile_started)
#define PROFILE_BUCKET(a)                                                      \
  profile_buckets[(a - PROFILE_FROM) / 4 / PROFILE_BUCKSIZE]
#define PROFILE_DO(a)                                                          \
  if ((a & (~U64(0x3))) >= PROFILE_AFTER)                                      \
    profile_started = true;                                                    \
  if (PROFILE_YN(a)) {                                                         \
    PROFILE_BUCKET(a)++;                                                       \
    profiled_insts++;                                                          \
  }

extern u64 profile_buckets[PROFILE_BUCKETS];
extern u64 profiled_insts;
extern bool profile_started;
#endif
#if defined(LS_MASTER) || defined(LS_SLAVE)
extern char *dbg_strptr;
#endif

/// Structure used for mapping memory ranges to devices.
struct SMemoryUser {
  CSystemComponent *component; /**< Device that occupies this range. */
  int index; /**< Index within the device. Used by devices that occupy more than
                one range. */
  u64 base;  /**< Address of first byte. */
  u64 length; /**< Number of bytes in range. */
};

/// Structure used for configuration values.
struct SConfig {
  char *key;   /**< Name of the value. */
  char *value; /**< Value of the value. */
};

/**
 * \brief Emulated Typhoon 21272 chipset.
 *
 * Documentation consulted:
 *  - Tsunami/Typhoon 21272 Chipset Hardware Reference Manual  [HRM]
 *(http://download.majix.org/dec/tsunami_typhoon_21272_hrm.pdf)
 *  - AlphaServer ES40 and AlphaStation ES40 Service Guide [SG]
 *(http://www.dec-store.com/PD_00158.aspx)
 *  - Tru64 include file dc104x.h [T64]
 *(http://samy.pl/packet/MISC/tru64/usr/include/alpha/dc104x.h)
 *  .
 *
 * The ES40 emulator has the following chipset configuration:
 *   - 1 x 21274-C1 Cchip (controller chip) - The Cchip controls the other chips
 *in the chipset, as well as the DRAM memory array in a system. The Cchip
 *interfaces with the CPU's command and address buses.
 *   - 8 x 21274-D1 Dchip (data slice chip) - The Dchips interface with the
 *system data bus and provide the data path between the CPU, DRAM memory, and
 *the Pchip(s).
 *   - 2 x 21272-P1 Pchip (peripheral interface chip) - The interface to the PCI
 *bus.
 *   .
 **/
/// Host-side open-drain drivers for the Tsunami MPD I2C pins.
struct MPDState {
  // Host open-drain drivers (1 = released high, 0 = pulling low)
  bool cks_out = true; // SCL
  bool ds_out = true;  // SDA
};

/// Which parts of physical memory compiled code was built from, and how
/// many writes have landed on one of them.
///
/// An IMB means "I may have changed code": every compiled block then has to
/// prove its source words are still what it was built from, and at the SRM
/// prompt that is seven million re-hashes per hundred million instructions
/// which have never once found a changed byte -- three hundred million of
/// them across a Windows boot, not one of them changed. The firmware simply
/// issues an IMB from a polling loop. This map lets the flush ask a cheaper
/// question first: has anything been written to memory a block was compiled
/// from since the last flush? If not, there is nothing to flush.
///
/// It is kept at two granularities because the two users need different
/// things. Whether compiled code may store into a page inline is a question
/// about a page, because that is the unit the data page cache maps. Whether
/// a write can have changed code is a question about a much smaller range:
/// the SRM console keeps a counter 1.5 KB away from its own code on the same
/// page and writes it fourteen million times a second, so at page
/// granularity every flush would still find that page written and the
/// answer would always be "maybe". At 256 bytes the counter and the code are
/// plainly apart.
///
/// The map is conservative in the only direction that is safe: bits are set
/// when code is compiled and never cleared, so a page that once held code
/// keeps the slower write path even after the guest reuses it. Writes are
/// counted, not located -- one counter for the whole machine is all a flush
/// needs, and it keeps the write path down to a load, a test and a branch
/// that is almost never taken. Every processor and every device thread
/// shares one map, so a write by one processor invalidates another's blocks
/// and DMA into a code page counts like a store.
class CSystem;

class CCodePageMap {
public:
  static constexpr int kPageShift = 13; // the Alpha's 8 KB page
  static constexpr int kLineShift = 8;  // 256 bytes: the dirty-decision unit
  void init(CSystem *sys, u64 dram_bytes, void *page_bits, void *line_bits);
  /// A block was compiled from [phys, phys+bytes): mark its page and lines.
  void note_code(u64 phys, size_t bytes);
  /// Something wrote guest memory somewhere we cannot place (a firmware
  /// reload, a restored savefile): treat everything as written.
  void note_write_all() { m_gen.fetch_add(1, std::memory_order_release); }
  u64 write_gen() const { return m_gen.load(std::memory_order_acquire); }
  /// Does this page hold any compiled code? (The data page cache's unit:
  /// such a page is never offered to compiled code for an inline store.)
  bool holds_code(u64 phys) const {
    const u64 pg = phys >> kPageShift;
    return m_page_bits && pg < m_pages &&
           (m_page_bits[pg >> 3] & (1u << (pg & 7)));
  }
  /// Was code compiled from this 256-byte line?
  bool holds_code_line(u64 phys) const {
    const u64 ln = phys >> kLineShift;
    return m_line_bits && ln < m_lines &&
           (m_line_bits[ln >> 3] & (1u << (ln & 7)));
  }
  /// A guest store or a DMA transfer landed at phys.
  void note_write(u64 phys) {
    if (holds_code_line(phys)) {
      m_gen.fetch_add(1, std::memory_order_release);
      if (m_trace)
        trace_write(phys);
    }
  }
  void note_write_range(u64 phys, size_t bytes);
  u64 code_pages() const { return m_marked.load(std::memory_order_relaxed); }
  /// ALPHABOX_TRACE_CODEWRITE=1: which lines are being written and how
  /// often. A guest that keeps busy data next to its code is what this
  /// optimisation lives or dies by, and this is how to see it.
  void trace_write(u64 phys);

private:
  std::atomic<u64> m_gen{0};    // writes that landed on a compiled line
  std::atomic<u64> m_marked{0}; // code pages marked so far
  u8 *m_page_bits = nullptr;    // one bit per 8 KB page
  u8 *m_line_bits = nullptr;    // one bit per 256 bytes
  u64 m_pages = 0, m_lines = 0;
  CSystem *m_sys = nullptr;
  bool m_trace = false;
};

class CSystem {
  static void *alloc_guest_memory(size_t bytes);
  static void free_guest_memory(void *p, size_t bytes);

public:
#ifdef ALPHABOX_HVF
  // The device threads outside and the dispatch loop inside both read and
  // write this object, so under ALPHABOX_HV=1 it is placed in memory the
  // two share. Elsewhere it is an ordinary allocation.
  static void *operator new(size_t n);
  static void operator delete(void *p) noexcept;
#endif

public:
  void DumpMemory(unsigned int filenum);
  char *PtrToMem(u64 address);
  unsigned int get_memory_bits();
  void RestoreState(const char *fn);
  void SaveState(const char *fn);
  u64 PCI_Phys(int pcibus, u32 address);
  u64 PCI_Phys_direct_mapped(u32 address, u64 wsm, u64 tba);
  u64 PCI_Phys_scatter_gather(u32 address, u64 wsm, u64 tba);
  void interrupt(int number, bool assert);
  // Interval-tick sequence, bumped on every Cchip timer tick (CPU
  // instruction pacing, see CAlphaCPU::jit_run).
  u32 get_tick_seq() const {
    return m_tick_seq.load(std::memory_order_relaxed);
  }
  int LoadROM();
  /// The machine's code-page map: every processor's JIT marks the pages it
  /// compiled from here, and every write path reports to it.
  CCodePageMap *code_pages() { return &m_code_pages; }
  void init_code_page_map();
  /// A page has just become a code page. Every processor may hold a cached
  /// translation that still lets compiled code store into it inline, so ask
  /// them all to drop what they cached; each bumps the map's write count as
  /// it does, which makes the next flush a real one and covers any store it
  /// may already have made through such a translation.
  void request_code_page_flush();
  u64 ReadMem(u64 address, int dsize, CSystemComponent *source);
  void WriteMem(u64 address, int dsize, u64 data, CSystemComponent *source);
  void Run();
  int SingleStep();

  void init();
  void start_threads();
  void stop_threads();

  // Firmware-triggered system reset support (LFU writes to the TIG SRCR
  // registers after a flash update). The CPU thread polls
  // IsSystemResetRequested() and parks until the main thread processes it.
  void RequestSystemReset();
  bool IsSystemResetRequested() const;
  bool ProcessPendingReset();
  void ResetChipsetState();

  // Native PALcode for every CPU: requested while the CPUs are constructed if
  // any of them sets palcode.vms.nohle (always in JIT builds), so that no CPU
  // runs the vmspal replacement routines while another runs native PALcode.
  void request_native_pal(const char *why) {
    if (!m_native_pal)
      printf("%%SYS-I-NATIVEPAL: %s: native PALcode on all CPUs (vmspal "
             "replacement routines disabled).\n",
             why);
    m_native_pal = true;
  }
  bool native_pal_requested() const { return m_native_pal; }

  // exit_on_pal_halt: a kernel-mode CALL_PAL HALT asks the main loop (Run) to
  // exit gracefully; the CPU carries on into the HALT until then.
  bool exit_on_pal_halt() const { return m_exit_on_pal_halt; }
  void RequestPalHaltExit() {
    m_pal_halt_exit.store(true, std::memory_order_relaxed);
  }

  // True while we are performing an in-process reset (stop/reset/start).
  // Devices (S3/SDL) use this to PAUSE instead of destroying the window.
  void SetResetInProgress(bool v) {
    m_reset_in_progress.store(v, std::memory_order_release);
  }
  bool IsResetInProgress() const {
    return m_reset_in_progress.load(std::memory_order_acquire);
  }

  /// The machine this is: which board, its slots, interrupts and firmware
  /// (Platform.hpp). Chosen by the "platform" configuration value.
  const platform_config &platform() const { return *m_platform; }

  int RegisterMemory(CSystemComponent *component, int index, u64 base,
                     u64 length);

  /// A device's bulk data register, at its absolute physical address. The
  /// processor consults this before leaving the VM for a device access.
  struct BulkPort {
    u64 addr = 0;
    SBulkPort d;
  };
  static constexpr int kMaxBulkPorts = 8;
  const BulkPort *bulk_for(u64 addr) const {
    for (int i = 0; i < m_nbulk; i++)
      if (m_bulk[i].addr == addr)
        return &m_bulk[i];
    return nullptr;
  }
  void RegisterComponent(CSystemComponent *component);
  void UnregisterComponent(CSystemComponent *component);
  int RegisterCPU(class CAlphaCPU *cpu);

  /// Device memory that is plain bytes on the host -- a linear framebuffer
  /// -- offered for direct access: a CPU's data page cache then maps its
  /// pages like DRAM and compiled code reads and writes them inline, with
  /// no device call. One range (the S3's linear window). The owner offers it
  /// when the window is live and withdraws it (size 0) before the bytes stop
  /// being the truth; every CPU's page cache is flushed on either change,
  /// on the CPU's own thread. The end is published last, so a CPU that
  /// reads a torn triple sees no range rather than a wrong one.
  void set_direct_memory(u64 base, u64 size, u8 *host);
  inline u8 *direct_host_page(u64 phys_page) const {
    const u64 end = m_direct_end.load(std::memory_order_acquire);
    if (phys_page < end && phys_page >= m_direct_base)
      return m_direct_host + (phys_page - m_direct_base);
    return nullptr;
  }

  const platform_config *m_platform = nullptr; ///< the machine (Platform.hpp)

  /**
   * Report an access no device claimed: what, where, how wide, and the
   * instruction that made it. Off unless ALPHABOX_TRACE_UNKNOWN is set in
   * the environment; bringing up an unfamiliar firmware is mostly reading
   * this trace (docs/platforms.md).
   */
  void trace_unknown(const char *space, u64 address, int dsize, bool write,
                     u64 data, CSystemComponent *source);
  static bool trace_unknown_on();

  /**
   * Report the registers a console uses to start other processors and hand
   * work to them, when ALPHABOX_TRACE_MP is set: how a machine brings its
   * processors up is board-specific (docs/platforms.md).
   */
  void start_secondaries();
  void trace_mp(const char *what, u32 reg, u64 value);
  static bool trace_mp_on();

  CSystem(CConfigurator *cfg);
  void ResetMem(unsigned int membits);

  CAlphaCPU *get_cpu(int cpunum) { return acCPUs[cpunum]; };
  int get_cpu_num() { return iNumCPUs; };

  /// Modelled DIMM population (see init_spd_from_config_mb).
  struct SDimmLayout {
    int n_arrays = 1;        ///< populated arrays = populated MMBs (1, 2 or 4)
    int dimms_per_array = 4; ///< 8 (twice-split) or 4 (lower slot set only)
    uint32_t dimm_mb = 0;    ///< capacity of each (identical) DIMM
  };
  const SDimmLayout &get_dimm_layout() const { return m_dimm_layout; };
  const std::vector<uint8_t> &get_dimm_spd() const { return m_dimm_spd; };

  virtual ~CSystem();
  unsigned int iNumMemoryBits;

  void panic(char *message, int flags);

#define PANIC_NOSHUTDOWN 0
#define PANIC_SHUTDOWN 1
#define PANIC_ASKSHUTDOWN 2
#define PANIC_LISTING 4
  void clear_clock_int(int ProcNum);
  // ack interprocessor interrupt: clear MISC<IPINTR>, drop b_irq<3>
  void clear_ipi(int ProcNum);
  u64 get_c_misc();
  u64 get_c_dir(int ProcNum);
  u64 get_c_dim(int ProcNum);
  void set_c_dim(int ProcNum, u64 value);

  class CLLSCDRAMGuard {
  public:
    CLLSCDRAMGuard(CSystem *system, bool active);
    ~CLLSCDRAMGuard();
    CLLSCDRAMGuard(const CLLSCDRAMGuard &) = delete;
    CLLSCDRAMGuard &operator=(const CLLSCDRAMGuard &) = delete;

  private:
    CSystem *system;
  };

  class CPCIDMAWriteGuard {
  public:
    CPCIDMAWriteGuard(CSystem *system, bool active);
    ~CPCIDMAWriteGuard();
    CPCIDMAWriteGuard(const CPCIDMAWriteGuard &) = delete;
    CPCIDMAWriteGuard &operator=(const CPCIDMAWriteGuard &) = delete;
    void invalidate(u64 address, size_t bytes);

  private:
    CSystem *system;
    bool touched_code = false; // the transfer landed on compiled code
  };

  // LDx_L: record locked range + loaded value
  void cpu_lock(int cpuid, u64 address, u64 value);
  bool cpu_take_lock(int cpuid, u64 address, u64 *expected, bool *same_address);
  // STx_C: consume this CPU's lock and do the conditional store (1 = success).
  u64 cpu_stx_c(int cpuid, u64 phys, int size_bits, u64 value, char *dram,
                u64 dram_sz, CSystemComponent *source);
  // exception/interrupt: drop the lock
  void cpu_clear_lock(int cpuid);

private:
  u64 cchip_csr_read(u32 address, CSystemComponent *source);
  void cchip_csr_write(u32 address, u64 data, CSystemComponent *source);
  u64 pchip_csr_read(int num, u32 address);
  void pchip_csr_write(int num, u32 address, u64 data);
  u8 dchip_csr_read(u32 address);
  void dchip_csr_write(u32 address, u8 data);
  u8 tig_read(u32 address);
  void tig_write(u32 address, u8 data);
  void tig_update_halt_lines();

  CCodePageMap m_code_pages;
  void *m_code_page_bits = nullptr, *m_code_line_bits = nullptr;
  size_t m_code_page_bytes = 0, m_code_line_bytes = 0;

  // --- MPD / SPD wiring ---
  MPDState m_mpd;
  I2CBus m_mpd_bus;

  // Build SPD images that match configured memory.
  void init_spd_from_config_mb(uint32_t total_mb);
  static std::vector<uint8_t> build_sdram_spd(uint32_t dimm_mb,
                                              bool registered_ecc = true);
  SDimmLayout m_dimm_layout;
  std::vector<uint8_t> m_dimm_spd; ///< SPD image shared by all modelled DIMMs

  std::atomic<bool> m_reset_requested{false};
  std::atomic<bool> m_reset_in_progress{false};

  bool m_native_pal = false;                // see request_native_pal()
  bool m_exit_on_pal_halt = false;          // sys0 exit_on_pal_halt
  std::atomic<bool> m_pal_halt_exit{false}; // set by a CPU on CALL_PAL HALT

  // Serializes drir RMW + delivery in interrupt() across device threads. On
  // CSystem (not in saved 'state'), so SaveState is unaffected.
  std::mutex drir_lock;
  std::atomic<u32> m_tick_seq{0}; // interval-tick sequence

  int iNumCPUs;
  // ABA guard: a per-cache-line STx_C sequence number. The value compare in
  // cpu_stx_c cannot see another CPU writing a different value and putting the
  // old one back between our LDx_L and STx_C; a changed sequence can.
  static const u32 kLLBuckets = 65536; // 64-byte line hash, power of two
  std::atomic<u32> m_ll_seq[kLLBuckets];
  std::atomic<u8> m_ll_lock[kLLBuckets]; // seq check + CAS + bump, per bucket
  u32 m_ll_seq_snap[4];                  // per-CPU sequence at LDx_L time

  u64 cpu_lock_value[4]; // per-CPU LDx_L value, for same-address STx_C

  // writer bit + active LL/SC operation count
  std::atomic<u32> cpu_llsc_dma_gate{0};

  void cpu_llsc_enter();
  void cpu_llsc_leave();
  void pci_dma_write_enter();
  void pci_dma_write_leave();

public:
  // Model the EV68 invalidating probe for reservation lines touched by DMA.
  void cpu_clear_external_locks(u64 address, size_t bytes);

private:
  /// The state structure contains all elements that need to be saved to the
  /// statefile.
  struct SSys_state {
    std::atomic<int> cpu_lock_flags;
    u64 cpu_lock_address[4];

    /**
     * TIGbus state data
     *
     * More details in: HRM, 6.3; T64. Detailed information is hard to find...
     *
     * The TIGbus (TTL Integrated Glue Logic) is the interface between the
     *chipset and the interrupt controller, flash ROM, and possibly some other
     *system components.
     **/
    struct SSys_tig {
      u8 FwWrite;
      u8 HaltA;
      u8 HaltB;
      u8 ModInfo;
      u8 ipcr[5]; ///< ipcr0-4 (0xa00-0xb00): PALcode MP restart handshake
    } tig;

    /**
     * CCHIP state data
     *
     * More details in: HRM, 1.2.1.
     *
     * The 21274-C1 Cchip (controller chip) is the heart of the ES40's Typhoon
     *chipset. It interfaces directly with the CPU's through the System address
     *ports, it issues controls to the Dchips (data slice chips) and Pchips
     *(peripheral interface chips) using the Dchip control ports, and the CAPbus
     *(C-And-P-chip bus). It controls memory using the DRAM command and address
     *ports. It also controls the TIGbus.
     **/
    struct SSys_cchip {

      /**
       * DIM: Device Interrupt Mask Registers.
       *
       * These mask registers control which interrupts are allowed to go through
       *to the CPUs. No interrupt in DRIR will get through to the masked
       *interrupt registers (and on to interrupt the CPUs) unless the
       *corresponding mask bit is set in DIMn. All bits are initialized to 0 at
       *reset.
       **/
      u64 dim[4];

      /**
       * DRIR: Device Raw Interrupt Request Register.
       *
       * DRIR indicates which of the 64 possible device interrupts is asserted.
       *
       * \code
       * +---------+---------+---------+------+-------------------------------------+
       * | Field   | Bits    | Type    | Init | Description |
       * +---------+---------+---------+------+-------------------------------------+
       * | ERR     | <63:58> | RO      | 0    | IRQ0 error interrupts | | | | |
       *|    <63> Chip detected MISC<NXM>     | |         |         |         |
       *|    <62> hookup to Pchip0 error      | |         |         |         |
       *|    <61> hookup to Pchip1 errror     |
       * +---------+---------+---------+------+-------------------------------------+
       * | RES     | <57:56> | RO      | 0    | Reserved |
       * +---------+---------+---------+------+-------------------------------------+
       * | DEV     | <55:0>  | RO      | 0    | PCI interrupts pending to the
       *CPU   |
       * +---------+---------+---------+------+-------------------------------------+
       * \endcode
       *
       * Combined with DIM[n] to form DIR[n]:
       *
       * DIR: Device Interrupt Request Registers.
       *
       * These registers indicate which interrupts are pending to the CPUs. If a
       *raw request bit is set and the corresponding mask bit is set, then the
       *corresponding bit in this register will be set and the appropriate CPU
       *will be interrupted.
       **/
      u64 drir;

      /**
       * Miscellaneous Register (MISC - RW).
       *
       * +---------+---------+---------+------+-------------------------------------+
       * | Field   | Bits    | Type    | Init | Description |
       * +---------+---------+---------+------+-------------------------------------+
       * | RES     | <63:44> | MBZ,RAZ | 0    | Reserved. | | DEVSUP  | <43:40>
       *| WO      | 0    | Suppress IRQ1 interrupts to the CPU | |         | |
       *|      | corresponding to a 1 in this field  | |         |         | |
       *| until the interrupt polling machine | |         |         |         |
       *| has completed a poll of all PCI     | |         |         |         |
       *| devices.                            |
       * +---------+---------+---------+------+-------------------------------------+
       * | REV     | <39:32> | RO      | 8    | Latest revision of Cchip |
       * +---------+---------+---------+------+-------------------------------------+
       * | NXS     | <31:29> | RO      | 0    | NXM source - Device that caused
       *NXM | |         |         |         |      | - UNPREDICTABLE if NXM is
       *not set.  | |         |         |         |      |   Value Source | | |
       *|         |      |   0..3  CPU 0..3                    | |         | |
       *|      |   4..5  Pchip 0..1                  |
       * +---------+---------+---------+------+-------------------------------------+
       * | NXM     | <28>    | R,W1C   | 0    | Nonexistent memory address
       *detected.| |         |         |         |      | Sets DRIR<63> and
       *locks the NXS     | |         |         |         |      | field until
       *it is cleared.          |
       * +---------+---------+---------+------+-------------------------------------+
       * | RES     | <27:25> | MBZ,RAZ | 0    | Reserved. |
       * +---------+---------+---------+------+-------------------------------------+
       * | ACL     | <24>    | WO      | 0    | Arbitration clear - writing a 1
       *to  | |         |         |         |      | this bit clears ABT and ABW
       *fields. |
       * +---------+---------+---------+------+-------------------------------------+
       * | ABT     | <23:20> | R,W1S   | 0    | Arbitration try - writing a 1 to
       *| |         |         |         |      | these bits sets them. |
       * +---------+---------+---------+------+-------------------------------------+
       * | ABW     | <19:16> | R,W1S   | 0    | Arbitration won - writing a 1 to
       *| |         |         |         |      | these bits sets them unless one
       *is  | |         |         |         |      | already set, in which case
       *the      | |         |         |         |      | write is ignored. |
       * +---------+---------+---------+------+-------------------------------------+
       * | IPREQ   | <15:12> | WO      | 0    | Interprocessor interrupt request
       *-  | |         |         |         |      | write a 1 to the bit
       *corresponding  | |         |         |         |      | to the CPU you
       *want to interrupt.   | |         |         |         |      | Writing a
       *1 here sets the corres-   | |         |         |         |      |
       *ponding bit in IPINTR.              |
       * +---------+---------+---------+------+-------------------------------------+
       * | IPINTR  | <11:8>  | R,W1C   | 0    | Interprocessor interrupt pending
       *-  | |         |         |         |      | one bit per CPU. Pin irq<3>
       *is      | |         |         |         |      | asserted to the CPU
       *corresponding   | |         |         |         |      | to a 1 in this
       *field.               |
       * +---------+---------+---------+------+-------------------------------------+
       * | ITINTR  | <7:4>   | R,W1C   | 0    | Interval timer interrupt pending
       *-  | |         |         |         |      | one bit per CPU. Pin irq<2>
       *is      | |         |         |         |      | asserted to the CPU
       *corresponding   | |         |         |         |      | to a 1 in this
       *field.               |
       * +---------+---------+---------+------+-------------------------------------+
       * | RES     | <3:2>   | MBZ,RAZ | 0    | Reserved. |
       * +---------+---------+---------+------+-------------------------------------+
       * | CPUID   | <1:0>   | RO      |      | ID of the CPU performing the
       *read.  |
       * +---------+---------+---------+------+-------------------------------------+
       * \endcode
       **/
      u64 misc;
      u64 csc;
    } cchip;

    /**
     * DCHIP state data
     *
     * More details in: HRM, 1.2.2.
     *
     * The ES40 contains eight 21274-D1 Dchips (data slice chips). Each Dchip is
     * responsible for handling 8 bits of the 64-bit data bus (in the ES40,
     *other configurations using less Dchips are possible). Each Dchip
     *interfaces with the Cchip for control, with each of the Pchips, with each
     *of the CPU's and with each of the DRAM arrays.
     **/
    struct SSys_dchip {
      u8 drev;
      u8 dsc;
      u8 dsc2;
      u8 str;
    } dchip;

    /**
     * PCHIP state data
     *
     * More details in: HRM, 1.2.3.
     *
     * The ES40 contains two 21272-P1 Pchips (peripheral interface chips). Each
     *Pchip controls one 64-bit PCI bus, and interfaces it to the Cchip and the
     *Dchips.
     *
     * On PIO transfers from the CPU's (or PTP transfers from the other PCI
     *bus), the Pchip acts as bus master on the PCI bus.
     *
     * On DMA or PTP transfers from a PCI device, the Pchip acts as target on
     *the PCI bus. To determine on which addresses to respond, each Pchip
     *contains 4 DMA/PTP windows, that support both direct mapped and
     *scatter-gather DMA/PTP memory access.
     **/
    struct SSys_pchip {
      u64 plat;
      u64 perr;
      u64 perrmask;
      u64 pctl;
      u64 wsba[4];
      u64 wsm[4];
      u64 tba[4];
    } pchip[2];

    u32 cf8_address[2];
  } state;
  void *memory;

  //    void * memmap;
  int iNumComponents;
  CSystemComponent *acComponents[MAX_COMPONENTS];
  int iNumMemories;
  BulkPort m_bulk[kMaxBulkPorts];
  int m_nbulk = 0;
  struct SMemoryUser *asMemories[MAX_COMPONENTS];

  /// The same ranges again, laid out for the lookup every non-memory access
  /// begins with: the bounds inline rather than behind a hundred separate
  /// allocations, so the scan reads a few cache lines instead of chasing a
  /// pointer per range. Kept in step with asMemories by RegisterMemory.
  struct SMemoryBounds {
    u64 base;
    u64 end; // first byte past the range
  } aMemoryBounds[MAX_COMPONENTS];
  /// The range that answered last. Accesses arrive in runs -- a guest
  /// drawing a window sends thousands in a row to the same card -- so trying
  /// it first turns the scan into one comparison. A stale or torn value only
  /// costs a miss: the bounds are checked before it is used.
  int iLastMemory = -1;

  class CAlphaCPU *acCPUs[4];
public:
  u64 m_direct_base = 0;
  std::atomic<u64> m_direct_end{0};
  u8 *m_direct_host = nullptr;
private:

  CConfigurator *myCfg;

  int iSingleStep;

#if defined(IDB)
  int iSSCycles;
#endif
};

inline u64 CSystem::get_c_misc() { return state.cchip.misc; }

inline u64 CSystem::get_c_dir(int ProcNum) {
  return state.cchip.drir & state.cchip.dim[ProcNum];
}

inline u64 CSystem::get_c_dim(int ProcNum) { return state.cchip.dim[ProcNum]; }

inline void CSystem::set_c_dim(int ProcNum, u64 value) {
  state.cchip.dim[ProcNum] = value;
}

extern CSystem *theSystem;

/* constants for P-Chip CSR's */
#define PCI_PCTL_HOLE U64(0x0000000000000020) /* <5>     */
#define PCI_PCTL_HOLE_START 0x00080000
#define PCI_PCTL_HOLE_END 0x000fffff

/* constants for pci-to-phys-address-mapping */
#define PCI_WSM_MASK U64(0x00000000fff00000)     /* <31:20> */
#define PCI_ADD_MASK U64(0x00000000000fffff)     /* <19:0>  */
#define PCI_TBA_MASK U64(0x00000007fff00000)     /* <34:20> */
#define PCI_PTE_ADD_MASK U64(0x00000000000fe000) /* <19:13> */
#define PCI_PTE_ADD_SHIFT 10
#define PCI_PTE_TBA_MASK U64(0x00000007fffffc00) /* <34:10> */
#define PCI_PTE_MASK U64(0x00000007ffffe000)     /* <34:13> */
#define PCI_PTE_SHIFT 12
#define PCI_PTE_ADD2_MASK U64(0x0000000000001fff) /* <12:0>  */
#define PCI_PTE_PEER_BIT U64(0x0000000090000000)  /* <31,28> */

#define PHYS_PIO_ACCESS U64(0x0000080000000000) /* <43>    */
#endif                                          // !defined(INCLUDED_SYSTEM_H)
