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

/**
 * \file
 * Contains the code for the emulated Keyboard and mouse devices and controller.
 **/
#include <chrono>
#include "Keyboard.hpp"
#include "AliM1543C.hpp"
#include "StdAfx.hpp"
#include "System.hpp"
#include <math.h>

#include "gui/keymap.hpp"
#include "gui/scancodes.hpp"

/// ALPHABOX_MOUSE_DEBUG=1 traces guest aux commands and discarded mouse data.
static bool mouse_debug() {
  static const bool on = getenv("ALPHABOX_MOUSE_DEBUG") != nullptr;
  return on;
}

/// Report a byte the controller had to discard. Rate-limited: a guest that
/// stops reading must not flood the console.
static void kbc_drop_note(const char *what, unsigned data) {
  static unsigned drops = 0; // all callers hold kbdLock
  ++drops;
  if (drops <= 10 || (drops % 1000) == 0)
    printf("%%KBC-W-DROP: %s, dropping %02x (%u dropped so far)\n", what,
           data & 0xff, drops);
}

/**
 * Constructor.
 **/
CKeyboard::CKeyboard(CConfigurator *cfg, CSystem *c)
    : CSystemComponent(cfg, c) {
  if (theKeyboard != 0)
    FAILURE(Configuration, "More than one Keyboard controller");
  theKeyboard = this;
}

/**
 * Initialize the Keyboard device.
 **/
void CKeyboard::init() {
  cSystem->RegisterMemory(this, 0, U64(0x00000801fc000060), 1);
  cSystem->RegisterMemory(this, 1, U64(0x00000801fc000064), 1);

  // Everything not set explicitly below (buffers, queue, mouse button and
  // pending-motion state, ...) starts out zero.
  memset(&state, 0, sizeof(state));

  resetinternals(1);

  state.kbd_internal_buffer.led_status = 0;
  state.kbd_internal_buffer.scanning_enabled = 1;

  state.status.pare = 0;
  state.status.tim = 0;
  state.status.auxb = 0;
  state.status.keyl = 1;
  state.status.c_d = 1;
  state.status.sysf = 0;
  state.status.inpb = 0;
  state.status.outb = 0;

  state.kbd_clock_enabled = 1;
  state.aux_clock_enabled = 0;
  state.allow_irq1 = 1;
  state.allow_irq12 = 1;
  state.kbd_output_buffer = 0;
  state.aux_output_buffer = 0;
  state.last_comm = 0;
  state.expecting_port60h = 0;
  state.expecting_mouse_parameter = 0;
  state.scancodes_translate = 1;

  // Mouse initialization stuff
  state.mouse.captured = true;
  state.mouse.sample_rate = 100;   // reports per second
  state.mouse.resolution_cpmm = 4; // 4 counts per millimeter
  state.mouse.scaling = 1;         /* 1:1 (default) */
  state.mouse.mode = MOUSE_MODE_RESET;
  state.mouse.enable = 0;
  state.mouse.delayed_dx = 0;
  state.mouse.delayed_dy = 0;
  state.mouse.delayed_dz = 0;
  state.mouse.im_request = 0; // wheel mouse mode request
  state.mouse.im_mode = 0;    // wheel mouse mode

  myThread = nullptr;

  printf("kbc: $Id$\n");
}

void CKeyboard::start_threads() {
  if (!myThread) {
    printf(" kbd");
    StopThread = false;
    myThread = std::make_unique<std::thread>([this]() { this->run(); });
  }
}

void CKeyboard::stop_threads() {
  StopThread = true;
  if (myThread) {
    printf(" kbd");
    myThread->join();
    myThread = nullptr;
  }
}

/**
 * Destructor.
 **/
CKeyboard::~CKeyboard() { stop_threads(); }

// ALPHABOX_TRACE_KBC=1: every command and data byte the guest writes, with
// how many times it read the data port since the previous write and what
// the first and last of those reads returned -- a driver that spins on
// port 0x60 waiting for a byte shows up as one write followed by millions
// of reads of the same value.
static const bool s_trace_kbc = getenv("ALPHABOX_TRACE_KBC") != nullptr;
static u64 s_kbc_reads = 0, s_kbc_status_reads = 0;
static u8 s_kbc_first = 0, s_kbc_last = 0;
static double kbc_ms() {
  static const auto t0 = std::chrono::steady_clock::now();
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now() - t0)
      .count();
}

u64 CKeyboard::ReadMem(int index, u64 address, int dsize) {
  std::lock_guard<std::mutex> guard(kbdLock);
  switch (index) {
  case 0: {
    const u8 v = read_60();
    if (s_trace_kbc) {
      if (s_kbc_reads++ == 0)
        s_kbc_first = v;
      s_kbc_last = v;
    }
    return v;
  }
  case 1:
    if (s_trace_kbc)
      s_kbc_status_reads++;
    return read_64();
  default:
    FAILURE(InvalidArgument, "kbc: ReadMem index out of range");
  }
}

void CKeyboard::WriteMem(int index, u64 address, int dsize, u64 data) {
  std::lock_guard<std::mutex> guard(kbdLock);
  if (s_trace_kbc) {
    printf("KBCT %10.1f: since last write %llu data reads (first %02x last "
           "%02x), %llu status reads; now write port %02x = %02llx\n",
           kbc_ms(), (unsigned long long)s_kbc_reads, s_kbc_first, s_kbc_last,
           (unsigned long long)s_kbc_status_reads, index ? 0x64 : 0x60,
           (unsigned long long)(data & 0xff));
    s_kbc_reads = s_kbc_status_reads = 0;
  }
  switch (index) {
  case 0:
    write_60((u8)data);
    break;
  case 1:
    write_64((u8)data);
    break;
  default:
    FAILURE(InvalidArgument, "kbc: ReadMem index out of range");
  }
}

/**
 * Enqueue scancode for a keypress or key-release. Used by the GUI
 *implementation to send keypresses to the keyboard controller.
 **/
