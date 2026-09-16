/* AXPbox Alpha Emulator
 * Copyright (C) 2020 Tomáš Glozar
 * Copyright (C) 2026 Artur Goulão
 * Website: https://github.com/lenticularis39/axpbox
 *          https://github.com/artur/axpbox
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
 * Contains the code for the emulated DMA controller.
 *
 * Channel 4 (controller 1, local channel 0) is treated as a hardwired
 * cascade.  On a discrete PC/AT pair the guest must program channel 4 into
 * cascade mode and unmask it before channels 0-3 can reach the bus, but
 * integrated south bridges such as the M1543C wire the cascade internally,
 * and not every Alpha console/OS programs it (Linux/alpha does; SRM,
 * AlphaBIOS and NT are unverified).  Gating channels 0-3 on channel 4 would
 * only risk breaking floppy DMA for those guests, so no gate is applied.
 * Only the owning controller's disable bit and channel mask block a transfer.
 **/
#include "DMA.hpp"
#include "AliM1543C.hpp"
#include "PCIDevice.hpp"
#include "StdAfx.hpp"
#include "System.hpp"

#if defined(DEBUG_DMA)
#define DMA_DEBUG(...) printf(__VA_ARGS__)
#else
#define DMA_DEBUG(...) ((void)0)
#endif

CDMA *theDMA = 0;

/**
 * Constructor.
 **/
CDMA::CDMA(CConfigurator *cfg, CSystem *c) : CSystemComponent(cfg, c) {
  // DMA Setup
#define LEGACY_IO(id, port, size)                                              \
  c->RegisterMemory(this, id, U64(0x00000801fc000000) + port, size)
  LEGACY_IO(DMA0_IO_CHANNEL, 0x00, 8);
  LEGACY_IO(DMA0_IO_MAIN, 0x08, 8);
  LEGACY_IO(DMA_IO_LPAGE, 0x81, 11);
  LEGACY_IO(DMA1_IO_CHANNEL, 0xc0, 16);
  LEGACY_IO(DMA1_IO_MAIN, 0xd0, 16);
  LEGACY_IO(DMA_IO_HPAGE, 0x481, 11);
  LEGACY_IO(DMA0_IO_EXT, 0x040b, 1);
  LEGACY_IO(DMA1_IO_EXT, 0x04D6, 1);

  memset(&state, 0, sizeof(state));
  for (int ctrlr = 0; ctrlr < 2; ctrlr++) {
    state.controller[ctrlr].mask = 0x0f;
    state.controller[ctrlr].lobyte = true;
  }

  theDMA = this;
  printf("dma: $Id$\n");
}

/**
 * Destructor.
 **/
CDMA::~CDMA() {}
int CDMA::DoClock() { return 0; }

#if defined(DEBUG_DMA)
static const char *const dma_index_names[] = {
    "DMA0_IO_MAIN",    "DMA1_IO_MAIN",    "DMA_IO_LPAGE", "DMA_IO_HPAGE",
    "DMA0_IO_CHANNEL", "DMA1_IO_CHANNEL", "DMA0_IO_EXT",  "DMA1_IO_EXT"};

#define DMA_INDEX(n) dma_index_names[n - DMA_IO_BASE]
#endif

/**
 * Map a page register offset (0x81/0x481 based) to its channel, or -1 for
 * the reserved/unassigned ports in the range.
 **/
static int dma_page_channel(u64 address) {
  static const int channelmap[] = {2, 3, 1, -1, -1, -1, 0, -1, 6, 7, 5};

  if (address >= sizeof(channelmap) / sizeof(channelmap[0]))
    return -1;
  return channelmap[address];
}

/**
 * Bytes per DMA unit for a device channel.
 **/
static size_t dma_transfer_width(int channel) {
  if (channel < 0 || channel >= 8 || channel == 4)
    FAILURE(InvalidArgument,
            "dma: invalid device channel (channel 4 is cascade)");

  return channel < 4 ? 1 : 2;
}

/**
 * Move a planned transfer between a device buffer and PCI memory.
 *
 * \param page  Page component of the address (already masked for 24/32-bit
 *              addressing and word channels).
 **/
