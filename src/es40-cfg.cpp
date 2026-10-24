/* Alphabox Alpha Emulator
 * Copyright (C) 2020 Tomáš Glozar
 * Website: https://github.com/lenticularis39/axpbox
 *
 * Forked from: ES40 emulator
 * Copyright (C) 2007-2008 by the ES40 Emulator Project
 * Copyright (C) 2007 by Camiel Vanderhoeven
 *
 * Contains code from ES40-Emu:
 * Copyright (C) 2023-2026 gdwnldsKSC and the ES40-Emu contributors
 * Copyright (C) 2026 Cacodemon345
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
 * Configuration file creator: `alphabox configure`.
 *
 * Asks for a machine, one question at a time, and writes its configuration
 * file. What it offers comes from the emulator's own tables where they
 * exist: the boards and their limits, slots and own PCI functions
 * (src/platforms/Platforms.cpp), the network backends this build has
 * (NetworkBackend.cpp). The file it writes is checked as
 * `alphabox run --check` would check it.
 *
 * Each question has a key, by which the answer can be given beforehand
 * (Question.hpp): `--set key=value`, an --answers file of `key = value`
 * lines, and --defaults for the rest, which together make a session that
 * asks nothing.
 **/

#include "StdAfx.hpp"
#include "banner.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

// C++ includes
#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "Configurator.hpp"
#include "NetworkBackend.hpp"
#include "Platform.hpp"
#if defined(HAVE_PCAP)
#include "NetworkPcap.hpp"
#endif

using namespace std;

// Question classes

#include "FreeTextQuestion.hpp"
#include "MultipleChoiceQuestion.hpp"
#include "NumberQuestion.hpp"
#include "Question.hpp"
#include "ShrinkingChoiceQuestion.hpp"

/**
 * A value as the configuration file quotes it: in double quotes, a quote in
 * it doubled.
 **/
static string quote(const string &s) {
  string q = "\"";
  for (char c : s) {
    if (c == '"')
      q += '"';
    q += c;
  }
  return q + "\"";
}

/**
 * Whether `s` is a number as the configuration reads one: groups of digits,
 * each but the last ending in K, M, G or T (Configurator.cpp).
 **/
static bool is_size(const string &s) {
  if (s.empty())
    return false;
  for (size_t i = 0; i < s.size();) {
    if (!isdigit((unsigned char)s[i]))
      return false;
    while (i < s.size() && isdigit((unsigned char)s[i]))
      i++;
    if (i < s.size() && !strchr("KMGT", s[i]))
      return false;
    if (i < s.size())
      i++;
  }
  return true;
}

/**
 * A free-text question for a size ("10M"), asked until the answer is one.
 **/
static string ask_size(const string &key, const string &question,
                       const string &def) {
  FreeTextQuestion q;
  q.setKey(key);
  q.setQuestion(question);
  q.setOptions("e.g. 1440K, 64M, 4G");
  q.setExplanation("A number of bytes, with K, M, G or T for 1024 to the "
                   "power 1 to 4; groups add up (2G512M).");
  q.setDefault(def);
  for (;;) {
    string a = q.ask();
    if (is_size(a))
      return a;
    if (a != "?")
      cout << "\nPlease enter a size such as 512M or 4G.\n\n";
  }
}

/**
 * A yes/no question; returns whether the answer is yes.
 **/
static bool ask_yes(const string &key, const string &question,
                    const string &explanation, bool def) {
  MultipleChoiceQuestion q;
  q.setKey(key);
  q.setQuestion(question);
  q.setExplanation(explanation);
  q.addAnswer("yes", "true", "Yes");
  q.addAnswer("no", "false", "No");
  q.setDefault(def ? "yes" : "no");
  return q.ask() == "true";
}

/**
 * Add disks for a controller to the configuration file.
 *
 * \param disk_q: A ShrinkingChoiceQuestion that contains
 *                all allowed disk names for this controller,
 *                and a special answer "none" with a value
 *                of "".
 * \param os:     Output stream for the configuration file.
 * \param indent: The controller's body's indentation.
 * \param key:    The prefix of the questions' keys ("ide": ide.type,
 *                ide.path, ...), so that each controller's disks take
 *                their own answers.
 * \param floppy: Floppy drives: never a CD, 1.44 MB by default.
 **/
