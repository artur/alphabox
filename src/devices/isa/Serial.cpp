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
 * Contains the code for the emulated Serial Port devices.
 **/

// =============================================================================
// Design:
//
//   socket --> recv() --> raw buf --> cook IAC --> staging buffer --> FIFO
//                                                      |              |
//                                                      |              v
//                                                 every execute()   guest reads
//                                                   (~20 ms)       via ReadMem
//
//   - recv() and IAC cooking happen every RECV_TICKS cycles (~200ms).
//     Cooked data goes into the staging buffer.
//   - drain_staging() runs every execute() cycle (~20ms) but only
//     delivers one character when the guest has consumed the previous
//     one (FIFO empty).  This self-clocks to the emulated CPU speed,
//     matching how a real UART works — the next character doesn't
//     arrive until the line shifts it in, and the CPU is always fast
//     enough to read it before then.
//   - If staging still has data, recv() is skipped — TCP provides
//     backpressure at the OS level.
// =============================================================================

#include "Serial.hpp"
#include "AliM1543C.hpp"
#include "StdAfx.hpp"
#include "System.hpp"

#include <chrono>
#include <string>
#include <vector>
#if !defined(_WIN32)
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;
#endif

#define UART_BASE_CLOCK 1843200
#define CYCLE_TIME_MS 20

#define RECV_TICKS 10

int iCounter = 0;

#define FIFO_SIZE 1024

/// Output kept for a client that is not reading (see write()).
static const size_t OUT_QUEUE_MAX = 256 * 1024;

#if defined(MSG_NOSIGNAL)
static const int SEND_FLAGS = MSG_NOSIGNAL;
#else
static const int SEND_FLAGS = 0;
#endif

//#define DEBUG_SERIAL 1

/**
 * Constructor.
 **/
CSerial::CSerial(CConfigurator *cfg, CSystem *c, u16 number)
    : CSystemComponent(cfg, c),
      // IRQ4 for serial0 (COM1), IRQ3 for serial1 (COM2), driven as a level
      // that follows the cause: pic_set_line makes one edge per rising
      // transition. A board without the ALi (the ES47, whose CMM reads the
      // port by polling) has no line to drive.
      uart(
          state,
          [this](bool active) {
            if (theAli)
              theAli->pic_set_line(0, 4 - state.iNumber, active);
          },
          [this](u8 byte) { write((const char *)&byte, 1); }) {
  state.iNumber = number;
}

/**
 * Initialize the Serial port device.
 **/
