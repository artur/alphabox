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

#include "NicAddress.hpp"
#include "Configurator.hpp"

#include <cctype>

static int nic_count = 0;

static int hex_digit(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  c = (char)tolower((unsigned char)c);
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  return -1;
}

/// Parse xx-xx-xx-xx-xx-xx (any of "-:." as separators).
static bool parse_mac(const char *text, u8 mac[6]) {
  if (strlen(text) != 17)
    return false;
  for (int i = 0; i < 6; i++) {
    const char *p = text + i * 3;
    int hi = hex_digit(p[0]);
    int lo = hex_digit(p[1]);
    if (hi < 0 || lo < 0)
      return false;
    if (i < 5 && !strchr("-:.", p[2]))
      return false;
    mac[i] = (u8)(hi << 4 | lo);
  }
  return true;
}

void nic_station_address(CConfigurator *cfg, const char *devid_string,
                         u8 mac[6]) {
  const u8 def[6] = {0x08, 0x00, 0x2B, 0xE5, 0x40, (u8)nic_count++};
  memcpy(mac, def, 6);

  const char *text = cfg->get_text_value("mac");
  if (text) {
    if (!parse_mac(text, mac))
      FAILURE_1(Configuration,
                "MAC address (%s) should have xx-xx-xx-xx-xx-xx format", text);
    printf("%s: MAC set to %s\n", devid_string, text);
    return;
  }

  printf("%s: MAC defaulted to %02X-%02X-%02X-%02X-%02X-%02X\n", devid_string,
         mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}
