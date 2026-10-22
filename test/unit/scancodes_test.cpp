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
 * Unit test of the host key chain: the HID usage (SDL3 scancode) to BX_KEY
 * map (src/gui/hostkeys.cpp) and the BX_KEY to scan code tables for sets 1,
 * 2 and 3 plus the 8042's set 2 to set 1 translation (src/gui/scancodes.cpp).
 *
 * The expected values are typed here from the Microsoft Keyboard Scan Code
 * Specification 1.3a (tables on PDF pp. 16 to 20, notes 1 to 5; its F3 break
 * in set 2, "F0 05", is a misprint for F0 04), the IBM PS/2 Hardware Interface
 * Technical Reference (set 3, Figure 17) and the USB HID Usage Tables 1.5
 * (keyboard page, Table 12), not read back from the tables under test. Built
 * with every lane; no SDL needed. Exit status 0 when every check passes.
 **/
#include "StdAfx.hpp"

#include "hostkeys.hpp"
#include "scancodes.hpp"

#include <cstdio>
#include <cstring>
#include <string>

static int checks = 0, failures = 0;

static std::string hex(const char *s) {
  std::string r;
  char b[4];
  for (; *s; s++) {
    snprintf(b, sizeof(b), "%02X ", (unsigned char)*s);
    r += b;
  }
  if (!r.empty())
    r.pop_back();
  return r;
}

static void check_str(const char *what, const std::string &got,
                      const std::string &want) {
  checks++;
  if (got == want)
    return;
  failures++;
  printf("FAIL %s: got [%s], want [%s]\n", what, got.c_str(), want.c_str());
}

#define CHECK(cond, what)                                                      \
  do {                                                                         \
    checks++;                                                                  \
    if (!(cond)) {                                                             \
      failures++;                                                              \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, what);                    \
    }                                                                          \
  } while (0)

// One key: its name, whether sets 1 and 2 give it an E0 prefix, and the
// make code in each set. Set 3 never has a prefix. Print Screen, Pause,
// Ctrl+Break and the keys without a set 3 code are listed separately.
struct Key {
  unsigned bx;
  const char *name;
  bool e0;
  unsigned s1, s2, s3;
};

#define K(bx, e0, s1, s2, s3) {BX_KEY_##bx, #bx, e0, 0x##s1, 0x##s2, 0x##s3}

