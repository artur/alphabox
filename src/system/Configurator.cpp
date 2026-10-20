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
 * Contains the code for the configuration file interpreter.
 **/
#include "Configurator.hpp"
#include "AliM1543C.hpp"
#include "AliM1543C_ide.hpp"
#include "AliM1543C_pmu.hpp"
#include "AliM1543C_usb.hpp"
#include "AlphaCPU.hpp"
#include "CirrusGD5430.hpp"
#include "CirrusGD5434.hpp"
#include "Cmd649.hpp"
#include "DMA.hpp"
#include "DiskDevice.hpp"
#include "DiskFile.hpp"
#include "DiskRam.hpp"
#include "Ehci.hpp"
#include "FloppyController.hpp"
#include "Keyboard.hpp"
#include "Mach64.hpp"
#include "Permedia2.hpp"
#include "Port80.hpp"
#include "Radeon.hpp"
#include "S3Trio64.hpp"
#include "S3Virge.hpp"
#include "Serial.hpp"
#include "StdAfx.hpp"
#include "System.hpp"
#include "Tga.hpp"
#include "VirtioBlk.hpp"
#include "VirtioNet.hpp"
#include "gui/plugin.hpp"
#if defined(HAVE_PCAP) || defined(__linux__)
#include "Tulip.hpp"
#endif
#include "ES137x.hpp"
#include "I8255x.hpp"
#include "Isp1040.hpp"
#include "PCIBridge.hpp"
#include "Sym53C8xx.hpp"

/**
 * Check mode (Configurator.hpp): what initialize() keeps in place of the
 * devices it does not construct.
 **/
static bool s_check = false;
static int s_check_errors = 0;
static int s_check_warnings = 0;
static bool s_check_gui = false; ///< a gui section was seen
static int s_check_cpus = 0;     ///< processors in the machine section
static const platform_config *s_check_platform = nullptr;

void CConfigurator::set_check_mode(bool on) {
  s_check = on;
  s_check_errors = 0;
  s_check_warnings = 0;
  s_check_gui = false;
  s_check_cpus = 0;
  s_check_platform = nullptr;
}

int CConfigurator::check_errors() { return s_check_errors; }
int CConfigurator::check_warnings() { return s_check_warnings; }
bool CConfigurator::check_found_system() { return s_check_platform != nullptr; }

/**
 * One of the configurator's messages: on stdout, or in check mode on
 * stderr, where stdout is the dump. Check mode counts the warnings.
 **/
static void cfg_msg(bool warning, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vfprintf(s_check ? stderr : stdout, fmt, ap);
  va_end(ap);
  if (s_check && warning)
    s_check_warnings++;
}

/// The configuration file's name, for messages: one per process.
static std::string s_source;

/// "file:line:col: ", or "" for what has no place in the file (line 0).
static std::string place_of(int line, int col) {
  if (!line)
    return std::string();
  return s_source + ":" + std::to_string(line) + ":" + std::to_string(col) +
         ": ";
}

/**
 * A configuration error that already says where in the file it is: the
 * sections it passes through on its way out leave it as it is.
 **/
class CPlacedConfigurationException : public CConfigurationException {
public:
  using CConfigurationException::CConfigurationException;
};

/**
 * The configuration language's tokens: names and unquoted values (words),
 * quoted values (strings), and = ; { }.
 **/
struct CConfigToken {
  enum Kind { WORD, STRING, EQUALS, SEMICOLON, OPEN, CLOSE, END } kind;
  std::string text; ///< a word's or a string's text, unquoted
  int line;
  int col;
};

/**
 * Splits a configuration into tokens, skipping white space and comments,
 * and knows where each one is. The language:
 *
 *  - `// ...` and, when it is the first thing on its line, `; ...` comment
 *    out the rest of the line; `/ * ... * /` (without the spaces) a block;
 *  - a word is letters, digits, '_' and '.': a name, a class or a value;
 *  - a quoted value is "...", on one line, with "" for a quote in it;
 *  - anything else is refused, with its line and column.
 *
 * A file may start with a UTF-8 byte order mark, which is skipped.
 **/
class CConfigLexer {
public:
  /// `source` names the file in messages; nullptr for text the emulator
  /// wrote itself, whose tokens get no place (line 0).
  CConfigLexer(const char *text, size_t len, const char *source)
      : p(text), end(text + len), placed(source != nullptr) {
    if (placed && len >= 3 && (u8)text[0] == 0xef && (u8)text[1] == 0xbb &&
        (u8)text[2] == 0xbf)
      p += 3;
  }

  CConfigToken next();

  /// Refuse the configuration: "file:line:col: message".
  [[noreturn]] void fail(int at_line, int at_col, const char *fmt, ...);
  [[noreturn]] void fail(const CConfigToken &t, const char *fmt, ...);

  /// A token as an error message names it.
  static std::string describe(const CConfigToken &t);

private:
  CConfigToken token(CConfigToken::Kind kind, int l, int c,
                     std::string text = std::string()) {
    return CConfigToken{kind, std::move(text), placed ? l : 0, placed ? c : 0};
  }
  void advance() {
    if (*p == '\n') {
      line++;
      col = 1;
      line_has_token = false;
    } else
      col++;
    p++;
  }
  [[noreturn]] void vfail(int at_line, int at_col, const char *fmt, va_list ap);

  const char *p;
  const char *end;
  bool placed;
  int line = 1;
  int col = 1;
  bool line_has_token = false; ///< a ';' after one ends a value
};

static bool is_word_char(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_' || c == '.';
}

