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
 */

/**
 * \file
 * Unit test of the 8042 keyboard controller, keyboard and mouse
 * (src/devices/isa/Keyboard.cpp), driven through ports 60h and 64h the way a
 * guest driver does, with host keys and mouse motion fed in as the GUI does.
 * The rest of the machine is stubbed below: the controller's interrupt lines
 * are recorded instead of reaching the 8259.
 *
 * Built with the emulator; run with `ctest --test-dir <lane>` or directly.
 * Exit status 0 when every check passes.
 **/
#include "AliM1543C.hpp"
#include "Keyboard.hpp"
#include "StdAfx.hpp"
#include "System.hpp"

#include <string>
#include <vector>

// ---- stubs for what Keyboard.cpp needs from the rest of the machine ----

static bool irq_line[2]; // [0] IRQ1 (keyboard), [1] IRQ12 (mouse)

CSystemComponent::CSystemComponent(CConfigurator *cfg, CSystem *system)
    : devid_string(nullptr), cSystem(system), myCfg(cfg) {}
CSystemComponent::~CSystemComponent() {}
int CSystem::RegisterMemory(CSystemComponent *, int, u64, u64) { return 0; }
void CAliM1543C::pic_set_line(int index, int intno, bool active) {
  (void)intno;
  irq_line[index & 1] = active;
}
alignas(CAliM1543C) static unsigned char ali_storage[sizeof(CAliM1543C)];
CAliM1543C *theAli = reinterpret_cast<CAliM1543C *>(ali_storage);
alignas(CSystem) static unsigned char sys_storage[sizeof(CSystem)];

// ---- the harness ----

static CKeyboard *kb;
static int failures = 0, checks = 0;

static void out60(u8 v) { kb->WriteMem(0, 0x60, 8, v); }
static void out64(u8 v) { kb->WriteMem(1, 0x64, 8, v); }
static u8 in60() { return (u8)kb->ReadMem(0, 0x60, 8); }
static u8 in64() { return (u8)kb->ReadMem(1, 0x64, 8); }

/// Every byte the controller has for the guest, read as a polling driver
/// does; aux bytes are marked with a leading 'm'.
static std::string drain(int max = 128) {
  std::string s;
  char b[8];
  for (int i = 0; i < max; i++) {
    const u8 st = in64();
    if (!(st & 1))
      break;
    const u8 v = in60();
    snprintf(b, sizeof(b), "%s%s%02x", s.empty() ? "" : " ",
             (st & 0x20) ? "m" : "", v);
    s += b;
  }
  return s;
}

static void check(const char *what, const std::string &got,
                  const std::string &want) {
  checks++;
  if (got == want)
    return;
  failures++;
  printf("FAIL %s\n     got  [%s]\n     want [%s]\n", what, got.c_str(),
         want.c_str());
}

static void check_true(const char *what, bool ok) {
  checks++;
  if (ok)
    return;
  failures++;
  printf("FAIL %s\n", what);
}

/// A fresh controller, as at power-up.
static void fresh() {
  kb->init();
  drain();
}

static void press(u32 key) { kb->gen_scancode(key); }
static void release(u32 key) { kb->gen_scancode(key | BX_KEY_RELEASED); }
static void tap(u32 key) {
  press(key);
  release(key);
}

/// Mouse command through D4, with whatever it answers.
static std::string aux(std::initializer_list<u8> bytes) {
  for (u8 b : bytes) {
    out64(0xD4);
    out60(b);
  }
  return drain();
}

/// The mouse set up the way the drivers do: interface on, reporting on.
static void mouse_streaming() {
  out64(0xA8);
  check("mouse enable", aux({0xF4}), "mfa");
}

/// Host motion turned into a stream packet, as the worker thread does.
static std::string motion(int dx, int dy, int dz, unsigned buttons) {
  kb->mouse_motion(dx, dy, dz, buttons);
  kb->execute();
  return drain();
}

// ---- the tests ----