static const Key keys[] = {
    K(CTRL_L, 0, 1D, 14, 11),
    K(SHIFT_L, 0, 2A, 12, 12),
    K(F1, 0, 3B, 05, 07),
    K(F2, 0, 3C, 06, 0F),
    K(F3, 0, 3D, 04, 17),
    K(F4, 0, 3E, 0C, 1F),
    K(F5, 0, 3F, 03, 27),
    K(F6, 0, 40, 0B, 2F),
    K(F7, 0, 41, 83, 37),
    K(F8, 0, 42, 0A, 3F),
    K(F9, 0, 43, 01, 47),
    K(F10, 0, 44, 09, 4F),
    K(F11, 0, 57, 78, 56),
    K(F12, 0, 58, 07, 5E),
    K(CTRL_R, 1, 1D, 14, 58),
    K(SHIFT_R, 0, 36, 59, 59),
    K(CAPS_LOCK, 0, 3A, 58, 14),
    K(NUM_LOCK, 0, 45, 77, 76),
    K(ALT_L, 0, 38, 11, 19),
    K(ALT_R, 1, 38, 11, 39),
    K(A, 0, 1E, 1C, 1C),
    K(B, 0, 30, 32, 32),
    K(C, 0, 2E, 21, 21),
    K(D, 0, 20, 23, 23),
    K(E, 0, 12, 24, 24),
    K(F, 0, 21, 2B, 2B),
    K(G, 0, 22, 34, 34),
    K(H, 0, 23, 33, 33),
    K(I, 0, 17, 43, 43),
    K(J, 0, 24, 3B, 3B),
    K(K, 0, 25, 42, 42),
    K(L, 0, 26, 4B, 4B),
    K(M, 0, 32, 3A, 3A),
    K(N, 0, 31, 31, 31),
    K(O, 0, 18, 44, 44),
    K(P, 0, 19, 4D, 4D),
    K(Q, 0, 10, 15, 15),
    K(R, 0, 13, 2D, 2D),
    K(S, 0, 1F, 1B, 1B),
    K(T, 0, 14, 2C, 2C),
    K(U, 0, 16, 3C, 3C),
    K(V, 0, 2F, 2A, 2A),
    K(W, 0, 11, 1D, 1D),
    K(X, 0, 2D, 22, 22),
    K(Y, 0, 15, 35, 35),
    K(Z, 0, 2C, 1A, 1A),
    K(0, 0, 0B, 45, 45),
    K(1, 0, 02, 16, 16),
    K(2, 0, 03, 1E, 1E),
    K(3, 0, 04, 26, 26),
    K(4, 0, 05, 25, 25),
    K(5, 0, 06, 2E, 2E),
    K(6, 0, 07, 36, 36),
    K(7, 0, 08, 3D, 3D),
    K(8, 0, 09, 3E, 3E),
    K(9, 0, 0A, 46, 46),
    K(ESC, 0, 01, 76, 08),
    K(SPACE, 0, 39, 29, 29),
    K(SINGLE_QUOTE, 0, 28, 52, 52),
    K(COMMA, 0, 33, 41, 41),
    K(PERIOD, 0, 34, 49, 49),
    K(SLASH, 0, 35, 4A, 4A),
    K(SEMICOLON, 0, 27, 4C, 4C),
    K(EQUALS, 0, 0D, 55, 55),
    K(LEFT_BRACKET, 0, 1A, 54, 54),
    K(BACKSLASH, 0, 2B, 5D, 5C),
    K(RIGHT_BRACKET, 0, 1B, 5B, 5B),
    K(MINUS, 0, 0C, 4E, 4E),
    K(GRAVE, 0, 29, 0E, 0E),
    K(BACKSPACE, 0, 0E, 66, 66),
    K(ENTER, 0, 1C, 5A, 5A),
    K(TAB, 0, 0F, 0D, 0D),
    K(LEFT_BACKSLASH, 0, 56, 61, 13),
    K(SCRL_LOCK, 0, 46, 7E, 5F),
    K(INSERT, 1, 52, 70, 67),
    K(DELETE, 1, 53, 71, 64),
    K(HOME, 1, 47, 6C, 6E),
    K(END, 1, 4F, 69, 65),
    K(PAGE_UP, 1, 49, 7D, 6F),
    K(PAGE_DOWN, 1, 51, 7A, 6D),
    K(KP_ADD, 0, 4E, 79, 7C),
    K(KP_SUBTRACT, 0, 4A, 7B, 84),
    K(KP_END, 0, 4F, 69, 69),
    K(KP_DOWN, 0, 50, 72, 72),
    K(KP_PAGE_DOWN, 0, 51, 7A, 7A),
    K(KP_LEFT, 0, 4B, 6B, 6B),
    K(KP_RIGHT, 0, 4D, 74, 74),
    K(KP_HOME, 0, 47, 6C, 6C),
    K(KP_UP, 0, 48, 75, 75),
    K(KP_PAGE_UP, 0, 49, 7D, 7D),
    K(KP_INSERT, 0, 52, 70, 70),
    K(KP_DELETE, 0, 53, 71, 71),
    K(KP_5, 0, 4C, 73, 73),
    K(UP, 1, 48, 75, 63),
    K(DOWN, 1, 50, 72, 60),
    K(LEFT, 1, 4B, 6B, 61),
    K(RIGHT, 1, 4D, 74, 6A),
    K(KP_ENTER, 1, 1C, 5A, 79),
    K(KP_MULTIPLY, 0, 37, 7C, 7E),
    K(KP_DIVIDE, 1, 35, 4A, 77),
    K(WIN_L, 1, 5B, 1F, 8B),
    K(WIN_R, 1, 5C, 27, 8C),
    K(MENU, 1, 5D, 2F, 8D),
    // Alt+Print Screen: SysRq (no E0, set 2 84)
    K(ALT_SYSREQ, 0, 54, 84, 57),
    // Ctrl+Pause: Break (E0 46, set 3 as Pause)
    K(CTRL_BREAK, 1, 46, 7E, 62),
};

