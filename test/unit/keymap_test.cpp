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
 * Unit test of the keymap file loader (src/gui/keymap.cpp, from Bochs): the
 * entries it reads, and that a wrong line, an over-long line, a missing file
 * or a file with nothing usable in it is reported and skipped, never used to
 * build an entry or to end the load early. Also that the BX key names the
 * loader knows match the BX_KEY_* numbers (the scan code tables are indexed
 * by those). Built with every lane; no SDL needed. Exit status 0 when every
 * check passes.
 **/
#include "StdAfx.hpp"

#include "gui.hpp"
#include "keymap.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>

extern const char *bx_key_symbol[BX_KEY_NBKEYS];

// The one-argument loadKeymap asks the configuration; the test calls the
// two-argument one, so these only satisfy the linker.
char *CConfigurator::get_text_value(const char *, const char *def) {
  return (char *)def;
}
bool CConfigurator::get_bool_value(const char *, bool def) { return def; }

static int checks = 0, failures = 0;

#define CHECK_EQ(got, want)                                                    \
  do {                                                                         \
    const long g_ = (long)(got), w_ = (long)(want);                            \
    checks++;                                                                  \
    if (g_ != w_) {                                                            \
      failures++;                                                              \
      printf("FAIL %s:%d: %s = %ld, want %ld\n", __FILE__, __LINE__, #got, g_, \
             w_);                                                              \
    }                                                                          \
  } while (0)

// A host key name set that is not SDL's: K_A, K_B, K_SEMI.
static u32 host_names(const char *name) {
  if (!strcmp(name, "K_A"))
    return 0x61;
  if (!strcmp(name, "K_B"))
    return 0x62;
  if (!strcmp(name, "K_SEMI"))
    return 0x3b;
  return BX_KEYMAP_UNKNOWN;
}

static std::string tmpfile_with(const std::string &text) {
  char path[] = "/tmp/keymap_test_XXXXXX";
  int fd = mkstemp(path);
  if (fd < 0)
    return "";
  (void)!write(fd, text.data(), text.size());
  close(fd);
  return path;
}

static void test_names() {
  // Every BX_KEY the scan code tables know has its name in the loader's
  // table, in order: a keymap file names keys by these.
  CHECK_EQ(strcmp(bx_key_symbol[BX_KEY_CTRL_L], "BX_KEY_CTRL_L"), 0);
  CHECK_EQ(strcmp(bx_key_symbol[BX_KEY_A], "BX_KEY_A"), 0);
  CHECK_EQ(strcmp(bx_key_symbol[BX_KEY_BACKSLASH], "BX_KEY_BACKSLASH"), 0);
  CHECK_EQ(
      strcmp(bx_key_symbol[BX_KEY_LEFT_BACKSLASH], "BX_KEY_LEFT_BACKSLASH"), 0);
  CHECK_EQ(strcmp(bx_key_symbol[BX_KEY_MENU], "BX_KEY_MENU"), 0);
  CHECK_EQ(strcmp(bx_key_symbol[BX_KEY_CTRL_BREAK], "BX_KEY_CTRL_BREAK"), 0);
  CHECK_EQ(strcmp(bx_key_symbol[BX_KEY_POWER_WAKE], "BX_KEY_POWER_WAKE"), 0);
  for (int i = 0; i < BX_KEY_NBKEYS; i++) {
    checks++;
    if (!bx_key_symbol[i] || strncmp(bx_key_symbol[i], "BX_KEY_", 7)) {
      failures++;
      printf("FAIL: bx_key_symbol[%d] is not a BX_KEY_ name\n", i);
    }
  }
}

static void test_load() {
  const std::string f = tmpfile_with("# a keymap\n"
                                     "\n"
                                     "BX_KEY_A  'a'  K_A  # trailing comment\n"
                                     "BX_KEY_A+BX_KEY_SHIFT_L 'A' K_A\n"
                                     "BX_KEY_SEMICOLON ';' K_SEMI\n"
                                     "BX_KEY_SPACE space K_B\n");
  bx_keymap_c km(nullptr);
  km.loadKeymap(host_names, f.c_str());
  unlink(f.c_str());
  CHECK_EQ(km.isKeymapLoaded(), 1);
  BXKeyEntry *e = km.findHostKey(0x3b);
  CHECK_EQ(e != nullptr, 1);
  if (e) {
    CHECK_EQ(e->baseKey, BX_KEY_SEMICOLON);
    CHECK_EQ(e->ascii, ';');
    CHECK_EQ(e->modKey, BX_KEYMAP_UNKNOWN);
  }
  e = km.findHostKey(0x62);
  CHECK_EQ(e != nullptr, 1);
  if (e) {
    CHECK_EQ(e->baseKey, BX_KEY_SPACE);
    CHECK_EQ(e->ascii, ' ');
  }
  CHECK_EQ(km.findHostKey(0x7a) == nullptr, 1);
}

// One wrong line must cost that line only: the entries after it load, and the
// wrong line leaves no entry (nor reuses the ascii of the line before it).
static void test_bad_lines() {
  std::string text = "BX_KEY_B 'b' K_B\n"
                     "BX_KEY_NOSUCH 'x' K_A\n" // unknown BX key
                     "BX_KEY_C 'c' K_NOSUCH\n" // unknown host key
                     "BX_KEY_D\n"              // one column
                     "BX_KEY_D 'd'\n"          // two columns
                     "BX_KEY_D dee K_A\n"      // ascii not valid
                     "BX_KEY_D ' K_A\n"        // a lone quote
                     "BX_KEY_SEMICOLON ';' K_SEMI\n";
  // a line of 400 characters, then a good one
  text += "BX_KEY_E 'e' K_A " + std::string(400, 'x') + "\n";
  text += "BX_KEY_A 'a' K_A\n";
  const std::string f = tmpfile_with(text);
  bx_keymap_c km(nullptr);
  km.loadKeymap(host_names, f.c_str());
  unlink(f.c_str());
  CHECK_EQ(km.findHostKey(0x62) != nullptr, 1); // the first line
  CHECK_EQ(km.findHostKey(0x3b) != nullptr, 1); // after the wrong ones
  BXKeyEntry *e = km.findHostKey(0x61);         // the one after the long line
  CHECK_EQ(e != nullptr, 1);
  if (e)
    CHECK_EQ(e->baseKey, BX_KEY_A); // not the long line's BX_KEY_E
}

static void test_empty_and_missing() {
  bx_keymap_c a(nullptr);
  a.loadKeymap(host_names, "/nonexistent/keymap");
  CHECK_EQ(a.isKeymapLoaded(), 0);

  const std::string f =
      tmpfile_with("# only a comment\nBX_KEY_NOSUCH 'x' K_A\n");
  bx_keymap_c b(nullptr);
  b.loadKeymap(host_names, f.c_str());
  unlink(f.c_str());
  CHECK_EQ(b.isKeymapLoaded(), 0);

  // a file whose last line has no newline
  const std::string g = tmpfile_with("BX_KEY_B 'b' K_B");
  bx_keymap_c c(nullptr);
  c.loadKeymap(host_names, g.c_str());
  unlink(g.c_str());
  CHECK_EQ(c.findHostKey(0x62) != nullptr, 1);
}

int main() {
  test_names();
  test_load();
  test_bad_lines();
  test_empty_and_missing();
  printf("keymap_test: %d checks, %d failed\n", checks, failures);
  return failures ? 1 : 0;
}