void CKeyboard::gen_scancode(u32 key) {
  std::lock_guard<std::mutex> guard(kbdLock);

  unsigned char *scancode;
  size_t i;

#ifdef DEBUG_KBD
  printf("gen_scancode(): %s %s  \n", bx_keymap->getBXKeyName(key),
         (key >> 31) ? "released" : "pressed");
  if (!state.scancodes_translate)
    BX_DEBUG(("keyboard: gen_scancode with scancode_translate cleared"));
#endif

  // Ignore scancode if keyboard clock is driven low
  if (state.kbd_clock_enabled == 0)
    return;

  // Ignore scancode if scanning is disabled
  if (state.kbd_internal_buffer.scanning_enabled == 0)
    return;

  // Source: http://www.win.tue.nl/~aeb/linux/kbd/scancodes-10.html
  //
  // Three scancode sets
  //
  // The usual PC keyboards are capable of producing three sets
  // of scancodes. Writing 0xf0 followed by 1, 2 or 3 to port
  // 0x60 will put the keyboard in scancode mode 1, 2 or 3.
  // Writing 0xf0 followed by 0 queries the mode, resulting in
  // a scancode byte 43, 41 or 3f from the keyboard.
  //
  // Set 1 contains the values that the XT keyboard (with only
  // one set of scancodes) produced, with extensions for new
  // keys. Someone decided that another numbering was more
  // logical and invented scancode Set 2. However, it was
  // realized that new scancodes would break old programs, so
  // the keyboard output was fed to a 8042 microprocessor on
  // the motherboard that could translate Set 2 back into Set
  // 1. Indeed a smart construction. This is the default today.
  // Finally there is the PS/2 version, Set 3, more regular,
  // but used by almost nobody.
  //
  // Sets 2 and 3 are designed to be translated by the 8042.
  // Set 1 should not be translated.
  //
  // Make and Break Codes
  //
  // The key press / key release is coded as follows:
  //
  // For Set 1, if the make code of a key is c, the break
  // code will be c+0x80. If the make code is e0 c, the
  // break code will be e0 c+0x80. The Pause key has make
  // code e1 1d 45 e1 9d c5 and does not generate a break code.
  //
  // For Set 2, if the make code of a key is c, the break code
  // will be f0 c. If the make code is e0 c, the break code
  // will be e0 f0 c. The Pause key has the 8-byte make code
  // e1 14 77 e1 f0 14 f0 77.
  //
  // For Set 3, by default most keys do not generate a break
  // code - only CapsLock, LShift, RShift, LCtrl and LAlt do.
  // However, by default all non-traditional keys do generate
  // a break code - thus, LWin, RWin, Menu do, and for example
  // on the Microsoft Internet keyboard, so do Back, Forward,
  // Stop, Mail, Search, Favorites, Web/Home, MyComputer,
  // Calculator, Sleep. On my BTC keyboard, also the Macro key
  // does.
  //
  // In Scancode Mode 3 it is possible to enable or disable
  // key repeat and the production of break codes either on a
  // key-by-key basis or for all keys at once. And just like
  // for Set 2, key release is indicated by a f0 prefix in
  // those cases where it is indicated. There is nothing
  // special with the Pause key in scancode mode 3.
  if (key & BX_KEY_RELEASED)
    scancode =
        (unsigned char *)scancodes[(key & 0xFF)][state.current_scancodes_set]
            .brek;
  else
    scancode =
        (unsigned char *)scancodes[(key & 0xFF)][state.current_scancodes_set]
            .make;

  // Translation
  //
  // The 8042 microprocessor translates the incoming byte stream
  // produced by the keyboard, and turns an f0 prefix into an OR
  // with 80 for the next byte.
  //
  // Unless told not to translate, the keyboard controller translates
  // keyboard scancodes into the scancodes it returns to the CPU using
  // the following table (in hex):
  //
  // +----+-------------------------------------------------+
  // |    | 00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f |
  // +----+-------------------------------------------------+
  // | 00 | ff 43 41 3f 3d 3b 3c 58 64 44 42 40 3e 0f 29 59 |
  // | 10 |     65 38 2a 70 1d 10 02 5a 66 71 2c 1f 1e 11 03 5b |
  // | 20 |     67 2e 2d 20 12 05 04 5c 68 39 2f 21 14 13 06 5d |
  // | 30 |     69 31 30 23 22 15 07 5e 6a 72 32 24 16 08 09 5f |
  // | 40 |     6b 33 25 17 18 0b 0a 60 6c 34 35 26 27 19 0c 61 |
  // | 50 |     6d 73 28 74 1a 0d 62 6e 3a 36 1c 1b 75 2b 63 76 |
  // | 60 |     55 56 77 78 79 7a 0e 7b 7c 4f 7d 4b 47 7e 7f 6f |
  // | 70 |     52 53 50 4c 4d 48 01 45 57 4e 51 4a 37 49 46 54 |
  // | 80 |     80 81 82 41 54 85 86 87 88 89 8a 8b 8c 8d 8e 8f |
  // | 90 |     90 91 92 93 94 95 96 97 98 99 9a 9b 9c 9d 9e 9f |
  // | a0 |     a0 a1 a2 a3 a4 a5 a6 a7 a8 a9 aa ab ac ad ae af |
  // | b0 |     b0 b1 b2 b3 b4 b5 b6 b7 b8 b9 ba bb bc bd be bf |
  // | c0 |     c0 c1 c2 c3 c4 c5 c6 c7 c8 c9 ca cb cc cd ce cf |
  // | d0 |     d0 d1 d2 d3 d4 d5 d6 d7 d8 d9 da db dc dd de df |
  // | e0 |     e0 e1 e2 e3 e4 e5 e6 e7 e8 e9 ea eb ec ed ee ef |
  // | f0 |     -  f1 f2 f3     f4 f5 f6 f7 f8 f9 fa fb fc fd fe ff |
  // +----+-------------------------------------------------+
  u8 bytes[16];
  int count = 0;
  size_t len = strlen((const char *)scancode);
  if (len > sizeof(bytes))
    len = sizeof(bytes);

  if (state.scancodes_translate) {

    // Translate before send
    u8 escaped = 0x00;

    for (i = 0; i < len; i++) {
      if (scancode[i] == 0xF0) {
        escaped = 0x80;
      } else {
#ifdef DEBUG_KBD
        printf("gen_scancode(): writing translated %02x   \n",
               translation8042[scancode[i]] | escaped);
#endif
        bytes[count++] = translation8042[scancode[i]] | escaped;
        escaped = 0x00;
      }
    }
  } else {

    // Send raw data
    for (i = 0; i < len; i++) {
#ifdef DEBUG_KBD
      printf("gen_scancode(): writing raw %02x   \n", scancode[i]);
#endif
      bytes[count++] = scancode[i];
    }
  }

  kbd_enQ_sequence(bytes, count);
}

/**
 * Reset keyboard internals.
 **/
void CKeyboard::resetinternals(bool powerup) {
  // The keyboard discards the keystrokes it has not sent yet, including the
  // rest of a key event whose first byte already went out.
  state.kbd_internal_buffer.num_elements = 0;
  for (int i = 0; i < BX_KBD_ELEMENTS; i++) {
    state.kbd_internal_buffer.buffer[i] = 0;
    state.kbd_internal_buffer.seq_start[i] = false;
  }
  state.kbd_internal_buffer.head = 0;

  state.kbd_internal_buffer.expecting_typematic = 0;
  state.kbd_internal_buffer.expecting_make_break = 0;

  // Default scancode set is mf2 (translation is controlled by the 8042)
  state.expecting_scancodes_set = 0;

  state.current_scancodes_set = 2; // startup in set 2

  if (powerup) {
    state.kbd_internal_buffer.expecting_led_write = 0;
    state.kbd_internal_buffer.delay = 1;          // 500 mS
    state.kbd_internal_buffer.repeat_rate = 0x0b; // 10.9 chars/sec
  }
}

/**
 * Enqueue the bytes of one key event (make or break sequence) into the
 * keyboard's internal buffer. The event is stored whole or not at all, so a
 * full buffer never leaves half a multi-byte sequence behind.
 **/
void CKeyboard::kbd_enQ_sequence(const u8 *bytes, int count) {
  SKb_state::SAli_kbdib &kb = state.kbd_internal_buffer;

  if (count <= 0)
    return;
  if (kb.num_elements + count > BX_KBD_ELEMENTS) {
    kbc_drop_note("keyboard buffer full, key event lost", bytes[0]);
    return;
  }

  for (int i = 0; i < count; i++) {
#ifdef DEBUG_KBD
    BX_DEBUG(("enQ: putting scancode 0x%02x in internal buffer",
              (unsigned)bytes[i]));
#endif
    int tail = (kb.head + kb.num_elements) % BX_KBD_ELEMENTS;
    kb.buffer[tail] = bytes[i];
    kb.seq_start[tail] = (i == 0);
    kb.num_elements++;
  }

  // Event-driven: deliver to the output buffer now (if free) and drive the
  // IRQ, instead of waiting for the 20ms poll.
  kbd_service();
}

/**
 * Read a byte from keyboard port 60.
 **/
u8 CKeyboard::read_60() {
  if (!state.status.outb) {
#ifdef DEBUG_KBD
    BX_DEBUG(("read from port 60h with outb empty"));
    BX_DEBUG(("READ(60) = %02x", state.kbd_output_buffer));
#endif
    return state.kbd_output_buffer;
  }

  u8 val =
      state.status.auxb ? state.aux_output_buffer : state.kbd_output_buffer;
  state.status.outb = 0;
  state.status.auxb = 0;

  // Refill the output buffer from the queues and re-evaluate the IRQ lines.
  // kbd_service first drops the line for the byte just read (clearing the
  // 8259 edge latch), then raises a fresh edge if another byte is pending.
  kbd_service();
#ifdef DEBUG_KBD
  BX_DEBUG(("READ(60) = %02x", (unsigned)val));
#endif
  return val;
}

/**
 * Read a byte from keyboard port 64
 *
 * The keyboard controller status register
 *
 * The keyboard controller has an 8-bit status register. It can be inspected by
 *the CPU by reading port 0x64. (Typically, it has the value 0x14: keyboard not
 *locked, self-test completed.)
 *
 * \code
 * +------+-----+------+------+-----+------+------+------+
 * | PARE |	TIM | AUXB | KEYL | C/D | SYSF | INPB | OUTB |
 * +------+-----+------+------+-----+------+------+------+
 * \endcode
 *
 * Bit 7: Parity error
 *    0: OK.
 *    1: Parity error with last byte.
 *
 * Bit 6: Timeout
 *    0: OK.
 *    1: General timeout.
 *
 * Bit 5: Auxiliary output buffer full
 *    Bit 0 tells whether a read from port 0x60 will be valid. If it is valid,
 *this bit 5 tells what data will be read from port 0x60. 0: Keyboard data. 1:
 *Mouse data.
 *
 * Bit 4: Keyboard lock
 *    0: Locked.
 *    1: Not locked.
 *
 * Bit 3: Command/Data
 *    0: Last write to input buffer was data (written via port 0x60).
 *    1: Last write to input buffer was a command (written via port 0x64). (This
 *bit is also referred to as Address Line A2.)
 *
 * Bit 2: System flag
 *    Set to 0 after power on reset. Set to 1 after successful completion of the
 *keyboard controller self-test (Basic Assurance Test, BAT). Can also be set by
 *command (see below).
 *
 * Bit 1: Input buffer status
 *    0: Input buffer empty, can be written.
 *    1: Input buffer full, don't write yet.
 *
 * Bit 0: Output buffer status
 *    0: Output buffer empty, don't read yet.
 *    1: Output buffer full, can be read. (Bit 5 tells whether the available
 *data is from keyboard or mouse.) This bit is cleared when port 0x60 is read.
 **/
