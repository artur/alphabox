/* Alphabox Alpha Emulator
 * Copyright (C) 2020 Tomáš Glozar
 * Copyright (C) 2020 Martin Vorländer
 * Copyright (C) 2012 Dmitry Kalinkin
 * Copyright (C) 2026 Artur Goulão
 * Website: https://github.com/lenticularis39/axpbox
 *          https://github.com/artur/alphabox
 *
 * Forked from: ES40 emulator
 * Copyright (C) 2007-2008 by the ES40 Emulator Project
 * Copyright (C) 2007 by Camiel Vanderhoeven
 *
 * Contains code from ES40-Emu:
 * Copyright (C) 2024-2026 gdwnldsKSC and the ES40-Emu contributors
 * Website: https://github.com/ES40-Emu/es40
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

/**
 * \file
 * Contains the code for the emulated Ali M1543C chipset devices.
 **/
#include "AliM1543C.hpp"
#include <cerrno>
#include <cstdlib>
#include "StdAfx.hpp"
#include "System.hpp"
#include "Tga.hpp"
#include "VGA.hpp"
#ifdef __APPLE__
#include <mach/mach_time.h>
#else
#include <time.h>
#endif

/* Timer Calibration: Instructions per Microsecond (assuming 1 clock = 1
 * instruction) */
#define IPus 847
#define REFRESH_TOGGLE_NS 15085ULL // port 61h bit 4 refresh half-period

u32 ali_cfg_data[64] = {
    /*00*/ 0x153310b9, // CFID: vendor + device
    /*04*/ 0x0200000f, // CFCS: command + status
    /*08*/ 0x060100c3, // CFRV: class + revision
    /*0c*/ 0x00000000, // CFLT: latency timer + cache line size
    /*10*/ 0x00000000, // BAR0:
    /*14*/ 0x00000000, // BAR1:
    /*18*/ 0x00000000, // BAR2:
    /*1c*/ 0x00000000, // BAR3:
    /*20*/ 0x00000000, // BAR4:
    /*24*/ 0x00000000, // BAR5:
    /*28*/ 0x00000000, // CCIC: CardBus
    /*2c*/ 0x00000000, // CSID: subsystem + vendor
    /*30*/ 0x00000000, // BAR6: expansion rom base
    /*34*/ 0x00000000, // CCAP: capabilities pointer
    /*38*/ 0x00000000,
    /*3c*/ 0x00000000, // CFIT: interrupt configuration
    /*40*/ 0x00000000,
    // 0x44 byte = IDENRI (IDE channel 0 IRQ route).  0x0d → IRQ 14.
    /*44*/ 0x0000000d,
    /*48*/ 0x00000000, // PIRT[A:D] PCI INTx route — programmed by firmware
    /*4c*/ 0x00000000,
    /*50*/ 0x00000000,
    /*54*/ 0x00000200,
    /*58*/ 0x00000000,
    /*5c*/ 0x00000000, /*60*/ 0x00000000, /*64*/ 0x00000000,
    /*68*/ 0x00000000, /*6c*/ 0x00000000, /*70*/ 0x00000000,
    // 0x74 byte = USBIR (USB IRQ route),       0x03 → IRQ 10
    // 0x75 byte = IDENRII (IDE chan 1 route),  0x0f → IRQ 15
    // 0x76 byte = SCIIR (SCI route),           0x00 → disabled at reset
    // 0x77 byte = SMBIR (SMBus / SCI route),   0x01 → IRQ  9
    /*74*/ 0x01000f03, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0};

