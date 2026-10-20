/* Alphabox Alpha Emulator
 * Copyright (C) 2026 Artur Goulão
 * Website: https://github.com/artur/alphabox
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
 */

/* The cascaded pair of 8259A interrupt controllers of the ALi M1543C (see
 * I8259.hpp).
 */

#include "I8259.hpp"
#include "IrqStats.hpp"

#include <cstdio>

#ifdef DEBUG_PIC
static bool pic_messages = false;
#endif

void CI8259::reset() {
  std::lock_guard<std::mutex> guard(lock);
  for (int i = 0; i < 2; i++) {
    s.elcr[i] = 0;
    s.ltim[i] = 0;
    init_reset(i);
  }
}

/**
 * Reset the PIC to its post-ICW1 state.  ELCR/LTIM are intentionally not reset
 * Caller must hold the lock.
 **/
void CI8259::init_reset(int index) {
  s.last_irr[index] = 0;
  s.irr[index] &= s.elcr[index];
  s.imr[index] = 0;
  s.isr[index] = 0;
  s.priority_add[index] = 0;
  s.irq_base[index] = 0;
  s.read_reg_select[index] = 0;
  s.poll[index] = 0;
  s.special_mask[index] = 0;
  s.init_state[index] = 0;
  s.auto_eoi[index] = 0;
  s.rotate_on_aeoi[index] = 0;
  s.special_fnm[index] = 0;
  s.init4[index] = 0;
  s.single_mode[index] = 0;
  update_output(index);
}

/* Highest-priority bit in mask, considering rotation.  Returns 8 if mask==0. */
static int pic_priority_of(u8 mask, u8 priority_add) {
  if (mask == 0)
    return 8;
  int priority = 0;
  while ((mask & (1 << ((priority + priority_add) & 7))) == 0)
    priority++;
  return priority;
}

/**
 * Determine which IRQ (if any) the controller wants to deliver right now.
 * Returns -1 if nothing pending above the in-service priority threshold.
 * Caller must hold the lock.
 **/
int CI8259::get_irq(int index) {
  int mask = s.irr[index] & ~s.imr[index];
  int priority = pic_priority_of((u8)mask, s.priority_add[index]);
  if (priority == 8)
    return -1;

  // Compute current in-service priority.  In SFNM on the master, slave-
  // originated IRQs (cascade IRQ2) do not block other slave IRQs.
  int isr_mask = s.isr[index];
  if (s.special_mask[index])
    isr_mask &= ~s.imr[index];
  if (s.special_fnm[index] && index == 0)
    isr_mask &= ~(1 << 2);

  int cur_priority = pic_priority_of((u8)isr_mask, s.priority_add[index]);
  if (priority < cur_priority)
    return (priority + s.priority_add[index]) & 7;
  return -1;
}

/**
 * Recompute and drive the controller's output line.  For the master, this is
 * the wired-OR of all 8259 outputs that goes to Tsunami DRIR55 and from there
 * to the CPU's external IRQ1.  For the slave, this propagates into the
 * master's IRQ2 (cascade) and recurses to the master.  Caller must hold
 * the lock.
 **/
void CI8259::update_output(int index) {
  bool line_high = (get_irq(index) >= 0);

  if (index == 1) {
    // Slave output drives master IRQ2.  Cascade is conventionally edge-
    // triggered; honor ELCR/LTIM in case the OS programs it as level.
    const u8 cascade_mask = (1 << 2);
    const bool is_level = (s.elcr[0] & cascade_mask) || s.ltim[0];

    if (line_high) {
      if (is_level) {
        s.irr[0] |= cascade_mask;
      } else {
        if ((s.last_irr[0] & cascade_mask) == 0)
          s.irr[0] |= cascade_mask;
      }
      s.last_irr[0] |= cascade_mask;
    } else {
      // The cascade request is derived from the slave output, so it must
      // disappear with that output in either trigger mode. Keeping master
      // IRQ2 after the slave request was cancelled gives a phantom IRQ15 and
      // can leave master ISR2 in service for good.
      s.irr[0] &= ~cascade_mask;
      s.last_irr[0] &= ~cascade_mask;
    }
    update_output(0);
  } else {
    int_line(line_high);
  }
}

