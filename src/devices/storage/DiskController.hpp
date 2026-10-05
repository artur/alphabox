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
 * Contains definitions for the disk controller base class.
 **/
#if !defined(__DISKCONTROLLER_H__)
#define __DISKCONTROLLER_H__

#include <string>

/**
 * \brief Abstract base class for disk controllers (uses CDisk's)
 **/
class CDiskController {
public:
  CDiskController(int num_busses, int num_devs);
  ~CDiskController(void);

  virtual void register_disk(class CDisk *dsk, int bus, int dev);
  class CDisk *get_disk(int bus, int dev);

  /// This controller's number in its disks' default serial numbers, or -1
  /// when it does not show the guest a serial number (CDisk::get_serial).
  int serial_ordinal() const;

  /// The default serial number of the disk at (bus, dev) here: "ES40EM",
  /// this controller's part and the disk's (see CDisk::get_serial).
  std::string default_serial(int bus, int dev) const;

protected:
  /// A controller whose disks are numbered by where it sits rather than by
  /// a count -- the SCSI adapters, by their PCI place -- returns that part
  /// of the serial here: a letter and what follows it, which no counted
  /// controller's two digits can equal. Empty for a counted controller.
  virtual std::string serial_place() const { return std::string(); }

  /// Called by a controller that shows the guest its disks' serial numbers
  /// (ATA IDENTIFY, virtio-blk GET_ID): such controllers are numbered in
  /// configuration order, except that `first` -- the south bridge's IDE --
  /// is always number 0, so the boot disk keeps "ES40EM00000".
  void number_disk_serials(bool first);

private:
  int num_bus;
  int num_dev;

  class CDisk **disks;
};
#endif //! defined(__DISKCONTROLLER_H__)
