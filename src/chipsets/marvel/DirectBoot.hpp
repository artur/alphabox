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
 * The native EV8's direct start (docs/platforms/ev8-native.md, 4.2: N29,
 * and S12b). In 21464 mode the emulator loads a kernel (an ELF image such as
 * Linux's vmlinux), an initrd and a command line for the Alphabox console
 * (firmware/ev8/console.c), and tells the console where they are, and what
 * the machine is, in a boot block the console's image reserves.
 **/
#if !defined(INCLUDED_DIRECT_BOOT_H_)
#define INCLUDED_DIRECT_BOOT_H_

#include <string>

class CSystem;
class CMarvel;

/// The system block's keys (21464 mode only): empty strings for none.
struct direct_boot_config {
  std::string kernel, initrd, cmdline;
  bool auto_boot = true; ///< auto_action: boot at once, or stay at the prompt
};

/// After the boot image is in memory: if it carries the console's boot
/// block, fill it and load the kernel and initrd it is told about. A
/// configured kernel with no block in the image, or a kernel or initrd
/// that cannot be placed, is refused.
void direct_boot_load(CSystem *sys, const CMarvel &mv,
                      const direct_boot_config &boot);

#endif // !defined(INCLUDED_DIRECT_BOOT_H_)
