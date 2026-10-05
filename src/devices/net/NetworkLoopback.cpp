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

#include "NetworkLoopback.hpp"

bool CNetworkLoopback::init(const char *devid_string, CConfigurator *cfg) {
  printf("%s: loopback plug on the port (type = loopback): every frame "
         "transmitted is received back, nothing reaches the host.\n",
         devid_string);
  return true;
}

int CNetworkLoopback::send(const u8 *data, int len) {
  if (len <= 0)
    return 0;
  std::lock_guard<std::mutex> lock(mtx);
  if (queue.size() >= MAX_QUEUED)
    queue.pop_front();
  queue.emplace_back(data, data + len);
  return 0;
}

int CNetworkLoopback::receive(const u8 **data, int *len) {
  std::lock_guard<std::mutex> lock(mtx);
  if (queue.empty()) {
    *data = nullptr;
    *len = 0;
    return 0;
  }
  // Valid until the next receive(), as the interface promises.
  current = std::move(queue.front());
  queue.pop_front();
  *data = current.data();
  *len = (int)current.size();
  return 1;
}

void CNetworkLoopback::set_filter(const NetworkFilter &filter) {
  // The plug has no filter; the controller's own address filter decides.
}

void CNetworkLoopback::close() {
  std::lock_guard<std::mutex> lock(mtx);
  queue.clear();
}
