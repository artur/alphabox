/* Alphabox Alpha Emulator
 * Copyright (C) 2020 Tomáš Glozar
 * Website: https://github.com/lenticularis39/axpbox
 *
 * Forked from: ES40 emulator
 * Copyright (C) 2007-2008 by the ES40 Emulator Project
 * Copyright (C) 2007 by Camiel Vanderhoeven
 *
 *  This file is based upon Bochs.
 *
 *  Copyright (C) 2002  MandrakeSoft S.A.
 *
 *    MandrakeSoft S.A.
 *    43, rue d'Aboukir
 *    75002 Paris - France
 *    http://www.linux-mandrake.com/
 *    http://www.mandrakesoft.com/
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 */

/**
 * \file
 * Contains code for the bx_gui_c base class used for interfacing with
 * SDL and other device interfaces.
 **/

#include "StdAfx.hpp"

#include <chrono>
#include <string>

#include "gui.hpp"

#include "Keyboard.hpp"
#include "UsbKeyboard.hpp"
#include "scancodes.hpp"

bx_gui_c *bx_gui = NULL;

// ALPHABOX_TRACE_GUILOCK threshold in ms (0: off), and the thread the GUI
// was created on, which the trace calls "main".
static long gui_lock_trace_ms = 0;
static std::thread::id gui_main_thread;

bx_gui_c::bx_gui_c(void) {
  gui_main_thread = std::this_thread::get_id();
  if (const char *t = getenv("ALPHABOX_TRACE_GUILOCK")) {
    const long v = atol(t);
    gui_lock_trace_ms = v == 1 ? 1000 : v > 0 ? v : 0;
    if (gui_lock_trace_ms)
      printf("%%GUI-I-TRACELOCK: reporting gui-lock waits and holds of %ld ms "
             "or more.\n",
             gui_lock_trace_ms);
  }
}

bx_gui_c::~bx_gui_c() {}

void bx_gui_c::init(unsigned tilewidth, unsigned tileheight) {
  specific_init(tilewidth, tileheight);
  charmap_updated = 0;
}

// ALPHABOX_TRACE_KEYS=1: every key the GUI hands the guest, as the BX_KEY
// number and the set 1 bytes of the make or break.
static const bool s_trace_keys = getenv("ALPHABOX_TRACE_KEYS") != nullptr;

void gui_guest_key(u32 key_event) {
  if (s_trace_keys) {
    const u32 k = key_event & ~(u32)BX_KEY_RELEASED;
    std::string bytes;
    if (k < BX_KEY_NBKEYS) {
      const char *p = (key_event & BX_KEY_RELEASED) ? scancodes[k][0].brek
                                                    : scancodes[k][0].make;
      char h[4];
      for (; *p; p++) {
        snprintf(h, sizeof(h), "%02X ", (unsigned char)*p);
        bytes += h;
      }
    }
    printf("%%GUI-I-KEY: %s %u set1 [%s]\n",
           (key_event & BX_KEY_RELEASED) ? "up" : "down", (unsigned)k,
           bytes.c_str());
  }
  if (theKeyboard)
    theKeyboard->gen_scancode(key_event);
  if (CUsbKeyboard *k = theUsbKeyboard.load())
    k->key(key_event);
}

void gui_guest_mouse(int dx, int dy, int dz, unsigned buttons) {
  if (s_trace_keys)
    printf("%%GUI-I-MOUSE: dx %d dy %d dz %d buttons %02x\n", dx, dy, dz,
           buttons);
  if (theKeyboard)
    theKeyboard->mouse_motion(dx, dy, dz, buttons);
}

bool gui_guest_has_keyboard() { return theKeyboard || theUsbKeyboard.load(); }