static void dma_copy(u8 *data, size_t count, size_t width, u64 page,
                     u16 current, bool decrement, bool to_memory) {
  auto move = [to_memory](u64 addr, u8 *buf, size_t n) {
    if (to_memory)
      theAli->do_pci_write((u32)addr, buf, 1, n);
    else
      theAli->do_pci_read((u32)addr, buf, 1, n);
  };

  if (decrement) {
    // The address steps down one unit at a time; the byte order within a
    // word is preserved.
    for (size_t offset = 0; offset < count; offset += width, current--)
      move(page | ((u64)current * width), data + offset, width);
    return;
  }

  // The 16-bit current register wraps without carrying into the page
  // registers.  A transfer holds at most 65536 units, so it wraps at most once.
  size_t to_wrap = ((size_t)0x10000 - current) * width;
  size_t first = count < to_wrap ? count : to_wrap;
  move(page | ((u64)current * width), data, first);
  if (first < count)
    move(page, data + first, count - first);
}

u64 CDMA::ReadMem(int index, u64 address, int dsize) {
  u64 ret;
  u8 data = 0;
  int ctrlr;
  int num;

  switch (dsize) {
  case 32:
    ret = ReadMem(index, address, 8);
    ret |= ReadMem(index, address + 1, 8) << 8;
    ret |= ReadMem(index, address + 2, 8) << 16;
    ret |= ReadMem(index, address + 3, 8) << 24;
    return ret;

  case 16:
    ret = ReadMem(index, address, 8);
    ret |= ReadMem(index, address + 1, 8) << 8;
    return ret;

  case 8: {
    std::lock_guard<std::mutex> lock(dma_mutex);

    if (index == DMA1_IO_CHANNEL || index == DMA1_IO_MAIN)
      address >>= 1;

    switch (index) {
    case DMA0_IO_CHANNEL:
    case DMA1_IO_CHANNEL:
      ctrlr = (index == DMA1_IO_CHANNEL) ? 1 : 0;
      num = ((address & 0x0e) >> 1) + (ctrlr * 4);
      // Reads return the current (not base) registers.
      data = ((address & 1 ? state.channel[num].count
                           : state.channel[num].current) >>
              (state.controller[ctrlr].lobyte ? 0 : 8)) &
             0xff;
      state.controller[ctrlr].lobyte = !state.controller[ctrlr].lobyte;
      break;

    case DMA0_IO_MAIN:
    case DMA1_IO_MAIN:
      ctrlr = (index == DMA1_IO_MAIN) ? 1 : 0;
      if (address == 0) {
        // Pending requests are visible even when masked or disabled.
        data = state.controller[ctrlr].status | (get_requests(ctrlr) << 4);
        state.controller[ctrlr].status = 0;
      } else if (address == 7) {
        data = state.controller[ctrlr].mask & 0x0f;
      }
      break;

    case DMA_IO_LPAGE:
    case DMA_IO_HPAGE:
      num = dma_page_channel(address);
      if (num < 0)
        data = 0xff;
      else if (index == DMA_IO_LPAGE)
        data = state.channel[num].pagebase & 0xff;
      else
        data = (state.channel[num].pagebase >> 8) & 0xff;
      break;

    case DMA0_IO_EXT:
    case DMA1_IO_EXT:
      // EISA extended mode registers are write-only.
      data = 0xff;
      break;

    default:
      FAILURE(InvalidArgument, "dma: ReadMem index out of range");
    }

    DMA_DEBUG("dma: read %s,%02" PRIx64 ": %02" PRIx8 "\n", DMA_INDEX(index),
              address, data);
  }
  }
  return data;
}