void CSerial::init() {
  disabled = myCfg->get_bool_value("disabled");
  raw_mode = myCfg->get_bool_value("raw_mode");
  null_attach = myCfg->get_bool_value("null_attach");
  if (!(listenAddress = myCfg->get_text_value("address")))
    listenAddress = "0.0.0.0";

  char s[1000];
  char *nargv = s;
  int i = 0;

  cSystem->RegisterMemory(this, 0,
                          U64(0x00000801fc0003f8) - (0x100 * state.iNumber), 8);

  if (disabled) {
    // Port is claimed in the address map but presents itself as a missing UART:
    // scratchpad writes don't stick, MSR/LSR read 0xff, IIR=0xff. KDCOM and
    // SERIAL.SYS conclude "no UART here" and skip further probing/polling.
    state.rcvW = 0;
    state.rcvR = 0;
    listenSocket = INVALID_SOCKET;
    connectSocket = INVALID_SOCKET;
    printf("%s: disabled - guest will see no UART at this address.\n",
           devid_string);
    return;
  }

  if (null_attach) {
    // Bit-bucket UART: port exists in the address map and presents itself as a
    // healthy idle 16550 — TX shift register always empty (THRE/TSRE), modem
    // signals up (CTS/DSR), RX FIFO permanently empty. TX bytes from the guest
    // are silently discarded; nothing ever arrives at RX. No socket is opened,
    // no I/O thread runs, no <BREAK> menu. ReadMem/WriteMem stay on the normal
    // path so register reads return live values (e.g. LSR=0x60 because
    // rcvR==rcvW), MCR.LOOP self-test still works (loopback writes go to
    // rcvBuffer, never the socket), and TX-empty interrupts still fire when the
    // guest enables IER.bit1.
    uart.reset();
    iac_carry_len = 0;
    in_subneg = false;
    stageLen = 0;
    myThread = nullptr;
    listenSocket = INVALID_SOCKET;
    connectSocket = INVALID_SOCKET;
    printf("%s: null_attach - TX discarded, RX always empty.\n", devid_string);
    return;
  }

  // Start Telnet server
#if defined(_WIN32)

  // Windows Sockets only work after calling WSAStartup.
  WSADATA wsa;
  WSAStartup(0x0101, &wsa);
#endif // defined (_WIN32)
  struct sockaddr_in Address;

  socklen_t nAddressSize = sizeof(struct sockaddr_in);

  const u64 port = myCfg->get_num_value("port", false, 8000 + state.iNumber);
  if (port < 1 || port > 65535)
    FAILURE_2(Configuration, "%s: port %" PRIu64 " is not a TCP port (1-65535)",
              devid_string, port);
  listenPort = (int)port;

  // Resolved rather than parsed, so a host name such as "localhost" works,
  // and an address that does not resolve stops here: it used to fall back
  // to every interface, which opened the console (and its BREAK menu) to
  // the network when the user had asked for one address.
  addrinfo hints = {};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;
  addrinfo *res = nullptr;
  const int gai = getaddrinfo(listenAddress, nullptr, &hints, &res);
  if (gai || !res)
    FAILURE_3(Configuration, "%s: cannot listen on address \"%s\": %s",
              devid_string, listenAddress,
              gai ? gai_strerror(gai) : "no IPv4 address");
  memcpy(&Address, res->ai_addr, sizeof(Address));
  freeaddrinfo(res);
  Address.sin_port = htons((u16)listenPort);

  listenSocket = (int)socket(AF_INET, SOCK_STREAM, 0);
  if (listenSocket == INVALID_SOCKET)
    FAILURE_1(Runtime, "Serial: can't create a socket: %s\n", strerror(errno));

  int optval = 1;
  setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, (char *)&optval,
             sizeof(optval));
  // Without these checks a busy port went unnoticed: listen() on an unbound
  // socket binds an ephemeral one, so the emulator announced the configured
  // port and then waited for a connection that could never arrive.
  if (bind(listenSocket, (struct sockaddr *)&Address, sizeof(Address)))
    FAILURE_2(Runtime, "Serial: can't bind port %d: %s\n", listenPort,
              strerror(errno));
  if (listen(listenSocket, 8))
    FAILURE_2(Runtime, "Serial: can't listen on port %d: %s\n", listenPort,
              strerror(errno));

  printf("%s: Waiting for connection on port %d.\n", devid_string, listenPort);

  WaitForConnection();

  uart.reset();
  iac_carry_len = 0;
  in_subneg = false;
  stageLen = 0;
  myThread = nullptr;

  printf("%s: $Id$\n", devid_string);
}

void CSerial::start_threads() {
  char buffer[5];
  if (disabled || null_attach)
    return;
  if (!myThread) {
    sprintf(buffer, "srl%d", state.iNumber);
    printf(" %s", buffer);
    StopThread = false;
    threads_started = true;
    myThread = std::make_unique<std::thread>([this]() { this->run(); });
  }
}

void CSerial::stop_threads() {
  char buffer[5];
  if (disabled || null_attach)
    return;
  StopThread = true;
  {
    std::lock_guard<std::mutex> g(out_lock);
    out_cv.notify_one();
  }
  if (myThread) {
    sprintf(buffer, "srl%d", state.iNumber);
    printf(" %s", buffer);
    // Always join: the thread notices StopThread within 200 ms, also while it
    // waits for a telnet connection (see WaitForConnection). Destroying a
    // joinable std::thread would terminate the process.
    myThread->join();
    myThread = nullptr;
  }
}

/**
 * Destructor.
 **/
CSerial::~CSerial() { stop_threads(); }

// ALPHABOX_TRACE_SERIAL=<port number>: every register write on that port,
// with how many times the guest read IIR since the previous write and what
// the first of those reads returned -- a routine spinning on IIR shows as
// one write followed by millions of reads of one value.
static const int s_trace_serial = getenv("ALPHABOX_TRACE_SERIAL")
                                      ? atoi(getenv("ALPHABOX_TRACE_SERIAL"))
                                      : -1;