/**
 * Acknowledge an IRQ during the INTA cycle.  Sets ISR (unless AEOI), clears
 * IRR for edge-triggered lines, and re-evaluates the output.
 * Caller must hold the lock.
 **/
void CI8259::intack(int index, int irq) {
  g_irqstats.isa_ack[index * 8 + irq].fetch_add(1, std::memory_order_relaxed);
  if (s.auto_eoi[index]) {
    if (s.rotate_on_aeoi[index])
      s.priority_add[index] = (irq + 1) & 7;
  } else {
    s.isr[index] |= (1 << irq);
  }

  // Level-sensitive lines keep their IRR bit set as long as the source
  // holds the line high; only edge-sensitive bits clear at INTA.
  const bool is_level = (s.elcr[index] & (1 << irq)) || s.ltim[index];
  if (!is_level)
    s.irr[index] &= ~(1 << irq);

  update_output(index);
}

/**
 * Read a byte from one of the programmable interrupt controller's registers.
 **/
u8 CI8259::read(int index, u32 address) {
  std::lock_guard<std::mutex> guard(lock);
  u8 data;

  if (s.poll[index]) {
    int irq = get_irq(index);
    if (irq >= 0) {
      intack(index, irq);
      data = (u8)(irq | 0x80);
    } else {
      data = 0;
    }
    s.poll[index] = 0;
  } else {
    if (address == 0) {
      data = s.read_reg_select[index] ? s.isr[index] : s.irr[index];
    } else {
      data = s.imr[index];
    }
  }

#ifdef DEBUG_PIC
  if (pic_messages)
    printf("%%PIC-I-READ: read %02x from port %" PRId64 " on PIC %d\n", data,
           address, index);
#endif
  return data;
}

/**
 * Read a byte from the edge/level register of one of the programmable interrupt
 *controllers.
 **/
u8 CI8259::read_elcr(int index) {
  std::lock_guard<std::mutex> guard(lock);
  return s.elcr[index];
}

/**
 * Read the interrupt vector during a PCI IACK cycle.  This is the hook the
 * Tsunami chipset uses on INTACK; it must mirror what the master 8259 would
 * place on the data bus, including the cascade hand-off to the slave for
 * IRQs 8-15.
 **/
u8 CI8259::read_vector() {
  std::lock_guard<std::mutex> guard(lock);

  int irq = get_irq(0);
  int intno;

  if (irq >= 0) {
    if (irq == 2) {
      // Cascade: fetch vector from slave.
      int irq2 = get_irq(1);
      if (irq2 >= 0) {
        intack(1, irq2);
      } else {
        irq2 = 7; // spurious slave IRQ
      }
      intno = s.irq_base[1] + irq2;
      intack(0, irq);
    } else {
      intno = s.irq_base[0] + irq;
      intack(0, irq);
    }
  } else {
    // Spurious master IRQ.
    intno = s.irq_base[0] + 7;
  }

  return (u8)intno;
}

/**
 * Write a byte to one of the programmable interrupt controller's registers.
 * Implements the ICW1/ICW2/ICW3/ICW4 init sequence and the OCW1/OCW2/OCW3
 * operational commands.
 **/