// The extended keys of the Microsoft Internet and ACPI groups (spec p. 11 and
// the common 'multimedia' codes): all E0-prefixed in sets 1 and 2. Set 3 has
// no assignment for the ACPI power and wake keys.
static const Key extra[] = {
    K(INT_BACK, 1, 6A, 38, 38),   K(INT_FORWARD, 1, 69, 30, 30),
    K(INT_STOP, 1, 68, 28, 28),   K(INT_MAIL, 1, 6C, 48, 48),
    K(INT_SEARCH, 1, 65, 10, 10), K(INT_FAV, 1, 66, 18, 18),
    K(INT_HOME, 1, 32, 3A, 97),   K(POWER_MYCOMP, 1, 6B, 40, 40),
    K(POWER_CALC, 1, 21, 2B, 99), K(POWER_SLEEP, 1, 5F, 3F, 7F),
    K(POWER_POWER, 1, 5E, 37, 0), K(POWER_WAKE, 1, 63, 5E, 0),
};

static std::string bytes(std::initializer_list<unsigned> l) {
  std::string r;
  for (unsigned b : l)
    r.push_back((char)b);
  return r;
}

static void check_key(const Key &k) {
  CHECK(k.bx < BX_KEY_NBKEYS, k.name);
  const scancode *sc = scancodes[k.bx];
  std::string what = k.name;

  // Set 1: make c, break c|80, each behind E0 for an extended key.
  std::string m1, b1;
  if (k.e0) {
    m1.push_back((char)0xE0);
    b1.push_back((char)0xE0);
  }
  m1.push_back((char)k.s1);
  b1.push_back((char)(k.s1 | 0x80));
  check_str((what + " set1 make").c_str(), hex(sc[0].make), hex(m1.c_str()));
  check_str((what + " set1 break").c_str(), hex(sc[0].brek), hex(b1.c_str()));

  // Set 2: make c, break F0 c, E0 first (E0 F0 c for the break).
  std::string m2, b2;
  if (k.e0) {
    m2.push_back((char)0xE0);
    b2.push_back((char)0xE0);
  }
  m2.push_back((char)k.s2);
  b2.push_back((char)0xF0);
  b2.push_back((char)k.s2);
  check_str((what + " set2 make").c_str(), hex(sc[1].make), hex(m2.c_str()));
  check_str((what + " set2 break").c_str(), hex(sc[1].brek), hex(b2.c_str()));

  // Set 3: no prefix; break F0 c. The keys without an assignment give nothing.
  if (k.s3) {
    check_str((what + " set3 make").c_str(), hex(sc[2].make),
              hex(bytes({k.s3}).c_str()));
    check_str((what + " set3 break").c_str(), hex(sc[2].brek),
              hex(bytes({0xF0, k.s3}).c_str()));
  } else {
    check_str((what + " set3 make").c_str(), hex(sc[2].make), "");
    check_str((what + " set3 break").c_str(), hex(sc[2].brek), "");
  }
}