static u64 s_ser_iir_reads = 0;
static int s_ser_iir_first = -1;

u64 CSerial::ReadMem(int index, u64 address, int dsize) {
  if (disabled)
    return 0xffu; // missing-UART signature

  const u8 value = uart.read((u32)address);
  if (s_trace_serial == state.iNumber && address == 2) {
    if (s_ser_iir_first < 0)
      s_ser_iir_first = value;
    s_ser_iir_reads++;
  }
  return value;
}

void CSerial::WriteMem(int index, u64 address, int dsize, u64 data) {
  const u8 d = (u8)data;

  if (disabled)
    return; // ignore guest writes to a disabled port

  if (s_trace_serial == state.iNumber) {
    printf("SERT port %d: IIR reads since last write %llu (first %02x); write "
           "reg %llu = %02x (LCR %02x IER %02x FCR %02x)\n",
           state.iNumber, (unsigned long long)s_ser_iir_reads,
           s_ser_iir_first < 0 ? 0 : s_ser_iir_first, (unsigned long long)address,
           d, state.bLCR, state.bIER, state.bFCR);
    s_ser_iir_reads = 0;
    s_ser_iir_first = -1;
  }
  const u8 prev_mcr = state.bMCR;
  uart.write((u32)address, d);
  const u8 mcr = state.bMCR;
  if (address == 4 && prev_mcr != mcr)
    printf("SRL%d: MCR %02x -> %02x (LOOP=%d DTR=%d RTS=%d OUT1=%d OUT2=%d), "
           "MSR now %02x\n",
           state.iNumber, prev_mcr, mcr, (mcr >> 4) & 1, mcr & 1,
           (mcr >> 1) & 1, (mcr >> 2) & 1, (mcr >> 3) & 1, state.bMSR);
}

void CSerial::eval_interrupts() {
  if (!disabled)
    uart.update();
}

/**
 * Queue bytes for the client. The processors only append here (a THR
 * write arrives with the UART's lock held); the port's thread hands the
 * queue to the socket (send_output). A client that stops reading therefore
 * costs queue space, never a processor blocked in send(): the queue keeps
 * its newest bytes, as a terminal keeps the end of its scrollback.
 **/
void CSerial::write(const char *s, int dsize) {
  if (disabled || null_attach || dsize <= 0)
    return; // null_attach: drop TX silently; no socket to send on
  std::lock_guard<std::mutex> g(out_lock);
  out_queue.append(s, (size_t)dsize);
  if (out_queue.size() > OUT_QUEUE_MAX) {
    // The oldest quarter goes at once, not a byte per write: each erase
    // moves the whole queue.
    const size_t excess = out_queue.size() - OUT_QUEUE_MAX * 3 / 4;
    out_queue.erase(0, excess);
    out_dropped += excess;
  }
  if (!out_kick) {
    out_kick = true;
    out_cv.notify_one();
  }
}

/**
 * Hand output to the client without blocking. The port's thread owns
 * out_sending, what it took from the queue and the socket has not taken
 * yet, so a slow client costs no copying under the lock. Returns false
 * when the socket is full: the caller then waits for room (wait_writable)
 * rather than trying again at once. Called on the port's thread, or with
 * it stopped (the BREAK menu).
 **/
bool CSerial::send_output() {
  if (connectSocket == INVALID_SOCKET)
    return true;
  for (;;) {
    if (out_sent == out_sending.size()) {
      out_sending.clear();
      out_sent = 0;
      size_t dropped;
      {
        std::lock_guard<std::mutex> g(out_lock);
        out_sending.swap(out_queue);
        dropped = out_dropped;
        out_dropped = 0;
      }
      if (dropped)
        printf("%%SRL-W-DROPPED: %s: %zu bytes of output dropped, with no "
               "client reading them.\n",
               devid_string, dropped);
      if (out_sending.empty())
        return true;
    }
    const ssize_t n = send(connectSocket, out_sending.data() + out_sent,
                           out_sending.size() - out_sent, SEND_FLAGS);
    if (n > 0)
      out_sent += (size_t)n;
    else if (n < 0 && errno == EINTR)
      continue;
    else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      return false;
    else
      // A peer that vanished (EPIPE; SIGPIPE is ignored, see main_sim):
      // the receive side notices the closed socket and reconnects.
      return true;
  }
}