CConfigToken CConfigLexer::next() {
  for (;;) {
    if (p >= end)
      return token(CConfigToken::END, line, col);
    const char c = *p;
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' ||
        c == '\v') {
      advance();
      continue;
    }
    const bool second_slash = c == '/' && p + 1 < end && p[1] == '/';
    // A line comment, "//" anywhere or ";" first on the line: the rest of
    // the line (CR or LF ends it).
    if (second_slash || (c == ';' && !line_has_token)) {
      while (p < end && *p != '\n' && *p != '\r')
        advance();
      continue;
    }
    if (c == '/' && p + 1 < end && p[1] == '*') {
      const int l = line, cc = col;
      advance();
      advance();
      while (p < end && !(*p == '*' && p + 1 < end && p[1] == '/'))
        advance();
      if (p >= end)
        fail(l, cc, "the comment opened here is not closed (no */)");
      advance();
      advance();
      continue;
    }

    const int l = line, cc = col;
    line_has_token = true;
    switch (c) {
    case '=':
      advance();
      return token(CConfigToken::EQUALS, l, cc);
    case ';':
      advance();
      return token(CConfigToken::SEMICOLON, l, cc);
    case '{':
      advance();
      return token(CConfigToken::OPEN, l, cc);
    case '}':
      advance();
      return token(CConfigToken::CLOSE, l, cc);
    case '"': {
      std::string s;
      advance();
      for (;;) {
        if (p >= end)
          fail(l, cc, "the quoted value opened here is not closed");
        if (*p == '\n' || *p == '\r')
          fail(l, cc,
               "the quoted value opened here does not end on its line "
               "(multi-line strings are forbidden)");
        if (*p == 0)
          fail(line, col, "Illegal character NUL in a quoted value");
        if (*p == '"') {
          advance();
          if (p < end && *p == '"') {
            s += '"';
            advance();
            continue;
          }
          return token(CConfigToken::STRING, l, cc, s);
        }
        s += *p;
        advance();
      }
    }
    default:
      if (is_word_char(c)) {
        const char *start = p;
        while (p < end && is_word_char(*p))
          advance();
        return token(CConfigToken::WORD, l, cc, std::string(start, p));
      }
      if (c == '/')
        fail(l, cc,
             "Illegal character '/' (a value with one must be quoted; "
             "comments are // and /* */)");
      if ((u8)c >= 0x20 && (u8)c < 0x7f)
        fail(l, cc, "Illegal character '%c' (a value with one must be quoted)",
             c);
      fail(l, cc, "Illegal character 0x%02x", (unsigned)(u8)c);
    }
  }
}

void CConfigLexer::vfail(int at_line, int at_col, const char *fmt, va_list ap) {
  char msg[512];
  vsnprintf(msg, sizeof(msg), fmt, ap);
  throw CPlacedConfigurationException(
      (placed ? place_of(at_line, at_col) : std::string()) + msg,
      "the configuration parser");
}

void CConfigLexer::fail(int at_line, int at_col, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vfail(at_line, at_col, fmt, ap);
}

void CConfigLexer::fail(const CConfigToken &t, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vfail(t.line, t.col, fmt, ap);
}

std::string CConfigLexer::describe(const CConfigToken &t) {
  std::string s = t.text.size() > 40 ? t.text.substr(0, 40) + "..." : t.text;
  switch (t.kind) {
  case CConfigToken::WORD:
    return "'" + s + "'";
  case CConfigToken::STRING:
    return "the quoted value \"" + s + "\"";
  case CConfigToken::EQUALS:
    return "'='";
  case CConfigToken::SEMICOLON:
    return "';'";
  case CConfigToken::OPEN:
    return "'{'";
  case CConfigToken::CLOSE:
    return "'}'";
  default:
    return "the end of the file";
  }
}

/**
 * Parse a section's body (the master's: the whole file) up to its closing
 * brace. Each statement is `name = value;` or `name = class { body }`; a
 * ';' on its own is allowed (after a '}', say). `depth` is how deep this
 * section is (the master: 0).
 **/
void CConfigurator::parse(CConfigLexer &lex, int depth) {
  for (;;) {
    const CConfigToken t = lex.next();
    if (t.kind == CConfigToken::END) {
      if (pParent && !myAdded)
        lex.fail(myLine, myCol,
                 "the section %s (%s) is not closed: the file ends before "
                 "its '}'",
                 myName, myValue);
      return;
    }
    if (t.kind == CConfigToken::CLOSE) {
      if (!pParent)
        lex.fail(t, "'}' closes no section");
      return;
    }
    if (t.kind == CConfigToken::SEMICOLON)
      continue;
    if (t.kind != CConfigToken::WORD)
      lex.fail(t, "expected a name, found %s",
               CConfigLexer::describe(t).c_str());
    if (t.text.size() > CFG_MAX_NAME)
      lex.fail(t, "a name longer than %d characters", CFG_MAX_NAME);

    const CConfigToken eq = lex.next();
    if (eq.kind != CConfigToken::EQUALS)
      lex.fail(eq, "expected '=' after %s, found %s", t.text.c_str(),
               CConfigLexer::describe(eq).c_str());

    const CConfigToken v = lex.next();
    if (v.kind != CConfigToken::WORD && v.kind != CConfigToken::STRING)
      lex.fail(v, "expected a value or a class after '%s =', found %s",
               t.text.c_str(), CConfigLexer::describe(v).c_str());

    const CConfigToken after = lex.next();
    if (after.kind == CConfigToken::SEMICOLON) {
      add_value(strdup(t.text.c_str()), strdup(v.text.c_str()), t.line, t.col);
      continue;
    }
    if (after.kind != CConfigToken::OPEN)
      lex.fail(after, "expected ';' after the value of %s, found %s%s",
               t.text.c_str(), CConfigLexer::describe(after).c_str(),
               v.kind == CConfigToken::WORD && after.kind == CConfigToken::WORD
                   ? " (a value with spaces must be quoted)"
                   : "");

    // A section: `name = class {`.
    if (v.text.size() > CFG_MAX_NAME)
      lex.fail(v, "a class name longer than %d characters", CFG_MAX_NAME);
    if (depth + 1 > CFG_MAX_DEPTH)
      lex.fail(t, "sections nested more than %d deep", CFG_MAX_DEPTH);
    for (CConfigurator *c : pChildren) {
      if (!strcmp(c->myName, t.text.c_str())) {
        cfg_msg(true,
                "%%SYS-W-DUPSECTION: %s\"%s\" is defined more than once "
                "in the configuration file (first at line %d). Each "
                "definition creates its own device (a later telnet serial "
                "section overrides an earlier null_attach one, etc.) -- "
                "remove the duplicate.\n",
                place_of(t.line, t.col).c_str(), t.text.c_str(), c->myLine);
        break;
      }
    }
    CConfigurator *child = new CConfigurator(
        this, strdup(t.text.c_str()), strdup(v.text.c_str()), t.line, t.col);
    pChildren.push_back(child);
    child->parse(lex, depth + 1);
  }
}