void bx_gui_c::mouse_enabled_changed(bool val) {
  if (theKeyboard) {
    // The controller drops mouse input once the capture is off, so release
    // the buttons first: a button held when the grab ends (the hotkey, a
    // focus loss) would otherwise stay down in the guest, whose release the
    // host sends nowhere.
    if (!val)
      gui_guest_mouse(0, 0, 0, 0);
    theKeyboard->set_mouse_capture(val);
  }

  // This is only called when SIM->get_init_done is 1.  Note that VAL
  // is the new value of mouse_enabled, which may not match the old
  // value which is still in SIM->get_param_bool(BXPN_MOUSE_ENABLED)->get().
  bx_gui->mouse_enabled_changed_specific(val);
}

void bx_gui_c::set_text_charmap(u8 *fbuffer) {
  memcpy(&bx_gui->vga_charmap, fbuffer, 0x2000);
  for (unsigned i = 0; i < 256; i++)
    bx_gui->char_changed[i] = 1;
  bx_gui->charmap_updated = 1;
}

static int64_t gui_now_us() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

static const char *gui_site_name(const char *file) {
  if (!file)
    return "?";
  const char *slash = strrchr(file, '/');
  const char *bslash = strrchr(file, '\\');
  if (bslash && (!slash || bslash > slash))
    slash = bslash;
  return slash ? slash + 1 : file;
}

// "main" for the thread that created the GUI, else a number per thread.
static std::string gui_thread_name(std::thread::id id) {
  if (id == gui_main_thread)
    return "main";
  if (id == std::thread::id())
    return "none";
  char buf[32];
  snprintf(buf, sizeof(buf), "%08zx",
           (size_t)std::hash<std::thread::id>()(id) & 0xffffffff);
  return buf;
}

void bx_gui_c::lock(const char *file, int line) {
  if (!gui_lock_trace_ms) {
    guiMutex.lock();
    return;
  }

  // Traced: wait in steps of the threshold and name the holder each time a
  // step runs out, so a lock that is never released (a deadlock) keeps
  // saying who has it. The wait itself never gives up.
  const int64_t t0 = gui_now_us();
  while (!guiMutex.try_lock_for(std::chrono::milliseconds(gui_lock_trace_ms))) {
    const int64_t now = gui_now_us();
    const int64_t since = gui_lock_since_us.load();
    printf("%%GUI-W-LOCKWAIT: thread %s at %s:%d has waited %lld ms; held by "
           "thread %s from %s:%d for %lld ms\n",
           gui_thread_name(std::this_thread::get_id()).c_str(),
           gui_site_name(file), line, (long long)(now - t0) / 1000,
           gui_thread_name(gui_lock_owner.load()).c_str(),
           gui_site_name(gui_lock_file.load()), gui_lock_line.load(),
           since ? (long long)(now - since) / 1000 : -1LL);
    fflush(stdout);
  }
  const int64_t now = gui_now_us();
  if (now - t0 >= gui_lock_trace_ms * 1000) {
    printf("%%GUI-W-LOCKWAIT: thread %s at %s:%d got the lock after %lld ms\n",
           gui_thread_name(std::this_thread::get_id()).c_str(),
           gui_site_name(file), line, (long long)(now - t0) / 1000);
    fflush(stdout);
  }
  if (gui_lock_depth++ == 0) {
    gui_lock_file = file;
    gui_lock_line = line;
    gui_lock_owner = std::this_thread::get_id();
    gui_lock_since_us = now;
  }
}

void bx_gui_c::unlock() {
  if (gui_lock_trace_ms && gui_lock_depth > 0 && --gui_lock_depth == 0) {
    const int64_t held = gui_now_us() - gui_lock_since_us.load();
    if (held >= gui_lock_trace_ms * 1000) {
      printf("%%GUI-W-LOCKHOLD: thread %s held the gui lock from %s:%d for "
             "%lld ms\n",
             gui_thread_name(std::this_thread::get_id()).c_str(),
             gui_site_name(gui_lock_file.load()), gui_lock_line.load(),
             (long long)held / 1000);
      fflush(stdout);
    }
    gui_lock_since_us = 0;
    gui_lock_owner = std::thread::id();
    gui_lock_file = nullptr;
  }
  guiMutex.unlock();
}
