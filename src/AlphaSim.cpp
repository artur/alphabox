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

#include "DPR.hpp"
#include "Flash.hpp"
#include "StdAfx.hpp"
#ifdef ALPHABOX_HVF
#include "HvRuntime.hpp"
#endif
#include "System.hpp"
#include "banner.hpp"

#if defined(HAVE_SDL)
#include "SDL3/SDL.h"
#endif

/// "standard" locations for a configuration file.  This will be port specific.
const char *path[] = {
#if defined(_WIN32)
    ".\\es40.cfg", "c:\\es40.cfg", "c:\\windows\\es40.cfg",
#elif defined(__VMS)
    "[]ES40.CFG",
#else
    "./es40.cfg", "/etc/es40.cfg", "/usr/etc/es40.cfg",
    "/usr/local/etc/es40.cfg",
#endif
    0};

#ifdef DEBUG_BACKTRACE
#ifdef __GNUG__
#include <execinfo.h>
#include <signal.h>
#define HAS_BACKTRACE

#define BTCOUNT 100
void *btbuffer[BTCOUNT];

void segv_handler(int signum) {
  int nptrs = backtrace(btbuffer, BTCOUNT);
  char **strings;

  printf("%%SYS-F-SEGFAULT: The Alpha Simulator has Segfaulted.\n");
  printf("-SYS-F-SEGFAULT: Backtrace follows.\n");

  printf("backtrace() returned %d addresses.\n", nptrs);
  strings = backtrace_symbols(btbuffer, nptrs);
  if (strings == NULL) {
    perror("backtrace_symbols");
    exit(1);
  }

  for (int i = 0; i < nptrs; i++) {
    printf("%3d %s\n", nptrs - i, strings[i]);
  }

  free(strings);
  if (signum == SIGSEGV)
    _exit(1);
}

#else
#warning "Your compiler isn't configured to support backtraces."
#endif // __GNUG__
#endif

/**
 * Read the configuration file and build the configuration tree (and, unless
 * in check mode, the machine it describes).
 **/
static CConfigurator *load_configuration(const char *filename) {
  // Read to the end rather than ask for the size first: a pipe
  // (alphabox run <(generator)) has none.
  FILE *f = fopen(filename, "rb");
  if (f == NULL)
    FAILURE_2(File, "cannot open the configuration file %s: %s", filename,
              strerror(errno));
  std::string text;
  char buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
    text.append(buf, n);
  const int err = ferror(f) ? errno : 0;
  fclose(f);
  if (err)
    FAILURE_2(File, "cannot read the configuration file %s: %s", filename,
              strerror(err));
  return new CConfigurator(0, 0, 0, text.data(), text.size(), filename);
}

/**
 * `alphabox run --check` and `--dump-config`: build the configuration tree
 * with every rule applied but no device constructed (Configurator.hpp),
 * report what is wrong on stderr, and for --dump-config write the resolved
 * tree to stdout. The exit status is 0 when there was no error.
 **/
static int check_configuration(const char *filename, bool dump) {
  CConfigurator::set_check_mode(true);
  CConfigurator *cfg = nullptr;
  int errors = 0;
  try {
    cfg = load_configuration(filename);
    errors = CConfigurator::check_errors();
    if (!CConfigurator::check_found_system()) {
      fprintf(stderr, "%%SYS-E-CONFIG: no system initialized\n");
      errors++;
    }
  } catch (CException &e) {
    fprintf(stderr, "%%SYS-E-CONFIG: %s\n", e.text().c_str());
    errors = CConfigurator::check_errors() + 1;
  }
  if (dump && cfg)
    cfg->dump(stdout);
  const int warnings = CConfigurator::check_warnings();
  fprintf(stderr, "%%SYS-I-CHECK: %s: %d error%s, %d warning%s\n", filename,
          errors, errors == 1 ? "" : "s", warnings, warnings == 1 ? "" : "s");
  return errors ? 1 : 0;
}

static void run_usage() {
  fprintf(stderr,
          "usage: alphabox run [--check | --dump-config] [file]\n"
          "  file           the configuration (default: es40.cfg here, then "
          "in /etc,\n"
          "                 /usr/etc and /usr/local/etc)\n"
          "  --check        validate the configuration without starting the "
          "machine\n"
          "  --dump-config  as --check, and write the resolved configuration "
          "to stdout\n");
}

/**
 * Entry point for simulation.
 *
 * Does the following:
 *  - Try to find the configuration file.
 *  - Reads the configuration file, and uses the configurator to instantiate all
 *system components.
 *  - Runs the emulator.
 *  - Cleans up.
 *  .
 * Returns 0 when the run ended gracefully (or a check found no error), 1
 * after a failure, 2 for a wrong command line.
 **/