/**
 * Constructor of the master configurator (no parent): parse the whole
 * configuration, then initialize the sections it holds, which instantiate
 * their devices and initialize their own children in turn. With a parent:
 * a section the emulator adds (initialize()), whose body is `text`.
 **/
CConfigurator::CConfigurator(class CConfigurator *parent, char *name,
                             char *value, const char *text, size_t textlen,
                             const char *source)
    : CConfigurator(parent, name, value, 0, 0) {
  myAdded = parent != nullptr;
  if (!parent)
    s_source = source ? source : "configuration";
  CConfigLexer lex(text, textlen, parent ? nullptr : s_source.c_str());
  parse(lex, 0);

  if (parent == 0) {
    for (size_t i = 0; i < pChildren.size(); i++)
      initialize_child(pChildren[i]);
  }
}

/**
 * A section the parser found, its name at line:col.
 **/
CConfigurator::CConfigurator(class CConfigurator *parent, char *name,
                             char *value, int line, int col)
    : pParent(parent), myName(name), myValue(value), myDevice(nullptr),
      myClassId(c_none), myFlags(0), myAdded(false), myLine(line), myCol(col) {}

/**
 * Destructor.
 *
 * Nothing is freed: devices keep pointers to names and values for as long
 * as the process runs.
 **/
CConfigurator::~CConfigurator(void) {}

std::string CConfigurator::place() const { return place_of(myLine, myCol); }

/**
 * Add a value to our list of values.
 **/
void CConfigurator::add_value(char *n, char *v, int line, int col) {
  pValues.push_back(SCfg_Value{n, v, line, col});
}

/**
 * Return a text value, if the name of the value can't be found in
 * our list of values, return def.
 **/
char *CConfigurator::get_text_value(const char *n, const char *def) {
  for (const SCfg_Value &v : pValues) {
    if (!strcmp(v.name, n))
      return v.value;
  }

  return (char *)def;
}

/**
 * Return a boolean value, if the name of the value can't be found in
 * our list of values, or if the value isn't valid, return def.
 *
 * Valid values are strings that have a first character of:
 *  - t (true)
 *  - y (yes, evaluates to true)
 *  - 1 (evaluates to true)
 *  - f (false)
 *  - n (no, evaluates to false)
 *  - 0 (evaluates to false)
 *  .
 **/
bool CConfigurator::get_bool_value(const char *n, bool def) {
  for (const SCfg_Value &v : pValues) {
    if (!strcmp(v.name, n)) {
      switch (v.value[0]) {
      case 't':
      case 'T':
      case 'y':
      case 'Y':
      case '1':
        return true;

      case 'f':
      case 'F':
      case 'n':
      case 'N':
      case '0':
        return false;

      default:
        FAILURE_2(Configuration, "Illegal boolean value (%s) for %s", v.value,
                  n);
      }
    }
  }

  return def;
}

/**
 * Return a numeric value, if the name of the value can't be found in
 * our list of values, return def.
 **/
u64 CConfigurator::get_num_value(const char *n, bool decimal, u64 def) {
  u64 multiplier = decimal ? 1000 : 1024;
  for (const SCfg_Value &v : pValues) {
    if (!strcmp(v.name, n)) {
      u64 retval = 0;
      u64 partval = 0;
      char *val = v.value;
      int j = 0;
      for (;;) {
        while (val[j] && strchr("0123456789", val[j])) {
          partval *= 10;
          partval += val[j] - '0';
          j++;
        }

        switch (val[j]) {
        case 'T':
          partval *= multiplier;
          [[fallthrough]];
        case 'G':
          partval *= multiplier;
          [[fallthrough]];
        case 'M':
          partval *= multiplier;
          [[fallthrough]];
        case 'K':
          retval += partval * multiplier;
          partval = 0;
          j++;
          break;

        case '\0':
          retval += partval;
          return retval;

        default:
          FAILURE_2(Configuration, "Illegal numeric value (%s) for %s", v.value,
                    n);
        }
      }
    }
  }

  return def;
}

// THIS IS WHERE THINGS GET COMPLICATED...
#define NO_FLAGS 0

#define IS_CS 1
#define ON_CS 2

#define HAS_PCI 4
#define IS_PCI 8

#define HAS_ISA 16
#define IS_ISA 32

#define HAS_DISK 64
#define IS_DISK 128

#define IS_GUI 256
#define ON_GUI 512

#define IS_NIC 1024

#define N_P 2048 // no parent
#define ON_BOARD 4096 // only inside the board of the same name (see ports)
typedef struct {
  const char *name;
  classid id;
  int flags;
  const char *const *known_values; // values the class reads; others warn
} classinfo;

// Configuration values each device class actually reads. Anything else found
// in a device's config section is ignored by the code, so initialize() warns
// the user to remove it.
static const char *const kv_none[] = {0};
static const char *const kv_tsunami[] = {
    "memory.bits",      "rom.srm",   "rom.flash",     "rom.dpr",
    "rom.decompressed", "rom.nvram", "time",          "arc_year_compat",
    "exit_on_pal_halt", "platform",  "memory.arrays", "gdb",
    "gdb.wait",         0,
};
static const char *const kv_ev68cb[] = {"speed", "palcode.vms.nohle",
                                        "skip_memtest_hack",
                                        "timer.max_instr_per_tick", 0};
static const char *const kv_serial[] = {
    "port", "action", "address", "disabled", "raw_mode", "null_attach", 0};
static const char *const kv_ali[] = {"vga_console", "lpt.outfile", "timezone",
                                     0};
static const char *const kv_ali_ide[] = {"dma", 0};
static const char *const kv_ali_usb[] = {"port1", "port2", "port3", 0};
static const char *const kv_ehci[] = {"port1", "port2",      "port3",
                                      "port4", "companions", 0};
