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

#if !defined(INCLUDED_VIRTIO_BLK_H)
#define INCLUDED_VIRTIO_BLK_H

#include "DiskController.hpp"
#include "VirtioPci.hpp"

/**
 * \brief virtio-blk (legacy): a block device on one virtqueue.
 *
 * PCI 1AF4:1001, subsystem 1AF4:0002, class 018000. The disk is the
 * `disk0.0` child (file, device or ramdisk). Requests IN, OUT, FLUSH and
 * GET_ID; sectors are 512 bytes whatever the image's block size.
 **/
class CVirtioBlk : public CVirtioPci, public CDiskController {
public:
  CVirtioBlk(class CConfigurator *cfg, class CSystem *c, int pcibus,
             int pcidev);
  ~CVirtioBlk() override { stop_threads(); } // before the members go
  void register_disk(class CDisk *dsk, int bus, int dev) override;

  // Feature bits (device-specific).
  static constexpr u32 F_SIZE_MAX = 1u << 1, F_SEG_MAX = 1u << 2,
                       F_GEOMETRY = 1u << 4, F_RO = 1u << 5,
                       F_BLK_SIZE = 1u << 6, F_FLUSH = 1u << 9;
  // Request types and status.
  static constexpr u32 T_IN = 0, T_OUT = 1, T_FLUSH = 4, T_GET_ID = 8;
  static constexpr u8 S_OK = 0, S_IOERR = 1, S_UNSUPP = 2;
  static constexpr u32 kSizeMax = 0x10000; // bytes per segment
  static constexpr u32 kSegMax = kQueueSize - 2;

private:
  u32 device_features() override;
  void process_queue(int q) override;
  void selftest() override;
  u8 request(const Chain &c, u32 &written);
  void fill_config();
  void refresh_config() override { fill_config(); } // media may change

  class CDisk *m_disk = nullptr;
  std::vector<u8> m_buf; // one request's data
};

#endif