void CDMA::WriteMem(int index, u64 address, int dsize, u64 data) {
  int num = 0;
  switch (dsize) {
  case 32:
    WriteMem(index, address + 0, 8, (data >> 0) & 0xff);
    WriteMem(index, address + 1, 8, (data >> 8) & 0xff);
    WriteMem(index, address + 2, 8, (data >> 16) & 0xff);
    WriteMem(index, address + 3, 8, (data >> 24) & 0xff);
    return;

  case 16:
    WriteMem(index, address + 0, 8, (data >> 0) & 0xff);
    WriteMem(index, address + 1, 8, (data >> 8) & 0xff);
    return;

  case 8: {
    std::lock_guard<std::mutex> lock(dma_mutex);

    data &= 0xff;
    if (index == DMA1_IO_CHANNEL || index == DMA1_IO_MAIN)
      address >>= 1;

    switch (index) {
    case DMA0_IO_CHANNEL:
    case DMA1_IO_CHANNEL: {
      int ctrlr = (index == DMA1_IO_CHANNEL) ? 1 : 0;
      SDMA_state::SDMA_chan &ch =
          state.channel[((address & 0x0e) >> 1) + (ctrlr * 4)];
      bool lo = state.controller[ctrlr].lobyte;
      // A write loads both the base and the current register.
      if (address & 1) {
        ch.base_count = lo ? (ch.base_count & 0xff00) | data
                           : (ch.base_count & 0x00ff) | (data << 8);
        ch.count = ch.base_count;
      } else {
        ch.base =
            lo ? (ch.base & 0xff00) | data : (ch.base & 0x00ff) | (data << 8);
        ch.current = ch.base;
      }
      state.controller[ctrlr].lobyte = !lo;
      DMA_DEBUG("dma channel %d %s: %04x\n",
                (int)(((address & 0x0e) >> 1) + (ctrlr * 4)),
                address & 1 ? "count" : "base",
                address & 1 ? ch.base_count : ch.base);
      break;
    }

    case DMA1_IO_MAIN:
    case DMA0_IO_MAIN:
      num = (index == DMA1_IO_MAIN) ? 1 : 0;
      switch (address) {
      case 0: // command
        DMA_DEBUG("dma: command register %d written with %02" PRIx64 "\n", num,
                  data);
        state.controller[num].command = (u8)data;
        break;

      case 1: // request
        set_request(num, data & 0x03, (data & 0x04) >> 2);
        break;

      case 2: // single mask
        state.controller[num].mask =
            (state.controller[num].mask & ~(1 << (data & 0x03))) |
            (((data & 0x04) >> 2) << (data & 0x03));
        DMA_DEBUG("dma: mask single on %d: %d %s, mask now %x\n", num,
                  (int)(data & 0x03), data & 0x04 ? "masked" : "unmasked",
                  state.controller[num].mask);
        break;

      case 3: // mode register
        state.channel[(num * 4) + (data & 0x03)].mode = (u8)data;
        DMA_DEBUG("dma: channel %d mode %02" PRIx64
                  ": %s, address %s, autoinit %s, %s\n",
                  (int)((num * 4) + (data & 0x03)), data,
                  (data & 0x80 ? (data & 0x40 ? "cascade" : "block")
                               : (data & 0x40 ? "single" : "demand")),
                  (data & 0x20 ? "decrement" : "increment"),
                  (data & 0x10 ? "on" : "off"),
                  (data & 0x08 ? (data & 0x04 ? "illegal" : "read")
                               : (data & 0x04 ? "write" : "verify")));
        break;

      case 4: // clear flipflop
        DMA_DEBUG("dma: flipflop cleared for dma %d\n", num);
        state.controller[num].lobyte = true;
        break;

      case 5: // master reset
        DMA_DEBUG("dma: controller %d reset\n", num);
        state.controller[num].lobyte = true;
        state.controller[num].command = 0;
        state.controller[num].status = 0;
        state.controller[num].request = 0;
        state.controller[num].mask = 0x0f;
        break;

      case 6: // master enable
        state.controller[num].mask = 0x00;
        break;

      case 7: // master mask
        state.controller[num].mask = data & 0x0f;
        break;
      }
      break;

    case DMA_IO_LPAGE:
    case DMA_IO_HPAGE:
      num = dma_page_channel(address);
      if (num < 0)
        break;
      if (index == DMA_IO_LPAGE)
        state.channel[num].pagebase =
            (state.channel[num].pagebase & 0xff00) | data;
      else
        state.channel[num].pagebase =
            (state.channel[num].pagebase & 0x00ff) | (data << 8);
      DMA_DEBUG("dma channel %d pagebase: %04x\n", num,
                state.channel[num].pagebase);
      break;

    case DMA0_IO_EXT:
    case DMA1_IO_EXT:
      DMA_DEBUG("dma: extended mode register %d written: %02" PRIx64 "\n",
                index - DMA0_IO_EXT, data);
      break;

    default:
      FAILURE(InvalidArgument, "dma: WriteMem index out of range");
    }
    return;
  }
  }
}