/// Wait until the client socket has room again, or until `until`.
void CSerial::wait_writable(std::chrono::steady_clock::time_point until) {
  const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           until - std::chrono::steady_clock::now())
                           .count();
  const int timeout = ms < 1 ? 1 : (int)ms;
#if defined(_WIN32)
  WSAPOLLFD p = {(SOCKET)connectSocket, POLLWRNORM, 0};
  WSAPoll(&p, 1, timeout);
#else
  pollfd p = {(int)connectSocket, POLLOUT, 0};
  poll(&p, 1, timeout);
#endif
}

/// Send everything now, waiting up to `ms` for a slow client: the BREAK
/// menu's prompts and the last output before the port's thread stops.
void CSerial::flush_output(int ms) {
  const auto end =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (!send_output() && std::chrono::steady_clock::now() < end)
    wait_writable(end);
}

/**
 * The client went away. Its socket is closed (it used to be left open, one
 * leaked descriptor per reconnect), and what it had not received yet is
 * kept for the next client, with what the guest prints meanwhile -- except
 * in raw_mode, where a new debugger session must not start with the old
 * one's packets.
 **/
void CSerial::drop_client() {
#if defined(_WIN32)
  closesocket(connectSocket);
#else
  close((int)connectSocket);
#endif
  connectSocket = INVALID_SOCKET;
  std::lock_guard<std::mutex> g(out_lock);
  if (raw_mode)
    out_queue.clear();
  else
    out_queue.insert(0, out_sending, out_sent, std::string::npos);
  out_sending.clear();
  out_sent = 0;
}

void CSerial::write_cstr(const char *s) { write(s, (int)strlen(s)); }

/**
 * Received bytes into the UART's FIFO. Returns how many fit.
 **/
int CSerial::receive(const char *data, int dsize) {
  return uart.receive((const u8 *)data, dsize);
}

/**
 * drain_staging  — Move data from the staging buffer into the FIFO,
 *                  limited to max_bytes.  Compacts after partial drain.
 **/
void CSerial::drain_staging() {
  if (stageLen == 0)
    return;

  // Only deliver when the guest has consumed the previous character.
  // This self-clocks to the emulated CPU speed, just like a real
  // UART where the next character doesn't arrive until the baud
  // rate shifts it in — and the CPU is always fast enough to read
  // it before then.
  if (!uart.rx_empty())
    return; // previous character not yet read

  // Deliver exactly one character
  receive(stageBuf, 1);
  stageLen--;
  if (stageLen > 0)
    memmove(stageBuf, stageBuf + 1, stageLen);
}

/**
 * Thread entry point.
 **/
void CSerial::run() {
  if (disabled)
    return;
  try {
    // Output goes out as soon as a processor queues it (write() wakes this
    // thread); the receive side keeps its own 5 ms tick. While the client
    // is behind, the thread waits for room in the socket instead.
    auto next_tick = std::chrono::steady_clock::now();
    bool blocked = false;
    while (!StopThread) {
      if (blocked)
        wait_writable(next_tick);
      {
        std::unique_lock<std::mutex> lk(out_lock);
        if (!blocked)
          out_cv.wait_until(lk, next_tick,
                            [this] { return out_kick || StopThread; });
        out_kick = false;
      }
      blocked = !send_output();
      if (std::chrono::steady_clock::now() >= next_tick) {
        execute();
        next_tick =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(5);
      }
    }
    flush_output(1000); // what the guest printed last, before stopping
  }

  catch (CException &e) {
    printf("Exception in Serial thread: %s.\n", e.displayText().c_str());
    myThreadDead.store(true);

    // Let the thread die...
  }
}

/**
 * Check if threads are still running.
 *
 * Enter serial port menu if <break> was received.
 **/
void CSerial::check_state() {
  if (disabled || null_attach)
    return;
  if (breakHit)
    serial_menu();

  if (myThreadDead.load())
    FAILURE(Thread, "Serial thread has died");
}

/// A line of the BREAK menu, sent at once: the port's thread is stopped
/// while the menu is open.
void CSerial::menu_say(const char *s) {
  write_cstr(s);
  flush_output(2000);
}