u8 CKeyboard::read_64() {
  u8 val;

  /* status register */
  val = (state.status.pare << 7) | (state.status.tim << 6) |
        (state.status.auxb << 5) | (state.status.keyl << 4) |
        (state.status.c_d << 3) | (state.status.sysf << 2) |
        (state.status.inpb << 1) | (state.status.outb << 0);
  state.status.tim = 0;
#ifdef DEBUG_KBD_NOISY
  BX_DEBUG(("read from 0x64 returns 0x%02x", val));
#endif
  return val;
}

/**
 * Write a byte to keyboard port 60.
 **/
void CKeyboard::write_60(u8 value) {
#ifdef DEBUG_KBD
  printf("kbd: port 60 write: %02x.   \n", value);
#endif

  // data byte written last to 0x60
  state.status.c_d = 0;

  // if expecting data byte from command last sent to port 64h
  if (state.expecting_port60h) {
    state.expecting_port60h = 0;
#ifdef DEBUG_KBD
    if (state.status.inpb)
      printf("write to port 60h, not ready for write   \n");
#endif
    switch (state.last_comm) {
    case 0x60: // write command byte
    {

      // The keyboard controller is provided with some RAM, for example
      //  32 bytes, that can be accessed by the CPU. The most important
      //  part of this RAM is byte 0, the Controller Command Byte (CCB).
      //  It can be read/written by writing 0x20/0x60 to port 0x64 and
      //  then reading/writing a data byte from/to port 0x60.
      //
      //  This byte has the following layout.
      //
      //  +---+-------+----+----+---+------+-----+-----+
      //  | 0 | XLATE | ME | KE | 0 | SYSF | MIE | KIE |
      //  +---+-------+----+----+---+------+-----+-----+
      //
      //  Bit 6: Translate
      //     0: No translation.
      //     1: Translate keyboard scancodes, using the translation table
      //        given above. MCA type 2 controllers cannot set this bit
      //        to 1. In this case scan code conversion is set using
      //        keyboard command 0xf0 to port 0x60.
      //
      //  Bit 5: Mouse enable
      //     0: Enable mouse.
      //     1: Disable mouse by driving the clock line low.
      //
      //  Bit 4: Keyboard enable
      //     0: Enable keyboard.
      //     1: Disable keyboard by driving the clock line low.
      //
      //  Bit 2: System flag
      //     This bit is shown in bit 2 of the status register. A
      //     "cold reboot" is one with this bit set to zero. A
      //     "warm reboot" is one with this bit set to one (BAT
      //     already completed). This will influence the tests and
      //     initializations done by the POST.
      //
      //  Bit 1: Mouse interrupt enable
      //     0: Do not use mouse interrupts.
      //     1: Send interrupt request IRQ12 when the mouse output
      //        buffer is full.
      //
      //  Bit 0: Keyboard interrupt enable
      //     0: Do not use keyboard interrupts.
      //     1: Send interrupt request IRQ1 when the keyboard output
      //        buffer is full.
      //
      //     When no interrupts are used, the CPU has to poll bits 0
      //     (and 5) of the status register.
      bool scan_convert;

      // The keyboard controller is provided with some RAM, for example
      bool disable_keyboard;

      // The keyboard controller is provided with some RAM, for example
      bool disable_aux;

      scan_convert = (value >> 6) & 0x01;
      disable_aux = (value >> 5) & 0x01;
      disable_keyboard = (value >> 4) & 0x01;
      state.status.sysf = (value >> 2) & 0x01;
      state.allow_irq1 = (value >> 0) & 0x01;
      state.allow_irq12 = (value >> 1) & 0x01;
      set_kbd_clock_enable(!disable_keyboard);
      set_aux_clock_enable(!disable_aux);
      // The IRQ lines follow the new allow_irq bits in kbd_service() below.

#ifdef DEBUG_KBD
      BX_DEBUG((" allow_irq12 set to %u", (unsigned)state.allow_irq12));
      if (!scan_convert)
        BX_INFO(("keyboard: scan convert turned off"));
#endif

      // (mch) NT needs this
      state.scancodes_translate = scan_convert;
    } break;

    case 0xd1: // write output port
#ifdef DEBUG_KBD
      BX_DEBUG(("write output port with value %02xh", (unsigned)value));
#endif
      break;

    case 0xd4: // Write to mouse
      // I don't think this enables the AUX clock
      // set_aux_clock_enable(1); // enable aux clock line
      ctrl_to_mouse(value);

      // ??? should I reset to previous value of aux enable?
      break;

    case 0xd3: // write mouse output buffer
      // Queue in mouse output buffer
      controller_enQ(value, KBC_SRC_CTRL_AUX);
      break;

    case 0xd2:

      // Queue in keyboard output buffer
      controller_enQ(value, KBC_SRC_CTRL);
      break;

    default:
      printf("=== unsupported write to port 60h(lastcomm=%02x): %02x   \n",
             (unsigned)state.last_comm, (unsigned)value);
    }
  } else {

    /* pass byte to keyboard */
    if (state.kbd_clock_enabled == 0)
      set_kbd_clock_enable(1);
    ctrl_to_kbd(value);
  }

  // Guest I/O only delivers bytes the devices have already produced. Host
  // mouse motion is sampled by the worker thread (execute()), so a command
  // write can never manufacture a stream packet ahead of its own reply.
  kbd_service();
}

/**
 * Write a byte to keyboard port 64.
 **/