u32 ali_cfg_mask[64] = {
    /*00*/ 0x00000000, // CFID: vendor + device
    /*04*/ 0x00000000, // CFCS: command + status
    /*08*/ 0x00000000, // CFRV: class + revision
    /*0c*/ 0x00000000, // CFLT: latency timer + cache line size
    /*10*/ 0x00000000, // BAR0
    /*14*/ 0x00000000, // BAR1: CBMA
    /*18*/ 0x00000000, // BAR2:
    /*1c*/ 0x00000000, // BAR3:
    /*20*/ 0x00000000, // BAR4:
    /*24*/ 0x00000000, // BAR5:
    /*28*/ 0x00000000, // CCIC: CardBus
    /*2c*/ 0x00000000, // CSID: subsystem + vendor
    /*30*/ 0x00000000, // BAR6: expansion rom base
    /*34*/ 0x00000000, // CCAP: capabilities pointer
    /*38*/ 0x00000000,
    /*3c*/ 0x00000000, // CFIT: interrupt configuration
    // Chipset-config wmask.  Byte-granular bits set here mark each PCI-config
    // byte as OS-writable; bits left clear are read-only.  Without writable
    // bits in 0x5c-0x77, AlphaBIOS's writes to USB / IDE-1 / SCI / SMBus IRQ
    // route bytes are silently dropped and the table-build path produces
    // wrong IRQ-routing data.
    /*40*/ 0xffcfff7f,
    /*44*/ 0xff00cbdf,
    /*48*/ 0xffffffff, // PIRT[A:D]
    /*4c*/ 0x000000ff,
    /*50*/ 0xcfff8fff,
    /*54*/ 0xe0ffff00,
    /*58*/ 0x020f0d7f,
    /*5c*/ 0xffe0027f,
    /*60*/ 0x00000000, /*64*/ 0x00000000, /*68*/ 0x00000000,
    /*6c*/ 0x00ffbf00,
    /*70*/ 0xffefefff,
    /*74*/ 0x1fcf1fdf, // USBIR / IDENRII / SCIIR / SMBIR
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

/**
 * Constructor.
 **/
// The 8254's clock, and the TOY's: nanoseconds on the host's monotonic
// clock, and on its wall clock since the Unix epoch.
static u64 steady_ns() {
  return (u64)std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
static s64 real_ns() {
  return (s64)std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

CAliM1543C::CAliM1543C(CConfigurator *cfg, CSystem *c, int pcibus, int pcidev)
    : CPCIDevice(cfg, c, pcibus, pcidev), pit(state.pit, steady_ns),
      // The master's INT output: Cchip DRIR bit 55, the CPU's IRQ1.
      pic(state.pic, [this](bool high) { cSystem->interrupt(55, high); }),
      superio(state.superio,
              [this](int ldn, u16 base) {
                if (ldn == 3) // the parallel port's four registers
                  add_io(27, base, 4);
              }),
      isapnp(state.isapnp, [this](u16 port) { add_io(52, port, 1); }),
      toy(state.toy_stored_data, state.toy_access_ports, state.toy_offset,
          {real_ns, steady_ns}) {
  if (theAli != 0)
    FAILURE(Configuration, "More than one Ali");
  theAli = this;
}

/**
 * Initialize the Ali device.
 **/
void CAliM1543C::init() {
  add_function(0, ali_cfg_data, ali_cfg_mask);

  int i;
  char *filename;

  add_io(1, 0x61, 1);

  state.reg_61 = 0;

  add_io(2, 0x70, 4);
  cSystem->RegisterMemory(this, 2, U64(0x00000801fc000070), 4);
  for (i = 0; i < 4; i++)
    state.toy_access_ports[i] = 0;
  for (i = 0; i < 256; i++)
    state.toy_stored_data[i] = 0;
  state.toy_offset = 0;

  // The MC146818 is battery-backed: register A keeps its divider/rate across
  // power cycles, so on real hardware the 1024Hz periodic tick runs from
  // power-on. SRM's boot-CPU speed calibration (at "lowering IPL", BEFORE it
  // programs the RTC at "create timer") depends on that: with no ticks it
  // times out after ~0.7s and falls back to a hardcoded 2800ps cycle time =
  // the mystery 357 MHz readout.
  state.toy_stored_data[0x0a] = 0x26; // divider on, periodic rate 1024Hz

  arc_year_compat =
      myCfg->get_myParent()->get_bool_value("arc_year_compat", false);
  toy.set_arc_year_compat(arc_year_compat);

  // Optional absolute time override from sys0:
  //   time = "YYYY-MM-DD" or "YYYY-MM-DD HH:MM:SS"
  // Interpreted as UTC to match the runtime ARC/SRM TOY edit path
  // (CMc146818::set_time_from_registers). Sets the initial toy_offset;
  // subsequent guest TOY writes still take precedence and overwrite this
  // offset.
  char *faketime = myCfg->get_myParent()->get_text_value("time");
  if (faketime) {
    struct tm ft = {};
    int n = sscanf(faketime, "%d-%d-%d %d:%d:%d", &ft.tm_year, &ft.tm_mon,
                   &ft.tm_mday, &ft.tm_hour, &ft.tm_min, &ft.tm_sec);
    if (n < 3)
      FAILURE_1(Configuration,
                "Invalid time format: %s (use YYYY-MM-DD or "
                "YYYY-MM-DD HH:MM:SS)",
                faketime);
    ft.tm_year -= 1900;
    ft.tm_mon -= 1;
#ifdef _WIN32
    time_t set_time = _mkgmtime(&ft);
#else
    time_t set_time = timegm(&ft);
#endif
    if (set_time == (time_t)-1)
      FAILURE_1(Configuration, "Invalid time value: %s", faketime);
    time_t host_now;
    time(&host_now);
    state.toy_offset = (long)(set_time - host_now);
  } else {
    // Alphabox 'timezone' option: seed the TOY offset so the guest clock
    // shows host local time by default ("local"), UTC ("utc"), or either
    // with an offset appended: "+<n>" or "-<n>", then s|m|h|d|M|y
    // (e.g. "local+2h", "local-20M", "utc+20y"). Guest TOY writes and the
    // sys0 'time' override still take precedence over this initial seed.
    std::string timezone{myCfg->get_text_value("timezone", "local")};
    std::string timebase;
    bool offset_present = false;
    long offset = 0;

    if (timezone.rfind("local", 0) == 0)
      timebase = "local";
    else if (timezone.rfind("utc", 0) == 0)
      timebase = "utc";
    else
      FAILURE_1(Configuration, "Invalid timezone %s", timezone.c_str());

    // The offset: a sign, digits and one unit letter, nothing else.
    const char *rest = timezone.c_str() + timebase.length();
    if (*rest) {
      char *end = nullptr;
      errno = 0;
      if (*rest == '+' || *rest == '-')
        offset = strtol(rest, &end, 10);
      if (!end || end == rest + 1 || errno || !end[0] || end[1])
        FAILURE_1(Configuration, "Invalid timezone %s", timezone.c_str());
      offset_present = true;
    }

    time_t host_now;
    time(&host_now);
    time_t shifted = host_now;

    if (offset_present) {
      switch (timezone.at(timezone.length() - 1)) {
      case 's':
        shifted += offset;
        break;
      case 'm':
        shifted += offset * 60;
        break;
      case 'h':
        shifted += offset * 3600;
        break;
      case 'd':
        shifted += offset * 86400;
        break;
      case 'M':
      case 'y':
        break;
      default:
        FAILURE_1(Configuration, "Invalid timezone offset type %c",
                  timezone.at(timezone.length() - 1));
      }
    }

    struct tm target = {};
    if (timebase == "local") {
#ifdef _WIN32
      localtime_s(&target, &shifted);
#else
      localtime_r(&shifted, &target);
#endif
    } else {
#ifdef _WIN32
      gmtime_s(&target, &shifted);
#else
      gmtime_r(&shifted, &target);
#endif
    }

    if (offset_present) {
      switch (timezone.at(timezone.length() - 1)) {
      case 'M':
        target.tm_year += (int)(offset / 12);
        target.tm_mon += (int)(offset % 12);
        break;
      case 'y':
        target.tm_year += (int)offset;
        break;
      }
    }

    // The TOY itself runs in UTC + toy_offset, so express the target
    // wall-clock as a UTC delta against the host clock.
#ifdef _WIN32
    time_t seeded = _mkgmtime(&target);
#else
    time_t seeded = timegm(&target);
#endif
    if (seeded != (time_t)-1)
      state.toy_offset = (long)(seeded - host_now);
  }

  state.toy_stored_data[0x17] = myCfg->get_bool_value("vga_console") ? 1 : 0;

  // A graphics console needs a card the console can draw on: a VGA, or
  // the TGA, which the console drives with its own driver.
  if (state.toy_stored_data[0x17] && !theVGA && !theTGA) {
    printf("! CONFIGURATION WARNING ! vga_console set to true, but no VGA card "
           "installed.\n");
    state.toy_stored_data[0x17] = 0;
  }

  ResetPCI();

  // PIT Setup
  add_io(6, 0x40, 4);
  pit.reset();
  pit.load_gate(2, (state.reg_61 & 0x01) != 0);

  add_io(7, 0x20, 2);
  add_io(8, 0xa0, 2);
  add_io(30, 0x4d0, 2);
  add_io(9, 0x22, 2); // PIC backdoor control
  state.pic_control_index = 0;

  // odd one, byte read in PCI IACK (interrupt acknowledge) cycle. Interrupt
  // vector.
  cSystem->RegisterMemory(this, 20, U64(0x00000801f8000000), 1);

  pic.reset();

  // Initialize parallel port - IO registration is SuperIO driven, so not here,
  // just output handling
  filename = myCfg->get_text_value("lpt.outfile");
  if (filename) {
    lpt = fopen(filename, "ab");
  } else {
    lpt = NULL;
  }

  lpt_reset();

  // SuperIO
  add_io(40, 0x370, 2);

  // Populate the SuperIO chip-level / LDN register banks
  // power on defaults
  superio.reset();

  // ISA Plug-and-Play address (0x279) and write-data (0xA79) ports.
  // The OS-selectable READ_DATA port (id 52) is registered dynamically
  // when the OS programs it via PnP register 0x00.
  add_io(50, 0x279, 1);
  add_io(51, 0xa79, 1);
  isapnp.reset();

  myThread = nullptr;

  printf("%s: $Id$\n", devid_string);
}

void CAliM1543C::start_threads() {
  if (!myThread) {
    printf(" ali");
    StopThread = false;
    myThread = std::make_unique<std::thread>([this]() { this->run(); });
  }
}

void CAliM1543C::stop_threads() {
  StopThread = true;
  if (myThread) {
    printf(" ali");
    myThread->join();
    myThread = nullptr;
  }
}

/**
 * Destructor.
 **/
CAliM1543C::~CAliM1543C() {
  stop_threads();

  if (lpt)
    fclose(lpt);
}

/**
 * Read (byte,word,longword) from one of the legacy ranges. A word or longword
 * is served as byte accesses at consecutive ports, as the ISA bus does.
 *
 * Ranges are:
 *  - 1. I/O port 61h
 *  - 2. I/O ports 70h-73h (time-of-year clock)
 *  - 6. I/O ports 40h-43h (programmable interrupt timer)
 *  - 7. I/O ports 20h-21h (primary programmable interrupt controller)
 *  - 9. I/O ports 22h-23h (PIC8259 control register index/data backdoor)
 *  - 8. I/O ports a0h-a1h (secondary (cascaded) programmable interrupt
 *controller)
 *  - 20. PCI IACK address (interrupt vector)
 *  - 40. I/O ports 370h-371h (SuperIO)
 *  - 27. I/O ports 3bch-3bfh (parallel port)
 *  - 30. I/O ports 4d0h-4d1h (edge/level register of programmable interrupt
 *controller)
 *  .
 **/
u32 CAliM1543C::ReadMem_Legacy(int index, u32 address, int dsize) {
  // The interrupt vector is read in a PCI IACK cycle, whatever its size.
  if (dsize == 8 || index == 20)
    return read_byte(index, address);

  // The registers behind these ranges are eight bits wide: the bus serves a
  // 16- or 32-bit access as one byte access per byte lane, at consecutive
  // ports, the lowest port in the low byte.
  note_wide_access(index, address, dsize, false);
  u32 data = 0;
  for (int i = 0; i < dsize / 8; i++)
    data |= (u32)io_byte_read(index, address + (u32)i) << (8 * i);
  return data;
}

void CAliM1543C::add_io(int id, u32 base, u32 length) {
  add_legacy_io(id, base, length);
  m_io_base[id] = base;
  m_io_len[id] = length;
}

u8 CAliM1543C::io_byte_read(int index, u32 offset) {
  if (offset < m_io_len[index])
    return read_byte(index, offset);
  // Past the end of this range: the port is someone else's to answer.
  return (u8)cSystem->ReadMem(bus_address(true, m_io_base[index] + offset), 8,
                              this);
}

void CAliM1543C::io_byte_write(int index, u32 offset, u8 data) {
  if (offset < m_io_len[index])
    write_byte(index, offset, data);
  else
    cSystem->WriteMem(bus_address(true, m_io_base[index] + offset), 8, data,
                      this);
}

void CAliM1543C::note_wide_access(int index, u32 address, int dsize,
                                  bool write) {
  static std::atomic_bool reported{false};
  if (!reported.exchange(true))
    printf("%%ALI-W-WIDEIO: %d-bit %s of I/O port %03x served as byte "
           "accesses (reported once)\n",
           dsize, write ? "write" : "read", m_io_base[index] + address);
}

u8 CAliM1543C::read_byte(int index, u32 address) {
  switch (index) {
  case 1:
    return reg_61_read();

  case 2:
    return toy.read(address);

  case 6:
    return pit.read(address);

  case 7:
    return pic.read(0, address);

  case 8:
    return pic.read(1, address);

  case 9:
    return pic_control_read(address);

  case 20:
    return pic.read_vector();

  case 30:
    return pic.read_elcr(address);

  case 40:
    return superio.read(address);

  case 27:
    return lpt_read(address);

  case 50: // 0x279 ADDRESS — write-only on real cards
  case 51: // 0xA79 WRITE_DATA — write-only on real cards
    return 0xff;

  case 52: // OS-programmed READ_DATA port — no cards => 0xff
    return isapnp.data_read();
  }

  return 0;
}

/**
 * Write (byte,word,longword) to one of the legacy ranges. A word or longword
 * is served as byte accesses at consecutive ports, as the ISA bus does.
 *
 * Ranges are:
 *  - 1. I/O port 61h
 *  - 2. I/O ports 70h-73h (time-of-year clock)
 *  - 6. I/O ports 40h-43h (programmable interrupt timer)
 *  - 7. I/O ports 20h-21h (primary programmable interrupt controller)
 *  - 9. I/O ports 22h-23h (PIC8259 control register index/data backdoor)
 *  - 8. I/O ports a0h-a1h (secondary (cascaded) programmable interrupt
 *controller)
 *  - 12. I/O ports 00h-0fh (primary DMA controller)
 *  - 13. I/O ports c0h-dfh (secondary DMA controller)
 *  - 20. PCI IACK address (interrupt vector)
 *  - 40. I/O ports 370h-371h (SuperIO)
 *  - 27. I/O ports 3bch-3bfh (parallel port)
 *  - 30. I/O ports 4d0h-4d1h (edge/level register of programmable interrupt
 *controller)
 *  - 33. I/O ports 80h-8fh (DMA controller memory base low page register)
 *  - 34. I/O ports 480h-48fh (DMA controller memory base high page register)
 *  .
 **/
void CAliM1543C::WriteMem_Legacy(int index, u32 address, int dsize, u32 data) {
  if (dsize == 8) {
    write_byte(index, address, (u8)data);
    return;
  }
  // One byte access per byte lane, as for a read (ReadMem_Legacy). The
  // console writes the Super I/O's index and data ports with one 16-bit
  // write; that one is not worth a notice.
  if (index != 40)
    note_wide_access(index, address, dsize, true);
  for (int i = 0; i < dsize / 8; i++)
    io_byte_write(index, address + (u32)i, (u8)(data >> (8 * i)));
}

void CAliM1543C::write_byte(int index, u32 address, u8 data) {
  switch (index) {
  case 1:
    reg_61_write((u8)data);
    return;

  case 2:
    toy.write(address, data);
    return;

  case 6:
    pit.write(address, data);
    return;

  case 7:
    pic.write(0, address, data);
    return;

  case 8:
    pic.write(1, address, data);
    return;

  case 9:
    pic_control_write(address, (u8)data);
    return;

  case 30:
    pic.write_elcr(address, data);
    return;

  case 40:
    superio.write(address, data);
    return;

  case 27:
    lpt_write(address, (u8)data);
    return;

  case 50: // 0x279 ADDRESS
    isapnp.addr_write(data);
    return;

  case 51: // 0xA79 WRITE_DATA
    isapnp.data_write(data);
    return;

  case 52: // OS-programmed READ_DATA port — read-only on real cards
    return;
  }
}

/**
 * Read port 61h (speaker/ miscellaneous).
 *
 * BDW:
 * This may need some expansion to help with timer delays.  It looks like
 * the 8254 flips bits on occasion, and the linux kernel (at least) uses
 *   do {
 *     count++;
 *   } while ((inb(0x61) & 0x20) == 0 && count < TIMEOUT_COUNT);
 * to calibrate the cpu clock.
 *
 * Every 1500 reads the bit gets flipped so maybe the timing will
 * seem reasonable to the OS.
 */
// Sleep the calling thread until an absolute steady_clock instant, as
// precisely as the host allows: mach_wait_until on macOS (steady_clock is
// mach_absolute_time there), clock_nanosleep with TIMER_ABSTIME elsewhere.
#ifdef __APPLE__
static mach_timebase_info_data_t g_tb = [] {
  mach_timebase_info_data_t t;
  mach_timebase_info(&t);
  return t;
}();
#endif
// The pacing clock: nanoseconds on the same clock the deadline is waited
// on (mach_absolute_time on Apple, where steady_clock's epoch is not
// guaranteed to be it; CLOCK_MONOTONIC elsewhere).
static u64 pace_now_ns() {
#ifdef __APPLE__
  return mach_absolute_time() * g_tb.numer / g_tb.denom;
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
#endif
}
static void sleep_until_ns(u64 ns) {
#ifdef __APPLE__
  mach_wait_until(ns * g_tb.denom / g_tb.numer);
#else
  struct timespec ts;
  ts.tv_sec = (time_t)(ns / 1000000000ull);
  ts.tv_nsec = (long)(ns % 1000000000ull);
  clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr);
#endif
}

u8 CAliM1543C::reg_61_read() {
  // Bit 4: DRAM refresh request, toggling every ~15 us of wall-clock time
  // (delay loops poll it). Bit 5: counter 2's output, from its analytic phase
  // so pollers see jitter-free edges.
  u64 refresh_ns =
      (u64)std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count();
  // Pacing. A guest stall loop (a HAL's KeStallExecutionProcessor counts
  // refresh toggles) reads this port millions of times a second and burns a
  // host core for the length of its stall -- 22M reads over one 10 s phase
  // of a Windows 2000 boot. After a run of back-to-back reads inside one
  // half-period the thread sleeps to the NEXT toggle edge and answers with
  // the post-edge value: the guest sees the toggle sequence it would have
  // seen, at the wall-clock instants it happens (the edge is computed, not
  // slept "for"), and the core is free in between. ALPHABOX_PORT61_PACE=0
  // turns it off.
  //
  // Not to be confused with ALPHABOX_STALL_SKIP, which is a different guest
  // busy-wait and the opposite answer to it: this one keeps the guest's
  // timing exactly and frees the host core, that one gives the guest its
  // time back early. This port is a clock the guest is reading, so its
  // timing cannot be shortened; a delay loop's can.
  static const bool pace = [] {
    const char *e = getenv("ALPHABOX_PORT61_PACE");
    return !(e && e[0] == '0');
  }();
  // ALPHABOX_TRACE_PORT61=1: each burst of port 61h reads as wall-clock time
  // and count (a burst ends when 50 ms pass without a read), so pacing can
  // be checked for what matters: the guest's stall must take the same time.
  static const bool trace = getenv("ALPHABOX_TRACE_PORT61") != nullptr;
  if (trace) {
    // Bursts are summed up and printed at exit (the last one never ends
    // with a read after it): what matters is that the guest's stalls take
    // the same wall time paced as unpaced, with fewer reads.
    static u64 burst_start = 0, burst_last = 0, burst_n = 0;
    static u64 sum_ns = 0, longest_ns = 0, bursts = 0, reads = 0;
    static const bool registered = [] {
      atexit([] {
        const u64 open_ns = burst_last - burst_start;
        printf("STALL bursts of port 61h reads: %llu bursts over 1000 reads, "
               "%llu reads, %.1f ms in all, longest %.1f ms\n",
               (unsigned long long)(bursts + (burst_n > 1000)),
               (unsigned long long)(reads + burst_n),
               (double)(sum_ns + (burst_n > 1000 ? open_ns : 0)) / 1e6,
               (double)(longest_ns > open_ns ? longest_ns : open_ns) / 1e6);
      });
      return true;
    }();
    (void)registered;
    const u64 t = pace_now_ns();
    if (burst_n && t - burst_last > 50000000ull) {
      if (burst_n > 1000) {
        const u64 d = burst_last - burst_start;
        bursts++;
        reads += burst_n;
        sum_ns += d;
        if (d > longest_ns)
          longest_ns = d;
      }
      burst_n = 0;
    }
    if (!burst_n)
      burst_start = t;
    burst_last = t;
    burst_n++;
  }
  if (pace) {
    const u64 now = pace_now_ns();
    m_r61_run = (now - m_r61_last_ns < 3000) ? m_r61_run + 1 : 0;
    if (m_r61_run >= 64) {
      // The next edge on the pacing clock, and at most one half-period
      // away: a deadline can never be far off, whatever the clocks do.
      const u64 edge = (now / REFRESH_TOGGLE_NS + 1) * REFRESH_TOGGLE_NS;
      sleep_until_ns(edge);
      const u64 woke = pace_now_ns();
      // Report the toggle as the pacing clock sees it, so the sleep and the
      // value agree; the two clocks tick at the same rate.
      refresh_ns += (woke > now ? woke - now : 0);
      m_r61_run = 0;
      m_r61_last_ns = woke;
    } else {
      m_r61_last_ns = now;
    }
  }
  state.reg_61 &= ~0x30;
  if ((refresh_ns / REFRESH_TOGGLE_NS) & 1U)
    state.reg_61 |= 0x10;
  if (pit.out(2))
    state.reg_61 |= 0x20;
  return state.reg_61;
}

/**
 * Write port 61h (speaker/ miscellaneous).
 **/
void CAliM1543C::reg_61_write(u8 data) {
  state.reg_61 = (state.reg_61 & 0xf0) | (((u8)data) & 0x0f);
  // Bit 0 is counter 2's GATE input.
  pit.set_gate(2, (data & 0x01) != 0);
}

// ALPHABOX_IRQSTATS: print (and reset) the interrupt counters.
static void print_irqstats(double secs) {
  SIrqStats &s = g_irqstats;
  char buf[1400];
  const int cap = (int)sizeof(buf) - 48;
  int len = snprintf(buf, sizeof(buf), "%%IRQ-I-STATS %.1fs: cpu-int %llu [eir",
                     secs, (unsigned long long)s.cpu_int.exchange(0));
  for (int b = 0; b < 6; b++)
    len += snprintf(buf + len, sizeof(buf) - len, " %d:%llu", b,
                    (unsigned long long)s.cpu_eir[b].exchange(0));
  len += snprintf(buf + len, sizeof(buf) - len,
                  " sw:%llu ast:%llu] cchip-timer %llu | isa-ack",
                  (unsigned long long)s.cpu_sw.exchange(0),
                  (unsigned long long)s.cpu_ast.exchange(0),
                  (unsigned long long)s.cchip_timer.exchange(0));
  for (int i = 0; i < 16 && len < cap; i++)
    if (const u64 v = s.isa_ack[i].exchange(0))
      len += snprintf(buf + len, sizeof(buf) - len, " irq%d:%llu", i,
                      (unsigned long long)v);
  len += snprintf(buf + len, sizeof(buf) - len, " | isa-edge");
  for (int i = 0; i < 16 && len < cap; i++)
    if (const u64 v = s.isa_edge[i].exchange(0))
      len += snprintf(buf + len, sizeof(buf) - len, " irq%d:%llu", i,
                      (unsigned long long)v);
  len += snprintf(buf + len, sizeof(buf) - len, " | drir-rise");
  for (int i = 0; i < 64 && len < cap; i++)
    if (const u64 v = s.drir_rise[i].exchange(0))
      len += snprintf(buf + len, sizeof(buf) - len, " #%d:%llu", i,
                      (unsigned long long)v);
  printf("%s\n", buf);
}

/**
 * Thread entry point.
 **/
void CAliM1543C::run() {
  const bool irqstats = getenv("ALPHABOX_IRQSTATS") != nullptr;
  auto irq_last = std::chrono::steady_clock::now();
  try {
    for (;;) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      if (StopThread)
        return;
      do_pit_clock();
      if (irqstats) {
        const auto now = std::chrono::steady_clock::now();
        if (now - irq_last >= std::chrono::seconds(5)) {
          print_irqstats(std::chrono::duration<double>(now - irq_last).count());
          irq_last = now;
        }
      }
    }
  }

  catch (CException &e) {
    printf("Exception in Ali thread: %s.\n", e.displayText().c_str());
    myThreadDead.store(true);

    // Let the thread die...
  }
}

/**
 * Handle all events that need to be handled on a clock-driven basis.
 *
 * This is a slow-clocked device, which means this DoClock isn't called as often
 *as the CPU's DoClock. Do the following:
 *  - Handle PIT clock (wall-clock paced, so the call cadence only sets
 *granularity).
 *  .
 **/
void CAliM1543C::do_pit_clock() {
  // Counter 0's output is tied to IRQ0: one edge per rising edge of OUT0,
  // raised with the timer's lock released.
  for (int n = pit.clock(); n > 0; n--)
    pic_interrupt(0, 0);
}

u64 CAliM1543C::get_interval_period_ns() const {
  return toy.interval_period_ns();
}

// 8259 PIC

void CAliM1543C::pic_control_write(u32 address, u8 data) {
  if (address == 0) { // port 0x22 — index latch (write-only)
    state.pic_control_index = data;
    return;
  }

  // port 0x23 — data; dispatch by latched index
  switch (state.pic_control_index) {
  case 0x03:
    pic.write_elcr(0, data);
    return; // ELCR_1
  case 0x04:
    pic.write_elcr(1, data);
    return; // ELCR_2
  case 0x01:
  case 0x02:
  case 0x05:
    // PIC CFG_1/CFG_2/RTC_CFG — not modelled, ignore
    return;
  default:
    printf("%s: unknown PIC control index %02x = %02x\n", devid_string,
           state.pic_control_index, data);
    return;
  }
}

u8 CAliM1543C::pic_control_read(u32 address) {
  if (address == 0) {
    // 0x22 is write-only per the header; return last index as harmless readback
    return state.pic_control_index;
  }

  switch (state.pic_control_index) {
  case 0x03:
    return pic.read_elcr(0);
  case 0x04:
    return pic.read_elcr(1);
  case 0x01:
  case 0x02:
  case 0x05:
    return 0;
  default:
    return 0xff;
  }
}

/**
 * Assert, de-assert or drive an interrupt input of the 8259s (CI8259
 * interrupt, deassert and set_line say which a device wants).
 **/
void CAliM1543C::pic_interrupt(int index, int intno) {
  pic.interrupt(index, intno);
}

void CAliM1543C::pic_deassert(int index, int intno) {
  pic.deassert(index, intno);
}

void CAliM1543C::pic_set_line(int index, int intno, bool active) {
  pic.set_line(index, intno, active);
}

// M1543C interrupt routing bytes (PIRT, USBIR, ...): the low four bits name
// the ISA IRQ through a fixed table, 0 and the gaps meaning none.
int CAliM1543C::routed_irq(int config_offset) const {
  static const int table[16] = {-1, 9,  3,  10, 4,  5,  7,  6,
                                -1, 11, -1, 12, -1, 14, -1, 15};
  const u32 dw = endian_32(pci_state.config_data[0][config_offset / 4]);
  return table[(dw >> (8 * (config_offset & 3))) & 0xf];
}

bool CAliM1543C::irq_requested(int irq) { return pic.requested(irq); }

void CAliM1543C::add_usbir_listener(const void *owner,
                                    std::function<void()> fn) {
  std::lock_guard<std::mutex> guard(usbirLock);
  for (auto &l : usbir_listeners)
    if (l.first == owner) {
      l.second = std::move(fn);
      return;
    }
  usbir_listeners.emplace_back(owner, std::move(fn));
}

void CAliM1543C::remove_usbir_listener(const void *owner) {
  std::lock_guard<std::mutex> guard(usbirLock);
  for (size_t i = 0; i < usbir_listeners.size(); i++)
    if (usbir_listeners[i].first == owner) {
      usbir_listeners.erase(usbir_listeners.begin() + i);
      return;
    }
}

/**
 * A write to the bridge's USBIR byte moves the ISA IRQ the USB function's
 * level interrupt leaves by (datasheet 1.10, p80): the function re-drives
 * its line through the new routing, which releases the old one.
 **/
void CAliM1543C::config_write_custom(int func, u32 address, int dsize, u32, u32,
                                     u32) {
  if (func != 0 || !(address <= 0x74 && 0x74 < address + (u32)dsize / 8))
    return;
  std::vector<std::function<void()>> todo;
  {
    std::lock_guard<std::mutex> guard(usbirLock);
    for (auto &l : usbir_listeners)
      todo.push_back(l.second);
  }
  for (auto &fn : todo)
    fn();
}

static u32 ali_magic1 = 0xA111543C;
static u32 ali_magic2 = 0xC345111A;

/**
 * Save state to a Virtual Machine State file.
 **/
int CAliM1543C::SaveState(FILE *f) {
  long ss = sizeof(state);
  int res;

  if ((res = CPCIDevice::SaveState(f)))
    return res;

  // The timer's counts as of now: its phase references are host times, so
  // the file carries where each counter is in its count instead.
  SAli_state saved = state;
  pit.save(saved.pit);

  fwrite(&ali_magic1, sizeof(u32), 1, f);
  fwrite(&ss, sizeof(long), 1, f);
  fwrite(&saved, sizeof(saved), 1, f);
  fwrite(&ali_magic2, sizeof(u32), 1, f);
  printf("%s: %d bytes saved.\n", devid_string, (int)ss);
  return 0;
}

/**
 * Restore state from a Virtual Machine State file.
 **/
int CAliM1543C::RestoreState(FILE *f) {
  long ss;
  u32 m1;
  u32 m2;
  int res;
  size_t r;

  if ((res = CPCIDevice::RestoreState(f)))
    return res;

  r = fread(&m1, sizeof(u32), 1, f);
  if (r != 1) {
    printf("%s: unexpected end of file!\n", devid_string);
    return -1;
  }

  if (m1 != ali_magic1) {
    printf("%s: MAGIC 1 does not match!\n", devid_string);
    return -1;
  }

  r = fread(&ss, sizeof(long), 1, f);
  if (r != 1) {
    printf("%s: unexpected end of file!\n", devid_string);
    return -1;
  }

  if (ss != sizeof(state)) {
    printf("%s: STRUCT SIZE does not match!\n", devid_string);
    return -1;
  }

  r = fread(&state, sizeof(state), 1, f);
  if (r != 1) {
    printf("%s: unexpected end of file!\n", devid_string);
    return -1;
  }

  r = fread(&m2, sizeof(u32), 1, f);
  if (r != 1) {
    printf("%s: unexpected end of file!\n", devid_string);
    return -1;
  }

  if (m2 != ali_magic2) {
    printf("%s: MAGIC 2 does not match!\n", devid_string);
    return -1;
  }

  // The 8254's GATE2 input is port 61h bit 0, which the state just set;
  // then each counter goes on from its saved count.
  pit.load_gate(2, (state.reg_61 & 0x01) != 0);
  pit.restored();
  // The ranges the guest placed: the parallel port and the PnP READ_DATA
  // port are mapped when it writes their registers, which a restore does
  // not do.
  superio.restored();
  isapnp.restored();

  printf("%s: %d bytes restored.\n", devid_string, (int)ss);
  return 0;
}

/**
 * Parallel Port information:
 * address 0 (R/W):  data pins.  On read, the last byte written is returned.
 *
 *
 * address 1 (R): status register
 * \code
 *   1 0 0 0 0 0 00 <-- default
 *   ^ ^ ^ ^ ^ ^ ^
 *   | | | | | | +- Undefined
 *   | | | | | +--- IRQ (undefined?)
 *   | | | | +----- printer has error condition
 *   | | | +------- printer is not selected.
 *   | | +--------- printer has paper (online)
 *   | +----------- printer is asserting 'ack'
 *   +------------- printer busy (active low).
 * \endcode
 *
 * address 2 (R/W): control register.
 * \code
 *   00 0 0 1 0 1 1  <-- default
 *   ^  ^ ^ ^ ^ ^ ^
 *   |  | | | | | +-- Strobe (active low)
 *   |  | | | | +---- Auto feed (active low)
 *   |  | | | +------ Initialize
 *   |  | | +-------- Select (active low)
 *   |  | +---------- Interrupt Control
 *   |  +------------ Bidirectional control (unimplemented)
 *   +--------------- Unused
 * \endcode
 **/
void CAliM1543C::lpt_reset() {
  state.lpt_data = '\xff';
  state.lpt_status = 0xd8;  // busy, ack, online, error
  state.lpt_control = 0x0c; // select, init
  state.lpt_init = false;
}

/**
 * Read a byte from one of the parallel port controller's registers.
 **/
u8 CAliM1543C::lpt_read(u32 address) {
  if (!(state.superio.ldn_regs[3][0x30] & 0x01))
    return 0xff;
  u8 data = 0;
  switch (address) {
  case 0:
    data = state.lpt_data;
    break;

  case 1:
    data = state.lpt_status;
    if ((state.lpt_status & 0x80) == 0 && (state.lpt_control & 0x01) == 0) {
      if (state.lpt_status & 0x40) { // test ack
        state.lpt_status &= ~0x40;   // turn off ack
      } else {
        state.lpt_status |= 0x40; // set ack.
        state.lpt_status |= 0x80; // set (not) busy.
      }
    }
    break;

  case 2:
    data = state.lpt_control;
  }

#ifdef DEBUG_LPT
  printf("%%LPT-I-READ: port %d = %x\n", address, data);
#endif
  return data;
}

/**
 * Write a byte to one of the parallel port controller's registers.
 **/
void CAliM1543C::lpt_write(u32 address, u8 data) {
  if (!(state.superio.ldn_regs[3][0x30] & 0x01))
    return;
#ifdef DEBUG_LPT
  printf("%%LPT-I-WRITE: port %d = %x\n", address, data);
#endif
  switch (address) {
  case 0:
    state.lpt_data = data;
    break;

  case 1:
    break;

  case 2:
    if ((data & 0x04) == 0) {
      state.lpt_init = true;
      state.lpt_status = 0xd8;
    } else {
      if (data & 0x08) {             // select bit
        if (data & 0x01) {           // strobe?
          state.lpt_status &= ~0x80; // we're busy

          // do the write!
          if (lpt && state.lpt_init)
            fputc(state.lpt_data, lpt);
          if (state.lpt_control & 0x10) {
            pic_interrupt(0, 7);
          }
        } else {

          // ?
        }
      }
    }

    state.lpt_control = data;
  }
}

void CAliM1543C::set_floppy_presence(bool driveA, bool driveB) {
  u8 cmos = 0;
  if (driveA)
    cmos |= 0x40; // Drive A: 1.44MB 3.5"
  if (driveB)
    cmos |= 0x04; // Drive B: 1.44MB 3.5"
  state.toy_stored_data[0x10] = cmos;
  state.superio.ldn_regs[0][0x30] = 0x01; // the controller exists regardless
}

/**
 * Check if threads are still running.
 **/
void CAliM1543C::check_state() {
  if (myThreadDead.load())
    FAILURE(Thread, "ALi thread has died");

  // HACK HACK HACK HACK ALERT
  // ENABLES BSD SYSTEMS TO BOOT WITHOUT panic: pci_display_console: no device
  // at 255/255/0 SRM builds the HWPRB / CTB Fully! BUT! Does not properly set
  // the CTB turboslot Theory: TGA/TGA2 would set properly, but SRM does not
  // 'know' about others to set. Investigate possibly how to make SRM happy.
  // This appears to be an issue on real hardware as well
  static bool ctb_fixed = false;

  if (!ctb_fixed && theVGA) {
    const u64 HWRPB_BASE = U64(0x2000);
    const u64 HWRPB_MAGIC = U64(0x0000004250525748); // "HWRPB\0\0\0" LE
    const u64 CTB_OFF_OFF = U64(0xB8);  // offset of rpb_ctb_off in HWRPB
    const u64 CTB_TYPE_OFF = U64(0x00); // offset of ctb_type in CTB
    const u64 CTB_TS_OFF = U64(0xF0);   // offset of ctb_turboslot in CTB
    const u64 CTB_GRAPHICS = U64(4);    // ctb_type for graphics console

    // Step 1: Check if HWRPB is built (magic signature present)
    u64 magic = cSystem->ReadMem(HWRPB_BASE + U64(0x08), 64, this);
    // Temporary: dump HWRPB fields to find ctb_off
    if (magic != HWRPB_MAGIC)
      return; // SRM hasn't built the HWRPB yet

    // Step 2: Read CTB offset from HWRPB
    u64 ctb_off = cSystem->ReadMem(HWRPB_BASE + CTB_OFF_OFF, 64, this);
    if (ctb_off == 0 || ctb_off > U64(0x100000))
      return; // CTB offset not valid (yet)

    // After step 2, before step 3, add a one-shot CTB dump:
    u64 ctb_phys = HWRPB_BASE + ctb_off;

#ifdef DEBUG_HWRPB_TURBOSLOT
    static bool ctb_dumped = false;
    if (!ctb_dumped) {
      printf("%s: CTB dump at phys 0x%" PRIx64 " (ctb_off=0x%" PRIx64 "):\n",
             devid_string, ctb_phys, ctb_off);
      for (int i = 0; i < 0x100; i += 8) {
        u64 val = cSystem->ReadMem(ctb_phys + i, 64, this);
        printf("  CTB+0x%03X (0x%05" PRIx64 "): 0x%016" PRIx64 "\n", i,
               ctb_phys + i, val);
      }
      ctb_dumped = true;
    }
#endif

    // Step 3: Verify this is a graphics console CTB
    u64 ctb_type = cSystem->ReadMem(ctb_phys + CTB_TYPE_OFF, 64, this);
    if (ctb_type == 0)
      return; // CTB not populated yet — try again later
    if (ctb_type != CTB_GRAPHICS) {
      // Confirmed serial/printer console — turboslot doesn't matter
      ctb_fixed = true;
      return;
    }

    // Step 4: Read current turboslot value
    u64 turboslot = cSystem->ReadMem(ctb_phys + CTB_TS_OFF, 64, this);

    // Step 5: Check if it needs fixing
    // SRM leaves this as all-FF for non-TGA adapters
    // Also fix if it's zero (uninitialized)
    bool needs_fix = ((turboslot & U64(0xFFFF)) == U64(0xFFFF));

    if (needs_fix) {
      int vga_bus = theVGA->pci_bus() & 0xFF;
      int vga_dev = theVGA->pci_dev() & 0xFF;
      u64 ts = (U64(0x0003) << 16) | (((u64)vga_bus) << 8) | ((u64)vga_dev);
      cSystem->WriteMem(ctb_phys + CTB_TS_OFF, 64, ts, this);
      static bool printed = false;
#ifdef DEBUG_HWRPB_TURBOSLOT
      if (!printed) {
        printf("%s: fixed CTB turboslot at phys 0x%" PRIx64 " from 0x%" PRIx64
               " to 0x%08" PRIx64 " (PCI bus=%d dev=%d)\n",
               devid_string, ctb_phys + CTB_TS_OFF, turboslot, ts, vga_bus,
               vga_dev);
      }
#endif
    } else if (turboslot ==
               ((U64(0x0003) << 16) | (((u64)(theVGA->pci_bus() & 0xFF)) << 8) |
                ((u64)(theVGA->pci_dev() & 0xFF)))) {
      // Our value is there and SRM didn't overwrite it — we're done
      ctb_fixed = true;
    }
  }
}

CAliM1543C *theAli = 0;