/// The controller's own tests and the keyboard's identify and echo.
static void test_controller_basics() {
  fresh();
  out64(0xAA);
  check("AA self test", drain(), "55");
  out64(0xAB);
  check("AB keyboard interface test", drain(), "00");
  out64(0xA9);
  check("A9 aux interface test", drain(), "00");
  out60(0xF2);
  check("F2 identify, translated", drain(), "fa ab 41");
  out60(0xEE);
  check("EE echo", drain(), "ee");
  out64(0xD2);
  out60(0x5A);
  check("D2 writes the keyboard output buffer", drain(), "5a");
  out64(0xD3);
  out60(0x5B);
  check("D3 writes the aux output buffer", drain(), "m5b");
}

/// What SRM sets up on the graphics console (ALPHABOX_TRACE_KBC): command
/// byte 03, scan code set 3, defaults, right Alt and right Ctrl make/break.
/// OpenVMS's driver asks for every key make/break instead (F8).
static void test_srm_and_vms_set3() {
  fresh();
  out60(0xFF);
  out64(0x60);
  out60(0x03);
  out60(0xF0);
  out60(0x03);
  out60(0xF6);
  out60(0xFC);
  out60(0x39);
  out60(0xFC);
  out60(0x58);
  out60(0xF4);
  check("SRM's keyboard set-up", drain(), "fa aa fa fa fa fa fa fa fa fa");
  tap(BX_KEY_CTRL_R);
  tap(BX_KEY_SHIFT_L);
  tap(BX_KEY_A);
  check("SRM: right Ctrl, left Shift break; A does not", drain(),
        "58 f0 58 12 f0 12 1c");
  out60(0xF8);
  check("F8", drain(), "fa");
  tap(BX_KEY_A);
  check("OpenVMS: every key make/break", drain(), "1c f0 1c");
}

/// The mouse reset and wrap mode, which every driver relies on.
static void test_mouse_basics() {
  fresh();
  check("mouse reset", aux({0xFF}), "mfa maa m00");
  check("E9 after reset", aux({0xE9}), "mfa m00 m02 m64");
  mouse_streaming();
  check("motion", motion(3, -2, 0, 1), "m29 m03 mfe");
  check("wrap mode echoes", aux({0xEE, 0x12, 0x34, 0xEC}), "mfa m12 m34 mfa");
}

/// KBD-01: power-up and reset leave the keyboard in scan code set 2, which
/// the controller translates to set 1.
static void test_scan_set_default() {
  fresh();
  out60(0xF0);
  out60(0x00);
  check("F0 00 at power-up (translated)", drain(), "fa fa 41");
  out60(0xFF);
  check("FF reset", drain(), "fa aa");
  out60(0xF0);
  out60(0x00);
  check("F0 00 after FF (translated)", drain(), "fa fa 41");

  press(BX_KEY_CTRL_L);
  release(BX_KEY_CTRL_L);
  press(BX_KEY_ALT_L);
  release(BX_KEY_ALT_L);
  check("left Ctrl, left Alt, translated", drain(), "1d 9d 38 b8");

  out64(0x60);
  out60(0x01); // translation off, keyboard interrupt on
  out60(0xF0);
  out60(0x00);
  check("F0 00 untranslated", drain(), "fa fa 02");
  tap(BX_KEY_CTRL_L);
  check("left Ctrl in set 2", drain(), "14 f0 14");

  // set 3, as SRM and OpenVMS select it: only the shift keys break
  out60(0xF0);
  out60(0x03);
  check("F0 03", drain(), "fa fa");
  tap(BX_KEY_CTRL_L);
  tap(BX_KEY_A);
  check("set 3: left Ctrl make/break, A make only", drain(), "11 f0 11 1c");

  // Set Default (F6) and Default Disable (F5) keep the scan code set: SRM
  // selects set 3 and then sends F6.
  out60(0xF6);
  out60(0xF0);
  out60(0x00);
  check("F6 keeps set 3", drain(), "fa fa fa 03");
  out60(0xF5);
  out60(0xF0);
  out60(0x00);
  out60(0xF4);
  check("F5 keeps set 3", drain(), "fa fa fa 03 fa");
  // Reset (FF) is power-up: set 2 again
  out60(0xFF);
  out60(0xF0);
  out60(0x00);
  check("FF returns to set 2", drain(), "fa aa fa fa 02");
}