void CI8259::write(int index, u32 address, u8 data) {
  std::lock_guard<std::mutex> guard(lock);

#ifdef DEBUG_PIC
  if (pic_messages)
    printf("%%PIC-I-WRITE: write %02x to port %" PRId64 " on PIC %d\n", data,
           address, index);
#endif

  if (address == 0) {
    if (data & 0x10) {
      // ICW1: start init sequence.
      init_reset(index);
      s.init_state[index] = 1;
      s.init4[index] = (data & 0x01) ? 1 : 0;
      s.single_mode[index] = (data & 0x02) ? 1 : 0;
      s.ltim[index] = (data & 0x08) ? 1 : 0;
    } else if (data & 0x08) {
      // OCW3
      if (data & 0x04)
        s.poll[index] = 1;
      if (data & 0x02)
        s.read_reg_select[index] = data & 0x01;
      if (data & 0x40)
        s.special_mask[index] = (data >> 5) & 0x01;
    } else {
      // OCW2
      const int cmd = (data >> 5) & 7;
      const int level = data & 7;
      int eoi_irq;
      int priority;

      switch (cmd) {
      case 0: // rotate in auto-EOI mode (clear)
      case 4: // rotate in auto-EOI mode (set)
        s.rotate_on_aeoi[index] = (u8)(cmd >> 2);
        break;

      case 1: // non-specific EOI
      case 5: // rotate on non-specific EOI
        priority = pic_priority_of(s.isr[index], s.priority_add[index]);
        if (priority != 8) {
          eoi_irq = (priority + s.priority_add[index]) & 7;
          s.isr[index] &= ~(1 << eoi_irq);
          if (cmd == 5)
            s.priority_add[index] = (u8)((eoi_irq + 1) & 7);
          update_output(index);
        }
#ifdef DEBUG_PIC
        pic_messages = false;
#endif
        break;

      case 3: // specific EOI
        s.isr[index] &= ~(1 << level);
        update_output(index);
#ifdef DEBUG_PIC
        pic_messages = false;
#endif
        break;

      case 6: // set priority command
        s.priority_add[index] = (u8)((level + 1) & 7);
        update_output(index);
        break;

      case 7: // rotate on specific EOI
        s.isr[index] &= ~(1 << level);
        s.priority_add[index] = (u8)((level + 1) & 7);
        update_output(index);
#ifdef DEBUG_PIC
        pic_messages = false;
#endif
        break;

      default:
        // no operation
        break;
      }
    }
    return;
  }

  // address == 1: ICW2/3/4 during init, IMR (OCW1) otherwise.
  switch (s.init_state[index]) {
  case 0:
    // OCW1: write IMR.  Masking only gates delivery; IRR and ISR are
    // unaffected — re-evaluating the output picks up any newly-unmasked
    // pending IRR bit.
    s.imr[index] = data;
    update_output(index);
    break;

  case 1:
    // ICW2: vector base.
    s.irq_base[index] = data & 0xf8;
    s.init_state[index] = s.single_mode[index] ? (s.init4[index] ? 3 : 0) : 2;
    break;

  case 2:
    // ICW3: cascade configuration (we hard-wire master IRQ2 ↔ slave).
    s.init_state[index] = s.init4[index] ? 3 : 0;
    break;

  case 3:
    // ICW4
    s.special_fnm[index] = (data >> 4) & 0x01;
    s.auto_eoi[index] = (data >> 1) & 0x01;
    s.init_state[index] = 0;
    break;
  }
}

/**
 * Write a byte to the edge/level register of one of the programmable interrupt
 *controllers.
 **/
void CI8259::write_elcr(int index, u8 data) {
  std::lock_guard<std::mutex> guard(lock);
  s.elcr[index] = data;
  // Changing trigger mode can affect the wanted-IRQ calculation if any
  // previously-edge-latched bit is now considered level.
  update_output(index);
}

#define DEBUG_EXPR (index != 0 || (intno != 0 && intno > 4))

/**
 * Assert an interrupt — inner implementation, no lock.
 * Caller MUST hold the lock.
 *
 * Each call latches a fresh edge in IRR regardless of prior line state.  This
 * preserves the historical es40 API where pic_interrupt() is treated as a
 * single-shot "this device fired" event by KBD/PIT/serial/IDE.  Level-mode
 * pins (ELCR or LTIM) additionally have IRR follow the line until pic_deassert.
 **/
