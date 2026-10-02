/* Alphabox Alpha Emulator
 * Copyright (C) 2026 Artur Goulão
 * Website: https://github.com/artur/alphabox
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
 */

/**
 * \file
 * The PCF8584 I2C bus controller a board puts between the processor and its
 * own serial bus.
 *
 * The DS10 has one (docs/platforms/ds10.md): its console addresses two
 * byte-wide registers in PCI memory -- S0, the data shift register, at the
 * base and S1, control on write and status on read, at base+1 -- and reaches
 * the machine's serial ROMs through them. The part is a bus master here:
 * a register write is carried out on the wire before it returns, so the
 * status the firmware reads back afterwards is already the result.
 **/
#if !defined(INCLUDED_PCF8584_H)
#define INCLUDED_PCF8584_H

#include "SystemComponent.hpp"
#include "i2c_spd.hpp"

class CPCF8584 : public CSystemComponent {
public:
  /// \param base the PCI memory address of S0; S1 is at base + 1.
  CPCF8584(CConfigurator *cfg, class CSystem *c, u64 base);
  ~CPCF8584() override;

  u64 ReadMem(int index, u64 address, int dsize) override;
  void WriteMem(int index, u64 address, int dsize, u64 data) override;
  int SaveState(FILE *f) override;
  int RestoreState(FILE *f) override;

  /// The bus this controller drives, for the board to hang its parts on.
  I2CBus &bus() { return m_bus; }

private:
  I2CBus m_bus;

  /// The state structure contains all elements that need to be saved to the
  /// statefile.
  struct SPCF8584_state {
    u8 s0;     ///< data shift register: the byte last written or received
    u8 s1;     ///< control, as last written
    u8 status; ///< what a read of S1 returns
    u8 own;    ///< S0', the controller's own address (it is never a slave here)
    u8 clock;  ///< S2, the clock divider the firmware chose
    u8 vector; ///< S3, the interrupt vector (nothing uses it)
    bool reading;   ///< a master read is under way
    bool addressed; ///< the last address phase was acknowledged
  } state;

  // Bit-level bus driving: everything above is expressed in these.
  void scl_sda(bool scl, bool sda);
  void send_start();
  void send_stop();
  bool send_byte(u8 b); ///< true when the receiver acknowledged
  u8 recv_byte(bool ack);

  void begin_transfer(); ///< START + the address byte now in S0
  void trace_reg(const char *what, u64 address, u8 value);
  void set_bus_free(bool free);
};

#endif // !defined(INCLUDED_PCF8584_H)