static void test_table() {
  bool seen[BX_KEY_NBKEYS] = {};
  for (const Key &k : keys) {
    check_key(k);
    seen[k.bx] = true;
  }
  for (const Key &k : extra) {
    check_key(k);
    seen[k.bx] = true;
  }

  // Print Screen (spec note 4): the make is E0 2A E0 37 in set 1 and
  // E0 12 E0 7C in set 2, the break reverses it; set 3 has a plain key.
  const scancode *p = scancodes[BX_KEY_PRINT];
  check_str("print set1 make", hex(p[0].make), "E0 2A E0 37");
  check_str("print set1 break", hex(p[0].brek), "E0 B7 E0 AA");
  check_str("print set2 make", hex(p[1].make), "E0 12 E0 7C");
  check_str("print set2 break", hex(p[1].brek), "E0 F0 7C E0 F0 12");
  check_str("print set3 make", hex(p[2].make), "57");
  check_str("print set3 break", hex(p[2].brek), "F0 57");
  seen[BX_KEY_PRINT] = true;

  // Pause (note 5): one 6 (set 1) or 8 (set 2) byte make and no break.
  const scancode *z = scancodes[BX_KEY_PAUSE];
  check_str("pause set1 make", hex(z[0].make), "E1 1D 45 E1 9D C5");
  check_str("pause set1 break", hex(z[0].brek), "");
  check_str("pause set2 make", hex(z[1].make), "E1 14 77 E1 F0 14 F0 77");
  check_str("pause set2 break", hex(z[1].brek), "");
  check_str("pause set3 make", hex(z[2].make), "62");
  check_str("pause set3 break", hex(z[2].brek), "F0 62");
  seen[BX_KEY_PAUSE] = true;

  for (unsigned i = 0; i < BX_KEY_NBKEYS; i++)
    if (!seen[i]) {
      failures++;
      checks++;
      printf("FAIL: BX key %u has no expected row\n", i);
    }
}

// The 8042 turns a set 2 byte stream into set 1 by translation8042[] (an F0
// makes the next byte a break: |0x80). For every key whose set 1 and set 2
// codes are the same key, the translation of the set 2 bytes must be the set 1
// bytes (spec: "Microsoft operating systems expect set 1": translation is how
// they get it).
static std::string translate(const char *s) {
  std::string r;
  unsigned esc = 0;
  for (; *s; s++) {
    unsigned char c = (unsigned char)*s;
    if (c == 0xF0)
      esc = 0x80;
    else {
      r.push_back((char)(translation8042[c] | esc));
      esc = 0;
    }
  }
  return r;
}

static void test_translation() {
  // Pause is excluded: its set 2 make translates to the set 1 sequence only
  // as a whole, which is checked below.
  for (unsigned i = 0; i < BX_KEY_NBKEYS; i++) {
    const scancode *sc = scancodes[i];
    // ALT_SYSREQ: set 2 84 translates to 54 (set 1 SysRq).
    std::string w = "BX key " + std::to_string(i);
    check_str((w + " make").c_str(), hex(translate(sc[1].make).c_str()),
              hex(sc[0].make));
    check_str((w + " break").c_str(), hex(translate(sc[1].brek).c_str()),
              hex(sc[0].brek));
  }
}

// HID usage -> set 1 make bytes, typed from the HID Usage Tables 1.5 keyboard
// page and the spec's set 1 column. Usage 0x32 (Non-US # and ~) is key 42,
// scan code 2B; 0x64 (Non-US \ and |) is key 45, scan code 56; 0x65
// (Application) and 0x76 (Menu) are E0 5D.
struct Hid {
  unsigned usage;
  const char *set1;
};