/// KBD-02: keys typed while the controller inhibits the keyboard wait in
/// the keyboard. An event that does not fit in its 16-byte buffer is lost
/// and the overrun code takes the last position; keys after it are lost
/// until the guest reads (IBM 84F9735, Keyboards pp. 19-20).
static void test_inhibit_and_overrun() {
  fresh();
  out64(0xAD);
  tap(BX_KEY_A);
  check("nothing while inhibited", drain(), "");
  out64(0xAE);
  check("make and break after AE", drain(), "1e 9e");
  out64(0x60);
  out60(0x51); // bit 4: keyboard disabled
  tap(BX_KEY_A);
  check("nothing while the command byte inhibits", drain(), "");
  out64(0x60);
  out60(0x41);
  check("make and break once released", drain(), "1e 9e");

  // Translated: nobody reads while A..T are pressed. A goes to the output
  // register, B..Q fill the buffer, R finds it full and the overrun code
  // replaces Q; S and T are lost.
  fresh();
  out64(0x60);
  out60(0x44); // translate, no interrupts: nobody reads
  for (int i = 0; i < 20; i++)
    press(BX_KEY_A + i);
  check("full buffer, translated", drain(),
        "1e 30 2e 20 12 21 22 23 17 24 25 26 32 31 18 19 ff");
  tap(BX_KEY_A);
  check("typing after an overrun", drain(), "1e 9e");

  // Untranslated set 2: a two-byte break that does not fit leaves room
  // for the overrun code, 00.
  fresh();
  out64(0x60);
  out60(0x04);
  for (int i = 0; i < 16; i++)
    press(BX_KEY_A + i);
  release(BX_KEY_A);
  check("break that does not fit, set 2", drain(),
        "1c 32 21 23 24 2b 34 33 43 3b 42 4b 3a 31 44 4d 00");
}

/// KBD-07: FF re-enables scanning after F5.
static void test_reset_enables_scanning() {
  fresh();
  out60(0xF5);
  check("F5", drain(), "fa");
  tap(BX_KEY_A);
  check("F5: no scanning", drain(), "");
  out60(0xFF);
  check("FF", drain(), "fa aa");
  tap(BX_KEY_A);
  check("FF: scanning again", drain(), "1e 9e");
}

/// KBD-08 and KBD-09: the controller commands IBM documents (84F9735,
/// Keyboard/Auxiliary Device Controller pp. 9-13) answer as listed there.
static void test_controller_commands() {
  fresh();
  out64(0xFE); // pulse output port bit 0: nothing comes back
  check("FE pulse", drain(), "");
  out64(0xF0);
  check("F0 pulse", drain(), "");
  out64(0xA4);
  check("A4 password installed?", drain(), "f1");
  out64(0xA5);
  out60(0x41);
  out60(0x42);
  out60(0x00);
  check("A5 password bytes up to a null are swallowed", drain(), "");
  out60(0xEE);
  check("the keyboard gets bytes again after the null", drain(), "ee");
  out64(0x21);
  check("21 controller RAM", drain(), "00");
  out64(0x3F);
  check("3F controller RAM", drain(), "00");
  out64(0x20);
  check("20 command byte at power-up", drain(), "63");
  out64(0x61);
  out60(0x12); // controller RAM write: swallowed
  check("61 xx", drain(), "");
  out64(0x20);
  check("command byte unchanged by 61", drain(), "63");
  out64(0xE0);
  check("E0 test inputs (Type 1: 0)", drain(), "00");
  out64(0xC0);
  check("C0 input port", drain(), "80");
}