static const char *const kv_uss344[] = {"port1", "port2", "port3", "port4", 0};
static const char *const kv_vga[] = {"rom", 0};
static const char *const kv_cirrus[] = {"rom", "chip", 0};
static const char *const kv_mach64[] = {"rom", "chip", "memory", 0};
static const char *const kv_permedia2[] = {"rom", "memory", 0};
static const char *const kv_s3virge[] = {"rom", "chip", "memory", 0};
static const char *const kv_tga[] = {"rom", "model", 0};
static const char *const kv_radeon[] = {"rom", "chip", "memory", "model", 0};
static const char *const kv_tulip[] = {
    "adapter",   "mac",        "queue",  "crc",    "trace_packets",
    "type",      "host_ip",    "bridge", "uplink", "tap_create",
    "udp_local", "udp_remote", 0};
static const char *const kv_i8255x[] = {
    "adapter", "mac",        "type",      "host_ip",    "bridge",
    "uplink",  "tap_create", "udp_local", "udp_remote", 0};
static const char *const kv_virtio_net[] = {
    "mac",     "type",       "host_ip",   "bridge",     "uplink",
    "adapter", "tap_create", "udp_local", "udp_remote", 0};
static const char *const kv_disk_file[] = {"file",
                                           "model_number",
                                           "serial_number",
                                           "serial_num",
                                           "rev_number",
                                           "rev_num",
                                           "read_only",
                                           "cdrom",
                                           "autocreate_size",
                                           "allow_guest_eject",
                                           "latency.access_us",
                                           "latency.command_us",
                                           "latency.mb_per_s",
                                           0};
static const char *const kv_disk_device[] = {"device",
                                             "model_number",
                                             "serial_number",
                                             "serial_num",
                                             "rev_number",
                                             "rev_num",
                                             "read_only",
                                             "cdrom",
                                             "latency.access_us",
                                             "latency.command_us",
                                             "latency.mb_per_s",
                                             0};
static const char *const kv_disk_ram[] = {"size",
                                          "file",
                                          "model_number",
                                          "serial_number",
                                          "serial_num",
                                          "rev_number",
                                          "rev_num",
                                          "read_only",
                                          "cdrom",
                                          "latency.access_us",
                                          "latency.command_us",
                                          "latency.mb_per_s",
                                          0};
static const char *const kv_gui_sdl[] = {
    "keyboard.use_mapping", "keyboard.map",
    "mouse.speed",          "mouse.invert_x",
    "mouse.invert_y",       "video.linear",
    "video.scale_ratio",    "video.scale_change_enable",
    "hotkey.mouse_capture", "hotkey.media",
    "hotkey.media_force",   "hotkey.ctrl_alt_delete",
    "hotkey.reset_window",  "hotkey.scale_up",
    "hotkey.scale_down",    0};
static const char *const kv_gui_x11[] = {"keyboard.use_mapping", "keyboard.map",
                                         "private_colormap", 0};

classinfo classes[] = {
    {"tsunami", c_tsunami, N_P | IS_CS | HAS_PCI, kv_tsunami},
    {"ev68cb", c_ev68cb, ON_CS, kv_ev68cb},
    // The EV7 parts are the same CPU class: the row (CpuModels.cpp) is what
    // differs.
    {"ev7", c_ev68cb, ON_CS, kv_ev68cb},
    {"ev7z", c_ev68cb, ON_CS, kv_ev68cb},
    {"ali", c_ali, IS_PCI | HAS_ISA, kv_ali},
    {"ali_ide", c_ali_ide, IS_PCI | HAS_DISK, kv_ali_ide},
    {"cmd649", c_cmd649, IS_PCI | HAS_DISK, kv_ali_ide},
    {"ali_usb", c_ali_usb, IS_PCI | HAS_DISK, kv_ali_usb},
    {"ali_pmu", c_ali_pmu, IS_PCI, kv_none},
    {"serial", c_serial, ON_CS, kv_serial},
    {"s3", c_s3, IS_PCI | ON_GUI, kv_vga},
    {"cirrus", c_cirrus, IS_PCI | ON_GUI, kv_cirrus},
    {"mach64", c_mach64, IS_PCI | ON_GUI, kv_mach64},
    {"permedia2", c_permedia2, IS_PCI | ON_GUI, kv_permedia2},
    {"s3virge", c_s3virge, IS_PCI | ON_GUI, kv_s3virge},
    {"radeon", c_radeon, IS_PCI | ON_GUI, kv_radeon},
    {"tga", c_tga, IS_PCI | ON_GUI, kv_tga},
    {"dec21040", c_tulip, IS_PCI | IS_NIC, kv_tulip},
    {"dec21041", c_tulip, IS_PCI | IS_NIC, kv_tulip},
    {"dec21140", c_tulip, IS_PCI | IS_NIC, kv_tulip},
    {"dec21143", c_tulip, IS_PCI | IS_NIC, kv_tulip},
    {"de600", c_i8255x, IS_PCI | IS_NIC, kv_i8255x},
    {"de602_port", c_i8255x, IS_PCI | IS_NIC | ON_BOARD, kv_i8255x},
    {"de602b_port", c_i8255x, IS_PCI | IS_NIC | ON_BOARD, kv_i8255x},
    {"dec21050", c_pci_bridge, IS_PCI | HAS_PCI, kv_none},
    {"dec21052", c_pci_bridge, IS_PCI | HAS_PCI, kv_none},
    {"dec21152", c_pci_bridge, IS_PCI | HAS_PCI, kv_none},
    {"dec21153", c_pci_bridge, IS_PCI | HAS_PCI, kv_none},
    {"dec21154", c_pci_bridge, IS_PCI | HAS_PCI, kv_none},
    {"de602", c_pci_bridge, IS_PCI | HAS_PCI, kv_none},
    {"de602b", c_pci_bridge, IS_PCI | HAS_PCI, kv_none},
    {"i82557", c_i8255x, IS_PCI | IS_NIC, kv_i8255x},
    {"i82558", c_i8255x, IS_PCI | IS_NIC, kv_i8255x},
    {"i82559", c_i8255x, IS_PCI | IS_NIC, kv_i8255x},
    {"i82559er", c_i8255x, IS_PCI | IS_NIC, kv_i8255x},
    {"isp1020", c_isp1040, IS_PCI | HAS_DISK, kv_none},
    {"isp1040", c_isp1040, IS_PCI | HAS_DISK, kv_none},
    {"isp1080", c_isp1040, IS_PCI | HAS_DISK, kv_none},
    {"isp1240", c_isp1040, IS_PCI | HAS_DISK, kv_none},
    {"sym53c810", c_sym53c8xx, IS_PCI | HAS_DISK, kv_none},
    {"sym53c825", c_sym53c8xx, IS_PCI | HAS_DISK, kv_none},
    {"sym53c875", c_sym53c8xx, IS_PCI | HAS_DISK, kv_none},
    {"sym53c895", c_sym53c8xx, IS_PCI | HAS_DISK, kv_none},
    {"sym53c896", c_sym53c8xx, IS_PCI | HAS_DISK, kv_none},
    {"floppy", c_floppy, ON_CS | HAS_DISK, kv_none},
    {"file", c_file, IS_DISK, kv_disk_file},
    {"device", c_device, IS_DISK, kv_disk_device},
    {"ramdisk", c_ramdisk, IS_DISK, kv_disk_ram},
    {"sdl", c_sdl, N_P | IS_GUI, kv_gui_sdl},
    {"win32", c_sdl, N_P | IS_GUI, kv_gui_sdl},
    {"X11", c_x11, N_P | IS_GUI, kv_gui_x11},
    {"ehci", c_ehci, IS_PCI | HAS_DISK, kv_ehci},
    {"uss344", c_ehci, IS_PCI | HAS_DISK, kv_uss344},
    {"virtio_blk", c_virtio_blk, IS_PCI | HAS_DISK, kv_none},
    {"virtio_net", c_virtio_net, IS_PCI | IS_NIC, kv_virtio_net},
    {"es1370", c_es137x, IS_PCI, kv_none},
    {"es1371", c_es137x, IS_PCI, kv_none},
    {0, c_none, 0, 0}};