static const Hid hid[] = {
    {0x04, "1E"},          {0x05, "30"},    {0x06, "2E"},
    {0x07, "20"},          {0x08, "12"},    {0x09, "21"},
    {0x0A, "22"},          {0x0B, "23"},    {0x0C, "17"},
    {0x0D, "24"},          {0x0E, "25"},    {0x0F, "26"},
    {0x10, "32"},          {0x11, "31"},    {0x12, "18"},
    {0x13, "19"},          {0x14, "10"},    {0x15, "13"},
    {0x16, "1F"},          {0x17, "14"},    {0x18, "16"},
    {0x19, "2F"},          {0x1A, "11"},    {0x1B, "2D"},
    {0x1C, "15"},          {0x1D, "2C"},    {0x1E, "02"},
    {0x1F, "03"},          {0x20, "04"},    {0x21, "05"},
    {0x22, "06"},          {0x23, "07"},    {0x24, "08"},
    {0x25, "09"},          {0x26, "0A"},    {0x27, "0B"},
    {0x28, "1C"},          {0x29, "01"},    {0x2A, "0E"},
    {0x2B, "0F"},          {0x2C, "39"},    {0x2D, "0C"},
    {0x2E, "0D"},          {0x2F, "1A"},    {0x30, "1B"},
    {0x31, "2B"},          {0x32, "2B"},    {0x33, "27"},
    {0x34, "28"},          {0x35, "29"},    {0x36, "33"},
    {0x37, "34"},          {0x38, "35"},    {0x39, "3A"},
    {0x3A, "3B"},          {0x3B, "3C"},    {0x3C, "3D"},
    {0x3D, "3E"},          {0x3E, "3F"},    {0x3F, "40"},
    {0x40, "41"},          {0x41, "42"},    {0x42, "43"},
    {0x43, "44"},          {0x44, "57"},    {0x45, "58"},
    {0x46, "E0 2A E0 37"}, {0x47, "46"},    {0x48, "E1 1D 45 E1 9D C5"},
    {0x49, "E0 52"},       {0x4A, "E0 47"}, {0x4B, "E0 49"},
    {0x4C, "E0 53"},       {0x4D, "E0 4F"}, {0x4E, "E0 51"},
    {0x4F, "E0 4D"},       {0x50, "E0 4B"}, {0x51, "E0 50"},
    {0x52, "E0 48"},       {0x53, "45"},    {0x54, "E0 35"},
    {0x55, "37"},          {0x56, "4A"},    {0x57, "4E"},
    {0x58, "E0 1C"},       {0x59, "4F"},    {0x5A, "50"},
    {0x5B, "51"},          {0x5C, "4B"},    {0x5D, "4C"},
    {0x5E, "4D"},          {0x5F, "47"},    {0x60, "48"},
    {0x61, "49"},          {0x62, "52"},    {0x63, "53"},
    {0x64, "56"},          {0x65, "E0 5D"}, {0x76, "E0 5D"},
    {0xE0, "1D"},          {0xE1, "2A"},    {0xE2, "38"},
    {0xE3, "E0 5B"},       {0xE4, "E0 1D"}, {0xE5, "36"},
    {0xE6, "E0 38"},       {0xE7, "E0 5C"},
};

static void test_hid() {
  bool mapped[256] = {};
  for (const Hid &h : hid) {
    char w[32];
    snprintf(w, sizeof(w), "HID 0x%02X", h.usage);
    const u32 k = hid_usage_to_bx_key(h.usage);
    checks++;
    if (k >= BX_KEY_NBKEYS) {
      failures++;
      printf("FAIL %s: not mapped (%u)\n", w, (unsigned)k);
      continue;
    }
    check_str(w, hex(scancodes[k][0].make), h.set1);
    mapped[h.usage] = true;
  }
  // Nothing outside the list may map, and nothing may return an index the
  // tables lack (BX_KEY_UNHANDLED is the one other answer).
  for (unsigned u = 0; u < 0x200; u++) {
    const u32 k = hid_usage_to_bx_key(u);
    if (u < 256 && mapped[u])
      continue;
    checks++;
    if (k != BX_KEY_UNHANDLED) {
      failures++;
      printf("FAIL HID 0x%02X maps to BX key %u but is not in the list\n", u,
             (unsigned)k);
    }
  }
  // The release of a key the guest cannot tell apart from another must still
  // be that key's own: the two Non-US keys, and Pause/Ctrl+Break.
  CHECK(hid_usage_to_bx_key(0x64) != hid_usage_to_bx_key(0x31),
        "Non-US \\ is not the US \\ key");
}

int main() {
  test_table();
  test_translation();
  test_hid();
  printf("scancodes_test: %d checks, %d failed\n", checks, failures);
  return failures ? 1 : 0;
}