/// KBD-11: a state whose queue counts or indexes are out of range is refused.
static void test_restore_validation() {
  fresh();
  tap(BX_KEY_A); // something in the state
  FILE *f = tmpfile();
  check_true("save", kb->SaveState(f) == 0);
  const long len = ftell(f);
  std::vector<unsigned char> img((size_t)len);
  rewind(f);
  check_true("read back", fread(img.data(), 1, img.size(), f) == img.size());
  rewind(f);
  check_true("restore of a good state", kb->RestoreState(f) == 0);
  check("state restored", drain(), "1e 9e");

  // Each field below indexes an array; any byte pattern in it that is out of
  // range must be refused. Patch every byte of the state in turn to 0xff and
  // make sure a restore either refuses it or leaves a controller that still
  // works.
  int refused = 0, accepted = 0;
  const size_t hdr = sizeof(u32) + sizeof(long);
  for (size_t at = hdr; at + sizeof(u32) < img.size(); at++) {
    std::vector<unsigned char> bad = img;
    bad[at] = 0xff;
    FILE *g = tmpfile();
    fwrite(bad.data(), 1, bad.size(), g);
    rewind(g);
    if (kb->RestoreState(g) != 0) {
      refused++;
    } else {
      accepted++;
      // exercise every queue
      drain();
      tap(BX_KEY_A);
      kb->mouse_motion(1, 1, 0, 1);
      kb->execute();
      drain();
      out64(0x20);
      drain();
    }
    fclose(g);
  }
  char what[96];
  snprintf(what, sizeof(what),
           "corrupt states: %d refused, %d accepted, none crashed", refused,
           accepted);
  check_true(what, refused > 0);

  // A refused state leaves the controller as it was.
  fresh();
  tap(BX_KEY_A);
  std::vector<unsigned char> junk = img;
  for (size_t at = hdr; at + sizeof(u32) < junk.size(); at++)
    junk[at] = 0xff;
  FILE *g = tmpfile();
  fwrite(junk.data(), 1, junk.size(), g);
  rewind(g);
  check_true("an all-FF state is refused", kb->RestoreState(g) != 0);
  check("the controller kept its own state", drain(), "1e 9e");
  fclose(g);
  fclose(f);
  fresh();
}

/// KBD-10: a key code outside the table is ignored.
static void test_key_range() {
  fresh();
  press(200);
  press(BX_KEY_UNHANDLED);
  press(BX_KEY_NBKEYS);
  release(200);
  check("out-of-range keys", drain(), "");
}

/// KBD-03: the E9 status byte: bit 0 right, bit 1 middle, bit 2 left.
static void test_mouse_status_buttons() {
  fresh();
  mouse_streaming();
  // status byte 1 is 0x20 (reporting on) plus the buttons
  motion(0, 0, 0, 0x02); // right
  check("E9 with right held", aux({0xE9}).substr(0, 7), "mfa m21");
  motion(0, 0, 0, 0x04); // middle
  check("E9 with middle held", aux({0xE9}).substr(0, 7), "mfa m22");
  motion(0, 0, 0, 0x01); // left
  check("E9 with left held", aux({0xE9}).substr(0, 7), "mfa m24");
}

/// KBD-04: an invalid resolution or rate gets Resend, never silence.
static void test_mouse_bad_parameters() {
  fresh();
  check("E8 04", aux({0xE8, 0x04}), "mfa mfe");
  check("E8 03 after the Resend", aux({0x03}), "mfa");
  check("E9 reads resolution 3", aux({0xE9}), "mfa m00 m03 m64");
  check("F3 00", aux({0xF3, 0x00}), "mfa mfe");
  check("F3 then a command aborts it", aux({0xF2}), "mfa m00");
}

/// KBD-05: F4 neither leaves remote mode nor changes the command byte.
static void test_mouse_enable_keeps_mode() {
  fresh();
  out64(0xA8);
  check("remote mode, enable", aux({0xF0, 0xF4}), "mfa mfa");
  check("E9 status: remote, enabled", aux({0xE9}).substr(0, 7), "mfa m60");
  check("remote mode: no stream packet", motion(4, 0, 0, 0), "");
  check("remote mode: EB reads it", aux({0xEB}), "mfa m08 m04 m00");
  fresh();
  out64(0xA7);
  aux({0xF4});
  out64(0x20);
  const std::string ccb = drain();
  check_true("A7, D4 F4: the command byte keeps the aux disable bit",
             ccb.size() == 2 && (strtoul(ccb.c_str(), 0, 16) & 0x20) != 0);
}

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0); // a crash keeps what came before it
  CSystem *sys = reinterpret_cast<CSystem *>(sys_storage);
  kb = new CKeyboard(nullptr, sys);
  test_controller_basics();
  test_srm_and_vms_set3();
  test_mouse_basics();
  test_inhibit_and_overrun();
  test_controller_commands();
  test_scan_set_default();
  test_reset_enables_scanning();
  test_restore_validation();
  test_key_range();
  test_mouse_status_buttons();
  test_mouse_bad_parameters();
  test_mouse_enable_keeps_mode();
  printf("kbc_test: %d of %d checks passed\n", checks - failures, checks);
  return failures ? 1 : 0;
}