void CKeyboard::write_64(u8 value) {
#ifdef DEBUG_KBD
  printf("kbd: port 64 write: %02x.   \n", value);
#endif

  u8 command_byte;

  // command byte written last to 0x64
  state.status.c_d = 1;
  state.last_comm = value;

  // most commands NOT expecting port60 write next
  state.expecting_port60h = 0;

  // Controller replies below are queued: when the output buffer is still
  // occupied (e.g. by a keystroke that arrived first) the reply follows that
  // byte instead of being lost.
  switch (value) {
  case 0x20: // get keyboard command byte
#ifdef DEBUG_KBD
    BX_DEBUG(("get keyboard command byte"));
#endif
    command_byte = (state.scancodes_translate << 6) |
                   ((!state.aux_clock_enabled) << 5) |
                   ((!state.kbd_clock_enabled) << 4) | (0 << 3) |
                   (state.status.sysf << 2) | (state.allow_irq12 << 1) |
                   (state.allow_irq1 << 0);
    controller_enQ(command_byte, KBC_SRC_CTRL);
    break;

  case 0x60: // write command byte
#ifdef DEBUG_KBD
    printf("kbd_ctrl: command 60: write command byte.   \n");
#endif

    // following byte written to port 60h is command byte
    state.expecting_port60h = 1;
    break;

  case 0xa0:
#ifdef DEBUG_KBD
    printf("kbd_ctrl: command a0: BIOS name (not supported).   \n");
#endif
    break;

  case 0xa1:
#ifdef DEBUG_KBD
    printf("kbd_ctrl: command a0: BIOS version (not supported).   \n");
#endif
    break;

  case 0xa7: // disable the aux device
    set_aux_clock_enable(0);
#ifdef DEBUG_KBD
    printf("kbd_ctrl: command a7: aux i/f disable.   \n");
#endif
    break;

  case 0xa8: // enable the aux device
    set_aux_clock_enable(1);
#ifdef DEBUG_KBD
    printf("kbd_ctrl: command a7: aux i/f enable.   \n");
#endif
    break;

  case 0xa9: // Test Mouse Port
#ifdef DEBUG_KBD
    printf("kbd_ctrl: command a9: aux i/f test.   \n");
#endif
    controller_enQ(0x00, KBC_SRC_CTRL); // no errors detected
    break;

  case 0xaa: // motherboard controller self test
#ifdef DEBUG_KBD
    printf("kbd_ctrl: command aa: self test.   \n");
#endif
    // The self-test re-initializes the controller: its output register and
    // reply queue are cleared and everything still pending on the input side
    // is discarded -- keystrokes, mouse stream packets (a packet whose first
    // byte was in the discarded output register cannot be completed anyway)
    // and accumulated motion -- so 0x55 is the next byte the guest reads.
    state.status.outb = 0;
    state.status.auxb = 0;
    state.kbd_controller_Qsize = 0;
    state.kbd_internal_buffer.head = 0;
    state.kbd_internal_buffer.num_elements = 0;
    state.mouse_internal_buffer.head = 0;
    state.mouse_internal_buffer.num_elements = 0;
    discard_mouse_motion();
    // No device transaction survives the reset either: a pending parameter
    // state would otherwise hold input back indefinitely.
    state.expecting_scancodes_set = 0;
    state.kbd_internal_buffer.expecting_typematic = 0;
    state.kbd_internal_buffer.expecting_led_write = 0;
    state.kbd_internal_buffer.expecting_make_break = 0;
    state.expecting_mouse_parameter = 0;
    state.mouse.im_request = 0;

    state.status.sysf = 1;              // self test complete
    controller_enQ(0x55, KBC_SRC_CTRL); // controller OK
    break;

  case 0xab: // Interface Test
#ifdef DEBUG_KBD
    printf("kbd_ctrl: command ab: kbd i/f test.   \n");
#endif
    controller_enQ(0x00, KBC_SRC_CTRL);
    break;

  case 0xad: // disable keyboard
    set_kbd_clock_enable(0);
#ifdef DEBUG_KBD
    printf("kbd_ctrl: command ad: kbd i/f disable.   \n");
#endif
    break;

  case 0xae: // enable keyboard
    set_kbd_clock_enable(1);
#ifdef DEBUG_KBD
    printf("kbd_ctrl: command ae: kbd i/f enable.   \n");
#endif
    break;

  case 0xaf: // get controller version
#ifdef DEBUG_KBD
    printf("kbd_ctrl: command af: controller version (not supported).   \n");
#endif
    break;

  case 0xc0: // read input port
#ifdef DEBUG_KBD
    printf("kbd_ctrl: command c0: read input port.   \n");
#endif

    // keyboard not inhibited
    controller_enQ(0x80, KBC_SRC_CTRL);
    break;

  case 0xd0: // read output port: next byte read from port 60h
#ifdef DEBUG_KBD
    printf("kbd_ctrl: command d0: read output port. (partial)   \n");
#endif
    // bit 5: aux OBF (IRQ12), bit 4: keyboard OBF (IRQ1), bit 0: no reset
    controller_enQ(
        (u8)(((state.status.outb && state.status.auxb && state.allow_irq12)
              << 5) |
             ((state.status.outb && !state.status.auxb && state.allow_irq1)
              << 4) |
             0x01),
        KBC_SRC_CTRL);
    break;

  case 0xd1: // write output port: next byte written to port 60h
#ifdef DEBUG_KBD
    printf("kbd_ctrl: command d1: write output port.   \n");
#endif

    // following byte to port 60h written to output port
    state.expecting_port60h = 1;
    break;

  case 0xd3: // write mouse output buffer
#ifdef DEBUG_KBD
    printf("kbd_ctrl: command d3: write aux output buffer.   \n");
#endif

    // following byte to port 60h written to output port as mouse write.
    state.expecting_port60h = 1;
    break;

  case 0xd4: // write to mouse
#ifdef DEBUG_KBD
    printf("kbd_ctrl: command d4: write to aux.   \n");
#endif

    // following byte written to port 60h
    state.expecting_port60h = 1;
    break;

  case 0xd2: // write keyboard output buffer
#ifdef DEBUG_KBD
    printf("kbd_ctrl: command d2: write kbd output buffer.   \n");
#endif
    state.expecting_port60h = 1;
    break;

  case 0xc1: // Continuous Input Port Poll, Low
  case 0xc2: // Continuous Input Port Poll, High
  case 0xe0: // Read Test Inputs
    BX_PANIC(("io write 0x64: command = %02xh", (unsigned)value));
    break;

  default:
    if (value == 0xff || (value >= 0xf0 && value <= 0xfd)) {

      /* useless pulse output bit commands ??? */
#ifdef DEBUG_KBD
      BX_DEBUG(("io write to port 64h, useless command %02x", (unsigned)value));
#endif
      break;
    }

    BX_ERROR(("unsupported io write to keyboard port 64, value = %x",
              (unsigned)value));
    break;
  }

  // As in write_60(): deliver pending bytes, never sample host motion here.
  kbd_service();
}

/**
 * Queue a reply from the keyboard device (ACK, ID, echo, ...). Replies use the
 * controller queue so they are never confused with, overwrite, or get stuck
 * behind buffered keystrokes.
 **/
void CKeyboard::kbd_response_enQ(u8 data) { controller_enQ(data, KBC_SRC_KBD); }

/**
 * Append a byte to the controller/response queue, tagged with its origin
 * (KBC_SRC_*). A full queue drops the byte. The byte reaches the output
 * buffer through kbd_service(), which every guest-write path runs before
 * returning.
 **/
void CKeyboard::controller_enQ(u8 data, u8 source) {
#ifdef DEBUG_KBD
  BX_DEBUG(
      ("controller_enQ(%02x) source=%u", (unsigned)data, (unsigned)source));
#endif
  if (state.kbd_controller_Qsize >= BX_KBD_CONTROLLER_QSIZE) {
    kbc_drop_note("controller queue full", data);
    return;
  }
  state.kbd_controller_Q[state.kbd_controller_Qsize] = data;
  state.kbd_controller_Qsrc[state.kbd_controller_Qsize] = source;
  state.kbd_controller_Qsize++;
}

/**
 * Put one byte into the controller's single output buffer, as keyboard
 * (IRQ1) or aux (IRQ12) data. The caller runs kbd_update_irq() afterwards.
 **/
void CKeyboard::load_output_buffer(u8 data, bool aux) {
  if (aux)
    state.aux_output_buffer = data;
  else
    state.kbd_output_buffer = data;
  state.status.outb = 1;
  state.status.auxb = aux;
  state.status.inpb = 0;
}

/**
 * Move the head of the controller queue into the output buffer, delivered as
 * the channel its source belongs to.
 **/
bool CKeyboard::controller_deQ() {
  if (state.status.outb || !state.kbd_controller_Qsize)
    return false;

  const u8 data = state.kbd_controller_Q[0];
  const u8 source = state.kbd_controller_Qsrc[0];
  for (unsigned i = 1; i < state.kbd_controller_Qsize; i++) {
    state.kbd_controller_Q[i - 1] = state.kbd_controller_Q[i];
    state.kbd_controller_Qsrc[i - 1] = state.kbd_controller_Qsrc[i];
  }
  state.kbd_controller_Qsize--;

  if (source == KBC_SRC_KBD)
    state.last_kbd_byte = data;
  else if (source == KBC_SRC_AUX)
    state.last_aux_byte = data;
  load_output_buffer(data, source == KBC_SRC_AUX || source == KBC_SRC_CTRL_AUX);
  return true;
}

/**
 * Move the next buffered keystroke byte into the output buffer.
 **/
void CKeyboard::deliver_kbd_byte() {
  SKb_state::SAli_kbdib &kb = state.kbd_internal_buffer;
  state.last_kbd_byte = kb.buffer[kb.head];
  load_output_buffer(kb.buffer[kb.head], false);
  kb.head = (kb.head + 1) % BX_KBD_ELEMENTS;
  kb.num_elements--;
}

/**
 * Move the next buffered mouse stream byte into the output buffer.
 **/
void CKeyboard::deliver_mouse_byte() {
  SKb_state::SAli_mib &mb = state.mouse_internal_buffer;
  state.last_aux_byte = mb.buffer[mb.head];
  load_output_buffer(mb.buffer[mb.head], true);
  mb.head = (mb.head + 1) % BX_MOUSE_BUFF_SIZE;
  mb.num_elements--;
}

/**
 * Whether new keystrokes must be held back: the keyboard is waiting for a
 * command parameter, or the controller for the data byte of a port-64
 * command. Holding keeps the command/ACK/parameter exchange contiguous.
 **/
