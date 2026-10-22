/* Alphabox Alpha Emulator
 * Copyright (C) 2020 Tomáš Glozar
 * Website: https://github.com/lenticularis39/axpbox
 *
 * Forked from: ES40 emulator
 * Copyright (C) 2007-2008 by the ES40 Emulator Project
 * Copyright (C) 2007 by Camiel Vanderhoeven
 */

/**
 * \file
 * The host keyboard's physical keys as the guest keyboard's BX_KEY_* codes.
 * No SDL here, so the table can be tested alone: SDL3's scancodes are the USB
 * HID usage IDs of the keyboard page (HID Usage Tables 1.5, Table 12) for
 * 0x00 to 0xE7, and sdl.cpp checks that with static_asserts.
 **/
#ifndef ALPHABOX_HOSTKEYS_HPP
#define ALPHABOX_HOSTKEYS_HPP

#include "gui.hpp"

/// The BX_KEY_* code of the key with this HID keyboard-page usage, or
/// BX_KEY_UNHANDLED when the guest keyboards have no such key.
u32 hid_usage_to_bx_key(unsigned usage);

#endif