void add_disks(ShrinkingChoiceQuestion *disk_q, ostream *os,
               const string &indent, const string &key, bool floppy = false) {
  const string in = indent + "  ";
  /* Loop until there are no more disks to be added.
   */
  for (;;) {
    /* If disk_q has only one answer left, this is the
     * "none" answer. This controller can contain no
     * more disks.
     */
    if (disk_q->countAnswers() == 1)
      break;

    /* If the answer value is "", the answer "none"
     * was given. Stop adding disks to this controller.
     */
    const string where = disk_q->ask();
    if (where == "")
      break;

    /* Find out if this should be using:
     *   - a disk image file
     *   - a raw device
     *   - a RAM DISK
     */
    MultipleChoiceQuestion type_q;
    type_q.setKey(key + ".type");
    type_q.setQuestion("How should " + where + " be emulated?");
    type_q.setDefault("file");
    type_q.setExplanation("Disks can be emulated in several ways.");
    type_q.addAnswer("file", "file",
                     "The disk uses a disk image on the host system's disk.");
    type_q.addAnswer("device", "device",
                     "The disk uses one of the host system's raw disks.");
    type_q.addAnswer("ramdisk", "ramdisk",
                     "The disk keeps its data in memory, lost when the "
                     "emulator stops; it can start as a copy of a file.");
    const string type = type_q.ask();

    *os << indent << where << " = " << type << "\n";
    *os << indent << "{\n";

    /* A CD-ROM? Not on a floppy controller.
     */
    bool cdrom = false;
    if (!floppy) {
      MultipleChoiceQuestion cdrom_q;
      cdrom_q.setKey(key + ".cdrom");
      cdrom_q.setQuestion("Should " + where + " be a disk or a cd-rom device?");
      cdrom_q.setExplanation("Do you want the OS to see this " + type +
                             " as a hard-disk, or as a cd-rom?");
      cdrom_q.addAnswer("disk", "false", "Hard-disk");
      cdrom_q.addAnswer("cd-rom", "true", "CD-ROM drive, read-only");
      cdrom_q.setDefault("disk");
      cdrom = cdrom_q.ask() == "true";
      if (cdrom)
        *os << in << "cdrom = true;\n";
    }

    if (type == "file" || type == "device") {
      /* For a file or device, we need to know what
       * file or device to use.
       */
      FreeTextQuestion img_q;
      img_q.setKey(key + ".path");
      img_q.setQuestion("What " + type + " should " + where + " use?");
      img_q.setExplanation("Enter the path to the " + type +
                           " to use for this disk.");
      img_q.setRequired(!cdrom); // an empty CD drive has none
      const string path = img_q.ask();
      if (path != "")
        *os << in << type << " = " << quote(path) << ";\n";

      /* A disk image that does not exist yet can be created, of a size.
       */
      if (type == "file" && !cdrom &&
          ask_yes(key + ".create",
                  "If the file doesn't exist, do you want us to create it?",
                  "The file will be created the first time the emulator "
                  "runs.",
                  true))
        *os << in << "autocreate_size = "
            << ask_size(key + ".size", "How large should the disk be?",
                        floppy ? "1440K" : "2G")
            << ";\n";
    }

    if (type == "ramdisk") {
      /* A RAM disk starts as a copy of a file, or blank, of a size.
       */
      FreeTextQuestion img_q;
      img_q.setKey(key + ".path");
      img_q.setQuestion("What file should " + where +
                        " start as a copy of (none for a blank disk)?");
      img_q.setExplanation("The RAM disk takes the file's size and contents; "
                           "what the guest writes is not written back.");
      img_q.setDefault(cdrom ? "" : "none");
      img_q.setRequired(true);
      const string path = img_q.ask();
      if (path != "none")
        *os << in << "file = " << quote(path) << ";\n";
      else
        *os << in << "size = "
            << ask_size(key + ".size", "How large should the RAM disk be?",
                        floppy ? "1440K" : "64M")
            << ";\n";
    }

    /* We also need to know whether this is a
     * writeable or a read-only device. CD-ROMs are read-only, and a
     * read-only RAM disk makes no sense.
     */
    if (!cdrom && type != "ramdisk" &&
        ask_yes(key + ".read_only", "Should " + where + " be read-only?",
                "You might want to write-protect this disk.", false))
      *os << in << "read_only = true;\n";

    /* The user can define a custom model, revision and serial number
     * for the device; blank keeps the default.
     */
    static const char *const ids[3][3] = {
        {".model", "model_number", "a disk model number"},
        {".rev", "rev_number", "a revision number"},
        {".serial", "serial_number", "a serial number"}};
    for (const auto &id : ids) {
      FreeTextQuestion ft_q;
      ft_q.setKey(key + id[0]);
      ft_q.setQuestion(string("Would you like to set ") + id[2] + "?");
      ft_q.setExplanation("Leave blank to choose the default.");
      if (ft_q.ask() != "")
        *os << in << id[1] << " = " << quote(ft_q.getAnswer()) << ";\n";
    }

    *os << indent << "}\n";
  }
}

/// The places a controller's disks can take, as disk_q answers.
static void disk_places(ShrinkingChoiceQuestion *q, int buses, int units,
                        int skip_unit = -1) {
  q->addAnswer("none", "", "stop adding disks");
  for (int b = 0; b < buses; b++)
    for (int u = 0; u < units; u++)
      if (u != skip_unit) {
        const string label =
            buses > 1 || units == 2 ? i2s(b) + "." + i2s(u) : i2s(u);
        q->addAnswer(label, "disk" + i2s(b) + "." + i2s(u),
                     "bus " + i2s(b) + ", unit " + i2s(u));
      }
}

/// Ask for the disks of a controller with `buses` x `units` places.
static void ask_disks(const string &key, const string &controller, ostream *os,
                      int buses, int units, int skip_unit = -1,
                      bool floppy = false) {
  ShrinkingChoiceQuestion q;
  q.setKey(key + ".disk");
  q.setQuestion("Do you want to add any disks to the " + controller + "?");
  q.setDefault("none");
  q.setExplanation("Add disks one at a time; 'none' when there are no "
                   "more.");
  disk_places(&q, buses, units, skip_unit);
  add_disks(&q, os, "    ", key, floppy);
}

/// The SCSI controllers: class, description, buses, units per bus.
struct scsi_part {
  const char *cls;
  const char *description;
  int buses;
  int units;
};
static const scsi_part scsi_parts[] = {
    {"sym53c810", "Symbios 53C810, narrow (KZPAA)", 1, 7},
    {"sym53c825", "Symbios 53C825, wide", 1, 16},
    {"sym53c875", "Symbios 53C875, wide Ultra", 1, 16},
    {"sym53c895", "Symbios 53C895, wide Ultra2 (KZPBA's successor)", 1, 16},
    {"sym53c896", "Symbios 53C896, two wide Ultra2 channels", 2, 16},
    {"isp1020", "QLogic ISP1020 (KZPBA-CA)", 1, 16},
    {"isp1040", "QLogic ISP1040 (KZPBA-CB)", 1, 16},
    {"isp1080", "QLogic ISP1080, Ultra2 (not known to the ES40 console)", 1,
     16},
    {"isp1240", "QLogic ISP1240, two buses (not known to the ES40 console)", 2,
     16},
};

/// The board's own function of `cls`, or nullptr.
static const board_function *function_of(const platform_config &board,
                                         const char *cls) {
  for (const board_function *f = board.functions; f && f->device_class; f++)
    if (!strcmp(f->device_class, cls))
      return f;
  return nullptr;
}

/// A memory size of 2^bits bytes, as a question shows it: 64M, 2G.
static string size_label(int bits) {
  return bits < 30 ? i2s(1 << (bits - 20)) + "M" : i2s(1 << (bits - 30)) + "G";
}

/**
 * The questions, and the configuration written from the answers to `out`.
 **/