bool CKeyboard::kbd_input_held() const {
  return state.expecting_port60h || state.expecting_scancodes_set ||
         state.kbd_internal_buffer.expecting_typematic ||
         state.kbd_internal_buffer.expecting_led_write ||
         state.kbd_internal_buffer.expecting_make_break;
}

/**
 * Whether new mouse stream packets must be held back: the mouse is waiting
 * for a command parameter, or the controller for the data byte of a port-64
 * command (e.g. between D4 and the mouse command byte).
 **/
bool CKeyboard::mouse_input_held() const {
  return state.expecting_port60h || state.expecting_mouse_parameter;
}

/**
 * Whether the keyboard buffer starts with the rest of a key event whose
 * first byte has already been moved to the output buffer.
 **/
bool CKeyboard::kbd_tail_pending() const {
  const SKb_state::SAli_kbdib &kb = state.kbd_internal_buffer;
  return kb.num_elements && !kb.seq_start[kb.head];
}

/**
 * Whether the mouse buffer starts with the rest of a packet whose first byte
 * has already been moved to the output buffer.
 **/
bool CKeyboard::mouse_tail_pending() const {
  const SKb_state::SAli_mib &mb = state.mouse_internal_buffer;
  return mb.num_elements && !mb.pkt_start[mb.head];
}

/**
 * Discard the whole mouse stream packets that have not started transmission.
 * The rest of a packet already partly delivered is kept so the guest driver
 * never loses packet framing.
 **/
void CKeyboard::drop_unsent_mouse_packets() {
  SKb_state::SAli_mib &mb = state.mouse_internal_buffer;
  int keep = 0;
  while (keep < mb.num_elements &&
         !mb.pkt_start[(mb.head + keep) % BX_MOUSE_BUFF_SIZE])
    keep++;
  if (mouse_debug() && mb.num_elements > keep)
    fprintf(stderr, "MOUSEDBG dropped %d unsent stream bytes (kept %d)\n",
            mb.num_elements - keep, keep);
  mb.num_elements = keep;
}

/**
 * Forget host motion (and intermediate button states) that has not been
 * turned into a packet yet. A current button state the guest has not seen
 * stays pending, so it is reported once packets can flow again.
 **/
void CKeyboard::discard_mouse_motion() {
  state.mouse.delayed_dx = 0;
  state.mouse.delayed_dy = 0;
  state.mouse.delayed_dz = 0;
  state.mouse.button_queue_len = 0;
  state.mouse.data_pending =
      state.mouse.button_status != state.mouse.reported_buttons;
}

/**
 * Enable or disable the keyboard clock. While disabled, buffered keystrokes
 * stay in the keyboard and new ones are not generated.
 **/
void CKeyboard::set_kbd_clock_enable(u8 value) {
  state.kbd_clock_enabled = (value != 0);
}

/**
 * Enable or disable the mouse clock. Disabling it discards motion not yet
 * packetized and whole unsent packets, so no stale movement surfaces later as
 * a cursor jump; the rest of a packet already started is kept and completes
 * once the clock runs again.
 **/
void CKeyboard::set_aux_clock_enable(u8 value) {
#ifdef DEBUG_KBD
  BX_DEBUG(("set_aux_clock_enable(%u)", (unsigned)value));
#endif
  state.aux_clock_enabled = (value != 0);
  if (!state.aux_clock_enabled) {
    discard_mouse_motion();
    drop_unsent_mouse_packets();
  }
}

/**
 * Send a byte from controller to keyboard
 **/
void CKeyboard::ctrl_to_kbd(u8 value) {
#ifdef DEBUG_KBD
  BX_DEBUG(("controller passed byte %02xh to keyboard", value));
#endif
  // Command bytes (ED..FF) never collide with parameter values, so one that
  // arrives while a parameter is expected aborts the pending command -- e.g.
  // a driver's FF reset after a reboot interrupted an LED update.
  if (value >= 0xED) {
    state.expecting_scancodes_set = 0;
    state.kbd_internal_buffer.expecting_typematic = 0;
    state.kbd_internal_buffer.expecting_led_write = 0;
    state.kbd_internal_buffer.expecting_make_break = 0;
  }

  if (state.kbd_internal_buffer.expecting_make_break) {
    state.kbd_internal_buffer.expecting_make_break = 0;
#ifdef DEBUG_KBD
    printf("setting key %x to make/break mode (unused)   \n", value);
#endif
    kbd_response_enQ(0xFA); // send ACK
    return;
  }

  if (state.kbd_internal_buffer.expecting_typematic) {
    state.kbd_internal_buffer.expecting_typematic = 0;
    state.kbd_internal_buffer.delay = (value >> 5) & 0x03;
#ifdef DEBUG_KBD
    switch (state.kbd_internal_buffer.delay) {
    case 0:
      BX_INFO(("setting delay to 250 mS (unused)"));
      break;
    case 1:
      BX_INFO(("setting delay to 500 mS (unused)"));
      break;
    case 2:
      BX_INFO(("setting delay to 750 mS (unused)"));
      break;
    case 3:
      BX_INFO(("setting delay to 1000 mS (unused)"));
      break;
    }
#endif
    state.kbd_internal_buffer.repeat_rate = value & 0x1f;
#ifdef DEBUG_KBD
    double cps =
        1 /
        ((double)(8 + (value & 0x07)) *
         (double)exp(log((double)2) * (double)((value >> 3) & 0x03)) * 0.00417);
    BX_INFO(("setting repeat rate to %.1f cps (unused)", cps));
#endif
    kbd_response_enQ(0xFA); // send ACK
    return;
  }

  if (state.kbd_internal_buffer.expecting_led_write) {
    state.kbd_internal_buffer.expecting_led_write = 0;
    state.kbd_internal_buffer.led_status = value;
#ifdef DEBUG_KBD
    BX_DEBUG(("LED status set to %02x",
              (unsigned)state.kbd_internal_buffer.led_status));
#endif
    kbd_response_enQ(0xFA); // send ACK %%%
    return;
  }

  if (state.expecting_scancodes_set) {
    state.expecting_scancodes_set = 0;
    if (value != 0) {
      if (value < 4) {
        state.current_scancodes_set = (value - 1);
#ifdef DEBUG_KBD
        BX_INFO(("Switched to scancode set %d",
                 (unsigned)state.current_scancodes_set + 1));
#endif
        kbd_response_enQ(0xFA);
      } else {
        BX_ERROR(("Received scancodes set out of range: %d", value));
        kbd_response_enQ(0xFF); // send ERROR
      }
    } else {

      // Send ACK (SF patch #1159626)
      kbd_response_enQ(0xFA);

      // Send current scancodes set to port 0x60
      if (state.scancodes_translate)
        kbd_response_enQ(translation8042[1 + state.current_scancodes_set]);
      else
        kbd_response_enQ(1 + state.current_scancodes_set);
    }

    return;
  }

  switch (value) {

    //    case 0x00: // ??? ignore and let OS timeout with no response
    // #ifdef DEBUG_KBD
    //      printf("kbd: command 00: ignored.   \n");
    // #endif
    //      kbd_response_enQ(0xFA); // send ACK %%%
    //      break;
    //
    //    case 0x05: // ???
    // #ifdef DEBUG_KBD
    //      printf("kbd: command 05:  unknown.   \n");
    // #endif
    //      // (mch) trying to get this to work...
    //      state.status.sysf = 1;
    //      break;
  case 0xed: // LED Write
    state.kbd_internal_buffer.expecting_led_write = 1;
#ifdef DEBUG_KBD
    printf("kbd: Expecting led write info.   \n");
#endif
    kbd_response_enQ(0xFA); // send ACK
    break;

  case 0xee: // echo
#ifdef DEBUG_KBD
    printf("kbd: command ee: echo.   \n");
#endif
    kbd_response_enQ(0xEE); // return same byte (EEh) as echo diagnostic
    break;

  case 0xf0: // Select alternate scan code set
    state.expecting_scancodes_set = 1;
#ifdef DEBUG_KBD
    printf("kbd: Expecting scancode set info.   \n");
#endif
    kbd_response_enQ(0xFA); // send ACK
    break;

  case 0xf2: // identify keyboard
#ifdef DEBUG_KBD
    printf("kbd: command f2: identify keyboard.   \n");
#endif

    //  Keyboard IDs
    //
    // Keyboards do report an ID as a reply to the command f2. An MF2 AT
    // keyboard reports ID ab 83. Translation turns this into ab 41.
    kbd_response_enQ(0xFA);
    kbd_response_enQ(0xAB);

    if (state.scancodes_translate)
      kbd_response_enQ(0x41);
    else
      kbd_response_enQ(0x83);
    break;

  case 0xf3: // typematic info
    state.kbd_internal_buffer.expecting_typematic = 1;
#ifdef DEBUG_KBD
    printf("kbd: Expecting typematic info.   \n");
#endif
    kbd_response_enQ(0xFA); // send ACK
    break;

  case 0xf4: // enable keyboard
    state.kbd_internal_buffer.scanning_enabled = 1;
#ifdef DEBUG_KBD
    printf("kbd: command f4: enable keyboard.   \n");
#endif
    kbd_response_enQ(0xFA); // send ACK
    break;

  case 0xf5: // reset keyboard to power-up settings and disable scanning
    resetinternals(1);
    kbd_response_enQ(0xFA); // send ACK
    state.kbd_internal_buffer.scanning_enabled = 0;
#ifdef DEBUG_KBD
    printf("kbd: command f5: reset and disable keyboard.   \n");
#endif
    break;

  case 0xf6: // reset keyboard to power-up settings and enable scanning
    resetinternals(1);
    kbd_response_enQ(0xFA); // send ACK
    state.kbd_internal_buffer.scanning_enabled = 1;
#ifdef DEBUG_KBD
    printf("kbd: command f6: reset and enable keyboard.   \n");
#endif
    break;

  case 0xfc: // PS/2 Set Key Type to Make/Break
    state.kbd_internal_buffer.expecting_make_break = 1;
#ifdef DEBUG_KBD
    printf("kbd: Expecting make/break info.   \n");
#endif
    kbd_response_enQ(0xFA); /* send ACK */
    break;

  case 0xfe: // resend: repeat the last byte sent (no ACK)
    kbd_response_enQ(state.last_kbd_byte);
    break;

  case 0xff: // reset: internal keyboard reset and afterwards the BAT
#ifdef DEBUG_KBD
    printf("kbd: command ff: reset keyboard w/BAT.   \n");
#endif
    resetinternals(1);
    kbd_response_enQ(0xFA); // send ACK
    kbd_response_enQ(0xAA); // BAT test passed
    break;

    // case 0xd3:
    //   kbd_response_enQ(0xfa);
    //   break;
  case 0xf7: // PS/2 Set All Keys To Typematic
  case 0xf8: // PS/2 Set All Keys to Make/Break
  case 0xf9: // PS/2 PS/2 Set All Keys to Make
  case 0xfa: // PS/2 Set All Keys to Typematic Make/Break
  case 0xfb: // PS/2 Set Key Type to Typematic
  case 0xfd: // PS/2 Set Key Type to Make
    printf("kbd: unhandled command: %02x, ACKing     \n", value);
    kbd_response_enQ(0xFA);
    break;

  default:
    printf("kbd: command %02x: not recognized!   \n", value);
    kbd_response_enQ(0xFE); /* send NACK */
    break;
  }
}