void CSerial::serial_menu() {
  fd_set readset;
  unsigned char buffer[FIFO_SIZE + 1];
  ssize_t size;
  struct timeval tv;
  bool exitLoop = false;

  cSystem->stop_threads();

  menu_say("\r\n<BREAK> received. What do you want to do?\r\n");
  menu_say("     0. Continue\r\n");
  menu_say("     1. Exit emulator gracefully\r\n");
  menu_say("     2. Abort emulator (no changes saved)\r\n");
  menu_say("     3. Save state to autosave.axp and continue\r\n");
  menu_say("     4. Load state from autosave.axp and continue\r\n");
  while (!exitLoop) {
    FD_ZERO(&readset);
    FD_SET(connectSocket, &readset);
    tv.tv_sec = 60;
    tv.tv_usec = 0;
    if (select(connectSocket + 1, &readset, NULL, NULL, &tv) <= 0) {
      menu_say("%SRL-I-TIMEOUT: no timely answer received. Continuing "
               "emulation.\r\n");
      break; // leave loop
    }

#if defined(_WIN32) || defined(__VMS)
    size = recv(connectSocket, (char *)buffer, FIFO_SIZE, 0);
#else
    size = read(connectSocket, &buffer, FIFO_SIZE);
#endif
    if (size <= 0) {
      // The peer closed (or errored) while the menu was open: nothing was
      // read, so buffer[0] is stale -- acting on it could exit the emulator.
      // Report on our own stdout: there is no reader left on the socket, and
      // writing to a closed peer is what raises SIGPIPE.
      printf("%%SRL-I-CONTINUE: serial connection lost, continuing "
             "emulation.\n");
      break;
    }
    switch (buffer[0]) {
    case '0':
      menu_say("%SRL-I-CONTINUE: continuing emulation.\r\n");
      exitLoop = true;
      break;

    case '1':
      menu_say("%SRL-I-EXIT: exiting emulation gracefully.\r\n");
      FAILURE(Graceful, "Graceful exit");
      break;

    case '2':
      menu_say("%SRL-I-ABORT: aborting emulation.\r\n");
      FAILURE(Abort, "Aborting");
      break;

    case '3':
      menu_say("%SRL-I-SAVESTATE: Saving state to autosave.axp.\r\n");
      cSystem->SaveState("autosave.axp");
      menu_say("%SRL-I-CONTINUE: continuing emulation.\r\n");
      exitLoop = true;
      break;

    case '4':
      menu_say("%SRL-I-LOADSTATE: Loading state from autosave.axp.\r\n");
      cSystem->RestoreState("autosave.axp");
      menu_say("%SRL-I-CONTINUE: continuing emulation.\r\n");
      exitLoop = true;
      break;

    default:
      menu_say("%SRL-W-INVALID: Not a valid answer.\r\n");
      break;
    }
  }

  breakHit = false;
  cSystem->start_threads();
}

