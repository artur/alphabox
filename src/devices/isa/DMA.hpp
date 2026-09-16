/* AXPbox Alpha Emulator
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
 * Contains the definitions for the emulated DMA controller.
 **/
#if !defined(INCLUDED_DMA_H)
#define INCLUDED_DMA_H

#include "SystemComponent.hpp"
#include <mutex>

/**
 * \brief Emulated DMA controller (two cascaded 8237s in the ALi M1543C).
 *
 * Devices move data with send_data()/recv_data() in one call per transfer;
 * the controller is not clocked and no bus arbitration is modelled.
 **/
class CDMA : public CSystemComponent {
public:
  CDMA(CConfigurator *cfg, CSystem *c);
  virtual ~CDMA();

  virtual int DoClock();
  virtual void WriteMem(int index, u64 address, int dsize, u64 data);
  virtual u64 ReadMem(int index, u64 address, int dsize);
  virtual int SaveState(FILE *f);
  virtual int RestoreState(FILE *f);

  struct SDMA_result {
    size_t transferred;  ///< Bytes moved to/from memory (0 for verify).
    bool blocked;        ///< No service: registers and buffer are unchanged.
    bool terminal_count; ///< This call exhausted the count (even w/ autoinit).
    bool external_eop;   ///< This call honoured the device's EOP request.
    bool verify;         ///< Verify mode: count advanced, memory untouched.
  };

  // Buffers and lengths are in bytes; length 0 means "the current count".
  // Channels 0-3 are byte channels, 5-7 word channels (length must be even);
  // channel 4 is the cascade and cannot be used by a device.
  // send: device -> memory (8237 "write"); recv: memory -> device ("read").
  // A transfer is blocked when the owning controller is disabled, the channel
  // is masked without a software request, the channel is in cascade mode, or
  // the programmed transfer type is illegal or of the wrong direction.
  // eop ends the transfer after the last unit of this call.  Terminal count or
  // EOP sets the TC status bit, clears the software request, and then either
  // reloads the base registers (autoinit) or masks the channel.
  SDMA_result send_data(int channel, void *data, size_t length = 0,
                        bool eop = false);
  SDMA_result recv_data(int channel, void *data, size_t length = 0,
                        bool eop = false);

  /// Current count plus one, in bytes (device channels only).
  size_t get_transfer_size(int channel);

private:
  SDMA_result transfer(int channel, void *data, size_t length, bool eop,
                       bool to_memory);
  void set_request(int ctrlr, int channel, int data);
  u8 get_requests(int ctrlr);
  bool advance_transfer(int channel, size_t units, bool eop);
  void complete_transfer(int channel);

  /// Guest port I/O can arrive on any CPU thread while a device transfers.
  std::mutex dma_mutex;

  /// The state structure contains all elements that need to be saved to the
  /// statefile.
  struct SDMA_state {
    /// DMA channel state
    struct SDMA_chan {
      u16 current;    ///< Current address register.
      u16 base;       ///< Base address register (autoinit reload).
      u16 pagebase;   ///< High page (bits 15-8) and low page (bits 7-0).
      u16 count;      ///< Current count register.
      u16 base_count; ///< Base count register (autoinit reload).
      u8 mode;
    } channel[8];

    /// DMA controller state
    struct SDMA_ctrl {
      u8 status; ///< Terminal-count bits (cleared on read).
      u8 command;
      u8 request; ///< Software request bits.
      u8 mask;
      bool lobyte; ///< Flip-flop shared by address and count registers.
    } controller[2];
  } state;
};

#define DMA_IO_BASE 0x1000
#define DMA0_IO_MAIN DMA_IO_BASE + 0
#define DMA1_IO_MAIN DMA_IO_BASE + 1
#define DMA_IO_LPAGE DMA_IO_BASE + 2
#define DMA_IO_HPAGE DMA_IO_BASE + 3
#define DMA0_IO_CHANNEL DMA_IO_BASE + 4
#define DMA1_IO_CHANNEL DMA_IO_BASE + 5
#define DMA0_IO_EXT DMA_IO_BASE + 6
#define DMA1_IO_EXT DMA_IO_BASE + 7

extern CDMA *theDMA;

#endif // !defined(INCLUDED_DMA_H)
