/* Alphabox Alpha Emulator
 * Copyright (C) 2020 Tomáš Glozar
 * Website: https://github.com/lenticularis39/axpbox
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
 * Contains the definitions for the configuration file interpreter.
 **/
#if !defined(__CONFIGURATOR_H__)
#define __CONFIGURATOR_H__

#include "StdAfx.hpp"

#include <string>
#include <vector>

/// How deep sections may nest. A machine is at most five deep (machine,
/// bridge, bridge, controller, disk); anything near this is a broken file.
#define CFG_MAX_DEPTH 16
/// The longest name or class a section or value may have.
#define CFG_MAX_NAME 255

typedef enum {
  c_none,

  // chipsets
  c_tsunami,

  // system devices
  c_ev68cb,
  c_serial,
  c_floppy,

  // pci devices
  c_ali,
  c_ali_ide,
  c_cmd649,
  c_ali_usb,
  c_ali_pmu,
  c_s3,
  c_cirrus,
  c_mach64,
  c_permedia2,
  c_s3virge,
  c_tga,
  c_radeon,
  c_tulip,
  c_i8255x,
  c_pci_bridge,
  c_sym53c8xx,
  c_isp1040,
  c_es137x,
  c_ehci,
  c_virtio_blk,
  c_virtio_net,

  // disk devices
  c_file,
  c_device,
  c_ramdisk,

  // gui's ("win32" and "X11" are the SDL GUI too)
  c_sdl
} classid;

class CConfigurator {
public:
  /**
   * The master configurator: parse `text` (a configuration file, read from
   * `source`, the name messages give) and initialize the sections it holds.
   * With a parent: a section the emulator adds itself (initialize()), its
   * body in `text` and no place in a file.
   **/
  CConfigurator(class CConfigurator *parent, char *name, char *value,
                const char *text, size_t textlen, const char *source = nullptr);
  ~CConfigurator(void);

  char *get_text_value(const char *n) { return get_text_value(n, (char *)0); };
  char *get_text_value(const char *n, const char *def);

  bool get_bool_value(const char *n) { return get_bool_value(n, false); };
  bool get_bool_value(const char *n, bool def);

  u64 get_num_value(const char *n, bool decimal_suffixes) {
    return get_num_value(n, decimal_suffixes, 0);
  };
  u64 get_num_value(const char *n, bool decimal_suffixes, u64 def);

  classid get_class_id() { return myClassId; };
  void *get_device() { return myDevice; };
  int get_flags() { return myFlags; };

  char *get_myName() { return myName; };
  char *get_myValue() { return myValue; };
  CConfigurator *get_myParent() { return pParent; };

  void initialize();

  /**
   * Check mode (`alphabox run --check`, `--dump-config`): initialize()
   * applies every rule of its own (classes, places, parents, the board's
   * slots, values a device does not read) and adds the sections the
   * machine always has, but constructs no device. An error ends the section
   * it is in and is counted; the walk goes on with the next one. Messages go
   * to stderr, so that a dump on stdout is a configuration file. Set before
   * the master configurator is constructed.
   **/
  static void set_check_mode(bool on);
  static int check_errors();
  static int check_warnings();
  /// Check mode: whether a machine section was found.
  static bool check_found_system();

  /**
   * On a machine section: how many thread processing units each of its
   * processor chips has, and in *chips how many chips its processor
   * sections describe. A part whose row has TPUs (the 21464's four) reads
   * `tpus` from its section, 1 to the row's count, 1 by default; more than
   * one only on a board that lists TPUs to its console as processors (the
   * Marvel boards). Every other part is one TPU. Refuses chips that differ
   * in their count, chips whose PIDs leave no bit pair for the TPUs'
   * numbers, and a machine of more than CSystem::kMaxCPUs processors
   * (docs/platforms/ev8.md, S4).
   **/
  int processor_tpus(int *chips);
  /// TPUs per chip of the machine being built, or checked: 1 until its
  /// machine section has been read.
  static int machine_tpus();
  /// Its processor chips: 0 until then.
  static int machine_chips();
  /// Whether a Marvel machine is in 21464 mode: its processors are native
  /// EV8s (docs/platforms/ev8-native.md, section 2).
  static bool machine_21464_mode();

  /// The resolved tree in the configuration syntax, values quoted.
  void dump(FILE *f, int depth = 0);

private:
  /// A section the parser found in the file, at line:col.
  CConfigurator(class CConfigurator *parent, char *name, char *value, int line,
                int col);
  void parse(class CConfigLexer &lex, int depth);
  void add_value(char *n, char *v, int line, int col);
  /// "file:line:col: " for this section, or "" for one the emulator added.
  std::string place() const;
  void initialize_section();
  void check_device();
  void create_device(int pcibus, int pcidev, int idebus, int idedev,
                     int fdcbus);
  void initialize_child(CConfigurator *child);
  void add_child(const char *name, const char *value, const char *text);
  const struct platform_config &board();

  class CConfigurator *pParent;
  std::vector<CConfigurator *> pChildren;
  char *myName;
  char *myValue;
  void *myDevice;
  classid myClassId;
  int myFlags;
  bool myAdded; ///< added by initialize(), not named in the file
  int myLine;   ///< where the section's name is (0: added)
  int myCol;
  /// The values, in the file's order. Devices keep the char pointers, so
  /// they are never freed or moved.
  struct SCfg_Value {
    char *name;
    char *value;
    int line; ///< where the value's name is (0: no place in a file)
    int col;
  };
  std::vector<SCfg_Value> pValues;
};
#endif //! defined(__CONFIGURATOR_H__)