void CI8259::interrupt_locked(int index, int intno) {
#ifdef DEBUG_PIC
  if (DEBUG_EXPR) {
    printf("%%PIC-I-INCOMING: Interrupt %d incomming on PIC %d", intno, index);
    pic_messages = true;
  }
#endif

  const u8 mask = (u8)(1 << intno);
  g_irqstats.isa_edge[index * 8 + intno].fetch_add(1,
                                                   std::memory_order_relaxed);

  // Latch a fresh edge: drop last_irr first, then raise.
  s.last_irr[index] &= ~mask;
  s.irr[index] |= mask;
  s.last_irr[index] |= mask;

#ifdef DEBUG_PIC
  if (DEBUG_EXPR) {
    if (s.imr[index] & mask)
      printf(" (masked, latched)\n");
    else
      printf("\n");
  }
#endif

  update_output(index);
}

/**
 * De-assert an interrupt — inner implementation, no lock.
 * Caller MUST hold the lock.
 *
 * For level-mode pins, this clears IRR (the line dropped low).  For edge-
 * mode pins, only last_irr drops; any IRR bit already latched stays pending
 * until INTA, which matches real-hardware edge semantics.
 **/
void CI8259::deassert_locked(int index, int intno) {
  const u8 mask = (u8)(1 << intno);
  const bool is_level = (s.elcr[index] & mask) || s.ltim[index];

  if (is_level)
    s.irr[index] &= ~mask;
  s.last_irr[index] &= ~mask;

  update_output(index);
}

/**
 * Assert an interrupt on one of the programmable interrupt controllers.
 **/
void CI8259::interrupt(int index, int intno) {
  std::lock_guard<std::mutex> guard(lock);
  interrupt_locked(index, intno);
}

/**
 * De-assert an interrupt on one of the programmable interrupt controllers.
 **/
void CI8259::deassert(int index, int intno) {
  std::lock_guard<std::mutex> guard(lock);
  deassert_locked(index, intno);
}

/**
 * Drive a device IRQ input line to a level (active/inactive) — the model for
 * sources whose "interrupt" is a level the device holds until the guest clears
 * the cause.
 *
 * Because the source is a level, IRR FOLLOWS the line: a rising edge latches
 * IRR (debounced via last_irr so a held line yields one interrupt), and a
 * falling line RETRACTS IRR.  This intentionally differs from a stock 8259
 * which keeps an edge latched until INTA: a real 8259 has a wire whose source
 * holds it, but here the "wire" IS the emulated device cause, and once that
 * cause disappears there is nothing left to deliver.
 *
 * caller must hold the lock
 **/
void CI8259::set_line_locked(int index, int intno, bool active) {
  const u8 mask = (u8)(1 << intno);
  if (active && !(s.last_irr[index] & mask))
    g_irqstats.isa_edge[index * 8 + intno].fetch_add(1,
                                                     std::memory_order_relaxed);

  if ((s.elcr[index] & mask) || s.ltim[index]) {
    // Level triggered: IRR follows the input line.
    if (active) {
      s.irr[index] |= mask;
      s.last_irr[index] |= mask;
    } else {
      s.irr[index] &= ~mask;
      s.last_irr[index] &= ~mask;
    }
  } else {
    // Edge triggered: latch IRR only on the rising edge. A held line produces
    // no duplicate edge; INTA consumes it unless the emulated cause falls
    // first.
    if (active) {
      if ((s.last_irr[index] & mask) == 0)
        s.irr[index] |= mask;
      s.last_irr[index] |= mask;
    } else {
      // The emulated cause disappeared before INTA: retract the request and
      // rearm edge detection.
      s.irr[index] &= ~mask;
      s.last_irr[index] &= ~mask;
    }
  }

  update_output(index);
}

/**
 * Drive a device IRQ input line — locked wrapper, see pic_set_line_inner.
 **/
void CI8259::set_line(int index, int intno, bool active) {
  std::lock_guard<std::mutex> guard(lock);
  set_line_locked(index, intno, active);
}
