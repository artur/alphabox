/* Alphabox Alpha Emulator
 * Copyright (C) 2020 Tomáš Glozar
 * Website: https://github.com/lenticularis39/axpbox
 *
 * Forked from: ES40 emulator
 * Copyright (C) 2007-2008 by the ES40 Emulator Project
 * Copyright (C) 2007 by Camiel Vanderhoeven
 *
 * Contains code from ES40-Emu:
 * Copyright (C) 2026 gdwnldsKSC and the ES40-Emu contributors
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
 * Contains the definitions for the ISA part of the emulated Ali M1543C chipset.
 **/
#if !defined(INCLUDED_ALIM1543C_H_)
#define INCLUDED_ALIM1543C_H_

#include "I8254.hpp"
#include "I8259.hpp"
#include "Mc146818.hpp"
#include "PCIDevice.hpp"
#include "SuperIo.hpp"

#include <chrono>

/**
 * \brief Emulated ISA part of the ALi M1543C chipset.
 *
 * The ALi M1543C device provides i/o and glue logic support to the system:
 * ISA, DMA, Interrupt, Timer, TOY Clock.
 *
 * Documentation consulted:
 *  - Ali M1543C B1 South Bridge Version 1.20
 *(http://mds.gotdns.com/sensors/docs/ali/1543dScb1-120.pdf)
 *  - Keyboard Scancodes, by Andries Brouwer
 *(http://www.win.tue.nl/~aeb/linux/kbd/scancodes.html)
 *  .
 **/
class CAliM1543C : public CPCIDevice {
public:
  virtual int SaveState(FILE *f);
  virtual int RestoreState(FILE *f);

  //    void instant_tick();
  //    void interrupt(int number);
  virtual void run();
  virtual void check_state();
  virtual void WriteMem_Legacy(int index, u32 address, int dsize, u32 data);
  virtual u32 ReadMem_Legacy(int index, u32 address, int dsize);

  void do_pit_clock();

  CAliM1543C(CConfigurator *cfg, class CSystem *c, int pcibus, int pcidev);
  virtual ~CAliM1543C();
  void pic_interrupt(int index, int intno);
  void pic_deassert(int index, int intno);
  void pic_set_line(int index, int intno, bool active);
  /// The ISA IRQ a routing byte of the bridge's configuration space (such
  /// as 0x74, USBIR) selects, or -1 when it routes nowhere.
  int routed_irq(int config_offset) const;

  void set_floppy_presence(bool driveA, bool driveB);

  void init();
  void start_threads();
  void stop_threads();

private:
  std::unique_ptr<std::thread> myThread;
  std::atomic_bool myThreadDead{false};
  bool StopThread;

  /// Register one of the bridge's ISA I/O ranges, keeping its base port and
  /// length so that a 16- or 32-bit access can be split into the byte
  /// accesses the ISA bus makes of it (ReadMem_Legacy).
  void add_io(int id, u32 base, u32 length);
  u32 m_io_base[MAX_DEV_RANGES] = {};
  u32 m_io_len[MAX_DEV_RANGES] = {};
  /// One byte of a legacy range, at an offset that may lie past its end
  /// (the next port then belongs to whoever decodes it).
  u8 read_byte(int index, u32 address);
  void write_byte(int index, u32 address, u8 data);
  u8 io_byte_read(int index, u32 offset);
  void io_byte_write(int index, u32 offset, u8 data);
  void note_wide_access(int index, u32 address, int dsize, bool write);

  // REGISTER 61 (NMI)
  u8 reg_61_read();
  void reg_61_write(u8 data);
  /// Pacing of a guest stall loop on port 61h (see reg_61_read): the time of
  /// the previous read and how many back-to-back reads there have been.
  u64 m_r61_last_ns = 0;
  unsigned m_r61_run = 0;



public:
  // Period in ns of the MC146818 SQW output (rate from TOY reg A).
  // CPU thread reads this every batch boundary to pace b_irq<2>.
  u64 get_interval_period_ns() const;

private:
  // interrupt controller: the 0x22/0x23 index/data backdoor (the 8259s
  // themselves are CI8259)
  u8 pic_control_read(u32 address);
  void pic_control_write(u32 address, u8 data);

  // LPT controller
  u8 lpt_read(u32 address);
  void lpt_write(u32 address, u8 data);
  void lpt_reset();

  /// The state structure contains all elements that need to be saved to the
  /// statefile.
  struct SAli_state {

    // REGISTER 61 (NMI)
    u8 reg_61;

    // REGISTERS 70 - 73: TOY
    u8 toy_stored_data[256];
    u8 toy_access_ports[4];
    long toy_offset; // seconds: (user-set time) - (host time)

    // Timer/Counter (the 8254, CI8254)
    SPit_state pit;

    // interrupt controller (the 8259s, CI8259)
    SPic_state pic;
    u8 pic_control_index;      // latched index for 0x22/0x23 backdoor
    u8 pic_control_regs[6]{};  // currently unused

    u8 lpt_data;
    u8 lpt_control;
    u8 lpt_status;
    bool lpt_init;

    // SuperIO (CSuperIo)
    SSuperIo_state superio;

    // ISA Plug-and-Play (CIsaPnp)
    SIsaPnp_state isapnp;
  } state;
  // SaveState writes the struct as it is: a change of layout makes old state
  // files unreadable (the size is that of an LP64 host).
  static_assert(sizeof(long) != 8 || sizeof(SAli_state) == 4736,
                "SAli_state is in the state file: its layout must not change");

  /// The 8254 timer, over state.pit.
  CI8254 pit;
  /// The two 8259 interrupt controllers, over state.pic.
  CI8259 pic;
  /// The Super I/O configuration ports (370h-371h), over state.superio.
  CSuperIo superio;
  /// The ISA Plug and Play ports, over state.isapnp.
  CIsaPnp isapnp;
  /// The TOY clock and CMOS RAM, over state.toy_*.
  CMc146818 toy;

  FILE *lpt;

  // sys0 "arc_year_compat" config: when true, encode TOY year as offset
  // from 1980 so the ARC console displays the correct year. Off by default
  // because OpenVMS writes the BBW year in its own (year - 2000) format on
  // SET TIME / shutdown, which would shift internal time forward 20 years
  // here. VMS users wanting ARC to also display correctly should leave
  // this off and accept ARC's year-20 display, or set it true and answer
  // the boot time prompts
  bool arc_year_compat;
};

extern CAliM1543C *theAli;
#endif // !defined(INCLUDED_ALIM1543C_H_)