int main_sim(int argc, char *argv[]) {
  const char *filename = 0;
  FILE *f;
  bool check = false;
  bool dump = false;

  // Options come before the file name.
  int argi = 1;
  for (; argi < argc && !strncmp(argv[argi], "--", 2); argi++) {
    if (!strcmp(argv[argi], "--check"))
      check = true;
    else if (!strcmp(argv[argi], "--dump-config"))
      check = dump = true;
    else if (!strcmp(argv[argi], "--help")) {
      run_usage();
      return 0;
    } else {
      fprintf(stderr, "alphabox run: unknown option %s\n", argv[argi]);
      run_usage();
      return 2;
    }
  }
  // What follows: the file name.
  const int nargs = argc - argi;
  char **args = argv + argi;
  if (nargs > 1) {
    fprintf(stderr, "alphabox run: more than one configuration file given\n");
    run_usage();
    return 2;
  }

  if (!check)
    print_alphabox_banner("Alphabox Alpha Emulator");

#if !defined(_WIN32)
  // A telnet client that goes away must not take the emulator with it: the
  // default action for SIGPIPE is to terminate the process, and the serial
  // console writes to that socket. Ignoring it turns the write into a plain
  // EPIPE return, and the receive path already notices the closed peer and
  // goes back to waiting for a new connection.
  signal(SIGPIPE, SIG_IGN);
#endif

#ifdef HAS_BACKTRACE
  signal(SIGSEGV, &segv_handler);
  signal(SIGUSR1, &segv_handler);
#endif
#ifdef ALPHABOX_HVF
  // ALPHABOX_HV=1: every CPU thread runs its dispatch loop at EL1 inside a
  // Hypervisor.framework VM (docs/hypervisor.md). stdout goes unbuffered:
  // a printf from inside writes through a private copy of stdio's state,
  // so each one must reach the file descriptor on its own.
  if (const char *e = getenv("ALPHABOX_HV")) {
    if (!check && atoi(e) != 0 && hv::init()) {
      hv::enable();
      setvbuf(stdout, nullptr, _IONBF, 0);
      printf("%%HV-I-ENABLED: CPU threads run at EL1 under "
             "Hypervisor.framework\n");
      atexit([] {
        const hv::Stats &s = hv::stats();
        printf("%%HV-I-STATS: %llu entries, %llu escapes, %llu stage-1 faults, "
               "%llu syscalls, %llu pages copied\n",
               (unsigned long long)s.entries, (unsigned long long)s.escapes,
               (unsigned long long)s.stage1_faults,
               (unsigned long long)s.syscalls,
               (unsigned long long)s.pages_copied);
      });
    }
  }
#endif
  try {
    if (nargs == 1) {
      filename = args[0];
    } else {
      for (int i = 0; path[i]; i++) {
        filename = path[i];
        f = fopen(filename, "r");
        if (f != NULL) {
          fclose(f);
          filename = path[i];
          break;
        } else {
          filename = NULL;
        }
      }
      if (filename == NULL)
        FAILURE(FileNotFound, "configuration file");
    }
    if (check)
      return check_configuration(filename, dump);

    load_configuration(filename);

    if (!theSystem)
      FAILURE(Configuration, "no system initialized");

    theSystem->LoadROM();
    if (theDPR) // the ES40's management-processor RAM; not every board has one
      theDPR->init();

#if defined(PROFILE)
    {
      u64 p_i;
      for (p_i = PROFILE_FROM; p_i < PROFILE_TO; p_i += (4 * PROFILE_BUCKSIZE))
        PROFILE_BUCKET(p_i) = 0;
      profiled_insts = 0;
    }
#endif
    theSystem->Run();
  } catch (CGracefulException &e) {
    printf("Exiting gracefully: %s\n", e.displayText().c_str());

    theSystem->stop_threads();

    // save flash and dpr rom only if not terminated with a fatal error
    // (a failure early in startup can leave either unconstructed)
    if (theSROM)
      theSROM->SaveStateF();
    if (theDPR)
      theDPR->SaveStateF();

#if defined(PROFILE)
    {
      FILE *p_fp;
      u64 p_max = 0;
      u64 p_i;
      int p_j;

      printf("Writing profile to profile.txt");

      p_fp = fopen("profile.txt", "w");
      for (p_i = PROFILE_FROM; p_i < PROFILE_TO;
           p_i += (4 * PROFILE_BUCKSIZE)) {
        if (PROFILE_BUCKET(p_i) > p_max)
          p_max = PROFILE_BUCKET(p_i);
      }
      fprintf(p_fp,
              "p_max = %10" PRId64 "; %10" PRId64 " profiled instructions.\n\n",
              p_max, profiled_insts);
      for (p_i = PROFILE_FROM; p_i < PROFILE_TO;
           p_i += (4 * PROFILE_BUCKSIZE)) {
        if (PROFILE_BUCKET(p_i)) {
          fprintf(p_fp, "%016" PRIx64 ": %10" PRId64 " ", p_i,
                  PROFILE_BUCKET(p_i));
          for (p_j = 0;
               p_j < (((float)PROFILE_BUCKET(p_i) / (float)p_max) * 100); p_j++)
            fprintf(p_fp, "*");
          fprintf(p_fp, "\n");
        }
      }
      fclose(p_fp);
    }
#endif
    delete theSystem;
  } catch (CException &e) {
    printf("Emulator Failure: %s\n", e.displayText().c_str());
    if (theSystem) {
      theSystem->stop_threads();
      delete theSystem;
    }
    return 1;
  }
  return 0;
}