/**
 * Send a byte from controller to mouse
 **/
void CKeyboard::ctrl_to_mouse(u8 value) {
#ifdef DEBUG_KBD
  BX_DEBUG(("MOUSE: ctrl_to_mouse(%02xh)", (unsigned)value));
  BX_DEBUG(("  enable = %u", (unsigned)state.mouse.enable));
  BX_DEBUG(("  allow_irq12 = %u", (unsigned)state.allow_irq12));
  BX_DEBUG(("  aux_clock_enabled = %u", (unsigned)state.aux_clock_enabled));
#endif
  // Debug aid: ALPHABOX_MOUSE_DEBUG=1 traces every command the guest sends to
  // the PS/2 aux device — shows whether a guest driver ever detects and
  // enables the mouse (0xf4 = enable stream mode).
  if (mouse_debug())
    fprintf(stderr,
            "MOUSEDBG aux cmd %02x (enable=%u mode=%u irq12=%u clock=%u)\n",
            (unsigned)value, (unsigned)state.mouse.enable,
            (unsigned)state.mouse.mode, (unsigned)state.allow_irq12,
            (unsigned)state.aux_clock_enabled);

  // A byte from the host makes the mouse discard the stream packets it has
  // not started sending; its reply is queued after the rest of a packet
  // already in progress (see kbd_service()).
  drop_unsent_mouse_packets();

  // an ACK (0xFA) is always the first response to any valid input
  // received from the system other than Set-Wrap-Mode & Resend-Command
  // Command bytes (E6..FF) never collide with parameter values (sample rates
  // <= 200 = C8h, resolutions 0-3), so one that arrives while a parameter is
  // expected aborts the pending command and is executed instead.
  if (state.expecting_mouse_parameter && value < 0xE6) {
    state.expecting_mouse_parameter = 0;
    switch (state.last_mouse_command) {
    case 0xf3: // Set Mouse Sample Rate
      state.mouse.sample_rate = value;
#ifdef DEBUG_KBD
      BX_DEBUG(("[mouse] Sampling rate set: %d Hz", value));
#endif
      if ((value == 200) && (!state.mouse.im_request)) {
        state.mouse.im_request = 1;
      } else if ((value == 100) && (state.mouse.im_request == 1)) {
        state.mouse.im_request = 2;
      } else if ((value == 80) && (state.mouse.im_request == 2)) {
#ifdef DEBUG_KBD
        BX_INFO(("wheel mouse mode enabled"));
#endif
        state.mouse.im_mode = 1;
        state.mouse.im_request = 0;
      } else {
        state.mouse.im_request = 0;
      }

      controller_enQ(0xFA, KBC_SRC_AUX); // ack
      break;

    case 0xe8: // Set Mouse Resolution
      switch (value) {
      case 0:
        state.mouse.resolution_cpmm = 1;
        break;
      case 1:
        state.mouse.resolution_cpmm = 2;
        break;
      case 2:
        state.mouse.resolution_cpmm = 4;
        break;
      case 3:
        state.mouse.resolution_cpmm = 8;
        break;
      default:
        BX_PANIC(("[mouse] Unknown resolution %d", value));
        break;
      }
      discard_mouse_motion(); // resets the movement counters

#ifdef DEBUG_KBD
      BX_DEBUG(("[mouse] Resolution set to %d counts per mm",
                state.mouse.resolution_cpmm));
#endif
      controller_enQ(0xFA, KBC_SRC_AUX); // ack
      break;

    default:
      BX_PANIC(("MOUSE: unknown last command (%02xh)",
                (unsigned)state.last_mouse_command));
    }
  } else {
    state.expecting_mouse_parameter = 0;
    state.last_mouse_command = value;

    // test for wrap mode first
    if (state.mouse.mode == MOUSE_MODE_WRAP) {

      // if not a reset command or reset wrap mode
      // then just echo the byte.
      if ((value != 0xff) && (value != 0xec)) {

        //        if (bx_dbg.mouse)
#ifdef DEBUG_KBD
        BX_INFO(("[mouse] wrap mode: Ignoring command %0X02.", value));
#endif
        controller_enQ(value, KBC_SRC_AUX);

        // bail out
        return;
      }
    }

    switch (value) {
    case 0xe6:                           // Set Mouse Scaling to 1:1
      controller_enQ(0xFA, KBC_SRC_AUX); // ACK
      state.mouse.scaling = 1;
#ifdef DEBUG_KBD
      BX_DEBUG(("[mouse] Scaling set to 1:1"));
#endif
      break;

    case 0xe7:                           // Set Mouse Scaling to 2:1
      controller_enQ(0xFA, KBC_SRC_AUX); // ACK
      state.mouse.scaling = 2;
#ifdef DEBUG_KBD
      BX_DEBUG(("[mouse] Scaling set to 2:1"));
#endif
      break;

    case 0xe8:                           // Set Mouse Resolution
      controller_enQ(0xFA, KBC_SRC_AUX); // ACK
      state.expecting_mouse_parameter = 1;
      break;

    case 0xea: // Set Stream Mode
               //        if (bx_dbg.mouse)
#ifdef DEBUG_KBD
      BX_INFO(("[mouse] Mouse stream mode on."));
#endif
      state.mouse.mode = MOUSE_MODE_STREAM;
      discard_mouse_motion();
      controller_enQ(0xFA, KBC_SRC_AUX); // ACK
      break;

    case 0xec: // Reset Wrap Mode
      // unless we are in wrap mode ignore the command
      if (state.mouse.mode == MOUSE_MODE_WRAP) {

        //          if (bx_dbg.mouse)
#ifdef DEBUG_KBD
        BX_INFO(("[mouse] Mouse wrap mode off."));
#endif

        // restore previous mode except disable stream mode reporting.
        // ### TODO disabling reporting in stream mode
        state.mouse.mode = state.mouse.saved_mode;
        discard_mouse_motion();
        controller_enQ(0xFA, KBC_SRC_AUX); // ACK
      }
      break;

    case 0xee: // Set Wrap Mode
               // Unsent stream packets were already dropped above, and wrap
               // mode creates no new ones (see create_mouse_packet()).
#ifdef DEBUG_KBD
      BX_INFO(("[mouse] Mouse wrap mode on."));
#endif
      state.mouse.saved_mode = state.mouse.mode;
      state.mouse.mode = MOUSE_MODE_WRAP;
      discard_mouse_motion();
      controller_enQ(0xFA, KBC_SRC_AUX); // ACK
      break;

    case 0xf0: // Set Remote Mode (polling mode, i.e. not stream mode.)
               // Unsent stream packets were already dropped above.
#ifdef DEBUG_KBD
      BX_INFO(("[mouse] Mouse remote mode on."));
#endif
      state.mouse.mode = MOUSE_MODE_REMOTE;
      discard_mouse_motion();
      controller_enQ(0xFA, KBC_SRC_AUX); // ACK
      break;

    case 0xf2:                           // Read Device Type
      controller_enQ(0xFA, KBC_SRC_AUX); // ACK
      if (state.mouse.im_mode)
        controller_enQ(0x03, KBC_SRC_AUX); // Device ID (wheel z-mouse)
      else
        controller_enQ(0x00, KBC_SRC_AUX); // Device ID (standard)
#ifdef DEBUG_KBD
      BX_DEBUG(("[mouse] Read mouse ID"));
#endif
      break;

    case 0xf3: // Set Mouse Sample Rate (sample rate written to port 60h)
      controller_enQ(0xFA, KBC_SRC_AUX); // ACK
      state.expecting_mouse_parameter = 1;
      break;

    case 0xf4: // Enable (in stream mode)
      state.mouse.enable = 1;
      state.mouse.mode = MOUSE_MODE_STREAM;
      set_aux_clock_enable(1);
      discard_mouse_motion();
      controller_enQ(0xFA, KBC_SRC_AUX); // ACK
#ifdef DEBUG_KBD
      BX_DEBUG(("[mouse] Mouse enabled (stream mode)"));
#endif
      break;

    case 0xf5: // Disable (in stream mode)
      state.mouse.enable = 0;
      discard_mouse_motion();
      controller_enQ(0xFA, KBC_SRC_AUX); // ACK
#ifdef DEBUG_KBD
      BX_DEBUG(("[mouse] Mouse disabled (stream mode)"));
#endif
      break;

    case 0xf6:                         // Set Defaults
      state.mouse.sample_rate = 100;   /* reports per second (default) */
      state.mouse.resolution_cpmm = 4; /* 4 counts per millimeter (default) */
      state.mouse.scaling = 1;         /* 1:1 (default) */
      state.mouse.enable = 0;
      state.mouse.mode = MOUSE_MODE_STREAM;
      discard_mouse_motion();
      controller_enQ(0xFA, KBC_SRC_AUX); // ACK
#ifdef DEBUG_KBD
      BX_DEBUG(("[mouse] Set Defaults"));
#endif
      break;

    case 0xff:                         // Reset
      state.mouse.sample_rate = 100;   /* reports per second (default) */
      state.mouse.resolution_cpmm = 4; /* 4 counts per millimeter (default) */
      state.mouse.scaling = 1;         /* 1:1 (default) */
      state.mouse.mode = MOUSE_MODE_RESET;
      state.mouse.enable = 0;
      discard_mouse_motion();
#ifdef DEBUG_KBD
      if (state.mouse.im_mode)
        BX_INFO(("wheel mouse mode disabled"));
#endif
      state.mouse.im_mode = 0;
      state.mouse.im_request = 0;
      controller_enQ(0xFA, KBC_SRC_AUX); // ACK
      controller_enQ(0xAA, KBC_SRC_AUX); // completion code
      controller_enQ(0x00, KBC_SRC_AUX); // ID code (standard after reset)
#ifdef DEBUG_KBD
      BX_DEBUG(("[mouse] Mouse reset"));
#endif
      break;

    case 0xe9: // Get mouse information
      // should we ack here? (mch): Yes
      controller_enQ(0xFA, KBC_SRC_AUX); // ACK
      controller_enQ(state.mouse.get_status_byte(), KBC_SRC_AUX);
      controller_enQ(state.mouse.get_resolution_byte(), KBC_SRC_AUX);
      controller_enQ(state.mouse.sample_rate, KBC_SRC_AUX);
#ifdef DEBUG_KBD
      BX_DEBUG(("[mouse] Get mouse information"));
#endif
      break;

    case 0xeb: // Read Data (send a packet when in Remote Mode)
    {
      // A solicited packet: it goes through the response queue right after
      // the ACK and carries (and consumes) the motion accumulated so far.
      controller_enQ(0xFA, KBC_SRC_AUX); // ACK
      u8 packet[4];
      int bytes = build_mouse_packet(packet);
      for (int i = 0; i < bytes; i++)
        controller_enQ(packet[i], KBC_SRC_AUX);
      discard_mouse_motion(); // resets the movement counters
    } break;

    case 0xfe: // Resend: repeat the last byte sent (no ACK)
      controller_enQ(state.last_aux_byte, KBC_SRC_AUX);
      break;

    case 0xbb: // OS/2 Warp 3 uses this command
#ifdef DEBUG_KBD
      BX_ERROR(("[mouse] ignoring 0xbb command"));
#endif
      break;

    default:
      BX_ERROR(("[mouse] ctrl_to_mouse(): got value of 0x%02x", value));
      controller_enQ(0xFE, KBC_SRC_AUX); /* send NACK */
    }
  }
}