// The part a class's `chip` or `model` value names. A name the family does
// not have is refused, in check mode as when the device is built.
static bool cirrus_is_gd5430(CConfigurator *cfg) {
  const std::string chip = cfg->get_text_value("chip", "gd5434");
  if (chip != "gd5434" && chip != "gd5430")
    FAILURE_1(Configuration, "cirrus: unknown chip \"%s\" (gd5430, gd5434)",
              chip.c_str());
  return chip == "gd5430";
}

static const mach64_chip_config &mach64_chip(CConfigurator *cfg) {
  const char *chip = cfg->get_text_value("chip", "ct");
  const mach64_chip_config *c = mach64_chip_by_name(chip);
  if (!c)
    FAILURE_1(Configuration,
              "mach64: unknown chip \"%s\" (ct, vt2, vt3, rage2p, ragepro)",
              chip);
  return *c;
}

static const virge_chip_config &virge_chip(CConfigurator *cfg) {
  const char *chip = cfg->get_text_value("chip", "dx");
  const virge_chip_config *c = virge_chip_by_name(chip);
  if (!c)
    FAILURE_1(Configuration,
              "s3virge: unknown chip \"%s\" (virge, vx, dx, gx, gx2)", chip);
  return *c;
}

static const tga_model_config &tga_model(CConfigurator *cfg) {
  const char *model = cfg->get_text_value("model", "e1");
  const tga_model_config *m = tga_model_by_name(model);
  if (!m)
    FAILURE_1(Configuration,
              "tga: unknown model \"%s\" (e1, 3d30, 4d20; the 24-plane "
              "e2/e3 are not modelled yet)",
              model);
  return *m;
}

/**
 * The numbers in a section's name, which say where its device is:
 * `prefix` followed by one number (`fdc0`, `serial1`, `pci.3`), or by two
 * separated by a '.' when `b` is given (`pci0.7`, `disk1.0`). Decimal
 * digits only, each at most its maximum, and nothing after them: whether
 * the board or the controller has that place is for them to say.
 **/
static bool place_numbers(const char *name, const char *prefix, int *a,
                          int amax, int *b = nullptr, int bmax = 0) {
  const size_t n = strlen(prefix);
  if (strncmp(name, prefix, n))
    return false;
  const char *p = name + n;
  for (int k = 0; k < (b ? 2 : 1); k++) {
    int *out = k ? b : a;
    const int max = k ? bmax : amax;
    if (k && *p++ != '.')
      return false;
    if (*p < '0' || *p > '9')
      return false;
    long v = 0;
    while (*p >= '0' && *p <= '9') {
      v = v * 10 + (*p++ - '0');
      if (v > max)
        return false;
    }
    *out = (int)v;
  }
  return *p == 0;
}

/**
 * The machine the configuration describes: the system's, or in check mode
 * the one its machine section names.
 **/
const platform_config &CConfigurator::board() {
  if (!s_check)
    return theSystem->platform();
  if (!s_check_platform)
    FAILURE(Configuration, "no machine section");
  return *s_check_platform;
}

/**
 * Determine what device this configurator represents, and instantiate it;
 * then call initialize for our children. In check mode the device is not
 * instantiated (check_device()). A configuration error says where in the
 * file the section is.
 **/
void CConfigurator::initialize() {
  try {
    initialize_section();
  } catch (CPlacedConfigurationException &) {
    throw;
  } catch (CConfigurationException &e) {
    throw CPlacedConfigurationException(place() + e.text(), e.where());
  }
}