static u32 dma_magic1 = 0x65324387;
static u32 dma_magic2 = 0x24092875;

/**
 * Save state to a Virtual Machine State file.
 **/
int CDMA::SaveState(FILE *f) {
  std::lock_guard<std::mutex> lock(dma_mutex);
  long ss = sizeof(state);

  if (fwrite(&dma_magic1, sizeof(u32), 1, f) != 1 ||
      fwrite(&ss, sizeof(long), 1, f) != 1 ||
      fwrite(&state, sizeof(state), 1, f) != 1 ||
      fwrite(&dma_magic2, sizeof(u32), 1, f) != 1) {
    printf("dma: error writing state file!\n");
    return -1;
  }

  printf("dma: %ld bytes saved.\n", ss);
  return 0;
}

/**
 * Restore state from a Virtual Machine State file.
 **/
int CDMA::RestoreState(FILE *f) {
  long ss = 0;
  u32 m1 = 0;
  u32 m2 = 0;
  SDMA_state restored;

  if (fread(&m1, sizeof(u32), 1, f) != 1) {
    printf("dma: unexpected end of file!\n");
    return -1;
  }

  if (m1 != dma_magic1) {
    printf("dma: MAGIC 1 does not match!\n");
    return -1;
  }

  if (fread(&ss, sizeof(long), 1, f) != 1) {
    printf("dma: unexpected end of file!\n");
    return -1;
  }

  if (ss != sizeof(restored)) {
    printf("dma: STRUCT SIZE does not match!\n");
    return -1;
  }

  if (fread(&restored, sizeof(restored), 1, f) != 1) {
    printf("dma: unexpected end of file!\n");
    return -1;
  }

  if (fread(&m2, sizeof(u32), 1, f) != 1) {
    printf("dma: unexpected end of file!\n");
    return -1;
  }

  if (m2 != dma_magic2) {
    printf("dma: MAGIC 2 does not match!\n");
    return -1;
  }

  {
    std::lock_guard<std::mutex> lock(dma_mutex);
    state = restored;
  }
  printf("dma: %ld bytes restored.\n", ss);
  return 0;
}

/**
 * Set or clear the software request bit for a channel.  Caller holds
 * dma_mutex.
 **/
void CDMA::set_request(int ctrlr, int channel, int data) {
  channel &= 0x03;
  if (data)
    state.controller[ctrlr].request |= (1 << channel);
  else
    state.controller[ctrlr].request &= ~(1 << channel);
}

/**
 * Request bits as seen in the status register.  Controller 0's pending
 * requests appear on controller 1's channel-4 (cascade) request input.
 * Caller holds dma_mutex.
 **/
u8 CDMA::get_requests(int ctrlr) {
  u8 requests = state.controller[ctrlr].request;
  if (ctrlr == 1 && !(state.controller[0].command & 0x04) &&
      (state.controller[0].request & 0x0f))
    requests |= 0x01;
  return requests & 0x0f;
}

size_t CDMA::get_transfer_size(int channel) {
  size_t width = dma_transfer_width(channel);
  std::lock_guard<std::mutex> lock(dma_mutex);
  return ((size_t)state.channel[channel].count + 1) * width;
}

/**
 * Advance the current registers by the transferred units and report terminal
 * count.  Caller holds dma_mutex.
 **/
bool CDMA::advance_transfer(int channel, size_t units, bool eop) {
  SDMA_state::SDMA_chan &ch = state.channel[channel];
  // Check before narrowing: a full transfer can contain 65536 units.
  bool terminal_count = units == (size_t)ch.count + 1;

  if (ch.mode & 0x20)
    ch.current -= (u16)units;
  else
    ch.current += (u16)units;
  ch.count -= (u16)units;

  if (terminal_count || eop)
    complete_transfer(channel);
  return terminal_count;
}

/**
 * Complete a transfer on TC or EOP.  Caller holds dma_mutex.
 **/
void CDMA::complete_transfer(int channel) {
  int ctrlr = channel < 4 ? 0 : 1;
  u8 bit = 1 << (channel & 0x03);
  SDMA_state::SDMA_chan &ch = state.channel[channel];

  state.controller[ctrlr].status |= bit;
  state.controller[ctrlr].request &= ~bit;
  if (ch.mode & 0x10) {
    ch.current = ch.base;
    ch.count = ch.base_count;
  } else {
    state.controller[ctrlr].mask |= bit;
  }
}