/**
 * Put one whole stream packet into the mouse buffer, or nothing if it does
 * not fit.
 **/
bool CKeyboard::mouse_enQ_packet(const u8 *packet, int bytes) {
  SKb_state::SAli_mib &mb = state.mouse_internal_buffer;

  if (mb.num_elements + bytes > BX_MOUSE_BUFF_SIZE) {
    kbc_drop_note("mouse buffer full, packet lost", packet[0]);
    return false;
  }

  for (int i = 0; i < bytes; i++) {
#ifdef DEBUG_KBD
    BX_DEBUG(("mouse_enQ(%02x)", (unsigned)packet[i]));
#endif
    int tail = (mb.head + mb.num_elements) % BX_MOUSE_BUFF_SIZE;
    mb.buffer[tail] = packet[i];
    mb.pkt_start[tail] = (i == 0);
    mb.num_elements++;
  }

  // Event-driven delivery, same as keystrokes -- see kbd_service().
  kbd_service();
  return true;
}

void CKeyboard::set_mouse_capture(bool val) {
  std::lock_guard<std::mutex> guard(kbdLock);
  state.mouse.captured = val;
}

void CKeyboard::mouse_motion(int delta_x, int delta_y, int delta_z,
                             unsigned button_state) {
  std::lock_guard<std::mutex> guard(kbdLock);

  if (!state.mouse.captured)
    return;

  // Motion counts only while the guest can receive it: stream mode with
  // reporting enabled, or remote mode (polled with EB). Anything else would
  // resurface later as a cursor jump.
  const bool stream =
      state.mouse.enable && state.mouse.mode == MOUSE_MODE_STREAM;
  const bool streaming = stream && state.aux_clock_enabled;

  const u8 buttons = (u8)(button_state & 0x07);
  if (buttons != state.mouse.button_status) {
    // Packets are built at most once per worker tick, so a press and release
    // can both happen before the next one. Keep a button state that has not
    // been packetized yet instead of overwriting it, or the click is lost.
    SKb_state::SAli_mouse &m = state.mouse;
    const u8 last_queued = m.button_queue_len
                               ? m.button_queue[m.button_queue_len - 1]
                               : m.reported_buttons;
    if (streaming && m.button_status != last_queued &&
        m.button_queue_len < BX_MOUSE_BUTTON_QSIZE)
      m.button_queue[m.button_queue_len++] = m.button_status;
    m.button_status = buttons; // also reported by the E9 status byte
  }

  if (!stream && state.mouse.mode != MOUSE_MODE_REMOTE)
    return;

  if (stream && !state.aux_clock_enabled) {
    // The controller inhibits the mouse: drop the motion, but keep a button
    // change pending so the guest learns of it once the clock runs again.
    if (state.mouse.button_status != state.mouse.reported_buttons)
      state.mouse.data_pending = true;
    return;
  }

  // Bound the backlog (a guest that stops reading must not wrap the s16).
  auto accumulate = [](s16 acc, int delta) {
    const int limit = 4096;
    int v = acc + delta;
    return (s16)(v > limit ? limit : v < -limit ? -limit : v);
  };
  state.mouse.delayed_dx = accumulate(state.mouse.delayed_dx, delta_x);
  state.mouse.delayed_dy = accumulate(state.mouse.delayed_dy, delta_y);
  state.mouse.delayed_dz = accumulate(state.mouse.delayed_dz, delta_z);
  state.mouse.data_pending = true;
}