// ---------------------------------------------------------------------------
// execute  — Main loop body, called every ~20ms from the serial thread.
// ---------------------------------------------------------------------------
void CSerial::execute() {
  fd_set readset;
  unsigned char raw[FIFO_SIZE + 16];
  unsigned char cbuffer[FIFO_SIZE + 1];
  unsigned char *b;
  unsigned char *c;
  unsigned char *end;
  ssize_t size;
  struct timeval tv;

  drain_staging();

  // ------------------------------------------------------------------
  // Every RECV_TICKS cycles: read from the socket and cook telnet out
  // ------------------------------------------------------------------
  state.serial_cycles++;
  if (state.serial_cycles >= RECV_TICKS) {
    // If staging buffer still has data, don't pull more off the socket.
    // TCP backpressure holds the rest at the OS level.
    if (stageLen > 0) {
      state.serial_cycles = 0;
      eval_interrupts();
      return;
    }

    FD_ZERO(&readset);
    FD_SET(connectSocket, &readset);
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    if (select(connectSocket + 1, &readset, NULL, NULL, &tv) > 0) {
      unsigned char *recv_buf = raw + iac_carry_len;

#if defined(_WIN32) || defined(__VMS)
      size = recv(connectSocket, (char *)recv_buf, FIFO_SIZE, 0);
#else
      size = read(connectSocket, recv_buf, FIFO_SIZE);
#endif

      // End of file, or an error other than an interrupted or empty read
      // (EINTR and EAGAIN used to count as a hang-up, abandoning a client
      // that was still there).
      extern volatile sig_atomic_t got_sigint;
      const bool gone = size == 0 || (size < 0 && errno != EINTR &&
                                      errno != EAGAIN && errno != EWOULDBLOCK);
      if (gone && !got_sigint) {
        printf("%%SRL-W-DISCONNECT: Write socket closed on other end for "
               "serial port %d.\n",
               state.iNumber);
        printf("-SRL-I-WAITFOR: Waiting for a new connection on port %d.\n",
               listenPort);
        drop_client();
        WaitForConnection();
        return;
      }

      if (size <= 0) {
        state.serial_cycles = 0;
        eval_interrupts();
        return;
      }

      // Prepend carryover from partial telnet sequence
      if (iac_carry_len > 0) {
        memcpy(raw, iac_carry, iac_carry_len);
        size += iac_carry_len;
        iac_carry_len = 0;
      }

      // raw_mode: skip telnet IAC cooking AND the staging buffer's baud-rate
      // drip-feed.  The byte stream must stay 8-bit clean and flow at line
      // rate for windbg/kgdb, which sends arbitrary binary (including 0xff)
      // in KD packets and times out if delivery is throttled.
      if (raw_mode) {
        int delivered = receive((const char *)raw, (int)size);
        if (delivered < (int)size)
          printf("%%SRL-W-RAWFULL: raw_mode FIFO full on port %d, dropped %d "
                 "byte(s)\n",
                 state.iNumber, (int)size - delivered);
        state.serial_cycles = 0;
        eval_interrupts();
        return;
      }

      b = raw;
      c = cbuffer;
      end = raw + size;

      while (b < end) {
        // ----- Subnegotiation scan (SB ... IAC SE) -----
        if (in_subneg) {
          while (b < end) {
            if (*b == IAC) {
              if (b + 1 >= end) {
                iac_carry[0] = IAC;
                iac_carry_len = 1;
                b = end;
                break;
              }
              if (*(b + 1) == SE) {
                b += 2;
                in_subneg = false;
                break;
              } else if (*(b + 1) == IAC) {
                b += 2;
              } else {
                b += 2;
              }
            } else {
              b++;
            }
          }
          continue;
        }

        // ----- Telnet IAC with bounds-safe lookahead -----
        if (*b == IAC) {
          ssize_t remaining = end - b;

          if (remaining < 2) {
            iac_carry[0] = IAC;
            iac_carry_len = 1;
            b = end;
            break;
          }

          unsigned char cmd = *(b + 1);

          if (cmd == IAC) {
            *c++ = 0xFF;
            b += 2;
            continue;
          } else if (cmd >= WILL && cmd <= DONT) {
            if (remaining < 3) {
              for (ssize_t i = 0; i < remaining; i++)
                iac_carry[i] = b[i];
              iac_carry_len = (int)remaining;
              b = end;
              break;
            }
            b += 3;
            continue;
          } else if (cmd == SB) {
            in_subneg = true;
            b += 2;
            continue;
          } else if (cmd == BREAK) {
            breakHit = true;
            b += 2;
            continue;
          } else if (cmd == AYT) {
            b += 2;
            continue;
          } else {
            b += 2;
            continue;
          }
        }

        // ----- Normal data byte -----
        *c = *b;
        c++;
        b++;
      }

      // Move cooked data into the staging buffer.
      // drain_staging() will drip-feed it into the FIFO at baud rate
      // on subsequent execute() cycles.
      if (c > cbuffer) {
        int cooked_len = (int)(c - cbuffer);

        if (cooked_len <= (int)(STAGE_SIZE - stageLen)) {
          memcpy(stageBuf + stageLen, cbuffer, cooked_len);
          stageLen += cooked_len;
        } else {
          // Staging buffer full — this means the guest is severely
          // behind.  Copy what fits; the rest is lost, but this
          // should only happen under extreme conditions.
          int space = STAGE_SIZE - stageLen;
          if (space > 0) {
            memcpy(stageBuf + stageLen, cbuffer, space);
            stageLen += space;
          }
#if defined(DEBUG_SERIAL)
          printf("%%SRL-W-STAGEDROP: staging buffer full on port %d, "
                 "dropped %d bytes\n",
                 state.iNumber, cooked_len - space);
#endif
        }
      }
    }

    state.serial_cycles = 0;
  }

  eval_interrupts();
}

