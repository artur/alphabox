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
 * Contains the definitions for the emulated Serial Port devices.
 **/
#if !defined(INCLUDED_SERIAL_H)
#define INCLUDED_SERIAL_H

#include "SystemComponent.hpp"
#include "Uart16550.hpp"
#include "telnet.hpp"
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>

#define STAGE_SIZE 8192

/**
 * \brief Emulated serial port.
 *
 * The serial port is translated to a telnet port.
 **/
class CSerial : public CSystemComponent {
public:
  void write(const char *s, int dsize);
  void write_cstr(const char *s);
  virtual void WriteMem(int index, u64 address, int dsize, u64 data);
  virtual u64 ReadMem(int index, u64 address, int dsize);
  CSerial(CConfigurator *cfg, CSystem *c, u16 number);
  virtual ~CSerial();
  int receive(const char *data, int dsize);
  virtual void check_state();
  virtual int SaveState(FILE *f);
  virtual int RestoreState(FILE *f);
  void eval_interrupts();
  void WaitForConnection();
  virtual void run();
  void execute();

  virtual void init();
  virtual void start_threads();
  virtual void stop_threads();

private:
  void serial_menu();
  void drain_staging();
  bool launch_action(std::string &why);
  void configure_client_socket();
  bool send_output();
  void wait_writable(std::chrono::steady_clock::time_point until);
  void flush_output(int ms);
  void menu_say(const char *s);
  void drop_client();

  std::unique_ptr<std::thread> myThread;
  std::atomic_bool myThreadDead{false};
  std::atomic_bool StopThread{false};

  /// Guest output on its way to the client (write(), send_output()).
  std::mutex out_lock;
  std::condition_variable out_cv;
  std::string out_queue;   ///< queued by write(), under out_lock
  bool out_kick = false;   ///< the queue grew since the thread last looked
  size_t out_dropped = 0;  ///< oldest bytes discarded, not yet reported
  std::string out_sending; ///< the thread's: taken from the queue, unsent
  size_t out_sent = 0;     ///< how much of out_sending the socket took
  /// A telnet BREAK arrived: set by the port's thread, acted on by
  /// check_state.
  std::atomic_bool breakHit{false};
  const char *listenAddress = nullptr;
  bool threads_started = false; ///< past init: a failing `action` only warns
#if !defined(_WIN32)
  pid_t action_pid = -1; ///< the last `action` program started
#endif

  unsigned char
      iac_carry[8];  // Partial telnet sequence carried across recv() calls
  int iac_carry_len; // Number of valid bytes in iac_carry
  bool in_subneg;    // Currently inside IAC SB ... IAC SE subnegotiation

  char stageBuf[STAGE_SIZE]; // Cooked data waiting for baud-rate delivery
  int stageLen;              // Number of valid bytes in stageBuf

  /// The registers and receive FIFO, as the state file stores them.
  SSrl_state state;
  /// The 16550 model working on `state` (declared after it).
  CUart16550 uart;
  int listenPort = 0;
  int64_t listenSocket;
  int64_t connectSocket;
  bool disabled =
      false; ///< If true, port is not exposed to guest; reads return 0xff,
             ///< writes ignored. Used to skip KDCOM probe on AXP64 2210 etc.
  bool raw_mode =
      false; ///< If true, skip telnet IAC processing and connect banner. Use
             ///< for windbg/kgdb where the byte stream must be 8-bit clean.
  bool null_attach =
      false; ///< If true, port exists on the bus but no socket is opened and no
             ///< I/O thread runs. Guest sees a healthy idle 16550 (THRE/TSRE,
             ///< CTS/DSR); TX bytes are silently dropped; RX FIFO is
             ///< permanently empty. MCR.LOOP self-test still works (no socket
             ///< touched). Use when the guest expects a UART to exist but you
             ///< don't want a telnet listener — bit-bucket semantics, like
             ///< QEMU's -serial null.
};
#endif // !defined(INCLUDED_SERIAL_H)