/**
 * Build a movement packet (3 bytes, 4 in wheel mode) from the accumulated
 * motion and consume the part of it the packet carries; motion beyond the
 * 9-bit range stays for the following packets.
 **/
int CKeyboard::build_mouse_packet(u8 *packet) {
  auto clamp = [](int v, int lo, int hi) {
    return v < lo ? lo : v > hi ? hi : v;
  };

  const int dx = clamp(state.mouse.delayed_dx, -256, 255);
  const int dy = clamp(state.mouse.delayed_dy, -256, 255);
  const int dz = clamp(state.mouse.delayed_dz, -127, 127);
  state.mouse.delayed_dx -= dx;
  state.mouse.delayed_dy -= dy;
  state.mouse.delayed_dz = 0;

  // The oldest button state not yet reported goes first.
  u8 buttons = state.mouse.button_status;
  if (state.mouse.button_queue_len) {
    buttons = state.mouse.button_queue[0];
    for (int i = 1; i < state.mouse.button_queue_len; i++)
      state.mouse.button_queue[i - 1] = state.mouse.button_queue[i];
    state.mouse.button_queue_len--;
  }
  state.mouse.reported_buttons = buttons;
  state.mouse.data_pending =
      state.mouse.delayed_dx != 0 || state.mouse.delayed_dy != 0 ||
      state.mouse.button_queue_len != 0 ||
      state.mouse.button_status != state.mouse.reported_buttons;

  u8 b1 = (buttons & 0x07) | 0x08; // bit3 always set
  if (dx < 0)
    b1 |= 0x10;
  if (dy < 0)
    b1 |= 0x20;
  packet[0] = b1;
  packet[1] = (u8)dx;
  packet[2] = (u8)dy;
  packet[3] = (u8)-dz;
  return state.mouse.im_mode ? 4 : 3;
}

/**
 * Turn pending host motion into an unsolicited stream packet. Packets are
 * created only while reporting is enabled in stream mode with the aux clock
 * running, and only when the controller's output path is idle and no command
 * transaction is waiting for its next byte; otherwise the motion keeps
 * accumulating for a later packet.
 **/
void CKeyboard::create_mouse_packet() {
  if (!state.mouse.data_pending)
    return;
  if (!state.mouse.enable || state.mouse.mode != MOUSE_MODE_STREAM ||
      !state.aux_clock_enabled)
    return;
  if (state.status.outb || state.kbd_controller_Qsize ||
      state.mouse_internal_buffer.num_elements || mouse_input_held())
    return;

  u8 packet[4];
  int bytes = build_mouse_packet(packet);
  mouse_enQ_packet(packet, bytes);
}

/**
 * Drive the 8042's two IRQ lines (keyboard IRQ1, mouse IRQ12) to match the
 * current output-buffer state.  OBF is a level held until the guest reads
 * port 0x60; pic_set_line() turns each 0->1 transition into one 8259 edge and
 * holds the request until it is serviced.
 **/
void CKeyboard::kbd_update_irq() {
  theAli->pic_set_line(
      0, 1, state.status.outb && !state.status.auxb && state.allow_irq1);
  theAli->pic_set_line(
      1, 4, state.status.outb && state.status.auxb && state.allow_irq12);
}

/**
 * Refill the single output buffer, if it is free, and drive the IRQ lines.
 *
 * Priority:
 *  1. the controller/response queue (controller replies, keyboard and mouse
 *     command replies), in FIFO order -- except that a device reply first
 *     lets that device finish a key event / packet already partly delivered,
 *     so a reply never lands inside a multi-byte sequence;
 *  2. buffered keystrokes, unless a keyboard transaction holds them back;
 *  3. mouse stream packets, unless a mouse transaction holds them back.
 * A hold only keeps a new key event or packet from starting; the rest of one
 * already started always completes (clock permitting).
 *
 * kbd_update_irq() is called both before and after the refill: the first call
 * drops the line for the byte just consumed (clearing the 8259 edge latch) so
 * the refilled byte yields a fresh edge.
 **/
void CKeyboard::kbd_service() {
  kbd_update_irq();

  if (!state.status.outb) {
    const u8 head_src =
        state.kbd_controller_Qsize ? state.kbd_controller_Qsrc[0] : 0xff;

    if (head_src == KBC_SRC_KBD && kbd_tail_pending()) {
      deliver_kbd_byte();
    } else if (head_src == KBC_SRC_AUX && mouse_tail_pending()) {
      deliver_mouse_byte();
    } else if (controller_deQ()) {
      // the head of the controller queue is now in the output buffer
    } else if (state.kbd_internal_buffer.num_elements &&
               state.kbd_clock_enabled &&
               (kbd_tail_pending() || !kbd_input_held())) {
      deliver_kbd_byte();
    } else if (state.mouse_internal_buffer.num_elements &&
               state.aux_clock_enabled &&
               (mouse_tail_pending() || !mouse_input_held())) {
      deliver_mouse_byte();
    }
  }

  kbd_update_irq();
}

/**
 * Worker-thread tick: sample pending host mouse motion into a packet, then
 * deliver queued bytes and drive the IRQ lines.
 **/
void CKeyboard::execute() {
  create_mouse_packet();
  kbd_service();
}

/**
 * Check if threads are still running.
 **/
void CKeyboard::check_state() {
  if (myThreadDead.load())
    FAILURE(Thread, "KBD thread has died");
}

/**
 * Thread entry point.
 **/
void CKeyboard::run() {
  try {
    for (;;) {
      if (StopThread)
        return;

      {
        std::lock_guard<std::mutex> guard(kbdLock);
        execute();
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  } catch (CException &e) {
    printf("Exception in kbd thread: %s.\n", e.displayText().c_str());
    myThreadDead.store(true);
  }
}

static u32 kb_magic1 = 0x65481687;
static u32 kb_magic2 = 0x24895375;

/**
 * Save state to a Virtual Machine State file.
 **/
int CKeyboard::SaveState(FILE *f) {
  long ss = sizeof(state);

  fwrite(&kb_magic1, sizeof(u32), 1, f);
  fwrite(&ss, sizeof(long), 1, f);
  fwrite(&state, sizeof(state), 1, f);
  fwrite(&kb_magic2, sizeof(u32), 1, f);
  printf("kbc: %d bytes saved.\n", (int)ss);
  return 0;
}

/**
 * Restore state from a Virtual Machine State file.
 **/
int CKeyboard::RestoreState(FILE *f) {
  long ss;
  u32 m1;
  u32 m2;
  size_t r;

  r = fread(&m1, sizeof(u32), 1, f);
  if (r != 1) {
    printf("kbc: unexpected end of file!\n");
    return -1;
  }

  if (m1 != kb_magic1) {
    printf("kbc: MAGIC 1 does not match!\n");
    return -1;
  }

  r = fread(&ss, sizeof(long), 1, f);
  if (r != 1) {
    printf("kbc: unexpected end of file!\n");
    return -1;
  }

  if (ss != sizeof(state)) {
    printf("kbc: STRUCT SIZE does not match!\n");
    return -1;
  }

  r = fread(&state, sizeof(state), 1, f);
  if (r != 1) {
    printf("kbc: unexpected end of file!\n");
    return -1;
  }

  r = fread(&m2, sizeof(u32), 1, f);
  if (r != 1) {
    printf("kbc: unexpected end of file!\n");
    return -1;
  }

  if (m2 != kb_magic2) {
    printf("kbc: MAGIC 2 does not match!\n");
    return -1;
  }

  printf("kbc: %d bytes restored.\n", (int)ss);
  return 0;
}

CKeyboard *theKeyboard = 0;