static int configure_session(const string &out) {
  print_alphabox_banner("Alphabox Alpha Emulator configuration utility");

  /* Explanation
   */
  printf("We are now going to set up a configuration file for the "
         "Emulator.\n");
  printf("It will be saved as %s.\n\n", out.c_str());
  printf(
      "For more detailed information to the current question, answer '?'.\n");

  /* Check if the file already exists.
   */
  ifstream check_file(out);
  const bool existed = check_file.is_open();
  if (existed) {
    check_file.close();
    MultipleChoiceQuestion overwrite_q;
    overwrite_q.setKey("overwrite");
    overwrite_q.setQuestion("The file " + out +
                            " already exists. Do you want to overwrite it?");
    overwrite_q.setExplanation("If you answer 'no', the program will exit "
                               "without overwriting the existing file.");
    overwrite_q.addAnswer("no", "no", "Keep the existing file and exit.");
    overwrite_q.addAnswer("yes", "yes",
                          "Overwrite the existing configuration file.");
    overwrite_q.setDefault("no");

    if (overwrite_q.ask() == "no") {
      printf("Exiting without changing %s.\n", out.c_str());
      return 0;
    }
  }

  /* Whether the file can be written is better known before the questions
   * than after them. The configuration is collected in memory and written
   * at the end, so a session that ends early leaves the file as it was.
   */
  {
    ofstream probe(out, ios::app);
    if (!probe) {
      printf("Cannot write %s: %s.\n", out.c_str(), strerror(errno));
      return 1;
    }
    probe.close();
    if (!existed)
      remove(out.c_str());
  }
  ostringstream os;

  /* **************************** *
   * The machine                  *
   * **************************** */

  MultipleChoiceQuestion platform_q;
  platform_q.setKey("platform");
  platform_q.setQuestion("Which machine do you want to emulate?");
  platform_q.setExplanation(
      "Each machine runs its own console firmware, from the firmware media "
      "you own (docs/configuration.md lists them).");
  for (size_t i = 0; const platform_config *p = platform_at(i); i++)
    platform_q.addAnswer(p->name, p->name,
                         string(p->description) + ", up to " +
                             i2s(p->max_cpus) + " processor" +
                             (p->max_cpus > 1 ? "s" : ""));
  platform_q.setDefault(DEFAULT_PLATFORM);
  const platform_config &board = *find_platform(platform_q.ask().c_str());
  const bool marvel = board.chipset == CHIPSET_MARVEL;
  const board_function *ali = function_of(board, "ali");

  /* **************************** *
   * GUI Choice                   *
   * **************************** */

  MultipleChoiceQuestion gui_q;
  gui_q.setKey("gui");
  gui_q.setQuestion("Do you want to have a GUI?");
  gui_q.setExplanation(
      "You need a GUI if you want to use an emulated graphics card or a "
      "sound card. You don't need this for most OS'es. If you don't need "
      "this, we recommend that you answer 'none' to this question.");
  gui_q.setDefault("none");
  gui_q.addAnswer("none", "", "No GUI. Graphics cards are not supported.");
#if defined(HAVE_SDL)
  gui_q.addAnswer("SDL", "sdl",
                  "Simple Directmedia Layer. Preferred GUI mechanism.");
#endif

  if (gui_q.countAnswers() == 1) {
    cout << "\nThe GUI is not available (this build has no SDL).\n";
    gui_q.setAnswer("");
  } else {
    gui_q.ask();
  }
  const bool gui = gui_q.getAnswer() != "";

  if (gui) {
    os << "gui = " << gui_q.getAnswer() << "\n";
    os << "{\n";

    if (ask_yes("gui.scale",
                "Do you want to set a custom scale ratio for the display "
                "output?",
                "The display output is scaled automatically based on system "
                "DPI by default, which can be overridden.",
                false)) {
      NumberQuestion vid_scale_ratio_q;
      vid_scale_ratio_q.setKey("gui.scale_ratio");
      vid_scale_ratio_q.setQuestion(
          "How many times should the display be scaled?");
      vid_scale_ratio_q.setExplanation(
          "Type an integer to set the scale ratio for the display output.");
      vid_scale_ratio_q.setRange(1, 8);
      vid_scale_ratio_q.setDefault("1");
      os << "  video.scale_ratio = " << vid_scale_ratio_q.ask() << ";\n";
    }

    MultipleChoiceQuestion vid_linear_q;
    vid_linear_q.setKey("gui.linear");
    vid_linear_q.setQuestion(
        "Should the display output be nearest or bilinear?");
    vid_linear_q.setExplanation(
        "This affects the resized display output. Nearest looks pixel-y but "
        "harsh, while linear does not look as harsh.");
    vid_linear_q.addAnswer("nearest", "false", "Nearest display output.");
    vid_linear_q.addAnswer("bilinear", "true", "Bilinear display output");
    vid_linear_q.setDefault("bilinear");
    os << "  video.linear = " << vid_linear_q.ask() << ";\n";

    os << "  video.scale_change_enable = "
       << (ask_yes("gui.scale_keys",
                   "Enable runtime display scale changes via hotkeys?",
                   "If enabled, Ctrl+PageUp and Ctrl+PageDown scale the "
                   "window up and down while the emulator runs (1x to 8x). "
                   "The change is not saved to this file.",
                   false)
               ? "true"
               : "false")
       << ";\n";

    os << "}\n\n";
  }

  os << "sys0 = tsunami\n";
  os << "{\n";
  os << "  platform = " << quote(board.name) << ";\n";

  /* **************************** *
   * Memory Size                  *
   * **************************** */

  MultipleChoiceQuestion mem_q;
  mem_q.setKey("memory");
  mem_q.setQuestion(marvel ? "How much memory should each processor have?"
                           : "How much RAM memory do you want to emulate?");
  mem_q.setExplanation(
      string(marvel ? "An EV7 has its own memory: this is each processor's. "
                    : "") +
      "Your system should have enough free memory to emulate the amount "
      "you choose here.");
  for (int b = board.min_memory_bits; b <= board.max_memory_bits; b++)
    mem_q.addAnswer(size_label(b), i2s(b), size_label(b) + "Bytes of memory.");
  mem_q.setDefault(size_label(
      std::max(board.min_memory_bits, std::min(28, board.max_memory_bits))));
  os << "  memory.bits = " << mem_q.ask() << ";\n";

  /* **************************** *
   * ROM Files                    *
   * **************************** */

  FreeTextQuestion rom_q;
  rom_q.setKey("rom.srm");
  rom_q.setQuestion("Where can the console firmware (SRM) image be found?");
  rom_q.setExplanation("This file is required: the " +
                       string(board.description) +
                       "'s console, from the "
                       "firmware media.");
  rom_q.setDefault(board.firmware_file);
  os << "  rom.srm = " << quote(rom_q.ask()) << ";\n";

  // What else the board keeps in files: the flash (and on the Tsunami
  // boards the management controller's DPR), or on the EV7 machines the
  // CMM's NVRAM.
  struct rom_file {
    const char *key, *question, *def;
    bool on;
  };
  const rom_file roms[] = {
      {"rom.flash", "Where should the Flash ROM image be saved?", "flash.rom",
       !marvel},
      {"rom.dpr", "Where should the DPR EEPROM image be saved?", "dpr.rom",
       board.chipset == CHIPSET_TSUNAMI},
      {"rom.nvram", "Where should the console's NVRAM be saved?",
       "cmm_nvram.bin", marvel},
  };
  for (const rom_file &r : roms) {
    if (!r.on)
      continue;
    FreeTextQuestion q;
    q.setKey(r.key);
    q.setQuestion(r.question);
    q.setExplanation("The file is created when missing, and saved as it "
                     "changes and when the emulator stops.");
    q.setDefault(r.def);
    os << "  " << r.key << " = " << quote(q.ask()) << ";\n";
  }
  os << "\n";

  /* **************************** *
   * CPU's                        *
   * **************************** */

  int cpus = 1;
  if (board.max_cpus > 1) {
    NumberQuestion cpu_q;
    cpu_q.setKey("cpus");
    cpu_q.setQuestion("How many CPU's do you want in the system?");
    cpu_q.setRange(1, board.max_cpus);
    cpu_q.setDefault(marvel ? "2" : "1");
    cpu_q.setExplanation("The " + string(board.description) + " takes up to " +
                         i2s(board.max_cpus) + ".");
    cpu_q.ask();
    cpus = cpu_q.getNum();
  }

  string cpu_class = board.cpu_model;
  if (marvel) {
    MultipleChoiceQuestion model_q;
    model_q.setKey("cpu.model");
    model_q.setQuestion("Which processor should the machine have?");
    model_q.setExplanation(
        "ev7z is the faster part of the same core, which the console names "
        "EV7 rev 3.0. ev8 is the cancelled 21464 in EV7-compatible mode: "
        "the EV7's console and PALcode, the 21464's identity and "
        "instructions (docs/platforms/ev8.md).");
    model_q.addAnswer("ev7", "ev7", "21364 (EV7)");
    model_q.addAnswer("ev7z", "ev7z", "21364 (EV7z)");
    model_q.addAnswer("ev8", "ev8", "21464 (EV8)");
    model_q.setDefault("ev7");
    cpu_class = model_q.ask();
  }

  NumberQuestion mhz_q;
  mhz_q.setKey("cpu.speed");
  mhz_q.setQuestion("What should the reported to guest platform speed of the "
                    "CPU's be in MHz?");
  mhz_q.setExplanation("This only changes the CPU speed reported to the OS; "
                       "not the speed of the emulation.");
  mhz_q.setRange(10, 2000);
  // The EV8's design clock is 1.4 GHz [21464 IDS Rev 1.1k, PDF p. 42].
  mhz_q.setDefault(!marvel               ? "800"
                   : cpu_class == "ev7z" ? "1300"
                   : cpu_class == "ev8"  ? "1400"
                                         : "1000");
  mhz_q.ask();

  // The memory test skip patches the ES40's own console.
  const bool skip_memtest =
      !strcmp(board.name, "es40") &&
      ask_yes("cpu.skip_memtest", "Do you want to skip memtest on SRM start?",
              "This makes startup significantly faster, but may not work "
              "with some versions of the firmware.",
              false);

  for (int i = 0; i < cpus; i++) {
    /* Repeat the CPU configuration for each
     * CPU. Differing CPU specs are not supported.
     */
    os << "  cpu" << i << " = " << cpu_class << "\n";
    os << "  {\n";
    os << "    speed = " << mhz_q.getAnswer() << "M;\n";
    if (skip_memtest)
      os << "    skip_memtest_hack = true;\n";
    os << "  }\n\n";
  }

  /* **************************** *
   * Serial Ports                 *
   * **************************** */

  /* There are two serial ports (0 and 1); the console's is serial0.
   */
  for (int i = 0; i < 2; i++) {
    /* Port number: a numeric port, or "none" for a bit-bucket
     * (null_attach) UART.
     */
    FreeTextQuestion port_q;
    port_q.setKey("serial" + i2s(i) + ".port");
    port_q.setQuestion("What telnet port should serial " + i2s(i) + " listen?");
    port_q.setOptions("1-65535 or 'none'");
    port_q.setDefault(i == 0 ? "21264" : "none");
    port_q.setExplanation(
        "You will telnet to this port to establish a connection with "
        "emulated serial port " +
        i2s(i) +
        "; the emulator waits at startup until a client connects. "
        "Answer 'none' to make this port a bit-bucket: the UART still "
        "exists on the bus and presents itself to the guest as a healthy "
        "idle 16550, but no telnet listener is opened and any bytes the "
        "guest transmits are silently discarded.");

    bool is_null = false;
    for (;;) {
      port_q.ask();
      if (port_q.getAnswer() == "none") {
        is_null = true;
        break;
      }
      try {
        int v = s2i(port_q.getAnswer());
        if (v >= 1 && v <= 65535)
          break;
      } catch (CLogicException &) {
      }
      cout << "\nPlease enter a port in 1..65535, or 'none'.\n\n";
    }

    os << "  serial" << i << " = serial\n";
    os << "  {\n";
    if (is_null) {
      os << "    null_attach = true;\n";
      os << "  }\n\n";
      continue;
    }
    os << "    port = " << port_q.getAnswer() << ";\n";

    FreeTextQuestion exec_q;
    exec_q.setKey("serial" + i2s(i) + ".program");
    exec_q.setQuestion(
        "What program should be started automatically for serial " + i2s(i) +
        "?");
    exec_q.setDefault("none");
    exec_q.setExplanation("Enter the path to a program to start this to create "
                          "an automatic connection with the serial port "
                          "(PuTTY, xterm). Set to 'none' to establish the "
                          "connection manually (telnet, nc).");

    if (exec_q.ask() != "none") {
      FreeTextQuestion arg_q;
      arg_q.setKey("serial" + i2s(i) + ".args");
      arg_q.setQuestion("What arguments should the program use to connect to "
                        "the serial port?");
      arg_q.setExplanation("Enter the arguments the program needs.");
      /* This is the argument format for PuTTy.
       */
      arg_q.setDefault("telnet://localhost:" + port_q.getAnswer());
      arg_q.ask();
#if defined(_WIN32)
      /* Quote the program path/name, as it may contain spaces.
       */
      string exec = exec_q.getAnswer();
      if (exec.size() >= 2 && exec.front() == '"' && exec.back() == '"')
        exec = exec.substr(1, exec.size() - 2);
      os << "    action = " << quote("\"" + exec + "\" " + arg_q.getAnswer())
         << ";\n";
#else
      os << "    action = "
         << quote(exec_q.getAnswer() + " " + arg_q.getAnswer()) << ";\n";
#endif
    }
    os << "  }\n\n";
  }

  /* **************************** *
   * Floppy Disks                 *
   * **************************** */

  /* The floppy controller is the ISA bridge's and always present. The user
   * only chooses whether to attach any drives to it.
   */
  if (ali) {
    ShrinkingChoiceQuestion fd_q;
    fd_q.setKey("floppy.disk");
    fd_q.setQuestion("Do you want to add any disks to the Floppy controller?");
    fd_q.setDefault("none");
    fd_q.setExplanation("Here, you can add floppy drives to your system.");
    fd_q.addAnswer("none", "", "stop adding disks");
    fd_q.addAnswer("0", "disk0.0", "A:");
    fd_q.addAnswer("1", "disk0.1", "B:");

    os << "  fdc0 = floppy\n";
    os << "  {\n";
    add_disks(&fd_q, &os, "    ", "floppy", true);
    os << "  }\n\n";
  }

  /* **************************** *
   * The board's own functions    *
   * **************************** */

  // The IDE controller and its disks: the ALi's, or the I/O expander's.
  for (const char *ide : {"ali_ide", "cmd649"}) {
    if (const board_function *f = function_of(board, ide)) {
      os << "  " << f->place << " = " << ide << "\n";
      os << "  {\n";
      ask_disks("ide", "IDE controller", &os, 2, 2);
      os << "  }\n\n";
    }
  }

  // The USB controller: the ALi's, which may be left out, or the I/O
  // expander's, the EV7 machines' only keyboard.
  for (const char *usb : {"ali_usb", "uss344"}) {
    const board_function *f = function_of(board, usb);
    if (!f ||
        (!marvel &&
         !ask_yes("usb",
                  "Enable the USB OHCI controller at " + string(f->place) + "?",
                  "The ALi M1543C's USB 1.1 controller.", true)))
      continue;
    os << "  " << f->place << " = " << usb << "\n";
    os << "  {\n";
    int port = 1;
    if (ask_yes("usb.tablet",
                "Should a USB tablet (an absolute pointer) "
                "be plugged in?",
                "For guests with a USB HID stack (Windows 2000 and later, "
                "OpenVMS's DECwindows): the guest's pointer follows the "
                "host's, with no mouse grab.",
                marvel))
      os << "    port" << port++ << " = \"tablet\";\n";
    if (ask_yes("usb.keyboard", "Should a USB keyboard be plugged in?",
                "The EV7 machines have no other keyboard.", marvel))
      os << "    port" << port++ << " = \"keyboard\";\n";
    os << "  }\n\n";
  }

  if (const board_function *f = function_of(board, "ali_pmu")) {
    if (ask_yes("pmu",
                "Enable the M7101 power-management / ACPI device at " +
                    string(f->place) + "?",
                "AliM1543C chipset power management unit", true))
      os << "  " << f->place << " = ali_pmu\n  {\n  }\n\n";
  }

  /* **************************** *
   * PCI slots                    *
   * **************************** */

  /* The board's slots, as its own slot rule says, less the places of its
   * own functions. Use a ShrinkingChoiceQuestion: once a card is using a
   * slot, it can't be used by another card.
   */
  ShrinkingChoiceQuestion pci_q;
  pci_q.setExplanation("Only free PCI slots are listed. pciH.D is hose H, "
                       "device D.");
  for (int hose = 0; hose < board.pci_hoses; hose++) {
    for (int dev = 0; dev < 32; dev++) {
      if (board.slot_refusal(board, hose, dev))
        continue;
      const string place = "pci" + i2s(hose) + "." + i2s(dev);
      bool own = false;
      for (const board_function *f = board.functions; f && f->device_class; f++)
        own |= place == f->place;
      if (!own)
        pci_q.addAnswer(place, place,
                        "Hose " + i2s(hose) + ", device " + i2s(dev));
    }
  }

  /* **************************** *
   * Graphics Card                *
   * **************************** */

  MultipleChoiceQuestion vga_q;
  vga_q.setAnswer("");
  if (gui && pci_q.countAnswers()) {
    vga_q.setKey("vga");
    vga_q.setQuestion(
        "What (if any) graphics card do you wish to add to the system?");
    vga_q.setExplanation(
        "Each runs its real VGA BIOS, which you supply "
        "(docs/configuration.md says which). The S3 Trio64 is the one most "
        "guests know.");
    vga_q.setDefault("s3");
    vga_q.addAnswer("none", "", "No graphics card");
    vga_q.addAnswer("s3", "s3", "S3 Trio64");
    vga_q.addAnswer("cirrus", "cirrus", "Cirrus Logic CL-GD5434 or GD5430");
    vga_q.addAnswer("mach64", "mach64", "ATI Mach64 family (Rage)");
    vga_q.addAnswer("permedia2", "permedia2", "3Dlabs Permedia 2");
    vga_q.addAnswer("s3virge", "s3virge", "S3 ViRGE family");
    vga_q.addAnswer("radeon", "radeon", "ATI Radeon 7500");
    vga_q.addAnswer("tga", "tga", "DEC TGA or TGA2 (no BIOS)");
    vga_q.ask();
  }
  const string vga = vga_q.getAnswer();

  if (vga != "") {
    // The family's part, where it has more than one.
    struct chip_q {
      const char *card, *key, *values;
    };
    static const chip_q chips[] = {
        {"cirrus", "chip", "gd5434 gd5430"},
        {"mach64", "chip", "ct vt2 vt3 rage2p ragepro"},
        {"s3virge", "chip", "dx virge vx gx gx2"},
        {"tga", "model", "e1 3d30 4d20"},
    };
    string chip_line;
    for (const chip_q &c : chips) {
      if (vga != c.card)
        continue;
      MultipleChoiceQuestion q;
      q.setKey("vga." + string(c.key));
      q.setQuestion("Which part should the " + vga + " card be?");
      q.setExplanation("docs/configuration.md describes each.");
      istringstream words(c.values);
      string w;
      while (words >> w) {
        q.addAnswer(w, w, vga + " " + w);
        if (q.countAnswers() == 1)
          q.setDefault(w);
      }
      chip_line = "    " + string(c.key) + " = " + quote(q.ask()) + ";\n";
    }

    pci_q.setKey("vga.slot");
    pci_q.setQuestion("What PCI slot should the " + vga + " card be on?");
    // An AGP card goes in the AGP slot where the board has one.
    const char *agp = !strcmp(board.name, "es47")     ? "pci3.5"
                      : !strcmp(board.name, "es45m1") ? "pci2.5"
                                                      : nullptr;
    pci_q.setDefault(vga == "radeon" && agp && pci_q.hasChoice(agp)
                         ? agp
                         : pci_q.getFirstChoice());
    const string slot = pci_q.ask();

    os << "  " << slot << " = " << vga << "\n";
    os << "  {\n";
    os << chip_line;
    if (vga != "tga") {
      FreeTextQuestion bios_q;
      bios_q.setKey("vga.rom");
      bios_q.setQuestion("Where can the card's VGA BIOS image be found?");
      bios_q.setExplanation("'default' leaves the card's own default file "
                            "name (docs/configuration.md lists them).");
      bios_q.setDefault("default");
      if (bios_q.ask() != "default")
        os << "    rom = " << quote(bios_q.getAnswer()) << ";\n";
    }
    os << "  }\n\n";
  }

  /* **************************** *
   * Free Form PCI Cards          *
   * **************************** */

  MultipleChoiceQuestion card_q;
  card_q.setKey("card");
  card_q.setQuestion("Would you like to add another PCI card to the system?");
  card_q.setDefault("none");
  card_q.setExplanation("Choose what PCI card you'd like to add. Choose none "
                        "if you have no more cards to add.");
  card_q.addAnswer("none", "", "No more cards to add");
  card_q.addAnswer("nic", "nic", "A network interface");
  card_q.addAnswer("scsi", "scsi", "A SCSI controller");
  card_q.addAnswer("ide", "cmd649", "A CMD 649 IDE controller");
  card_q.addAnswer("usb", "usb", "A USB controller (EHCI or OHCI)");
  card_q.addAnswer("virtio_blk", "virtio_blk", "A paravirtual disk");
#if defined(HAVE_SDL)
  card_q.addAnswer("audio", "audio", "An Ensoniq AudioPCI sound card");
#endif

  /* Loop until there are no more PCI
   * cards to add.
   */
  while (pci_q.countAnswers()) {
    /* If this answer has been answered with
     * "none", stop adding PCI cards.
     */
    const string card = card_q.ask();
    if (card == "")
      break;

    /* Which part.
     */
    string cls = card;
    MultipleChoiceQuestion part_q;
    part_q.setKey(card + ".model");
    part_q.setQuestion("Which " + card + " card should it be?");
    part_q.setExplanation("docs/configuration.md describes each.");
    if (card == "nic") {
      part_q.addAnswer("dec21143", "dec21143",
                       "DEC 21143 (DE500-BA), the console's ewa0");
      part_q.addAnswer("dec21140", "dec21140", "DEC 21140A (10/100)");
      part_q.addAnswer("dec21041", "dec21041", "DEC 21041 (10 Mb)");
      part_q.addAnswer("dec21040", "dec21040", "DEC 21040 (10 Mb)");
      part_q.addAnswer("de600", "de600",
                       "DE600-AA (Intel 82559), the console's eia0");
      part_q.addAnswer("i82557", "i82557", "Intel PRO/100, 82557");
      part_q.addAnswer("i82558", "i82558", "Intel PRO/100, 82558");
      part_q.addAnswer("i82559", "i82559", "Intel PRO/100, 82559");
      part_q.addAnswer("i82559er", "i82559er", "Intel 82559ER");
      part_q.addAnswer("virtio_net", "virtio_net", "A paravirtual NIC");
      part_q.setDefault("dec21143");
      cls = part_q.ask();
    } else if (card == "scsi") {
      for (const scsi_part &s : scsi_parts)
        part_q.addAnswer(s.cls, s.cls, s.description);
      part_q.setDefault("sym53c810");
      cls = part_q.ask();
    } else if (card == "usb") {
      part_q.addAnswer("ehci", "ehci",
                       "NEC uPD720101: USB 2.0 with two OHCI companions");
      part_q.addAnswer("uss344", "uss344", "Agere USS-344, USB 1.1");
      part_q.setDefault("ehci");
      cls = part_q.ask();
    } else if (card == "audio") {
      part_q.addAnswer("es1371", "es1371",
                       "Ensoniq ES1371 (AudioPCI 97), the one the console and "
                       "Windows 2000 know");
      part_q.addAnswer("es1370", "es1370", "Ensoniq ES1370 (AudioPCI)");
      part_q.setDefault("es1371");
      cls = part_q.ask();
    }

    /* Determine where to put this card.
     */
    pci_q.setKey(card + ".slot");
    pci_q.setQuestion("In what PCI slot would you like to put the " + cls +
                      " card?");
    pci_q.setDefault(pci_q.getFirstChoice());
    os << "  " << pci_q.ask() << " = " << cls << "\n";
    os << "  {\n";

    if (card == "nic") {
      /* The backend: what this build can use (NetworkBackend.cpp).
       */
      MultipleChoiceQuestion type_q;
      type_q.setKey("nic.type");
      type_q.setQuestion("What network backend type should be used?");
      type_q.setExplanation(
          "pcap captures on a host interface (needs capture permission); "
          "tap is a Linux TAP device (bridgeable, host-reachable); nat is a "
          "user-mode NAT router needing no privileges (libslirp); udp is a "
          "link to another program, one frame per datagram; loopback "
          "returns what is sent; null connects to nothing.");
      static const char *const types[][2] = {
          {"pcap", "capture on a host interface"},
          {"tap", "a Linux TAP device"},
          {"nat", "a user-mode NAT router (DHCP, DNS, TFTP; no privileges)"},
          {"udp", "a UDP link to another program"},
          {"loopback", "a loopback plug"},
          {"null", "not connected"}};
      for (const auto &t : types)
        if (!network_backend_refusal(t[0])) {
          type_q.addAnswer(t[0], t[0], t[1]);
          if (type_q.countAnswers() == 1)
            type_q.setDefault(t[0]);
        }
      const string net_type = type_q.ask();
      os << "    type = " << quote(net_type) << ";\n";

      if (net_type == "pcap") {
#if defined(HAVE_PCAP)
        MultipleChoiceQuestion if_q;
        if_q.setKey("nic.adapter");
        if_q.setQuestion("What host network interface should we connect to?");
        if_q.setExplanation("'ask' leaves the choice to the emulator, which "
                            "then asks at startup.");
        if_q.addAnswer("ask", "", "Ask at startup");
        pcap_if_t *alldevs;
        char errbuf[PCAP_ERRBUF_SIZE];
        if (pcap_findalldevs(&alldevs, errbuf) == -1) {
          printf("Error in pcap_findalldevs: %s\n", errbuf);
        } else {
          for (pcap_if_t *d = alldevs; d; d = d->next)
            if_q.addAnswer(d->name, d->name,
                           d->description ? d->description : "");
        }
        if_q.setDefault("ask");
        if (if_q.ask() != "")
          os << "    adapter = " << quote(if_q.getAnswer()) << ";\n";
#endif
      } else if (net_type == "tap") {
        FreeTextQuestion tap_q;
        tap_q.setKey("nic.adapter");
        tap_q.setQuestion("What TAP device name should be used?");
        tap_q.setExplanation(
            "The TAP device will be created if it doesn't exist "
            "(requires CAP_NET_ADMIN or root).");
        tap_q.setDefault("tap0");
        os << "    adapter = " << quote(tap_q.ask()) << ";\n";

        FreeTextQuestion bridge_q;
        bridge_q.setKey("nic.bridge");
        bridge_q.setQuestion(
            "Bridge to add TAP device to (leave empty for none)?");
        bridge_q.setExplanation(
            "If specified, the TAP device will be added to this bridge.");
        string bridge = bridge_q.ask();
        if (!bridge.empty())
          os << "    bridge = " << quote(bridge) << ";\n";
      } else if (net_type == "udp") {
        static const char *const ends[][3] = {
            {"nic.udp_local", "Where should the NIC listen (address:port)?",
             "127.0.0.1:5555"},
            {"nic.udp_remote", "Where should its frames go (address:port)?",
             "127.0.0.1:5556"}};
        for (const auto &e : ends) {
          FreeTextQuestion q;
          q.setKey(e[0]);
          q.setQuestion(e[1]);
          q.setExplanation("The other end is another program: a second "
                           "Alphabox, QEMU's -netdev dgram, or "
                           "test/tools/net_peer.py.");
          q.setDefault(e[2]);
          os << "    " << (e[0] + 4) << " = " << quote(q.ask()) << ";\n";
        }
      }

      FreeTextQuestion mac_q;
      mac_q.setKey("nic.mac");
      mac_q.setQuestion("What should the NIC's MAC address be (blank for the "
                        "default)?");
      mac_q.setExplanation("This should be unique on your network. The "
                           "default, 08-00-2B-E5-40-<n>, counts the NICs.");
      if (mac_q.ask() != "")
        os << "    mac = " << quote(mac_q.getAnswer()) << ";\n";
    } else if (card == "scsi") {
      for (const scsi_part &s : scsi_parts)
        if (cls == s.cls)
          ask_disks("scsi", cls + " controller", &os, s.buses, s.units,
                    s.units > 8 ? 7 : -1);
    } else if (card == "cmd649") {
      ask_disks("ide", "IDE controller", &os, 2, 2);
    } else if (card == "virtio_blk") {
      ask_disks("virtio_blk", "paravirtual disk controller", &os, 1, 1);
    }
    os << "  }\n\n";
  }

  /* **************************** *
   * The ISA bridge               *
   * **************************** */

  if (ali) {
    MultipleChoiceQuestion vgacons_q;
    vgacons_q.setKey("console");
    vgacons_q.setQuestion("Where would you like console output to go?");
    vgacons_q.setExplanation("This is the SRM console option");
    vgacons_q.addAnswer("serial", "false", "Console on serial port 0");
    vgacons_q.addAnswer("graphics", "true", "Console on graphics controller");
    vgacons_q.setDefault("graphics");

    if (vga != "") {
      /* If a VGA card is present, ask about
       * the console.
       */
      vgacons_q.ask();
    } else {
      /* No VGA card present, the console goes
       * to serial port 0.
       */
      vgacons_q.setAnswer("false");
    }

    FreeTextQuestion lpt_q;
    lpt_q.setKey("lpt");
    lpt_q.setQuestion("Where would you like printer output to go?");
    lpt_q.setExplanation("Output from the printer port will be saved to this "
                         "file. Leave blank if not wanted.");
    lpt_q.ask();

    os << "  " << ali->place << " = ali\n";
    os << "  {\n";
    os << "    vga_console = " << vgacons_q.getAnswer() << ";\n";
    if (lpt_q.getAnswer() != "")
      os << "    lpt.outfile = " << quote(lpt_q.getAnswer()) << ";\n";
    os << "  }\n\n";

    MultipleChoiceQuestion arc_compat_q;
    arc_compat_q.setKey("arc_year");
    arc_compat_q.setQuestion(
        "Which OS the reported year should be compatible with?");
    arc_compat_q.setExplanation(
        "This only affects the year reported to the guest. Use \"nt\" if "
        "planning to run Windows OSes.");
    arc_compat_q.addAnswer(
        "nt", "true",
        "Report the year as an offset from 1980, as expected "
        "by Windows OSes and AlphaBIOS.");
    arc_compat_q.addAnswer(
        "vms", "false",
        "Report the year as expected by OpenVMS, Tru64 UNIX, Linux and BSDs.");
    arc_compat_q.setDefault("vms");
    os << "  arc_year_compat = " << arc_compat_q.ask() << ";\n";

    if (ask_yes("time",
                "Do you want to set a fixed date and time when the VM starts?",
                "By default, the VM's date and time is initialized to the "
                "current host date and time at startup. If you want to set a "
                "fixed date and time instead, you can do so here.",
                false)) {
      FreeTextQuestion datetime_q;
      datetime_q.setKey("time.value");
      datetime_q.setQuestion("What date and time should the VM have at "
                             "startup? (format: YYYY-MM-DD HH:MM:SS)");
      datetime_q.setExplanation(
          "Enter the date and time in the format shown above. For example, "
          "'2000-01-01 12:00:00' for January 1st, 2000 at noon.");
      for (;;) {
        datetime_q.ask();
        struct tm ft = {};
        int n = sscanf(datetime_q.getAnswer().c_str(), "%d-%d-%d %d:%d:%d",
                       &ft.tm_year, &ft.tm_mon, &ft.tm_mday, &ft.tm_hour,
                       &ft.tm_min, &ft.tm_sec);
        if (n < 3) {
          cout << "\nInvalid date/time format.\n";
          continue;
        }
        char datestr[32];
        snprintf(datestr, sizeof(datestr), "%04d-%02d-%02d %02d:%02d:%02d",
                 ft.tm_year, ft.tm_mon, ft.tm_mday, ft.tm_hour, ft.tm_min,
                 ft.tm_sec);
        os << "  time = " << quote(datestr) << ";\n";
        break;
      }
    }
  }

  if (ask_yes("exit_on_halt",
              "Should the emulator exit when the guest halts to the console?",
              "Handy for scripted runs: an OS shutting down ends the run, "
              "which saves the flash and NVRAM images.",
              false))
    os << "  exit_on_pal_halt = true;\n";

  os << "}\n";

  /* Every answer given beforehand must have been asked for: a misspelt
   * key would otherwise be ignored without a word.
   */
  const string unused = Question::unusedAnswers();
  if (!unused.empty())
    throw CAbortException("answers given for questions that were not asked: " +
                          unused);

  /* Write the file.
   */
  ofstream outfile(out, ios::out | ios::trunc);
  outfile << os.str();
  outfile.close();
  if (!outfile) {
    printf("Writing %s failed: %s.\n", out.c_str(), strerror(errno));
    return 1;
  }
  printf("\n%s written.\n", out.c_str());

  /* And check it, as `alphabox run --check` does.
   */
  const string text = os.str();
  CConfigurator::set_check_mode(true);
  int errors = 0;
  try {
    new CConfigurator(0, 0, 0, text.data(), text.size(), out.c_str());
    errors = CConfigurator::check_errors();
  } catch (CException &e) {
    fprintf(stderr, "%%SYS-E-CONFIG: %s\n", e.text().c_str());
    errors++;
  }
  if (errors) {
    printf("%s does not pass alphabox run --check: %d error%s.\n", out.c_str(),
           errors, errors == 1 ? "" : "s");
    return 1;
  }
  printf("%s passes alphabox run --check.\n", out.c_str());
  return 0;
}