void CConfigurator::initialize_section() {
  myClassId = c_none;

  int i = 0;
  int pcibus = 0;
  int pcidev = 0;
  int idedev = 0;
  int idebus = 0;
  int fdcbus = 0;
  int number = 0;

  for (i = 0; classes[i].id != c_none; i++) {
    if (!strcmp(myValue, classes[i].name)) {
      myClassId = classes[i].id;
      myFlags = classes[i].flags;
      break;
    }
  }

  if (myClassId == c_none)
    FAILURE_2(Configuration, "Class %s for %s not known", myValue, myName);

  // Warn about configuration values this device class does not read;
  // they have no effect and should be removed from the config file.
  if (classes[i].known_values) {
    for (const SCfg_Value &v : pValues) {
      bool recognized = false;
      for (const char *const *k = classes[i].known_values; *k; k++) {
        if (!strcmp(v.name, *k)) {
          recognized = true;
          break;
        }
      }

      if (!recognized)
        cfg_msg(true,
                "%%SYS-W-UNKNOWNCFG: %s%s(%s): \"%s\" is not a recognized "
                "configuration value for this device; it has been ignored "
                "and should be removed from the configuration file.\n",
                place_of(v.line, v.col).c_str(), myName, myValue, v.name);
    }
  }

  if (myFlags & N_P) {
    if (pParent->get_flags())
      FAILURE_2(Configuration, "Class %s for %s needs a parent", myValue,
                myName);
  }

  if (myFlags & ON_CS) {
    if (!(pParent->get_flags() & IS_CS))
      FAILURE_2(Configuration, "Class %s for %s needs a chipset parent",
                myValue, myName);
  }

  const bool have_gui = s_check ? s_check_gui : bx_gui != nullptr;
  if (myFlags & ON_GUI) {
    if (!have_gui)
      FAILURE_2(Configuration, "Class %s for %s needs a GUI", myValue, myName);
  }

  if (myFlags & IS_GUI) {
    if (have_gui)
      FAILURE_2(Configuration, "Class %s for %s already found a gui", myValue,
                myName);
  }

#if !defined(HAVE_SDL)
  if (myClassId == c_sdl || myClassId == c_es137x)
    FAILURE_2(Configuration,
              "Class %s for %s needs compilation with SDL support", myValue,
              myName);
#endif
#if !defined(HAVE_X11)
  if (myClassId == c_x11)
    FAILURE_2(Configuration, "Class %s for %s needs an X11 platform", myValue,
              myName);
#endif
#if !defined(HAVE_PCAP) && !defined(__linux__)
  if (myFlags & IS_NIC)
    FAILURE_2(Configuration,
              "Class %s for %s needs networking support (libpcap or Linux TAP)",
              myValue, myName);
#endif
  if (myFlags & IS_PCI) {
    if (!(pParent->get_flags() & HAS_PCI)) {
      FAILURE_2(Configuration,
                "Class %s for %s should have a pci-bus capable parent device",
                myValue, myName);
    }

    const bool behind_bridge = pParent->get_class_id() == c_pci_bridge;
    if (behind_bridge) {
      // The secondary bus: same hose, devices 0-15 (IDSEL on AD16-AD31).
      // In check mode there is no bridge to number it.
      if (!place_numbers(myName, "pci.", &pcidev, 15))
        FAILURE_2(Configuration,
                  "Name %s for class %s should be pci.<device>, 0 to 15 "
                  "(inside a bridge)",
                  myName, myValue);
      pcibus =
          s_check
              ? 0
              : ((CPCIDevice *)(CPCIBridge *)pParent->get_device())->pci_bus();
    } else if (!place_numbers(myName, "pci", &pcibus, 999, &pcidev, 31))
      FAILURE_2(Configuration,
                "Name %s for class %s should be pci<hose>.<device>, the "
                "device 0 to 31",
                myName, myValue);
    if (myFlags & ON_BOARD) {
      const pci_bridge_config *board =
          behind_bridge ? CPCIBridge::find_chip(pParent->get_myValue())
                        : nullptr;
      if (!board || !board->port_class || strcmp(board->port_class, myValue))
        FAILURE_2(Configuration,
                  "%s (%s) is a port of a multi-port board and belongs "
                  "inside that board's block",
                  myName, myValue);
    }

    // Validate PCI slot assignments. On the Tsunami chipset, certain
    // pci0 slots are reserved for system-internal devices. Placing an
    // add-in device on one of those slots causes the SRM firmware to
    // malfunction (e.g. SCSI disks not detected, device conflicts).
    // Slots the board keeps for its own hardware are refused; which those
    // are is a property of the machine (src/platforms/).
    const bool is_system_device =
        (myClassId == c_ali || myClassId == c_ali_ide ||
         myClassId == c_ali_usb || myClassId == c_ali_pmu);
    if (!behind_bridge && !is_system_device) {
      const char *refusal = board().slot_refusal(board(), pcibus, pcidev);
      if (refusal)
        FAILURE_3(Configuration, "%s (%s): %s", myName, myValue, refusal);
    }
  }

  // The two floppy controllers and the two UARTs of the ISA bridge.
  if (myClassId == c_floppy && !place_numbers(myName, "fdc", &fdcbus, 1))
    FAILURE_2(Configuration, "Name %s for class %s should be fdc0 or fdc1",
              myName, myValue);
  if (myClassId == c_serial && !place_numbers(myName, "serial", &number, 1))
    FAILURE_2(Configuration,
              "Name %s for class %s should be serial0 or serial1", myName,
              myValue);

  if (myFlags & IS_DISK) {
    if (!(pParent->get_flags() & HAS_DISK)) {
      FAILURE_2(Configuration,
                "Class %s for %s should have a disk-controller parent device",
                myValue, myName);
    }

    if (!place_numbers(myName, "disk", &idebus, 99, &idedev, 99))
      FAILURE_2(Configuration,
                "Name %s for class %s should be disk<bus>.<device>", myName,
                myValue);
  }

  if (s_check)
    check_device();
  else
    create_device(pcibus, pcidev, idebus, idedev, fdcbus);

  // Synthesize mandatory system hardware omitted from the configuration.
  // The ES40 hardware always has two serial ports; SRM and guest OSes
  // expect both UARTs to exist, so a missing port becomes a null_attach
  // (bit-bucket) port.  The floppy controller is integrated in the M1543C
  // south bridge and always exists, even with no drives attached.
  if (myFlags & IS_CS) {
    bool have_serial[2] = {false, false};
    bool have_fdc0 = false;
    for (i = 0; i < (int)pChildren.size(); i++) {
      // A section misnamed is refused when its turn comes.
      if (!strcmp(pChildren[i]->get_myValue(), "serial") &&
          place_numbers(pChildren[i]->get_myName(), "serial", &number, 1))
        have_serial[number] = true;
      else if (!strcmp(pChildren[i]->get_myValue(), "floppy") &&
               !strcmp(pChildren[i]->get_myName(), "fdc0"))
        have_fdc0 = true;
    }

    if (!have_fdc0) {
      cfg_msg(false, "%%SYS-I-DEFAULTFDC: fdc0 is not configured; "
                     "adding the built-in floppy controller.\n");
      add_child("fdc0", "floppy", "");
    }

    // The board's own devices of ordinary classes (the DS25's 82559ER):
    // added with the board row's defaults where the configuration names
    // nothing at their place.
    for (const onboard_device *ob = myClassId == c_tsunami ? board().onboard
                                                           : nullptr;
         ob && ob->name; ob++) {
      bool have = false;
      for (const CConfigurator *c : pChildren)
        have |= !strcmp(c->myName, ob->name);
      if (have)
        continue;
      cfg_msg(false,
              "%%SYS-I-ONBOARD: %s is not configured; adding the board's "
              "%s (%s).\n",
              ob->name, ob->device_class, ob->defaults);
      add_child(ob->name, ob->device_class, ob->defaults);
    }

    for (number = 0; number < 2; number++) {
      if (have_serial[number])
        continue;

      cfg_msg(true,
              "%%SYS-W-NOSERIAL: serial%d is not configured; "
              "defaulting to null_attach mode.\n",
              number);
      char sname[16];
      snprintf(sname, sizeof(sname), "serial%d", number);
      add_child(sname, "serial", "null_attach=true;");
    }
  }

  // A multi-port board always carries its ports; the ones the
  // configuration leaves out are added unconnected.
  if (myClassId == c_pci_bridge) {
    const pci_bridge_config *board = CPCIBridge::find_chip(myValue);
    for (number = 0; board && number < board->ports; number++) {
      char name[16];
      snprintf(name, sizeof(name), "pci.%d", number);
      bool have = false;
      for (const CConfigurator *c : pChildren)
        have |= !strcmp(c->myName, name);
      if (have)
        continue;
      cfg_msg(false,
              "%%SYS-I-DEFAULTPORT: %s(%s): port %s is not configured; "
              "adding it unconnected (type = \"null\").\n",
              myName, myValue, name);
      add_child(name, board->port_class, "type=\"null\";");
    }
  }

  for (i = 0; i < (int)pChildren.size(); i++)
    initialize_child(pChildren[i]);

  if (!s_check && (myFlags & IS_CS))
    ((CSystem *)myDevice)->init();
}