static u32 srl_magic1 = 0x5A15A15A;
static u32 srl_magic2 = 0x1A51A51A;

/**
 * Save state to a Virtual Machine State file.
 **/
int CSerial::SaveState(FILE *f) {
  long ss = sizeof(state);

  fwrite(&srl_magic1, sizeof(u32), 1, f);
  fwrite(&ss, sizeof(long), 1, f);
  fwrite(&state, sizeof(state), 1, f);
  fwrite(&srl_magic2, sizeof(u32), 1, f);
  printf("%s: %d bytes saved.\n", devid_string, (int)ss);
  return 0;
}

/**
 * Restore state from a Virtual Machine State file.
 **/
int CSerial::RestoreState(FILE *f) {
  long ss;
  u32 m1;
  u32 m2;
  size_t r;

  r = fread(&m1, sizeof(u32), 1, f);
  if (r != 1) {
    printf("%s: unexpected end of file!\n", devid_string);
    return -1;
  }

  if (m1 != srl_magic1) {
    printf("%s: MAGIC 1 does not match!\n", devid_string);
    return -1;
  }

  r = fread(&ss, sizeof(long), 1, f);
  if (r != 1) {
    printf("%s: unexpected end of file!\n", devid_string);
    return -1;
  }

  if (ss != sizeof(state)) {
    printf("%s: STRUCT SIZE does not match!\n", devid_string);
    return -1;
  }

  r = fread(&state, sizeof(state), 1, f);
  if (r != 1) {
    printf("%s: unexpected end of file!\n", devid_string);
    return -1;
  }

  r = fread(&m2, sizeof(u32), 1, f);
  if (r != 1) {
    printf("%s: unexpected end of file!\n", devid_string);
    return -1;
  }

  if (m2 != srl_magic2) {
    printf("%s: MAGIC 2 does not match!\n", devid_string);
    return -1;
  }

  printf("%s: %d bytes restored.\n", devid_string, (int)ss);
  return 0;
}

/**
 * Set up an accepted client socket. Non-blocking, so that send_output
 * never waits for a slow client; TCP_NODELAY, because the queue already
 * batches what is ready and Nagle's algorithm would hold the rest of a
 * line until the client's delayed ACK.
 **/
void CSerial::configure_client_socket() {
  int one = 1;
  setsockopt(connectSocket, IPPROTO_TCP, TCP_NODELAY, (char *)&one,
             sizeof(one));
#if defined(SO_NOSIGPIPE)
  setsockopt(connectSocket, SOL_SOCKET, SO_NOSIGPIPE, (char *)&one,
             sizeof(one));
#endif
#if defined(_WIN32)
  u_long nb = 1;
  ioctlsocket(connectSocket, FIONBIO, &nb);
#else
  fcntl(connectSocket, F_SETFL, fcntl(connectSocket, F_GETFL) | O_NONBLOCK);
#endif
}

/**
 * Split the `action` command line into words. Blanks separate words; a
 * double-quoted stretch keeps its blanks and loses its quotes, so
 * `"c:\Program Files\PuTTY\putty.exe" telnet://localhost:8000` is two
 * words. A quote left open runs to the end of the line.
 **/
static std::vector<std::string> split_command(const char *line) {
  std::vector<std::string> words;
  std::string word;
  bool in_word = false, quoted = false;
  for (const char *p = line; *p; p++) {
    if (*p == '"') {
      quoted = !quoted;
      in_word = true;
    } else if ((*p == ' ' || *p == '\t') && !quoted) {
      if (in_word)
        words.push_back(word);
      word.clear();
      in_word = false;
    } else {
      word += *p;
      in_word = true;
    }
  }
  if (in_word)
    words.push_back(word);
  return words;
}

/**
 * Start the `action` program (typically a telnet client pointed at this
 * port) without waiting for it. Returns false, with the reason in `why`,
 * when it could not be started. A launcher that exits at once (`open`, a
 * script that backgrounds the client) is a success.
 **/