static void configure_usage() {
  fprintf(stderr,
          "usage: alphabox configure [--out FILE] [--set KEY=VALUE]... "
          "[--answers FILE] [--defaults]\n"
          "  --out FILE        the file to write (default: es40.cfg)\n"
          "  --set KEY=VALUE   answer the question KEY beforehand; a key asked "
          "more than\n"
          "                    once (card, disk.type) takes its answers in "
          "order\n"
          "  --answers FILE    KEY = VALUE lines (# comments), the same as "
          "--set\n"
          "  --defaults        take the default for every question not "
          "answered\n"
          "                    beforehand: a session that asks nothing\n"
          "Each question shows its key in braces; docs/configuration.md "
          "lists them.\n");
}

/**
 * Main program entry point.
 **/
int main_cfg(int argc, char *argv[]) {
#if defined _WIN32 && defined HAVE_PCAP
  if (!load_wpcap()) {
    printf("Failed to load wpcap.dll; is Npcap installed?\n");
    return -1;
  }
#endif
  string out = "es40.cfg";
  for (int i = 1; i < argc; i++) {
    const string a = argv[i];
    const bool has_arg = i + 1 < argc;
    if ((a == "--out" || a == "-o") && has_arg) {
      out = argv[++i];
    } else if (a == "--defaults") {
      Question::useDefaults(true);
    } else if (a == "--set" && has_arg) {
      const string kv = argv[++i];
      const size_t eq = kv.find('=');
      if (eq == string::npos || eq == 0) {
        fprintf(stderr, "alphabox configure: --set %s: not KEY=VALUE\n",
                kv.c_str());
        return 2;
      }
      Question::queueAnswer(kv.substr(0, eq), kv.substr(eq + 1));
    } else if (a == "--answers" && has_arg) {
      ifstream f(argv[++i]);
      if (!f) {
        fprintf(stderr, "alphabox configure: cannot read %s\n", argv[i]);
        return 2;
      }
      string line;
      for (int n = 1; getline(f, line); n++) {
        const size_t s = line.find_first_not_of(" \t\r");
        if (s == string::npos || line[s] == '#')
          continue;
        const size_t eq = line.find('=');
        if (eq == string::npos) {
          fprintf(stderr, "alphabox configure: %s:%d: not KEY = VALUE\n",
                  argv[i], n);
          return 2;
        }
        auto trim = [](string t) {
          const size_t b = t.find_first_not_of(" \t\r");
          const size_t e = t.find_last_not_of(" \t\r");
          return b == string::npos ? string() : t.substr(b, e - b + 1);
        };
        Question::queueAnswer(trim(line.substr(0, eq)),
                              trim(line.substr(eq + 1)));
      }
    } else if (a == "--help" || a == "-h") {
      configure_usage();
      return 0;
    } else {
      fprintf(stderr, "alphabox configure: unknown argument %s\n", a.c_str());
      configure_usage();
      return 2;
    }
  }
  try {
    return configure_session(out);
  } catch (CAbortException &e) {
    printf("\n%s: %s was not written.\n", e.text().c_str(), out.c_str());
    return 1;
  }
}