/**
 * Add a section the configuration does not name, with `text` as its body.
 **/
void CConfigurator::add_child(const char *name, const char *value,
                              const char *text) {
  pChildren.push_back(
      new CConfigurator(this, strdup(name), strdup(value), text, strlen(text)));
}

/**
 * Initialize a child section. In check mode an error ends that section
 * only: it is reported and counted, and the walk goes on.
 **/
void CConfigurator::initialize_child(CConfigurator *child) {
  if (!s_check) {
    child->initialize();
    return;
  }
  try {
    child->initialize();
  } catch (CException &e) {
    s_check_errors++;
    fprintf(stderr, "%%SYS-E-CONFIG: %s\n", e.text().c_str());
  }
}

/**
 * Check mode: what constructing the device would refuse that the class
 * table does not already say. The rest of a device's values are read, and
 * refused, only by the device itself.
 **/
void CConfigurator::check_device() {
  switch (myClassId) {
  case c_tsunami: {
    // As CSystem's constructor.
    if (s_check_platform)
      FAILURE(Configuration, "More than one system");
    const char *name = get_text_value("platform", DEFAULT_PLATFORM);
    s_check_platform = find_platform(name);
    if (!s_check_platform)
      FAILURE_1(Configuration, "Unknown platform %s", name);
    break;
  }

  case c_ev68cb: {
    // As CAlphaCPU's constructor and CSystem::RegisterCPU.
    const cpu_model *cpu = find_cpu_model(myValue);
    const cpu_model *takes = find_cpu_model(board().cpu_model);
    if (cpu && takes && takes->family != cpu->family)
      FAILURE_2(Configuration, "the %s takes %s processors",
                board().description, takes->part);
    if (++s_check_cpus > board().max_cpus)
      FAILURE_2(Configuration, "this machine takes at most %d processors (%s)",
                board().max_cpus, board().description);
    break;
  }

  case c_cirrus:
    cirrus_is_gd5430(this);
    break;

  case c_mach64:
    mach64_chip(this);
    break;

  case c_s3virge:
    virge_chip(this);
    break;

  case c_tga:
    tga_model(this);
    break;

  case c_sdl:
  case c_x11:
    s_check_gui = true;
    break;

  default:
    break;
  }
}

/**
 * Construct the device this configurator describes, at its place.
 **/
