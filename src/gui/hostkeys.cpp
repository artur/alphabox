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
 * HID keyboard-page usage to BX_KEY_* (see hostkeys.hpp).
 **/
#include "StdAfx.hpp"

#include "hostkeys.hpp"

u32 hid_usage_to_bx_key(unsigned usage) {
  // 0x04 to 0x1D: A to Z.
  static const u8 letters[26] = {
      BX_KEY_A, BX_KEY_B, BX_KEY_C, BX_KEY_D, BX_KEY_E, BX_KEY_F, BX_KEY_G,
      BX_KEY_H, BX_KEY_I, BX_KEY_J, BX_KEY_K, BX_KEY_L, BX_KEY_M, BX_KEY_N,
      BX_KEY_O, BX_KEY_P, BX_KEY_Q, BX_KEY_R, BX_KEY_S, BX_KEY_T, BX_KEY_U,
      BX_KEY_V, BX_KEY_W, BX_KEY_X, BX_KEY_Y, BX_KEY_Z};
  // 0x1E to 0x27: 1 to 9, 0.
  static const u8 digits[10] = {BX_KEY_1, BX_KEY_2, BX_KEY_3, BX_KEY_4,
                                BX_KEY_5, BX_KEY_6, BX_KEY_7, BX_KEY_8,
                                BX_KEY_9, BX_KEY_0};
  // 0x3A to 0x45: F1 to F12.
  static const u8 fkeys[12] = {BX_KEY_F1, BX_KEY_F2,  BX_KEY_F3,  BX_KEY_F4,
                               BX_KEY_F5, BX_KEY_F6,  BX_KEY_F7,  BX_KEY_F8,
                               BX_KEY_F9, BX_KEY_F10, BX_KEY_F11, BX_KEY_F12};
  // 0x59 to 0x63: keypad 1 to 9, 0, '.'.
  static const u8 keypad[11] = {
      BX_KEY_KP_END,     BX_KEY_KP_DOWN,   BX_KEY_KP_PAGE_DOWN, BX_KEY_KP_LEFT,
      BX_KEY_KP_5,       BX_KEY_KP_RIGHT,  BX_KEY_KP_HOME,      BX_KEY_KP_UP,
      BX_KEY_KP_PAGE_UP, BX_KEY_KP_INSERT, BX_KEY_KP_DELETE};

  if (usage >= 0x04 && usage <= 0x1D)
    return letters[usage - 0x04];
  if (usage >= 0x1E && usage <= 0x27)
    return digits[usage - 0x1E];
  if (usage >= 0x3A && usage <= 0x45)
    return fkeys[usage - 0x3A];
  if (usage >= 0x59 && usage <= 0x63)
    return keypad[usage - 0x59];

  switch (usage) {
  case 0x28:
    return BX_KEY_ENTER;
  case 0x29:
    return BX_KEY_ESC;
  case 0x2A:
    return BX_KEY_BACKSPACE;
  case 0x2B:
    return BX_KEY_TAB;
  case 0x2C:
    return BX_KEY_SPACE;
  case 0x2D:
    return BX_KEY_MINUS;
  case 0x2E:
    return BX_KEY_EQUALS;
  case 0x2F:
    return BX_KEY_LEFT_BRACKET;
  case 0x30:
    return BX_KEY_RIGHT_BRACKET;
  case 0x31: // \ and | (US)
  case 0x32: // Non-US # and ~: the same position (key 42) on a 102-key
             // keyboard, scan code 2B, which has no \ key of its own
    return BX_KEY_BACKSLASH;
  case 0x33:
    return BX_KEY_SEMICOLON;
  case 0x34:
    return BX_KEY_SINGLE_QUOTE;
  case 0x35:
    return BX_KEY_GRAVE;
  case 0x36:
    return BX_KEY_COMMA;
  case 0x37:
    return BX_KEY_PERIOD;
  case 0x38:
    return BX_KEY_SLASH;
  case 0x39:
    return BX_KEY_CAPS_LOCK;
  case 0x46:
    return BX_KEY_PRINT;
  case 0x47:
    return BX_KEY_SCRL_LOCK;
  case 0x48:
    return BX_KEY_PAUSE;
  case 0x49:
    return BX_KEY_INSERT;
  case 0x4A:
    return BX_KEY_HOME;
  case 0x4B:
    return BX_KEY_PAGE_UP;
  case 0x4C:
    return BX_KEY_DELETE;
  case 0x4D:
    return BX_KEY_END;
  case 0x4E:
    return BX_KEY_PAGE_DOWN;
  case 0x4F:
    return BX_KEY_RIGHT;
  case 0x50:
    return BX_KEY_LEFT;
  case 0x51:
    return BX_KEY_DOWN;
  case 0x52:
    return BX_KEY_UP;
  case 0x53:
    return BX_KEY_NUM_LOCK;
  case 0x54:
    return BX_KEY_KP_DIVIDE;
  case 0x55:
    return BX_KEY_KP_MULTIPLY;
  case 0x56:
    return BX_KEY_KP_SUBTRACT;
  case 0x57:
    return BX_KEY_KP_ADD;
  case 0x58:
    return BX_KEY_KP_ENTER;
  case 0x64: // Non-US \ and |: key 45, the 102nd key left of Z (scan code 56)
    return BX_KEY_LEFT_BACKSLASH;
  case 0x65: // Application: the context-menu key (E0 5D)
  case 0x76: // Menu
    return BX_KEY_MENU;
  case 0xE0:
    return BX_KEY_CTRL_L;
  case 0xE1:
    return BX_KEY_SHIFT_L;
  case 0xE2:
    return BX_KEY_ALT_L;
  case 0xE3:
    return BX_KEY_WIN_L;
  case 0xE4:
    return BX_KEY_CTRL_R;
  case 0xE5:
    return BX_KEY_SHIFT_R;
  case 0xE6:
    return BX_KEY_ALT_R;
  case 0xE7:
    return BX_KEY_WIN_R;
  default:
    return BX_KEY_UNHANDLED;
  }
}