bool CSerial::launch_action(std::string &why) {
  const std::vector<std::string> words =
      split_command(myCfg->get_text_value("action", ""));
  if (words.empty())
    return true;
#if defined(_WIN32)
  // The C runtime joins the arguments with blanks, so a word that has one
  // must carry its quotes again.
  std::vector<std::string> quoted;
  for (const std::string &w : words)
    quoted.push_back(w.find(' ') != std::string::npos ? "\"" + w + "\"" : w);
  std::vector<const char *> argv;
  for (const std::string &w : quoted)
    argv.push_back(w.c_str());
  argv.push_back(nullptr);
  if (_spawnvp(_P_NOWAIT, words[0].c_str(), argv.data()) < 0) {
    why = strerror(errno);
    return false;
  }
#else
  // A child that has finished since the previous launch is collected here,
  // so a port that reconnects all day leaves no zombies behind.
  if (action_pid > 0 && waitpid(action_pid, nullptr, WNOHANG) == action_pid)
    action_pid = -1;
  // posix_spawnp, not fork: this runs on the port's thread while the
  // processors run, and the child of a multi-threaded fork may only call
  // async-signal-safe functions before exec.
  std::vector<char *> argv;
  for (const std::string &w : words)
    argv.push_back(const_cast<char *>(w.c_str()));
  argv.push_back(nullptr);
  pid_t pid;
  const int err =
      posix_spawnp(&pid, argv[0], nullptr, nullptr, argv.data(), environ);
  if (err) {
    why = strerror(err);
    return false;
  }
  action_pid = pid;
#endif
  return true;
}

void CSerial::WaitForConnection() {
  struct sockaddr_in Address;
  socklen_t nAddressSize = sizeof(struct sockaddr_in);
  char s[100];

  std::string why;
  if (!launch_action(why)) {
    // At startup a bad `action` is a configuration error; later (a client
    // went away) the port keeps waiting for one to come back by hand.
    if (!threads_started)
      FAILURE_2(Configuration, "%s: cannot start the action program: %s",
                devid_string, why.c_str());
    printf("%%SRL-W-ACTION: %s: cannot start the action program: %s\n",
           devid_string, why.c_str());
  }

  Address.sin_addr.s_addr = INADDR_ANY;
  Address.sin_port = htons((u16)listenPort);
  Address.sin_family = AF_INET;

  //  Wait until we have a connection. Poll rather than block in accept(), so
  //  that stop_threads() can join the serial thread while it waits for a
  //  reconnect; without a connection the thread just returns.
  connectSocket = INVALID_SOCKET;
  while (connectSocket == INVALID_SOCKET) {
    if (StopThread)
      return;
    fd_set readset;
    struct timeval tv;
    FD_ZERO(&readset);
    FD_SET(listenSocket, &readset);
    tv.tv_sec = 0;
    tv.tv_usec = 200000;
    int ready = select((int)listenSocket + 1, &readset, NULL, NULL, &tv);
    if (ready < 0) // EINTR or a broken listener: don't spin
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    if (ready <= 0)
      continue;
    connectSocket =
        (int)accept(listenSocket, (struct sockaddr *)&Address, &nAddressSize);
  }
  configure_client_socket();

  iac_carry_len = 0;
  in_subneg = false;
  stageLen = 0;

  state.serial_cycles = 0;

  // The greeting goes before anything the guest printed while no client
  // was connected (drop_client keeps that). Not on serial1 (es40 kept it
  // clean for kgdb) and not in raw_mode.
  std::string greeting;
  if (state.iNumber != 1 && !raw_mode) {
    // Character-at-a-time mode, 8-bit clean both ways.
    static const unsigned char options[] = {
        IAC, DO,   TELOPT_ECHO,  IAC, DO,   TELOPT_NAWS, IAC, DO, TELOPT_LFLOW,
        IAC, WILL, TELOPT_ECHO,  IAC, WILL, TELOPT_SGA,  IAC, DO, TELOPT_BINARY,
        IAC, WILL, TELOPT_BINARY};
    greeting.assign((const char *)options, sizeof(options));
    snprintf(s, sizeof(s),
             "This is serial port #%d on Alphabox Alpha Emulator\r\n",
             state.iNumber);
    greeting += s;
  }
  std::lock_guard<std::mutex> g(out_lock);
  out_queue.insert(0, greeting);
  if (!out_queue.empty())
    out_kick = true;
}