void CConfigurator::create_device(int pcibus, int pcidev, int idebus,
                                  int idedev, int fdcbus) {
  int number = 0;

  switch (myClassId) {
  case c_tsunami:
    myDevice = new CSystem(this);
    // The board's own hardware: DPR, flash, I2C (platforms/<board>/).
    if (((CSystem *)myDevice)->platform().board_devices)
      ((CSystem *)myDevice)
          ->platform()
          .board_devices(this, (CSystem *)myDevice);
    break;

  case c_ev68cb:
    myDevice = new CAlphaCPU(this, (CSystem *)pParent->get_device());
    break;

  case c_ali:
    myDevice =
        new CAliM1543C(this, (CSystem *)pParent->get_device(), pcibus, pcidev);
    new CPort80(this, (CSystem *)pParent->get_device());
    new CKeyboard(this, (CSystem *)pParent->get_device());
    new CDMA(this, (CSystem *)pParent->get_device());
    break;

  case c_floppy:
    /* For disk controllers, myDevice points to the
     * CDiskController part of the class as it's used
     * to register disks to.
     */
    myDevice = (CDiskController *)new CFloppyController(
        this, (CSystem *)pParent->get_device(), fdcbus);
    break;

  case c_ali_ide:
    /* For disk controllers, myDevice points to the
     * CDiskController part of the class as it's used
     * to register disks to.
     */
    myDevice =
        (CDiskController *)new CAliM1543C_ide(this, theSystem, pcibus, pcidev);
    break;

  case c_cmd649:
    // A disk controller: see c_ali_ide.
    myDevice = (CDiskController *)new CCmd649(this, theSystem, pcibus, pcidev);
    break;

  case c_ali_usb:
    // A disk controller too (USB mass storage): see c_ali_ide.
    myDevice =
        (CDiskController *)new CAliM1543C_usb(this, theSystem, pcibus, pcidev);
    break;

  case c_ali_pmu:
    myDevice = new CAliM1543C_pmu(this, theSystem, pcibus, pcidev);
    break;

  case c_s3:
    myDevice = new CS3Trio64(this, theSystem, pcibus, pcidev);
    break;

  case c_cirrus:
    if (cirrus_is_gd5430(this))
      myDevice = new CCirrusGD5430(this, theSystem, pcibus, pcidev);
    else
      myDevice = new CCirrusGD5434(this, theSystem, pcibus, pcidev);
    break;

  case c_mach64:
    myDevice = new CMach64(this, theSystem, pcibus, pcidev, mach64_chip(this));
    break;

  case c_permedia2:
    myDevice = new CPermedia2(this, theSystem, pcibus, pcidev);
    break;

  case c_radeon:
    myDevice = new CRadeon(this, theSystem, pcibus, pcidev);
    break;

  case c_s3virge:
    myDevice = new CS3Virge(this, theSystem, pcibus, pcidev, virge_chip(this));
    break;

  case c_tga:
    myDevice = new CTga(this, theSystem, pcibus, pcidev, tga_model(this));
    break;

  case c_ehci:
    // A disk controller too (USB mass storage): see c_ali_ide. The class
    // name ("ehci", "uss344") names the card.
    myDevice = (CDiskController *)new CEhci(this, theSystem, pcibus, pcidev,
                                            *CEhci::find_chip(myValue));
    break;

  case c_virtio_blk:
    // A disk controller: see c_ali_ide.
    myDevice =
        (CDiskController *)new CVirtioBlk(this, theSystem, pcibus, pcidev);
    break;

  case c_virtio_net:
    myDevice = new CVirtioNet(this, theSystem, pcibus, pcidev);
    break;

#if defined(HAVE_SDL)
  case c_es137x:
    // The class name ("es1370", "es1371") names the part.
    myDevice = new CES137x(this, theSystem, pcibus, pcidev,
                           *CES137x::find_chip(myValue));
    break;
#endif

#if defined(HAVE_PCAP) || defined(__linux__)

  case c_tulip:
    // The class name ("dec21040", "dec21143") names the part.
    myDevice = new CTulip(this, theSystem, pcibus, pcidev,
                          *CTulip::find_chip(myValue));
    break;
#endif

  case c_i8255x:
    // The class name ("de600", "i82558") names the board.
    myDevice = new CI8255x(this, theSystem, pcibus, pcidev,
                           *CI8255x::find_chip(myValue));
    break;

  case c_pci_bridge:
    myDevice = new CPCIBridge(this, theSystem, pcibus, pcidev,
                              *CPCIBridge::find_chip(myValue));
    break;

  case c_isp1040:
    // As for the Symbios: myDevice points at the disk-controller part,
    // which is what disks are registered with. The class names the part.
    myDevice = (CDiskController *)new CIsp1040(this, theSystem, pcibus, pcidev,
                                               *CIsp1040::find_chip(myValue));
    break;

  case c_sym53c8xx:
    /* For disk controllers, myDevice points to the
     * CDiskController part of the class as it's used
     * to register disks to. The class name ("sym53c875")
     * names the part.
     */
    myDevice = (CDiskController *)new CSym53C8xx(
        this, theSystem, pcibus, pcidev,
        *CSym53C8xx::find_chip(myValue + strlen("sym53c")));
    break;

  case c_file:
    myDevice =
        new CDiskFile(this, theSystem, (CDiskController *)pParent->get_device(),
                      idebus, idedev);
    break;

  case c_device:
    myDevice = new CDiskDevice(this, theSystem,
                               (CDiskController *)pParent->get_device(), idebus,
                               idedev);
    break;

  case c_ramdisk:
    myDevice =
        new CDiskRam(this, theSystem, (CDiskController *)pParent->get_device(),
                     idebus, idedev);
    break;

  case c_serial:
    // serial0 or serial1 (initialize_section()).
    place_numbers(myName, "serial", &number, 1);
    myDevice = new CSerial(this, (CSystem *)pParent->get_device(), number);
    break;

  case c_sdl:
#if defined(HAVE_SDL)
    PLUG_load_plugin(this, sdl);
#endif
    break;

  case c_x11:
#if defined(HAVE_X11)
    PLUG_load_plugin(this, x11);
#endif
    break;

  default:
    FAILURE_1(Configuration, "Enum case not handled: %i", myClassId);
    break;
  }
}

static void dump_quoted(FILE *f, const char *s) {
  fputc('"', f);
  for (; *s; s++) {
    if (*s == '"')
      fputc('"', f);
    fputc(*s, f);
  }
  fputc('"', f);
}

/**
 * Write this section's body, and its children's, in the configuration
 * syntax: every value quoted, a value named twice commented out (the first
 * one is the one used), and a section the emulator added marked as such.
 * The master configurator (depth 0) writes the whole file.
 **/
void CConfigurator::dump(FILE *f, int depth) {
  const int in = 2 * depth;
  for (size_t v = 0; v < pValues.size(); v++) {
    bool again = false;
    for (size_t w = 0; w < v && !again; w++)
      again = !strcmp(pValues[w].name, pValues[v].name);
    fprintf(f, "%*s%s%s = ", in, "", again ? "// " : "", pValues[v].name);
    dump_quoted(f, pValues[v].value);
    fprintf(f, ";%s\n", again ? " (named twice: the first one is used)" : "");
  }
  for (size_t c = 0; c < pChildren.size(); c++) {
    CConfigurator *k = pChildren[c];
    if (!pParent && (c > 0 || !pValues.empty()))
      fputc('\n', f);
    fprintf(f, "%*s%s = %s%s\n%*s{\n", in, "", k->myName, k->myValue,
            k->myAdded ? " // added" : "", in, "");
    k->dump(f, depth + 1);
    fprintf(f, "%*s}\n", in, "");
  }
}