/**
 * Validate and account a transfer under the lock, then move the data without
 * holding it (the memory access may be slow and must not nest device locks).
 **/
CDMA::SDMA_result CDMA::transfer(int channel, void *data, size_t length,
                                 bool eop, bool to_memory) {
  size_t width = dma_transfer_width(channel);
  int ctrlr = channel < 4 ? 0 : 1;
  u8 bit = 1 << (channel & 0x03);
  const char *dir = to_memory ? "send" : "recv";
  SDMA_result result = {0, true, false, false, false};

  if (!theAli)
    return result;

  // ALi PCI config 0x42 bit 6 enables the high page registers (32-bit DMA
  // addressing); otherwise the address is ISA 24-bit.
  bool addr32 = (theAli->config_read(0, 0x42, 8) & 0x40) != 0;

  u64 page;
  u16 current;
  bool decrement;
  size_t count;
  {
    std::lock_guard<std::mutex> lock(dma_mutex);
    const SDMA_state::SDMA_chan &ch = state.channel[channel];

    if (state.controller[ctrlr].command & 0x04) {
      DMA_DEBUG("dma: %s on channel %d blocked: controller %d disabled\n", dir,
                channel, ctrlr);
      return result;
    }
    // The mask inhibits the device request, not a software request.
    if ((state.controller[ctrlr].mask & bit) &&
        !(state.controller[ctrlr].request & bit)) {
      DMA_DEBUG("dma: %s on channel %d blocked: masked\n", dir, channel);
      return result;
    }
    if ((ch.mode & 0xc0) == 0xc0) {
      DMA_DEBUG("dma: %s on channel %d blocked: cascade mode\n", dir, channel);
      return result;
    }
    // 8237 "write" (0x04) is device -> memory, "read" (0x08) memory -> device;
    // verify (0x00) has no direction, 0x0c is illegal.
    u8 type = ch.mode & 0x0c;
    if (type != 0x00 && type != (to_memory ? 0x04 : 0x08)) {
      DMA_DEBUG("dma: %s on channel %d blocked: transfer type %02x\n", dir,
                channel, type);
      return result;
    }

    count = ((size_t)ch.count + 1) * width;
    if (length > 0 && length < count)
      count = length;
    if (count % width)
      FAILURE(InvalidArgument,
              "dma: word-channel transfer length must be even");

    result.blocked = false;
    result.external_eop = eop;

    if (type == 0x00) {
      result.verify = true;
      result.terminal_count = advance_transfer(channel, count / width, eop);
      DMA_DEBUG("dma: verify on channel %d: %zx bytes%s\n", channel, count,
                result.terminal_count ? " (TC)" : "");
      return result;
    }

    // Hoisted out of the per-unit loop: the page does not change during a
    // transfer.  Word channels drive A16 from the current address.
    u16 pagebase = addr32 ? ch.pagebase : (ch.pagebase & 0x00ff);
    if (width == 2)
      pagebase &= 0xfffe;
    page = (u64)pagebase << 16;
    current = ch.current;
    decrement = (ch.mode & 0x20) != 0;

    result.terminal_count = advance_transfer(channel, count / width, eop);
    result.transferred = count;
  }

  DMA_DEBUG("dma: %s channel %d: %zx bytes @ %08" PRIx64 "%s%s\n", dir, channel,
            count, page | ((u64)current * width),
            decrement ? " (decrement)" : "",
            result.terminal_count ? " (TC)" : "");

  dma_copy((u8 *)data, count, width, page, current, decrement, to_memory);
  return result;
}

/**
 * Transfer device-buffer bytes to memory, up to the current DMA count.
 **/
CDMA::SDMA_result CDMA::send_data(int channel, void *data, size_t length,
                                  bool eop) {
  return transfer(channel, data, length, eop, true);
}

/**
 * Transfer memory bytes to a device buffer, up to the current DMA count.
 **/
CDMA::SDMA_result CDMA::recv_data(int channel, void *data, size_t length,
                                  bool eop) {
  return transfer(channel, data, length, eop, false);
}
