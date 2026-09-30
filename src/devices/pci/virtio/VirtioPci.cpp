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
 * The legacy virtio-pci transport: registers, split virtqueues, the device
 * thread and the interrupt. See VirtioPci.hpp and docs/virtio.md.
 **/

#include "VirtioPci.hpp"
#include "StdAfx.hpp"
#include "System.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>

// ALPHABOX_VIRTIO_TRACE=1: register accesses and queue events.
static const bool g_trace = getenv("ALPHABOX_VIRTIO_TRACE") != nullptr;
static std::atomic<int> g_instances{0};

static constexpr u32 kBarSize = 0x40;
static constexpr u16 kVendor = 0x1af4;

CVirtioPci::CVirtioPci(CConfigurator *cfg, CSystem *c, int pcibus, int pcidev,
                       u16 type, u32 class_code, int queues, const char *name)
    : CPCIDevice(cfg, c, pcibus, pcidev), m_name(name), m_queues(queues) {
  if (queues > 4)
    FAILURE(InvalidArgument, "virtio: at most 4 queues");
  u32 data[64] = {}, mask[64] = {};
  data[0x00 >> 2] = u32(0x1000 + type - 1) << 16 | kVendor; // transitional ID
  data[0x04 >> 2] = 0x00000000;
  data[0x08 >> 2] = class_code << 8;           // revision 0: legacy interface
  data[0x10 >> 2] = 0x00000001;                // BAR0: registers, I/O
  data[0x2c >> 2] = u32(type) << 16 | kVendor; // subsystem = device type
  data[0x3c >> 2] = 0x000001ff;                // INTA, no line yet
  mask[0x04 >> 2] = 0x00000157;                // command
  mask[0x0c >> 2] = 0x0000ffff;                // latency timer, cache line size
  mask[0x10 >> 2] = ~(kBarSize - 1);
  mask[0x3c >> 2] = 0x000000ff;
  add_function(0, data, mask);
  memset(&m_state, 0, sizeof(m_state));
  m_instance = g_instances++;
  ResetPCI();
}

CVirtioPci::~CVirtioPci() { stop_threads(); }

void CVirtioPci::kick() {
  {
    std::lock_guard<std::mutex> lk(m_kick_mx);
    m_kicked = true;
  }
  m_kick_cv.notify_one();
}

void CVirtioPci::start_threads() {
  if (!myThread) {
    printf(" %s", m_name);
    StopThread = false;
    myThread = std::make_unique<std::thread>([this]() { run(); });
    if (getenv("ALPHABOX_VIRTIO_SELFTEST") && !m_selftest_thread) {
      // ALPHABOX_VIRTIO_SELFTEST_DELAY=<s>: run it that much later, through
      // the PCI DMA windows the firmware has set up by then.
      const char *delay = getenv("ALPHABOX_VIRTIO_SELFTEST_DELAY");
      const int late_s = delay ? atoi(delay) : 0;
      m_st_direct = late_s <= 0;
      m_selftest = m_st_direct.load();
      m_selftest_thread = std::make_unique<std::thread>([this, late_s]() {
        try {
          if (late_s > 0) {
            for (int i = 0; i < late_s * 10 && !StopThread; ++i)
              st_wait_ms(100);
            if (StopThread || !st_find_window()) {
              m_selftest = false;
              return;
            }
            m_selftest = true;
          }
          selftest();
        } catch (CException &e) {
          printf("%%VIRTIO-E-SELFTEST: %s: %s\n", devid_string,
                 e.displayText().c_str());
        }
        m_selftest = false;
      });
    }
  }
}

void CVirtioPci::stop_threads() {
  StopThread = true;
  kick();
  if (m_selftest_thread) {
    m_selftest_thread->join();
    m_selftest_thread = nullptr;
  }
  if (myThread) {
    printf(" %s", m_name);
    myThread->join();
    myThread = nullptr;
  }
}

void CVirtioPci::check_state() {
  if (myThreadDead.load())
    FAILURE_1(Thread, "%s thread has died", m_name);
}

// The device thread: woken by a notify (or every poll_ms for a device that
// also has outside input), it takes what the driver made available.
void CVirtioPci::run() {
  try {
    while (!StopThread) {
      {
        std::unique_lock<std::mutex> lk(m_kick_mx);
        auto ready = [this]() { return m_kicked || StopThread; };
        if (const int ms = poll_ms())
          m_kick_cv.wait_for(lk, std::chrono::milliseconds(ms), ready);
        else
          m_kick_cv.wait(lk, ready);
        m_kicked = false;
      }
      if (StopThread)
        break;
      std::lock_guard<std::mutex> work(m_work);
      for (int q = 0; q < m_queues; ++q)
        if (queue_ready(q))
          process_queue(q);
      poll();
    }
  } catch (CException &e) {
    printf("Exception in %s thread: %s.\n", m_name, e.displayText().c_str());
    myThreadDead.store(true);
  } catch (std::exception &e) {
    printf("Exception in %s thread: %s.\n", m_name, e.what());
    myThreadDead.store(true);
  }
}

void CVirtioPci::ResetPCI() {
  CPCIDevice::ResetPCI();
  // The start-up self-test runs while the firmware resets the PCI bus; it
  // owns the device until it is done (and resets it itself then).
  if (m_selftest)
    return;
  std::lock_guard<std::mutex> work(m_work);
  std::lock_guard<std::mutex> lk(m_mx);
  reset_device();
}

// Status 0, or a machine reset: features, queues, ISR and status cleared.
void CVirtioPci::reset_device() {
  m_state.guest_features = 0;
  m_state.queue_sel = 0;
  m_state.status = 0;
  m_state.isr = 0;
  for (auto &q : m_state.q)
    q = {};
  m_broken_reported = false;
  device_reset();
  update_irq();
}

void CVirtioPci::update_irq() {
  const bool level = m_state.isr != 0;
  if (level != m_irq_level && g_trace)
    printf("VIRTIO %s irq %d\n", m_name, (int)level);
  m_irq_level = level;
  do_pci_interrupt(0, level);
}

void CVirtioPci::raise_isr(u8 bits) {
  std::lock_guard<std::mutex> lk(m_mx);
  m_state.isr |= bits;
  update_irq();
}

bool CVirtioPci::driver_ok() {
  std::lock_guard<std::mutex> lk(m_mx);
  return (m_state.status & S_DRIVER_OK) != 0;
}

u32 CVirtioPci::reg_read(u32 off, int size) {
  const int n = size / 8;
  // The common registers as bytes, then the device's configuration.
  u8 regs[REG_CONFIG];
  const u32 feat = device_features();
  const int sel = m_state.queue_sel;
  const u32 pfn = sel < m_queues ? m_state.q[sel].pfn : 0;
  const u16 qsize = sel < m_queues ? kQueueSize : 0;
  memcpy(regs + REG_HOST_FEATURES, &feat, 4);
  memcpy(regs + REG_GUEST_FEATURES, &m_state.guest_features, 4);
  memcpy(regs + REG_QUEUE_PFN, &pfn, 4);
  memcpy(regs + REG_QUEUE_SIZE, &qsize, 2);
  memcpy(regs + REG_QUEUE_SEL, &m_state.queue_sel, 2);
  memset(regs + REG_QUEUE_NOTIFY, 0, 2);
  regs[REG_STATUS] = m_state.status;
  regs[REG_ISR] = m_state.isr;
  if (off + n > REG_CONFIG)
    refresh_config();
  u32 v = 0;
  for (int i = 0; i < n; ++i) {
    const u32 a = off + i;
    u8 b = 0;
    if (a < REG_CONFIG)
      b = regs[a];
    else if (a - REG_CONFIG < m_config.size())
      b = m_config[a - REG_CONFIG];
    v |= u32(b) << (8 * i);
  }
  // Reading the ISR clears it and drops the interrupt.
  if (off <= REG_ISR && off + n > REG_ISR && m_state.isr) {
    m_state.isr = 0;
    update_irq();
  }
  return v;
}

void CVirtioPci::reg_write(u32 off, int size, u32 v) {
  if (off >= REG_CONFIG) {
    const int n = size / 8;
    for (int i = 0; i < n; ++i)
      if (off - REG_CONFIG + i < m_config.size())
        m_config[off - REG_CONFIG + i] = u8(v >> (8 * i));
    config_written(off - REG_CONFIG, n);
    return;
  }
  switch (off) {
  case REG_GUEST_FEATURES:
    m_state.guest_features = v & device_features();
    break;
  case REG_QUEUE_PFN:
    if (m_state.queue_sel < m_queues) {
      auto &q = m_state.q[m_state.queue_sel];
      q.pfn = v;
      q.last_avail = 0;
      q.used_idx = 0;
      if (g_trace)
        printf("VIRTIO %s queue %d at %08llx\n", m_name, m_state.queue_sel,
               (unsigned long long)v * kPage);
    }
    break;
  case REG_QUEUE_SEL:
    m_state.queue_sel = u16(v);
    break;
  case REG_QUEUE_NOTIFY:
    if (g_trace)
      printf("VIRTIO %s notify %d\n", m_name, (int)u16(v));
    kick();
    break;
  case REG_STATUS:
    if (u8(v) == 0) {
      reset_device();
    } else {
      const u8 was = m_state.status;
      m_state.status = u8(v);
      if ((m_state.status & S_DRIVER_OK) && !(was & S_DRIVER_OK))
        kick(); // buffers may be waiting already
    }
    break;
  default:
    break; // read-only registers
  }
}

u32 CVirtioPci::ReadMem_Bar(int func, int bar, u32 address, int dsize) {
  if (bar != 0)
    return 0;
  std::lock_guard<std::mutex> lk(m_mx);
  const u32 v = reg_read(address & (kBarSize - 1), dsize);
  if (g_trace)
    printf("VIRTIO %s R%d %02x = %x\n", m_name, dsize, address & (kBarSize - 1),
           v);
  return v;
}

void CVirtioPci::WriteMem_Bar(int func, int bar, u32 address, int dsize,
                              u32 data) {
  if (bar != 0)
    return;
  const u32 off = address & (kBarSize - 1);
  if (g_trace)
    printf("VIRTIO %s W%d %02x = %x\n", m_name, dsize, off, data);
  // A reset and a queue address change wait for the thread to finish what
  // it has in hand; everything else (a notify above all) never waits.
  if (off == REG_QUEUE_PFN || (off == REG_STATUS && u8(data) == 0)) {
    std::lock_guard<std::mutex> work(m_work);
    std::lock_guard<std::mutex> lk(m_mx);
    reg_write(off, dsize, data);
    return;
  }
  std::lock_guard<std::mutex> lk(m_mx);
  reg_write(off, dsize, data);
}

bool CVirtioPci::dma_read(u64 a, void *d, size_t n) {
  if (!n)
    return true;
  if (m_selftest && m_st_direct) {
    if (a + n > (1ull << cSystem->get_memory_bits()))
      return false;
    memcpy(d, cSystem->PtrToMem(a), n);
    return true;
  }
  if (a + n > 0x100000000ull)
    return false; // beyond a 32-bit PCI bus address
  do_pci_read(u32(a), d, 1, n);
  return true;
}

bool CVirtioPci::dma_write(u64 a, const void *s, size_t n) {
  if (!n)
    return true;
  if (m_selftest && m_st_direct) {
    if (a + n > (1ull << cSystem->get_memory_bits()))
      return false;
    memcpy(cSystem->PtrToMem(a), s, n);
    return true;
  }
  if (a + n > 0x100000000ull)
    return false;
  do_pci_write(u32(a), (void *)s, 1, n);
  return true;
}

bool CVirtioPci::queue_ready(int q) { return m_state.q[q].pfn != 0; }

// Take the next available buffer, following its descriptor chain (and an
// indirect table) into a list of segments. A malformed chain is reported
// once and handed back empty (the device completes it with length 0).
bool CVirtioPci::pop_chain(int q, Chain &c) {
  auto &qs = m_state.q[q];
  u16 avail_idx;
  if (!dma_read(avail_base(q) + 2, &avail_idx, 2))
    return false;
  if (avail_idx == qs.last_avail)
    return false;
  std::atomic_thread_fence(std::memory_order_acquire);
  const u16 slot = qs.last_avail % kQueueSize;
  u16 head;
  if (!dma_read(avail_base(q) + 4 + 2 * slot, &head, 2))
    return false;
  qs.last_avail++;
  c.head = head;
  c.segs.clear();
  c.readable = c.writable = 0;

  auto broken = [&](const char *why) {
    if (!m_broken_reported)
      printf("%s: queue %d: %s (descriptor chain dropped)\n", devid_string, q,
             why);
    m_broken_reported = true;
    c.segs.clear();
    c.readable = c.writable = 0;
    return true;
  };
  if (head >= kQueueSize)
    return broken("head out of range");

  u64 table = desc_base(q);
  u32 entries = kQueueSize;
  bool indirect = false;
  u32 i = head;
  for (u32 count = 0;;) {
    if (i >= entries || count++ >= entries)
      return broken("descriptor chain loops or runs out of the table");
    u8 d[16];
    if (!dma_read(table + 16 * i, d, 16))
      return broken("descriptor out of reach");
    u64 addr;
    u32 len;
    u16 flags, next;
    memcpy(&addr, d, 8);
    memcpy(&len, d + 8, 4);
    memcpy(&flags, d + 12, 2);
    memcpy(&next, d + 14, 2);
    if (flags & D_INDIRECT) {
      if (indirect || (flags & D_NEXT) || len < 16 || (len & 15))
        return broken("bad indirect descriptor");
      table = addr;
      entries = len / 16;
      indirect = true;
      i = 0;
      count = 0;
      continue;
    }
    c.segs.push_back({addr, len, (flags & D_WRITE) != 0});
    if (flags & D_WRITE)
      c.writable += len;
    else
      c.readable += len;
    if (!(flags & D_NEXT))
      break;
    i = next;
  }
  return true;
}

void CVirtioPci::push_used(int q, u16 head, u32 len) {
  auto &qs = m_state.q[q];
  u32 elem[2] = {head, len};
  dma_write(used_base(q) + 4 + 8 * (qs.used_idx % kQueueSize), elem, 8);
  std::atomic_thread_fence(std::memory_order_release);
  qs.used_idx++;
  dma_write(used_base(q) + 2, &qs.used_idx, 2);
}

void CVirtioPci::queue_interrupt(int q) {
  std::atomic_thread_fence(std::memory_order_seq_cst);
  u16 flags = 0;
  dma_read(avail_base(q), &flags, 2);
  if (!(flags & AVAIL_F_NO_INTERRUPT))
    raise_isr(ISR_QUEUE);
}

size_t CVirtioPci::chain_read(const Chain &c, size_t off, void *buf, size_t n) {
  u8 *out = (u8 *)buf;
  size_t done = 0;
  for (const Seg &s : c.segs) {
    if (s.write)
      continue;
    if (off >= s.len) {
      off -= s.len;
      continue;
    }
    const size_t take = std::min<size_t>(s.len - off, n - done);
    if (!dma_read(s.addr + off, out + done, take))
      return done;
    done += take;
    off = 0;
    if (done == n)
      break;
  }
  return done;
}

size_t CVirtioPci::chain_write(const Chain &c, size_t off, const void *buf,
                               size_t n) {
  const u8 *in = (const u8 *)buf;
  size_t done = 0;
  for (const Seg &s : c.segs) {
    if (!s.write)
      continue;
    if (off >= s.len) {
      off -= s.len;
      continue;
    }
    const size_t take = std::min<size_t>(s.len - off, n - done);
    if (!dma_write(s.addr + off, in + done, take))
      return done;
    done += take;
    off = 0;
    if (done == n)
      break;
  }
  return done;
}

// --- the self-test's driver side ------------------------------------------

u32 CVirtioPci::st_reg_read(u32 off, int size) {
  return ReadMem_Bar(0, 0, off, size);
}

void CVirtioPci::st_reg_write(u32 off, int size, u32 v) {
  WriteMem_Bar(0, 0, off, size, v);
}

void CVirtioPci::st_wait_ms(int ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// Each device's 1 MB, counted down from 16 MB below the top of guest RAM
// (the EHCI self-test has the top 16 MB).
u32 CVirtioPci::st_base() {
  return (u32)((1ull << cSystem->get_memory_bits()) - (32u << 20) +
               (u64(m_instance) << 20)) &
         ~(kPage - 1);
}

u8 *CVirtioPci::st_mem(u32 a) { return (u8 *)cSystem->PtrToMem(a); }

u8 CVirtioPci::st_isr_peek() {
  std::lock_guard<std::mutex> lk(m_mx);
  return m_state.isr;
}

// The interrupt follows the used ring's update: wait for it to land.
bool CVirtioPci::st_wait_irq(int timeout_ms) {
  for (int t = 0; t < timeout_ms; ++t) {
    if (m_irq_level)
      return true;
    st_wait_ms(1);
  }
  return false;
}

// The late self-test addresses its structures as a driver does: by the
// PCI bus address a direct-mapped DMA window gives this 1 MB of memory.
bool CVirtioPci::st_find_window() {
  const u32 phys = st_base();
  for (u64 bus = phys & 0xfffff; bus < 0x100000000ull; bus += 0x100000) {
    if (cSystem->PCI_Phys(myPCIBus, u32(bus)) == phys &&
        cSystem->PCI_Phys(myPCIBus, u32(bus + 0xfffff)) == phys + 0xfffff) {
      m_st_bus_off = bus - phys;
      printf("%%VIRTIO-I-SELFTEST: %s: through the DMA window, bus %08llx = "
             "memory %08x\n",
             m_name, (unsigned long long)bus, phys);
      return true;
    }
  }
  printf("%%VIRTIO-E-SELFTEST: %s: no direct-mapped DMA window reaches "
         "%08x\n",
         m_name, phys);
  return false;
}

bool CVirtioPci::st_say(const char *what, bool ok) {
  printf("%%VIRTIO-I-SELFTEST: %s: %-40s %s\n", m_name, what,
         ok ? "ok" : "FAILED");
  return ok;
}

bool CVirtioPci::st_setup_queue(StQueue &sq, int q, u32 at) {
  sq.index = q;
  sq.desc = at;
  sq.avail = at + 16 * kQueueSize;
  sq.used = (sq.avail + 6 + 2 * kQueueSize + kPage - 1) & ~(kPage - 1);
  sq.next_desc = sq.avail_idx = sq.used_seen = 0;
  memset(st_mem(at), 0, 3 * kPage);
  st_reg_write(REG_QUEUE_SEL, 16, q);
  if (st_reg_read(REG_QUEUE_SIZE, 16) != kQueueSize)
    return false;
  const u32 pfn = u32(st_bus(at) / kPage);
  st_reg_write(REG_QUEUE_PFN, 32, pfn);
  return st_reg_read(REG_QUEUE_PFN, 32) == pfn;
}

// Descriptors chained in order, the head put in the available ring.
u16 CVirtioPci::st_submit(StQueue &sq, const std::vector<StDesc> &d,
                          bool notify) {
  const u16 head = sq.next_desc;
  for (size_t i = 0; i < d.size(); ++i) {
    const u16 n = u16((sq.next_desc + 1) % kQueueSize);
    u8 e[16];
    const u64 addr = st_bus(d[i].addr);
    const u16 flags =
        u16((i + 1 < d.size() ? D_NEXT : 0) | (d[i].write ? D_WRITE : 0));
    memcpy(e, &addr, 8);
    memcpy(e + 8, &d[i].len, 4);
    memcpy(e + 12, &flags, 2);
    memcpy(e + 14, &n, 2);
    memcpy(st_mem(sq.desc + 16 * sq.next_desc), e, 16);
    sq.next_desc = n;
  }
  memcpy(st_mem(sq.avail + 4 + 2 * (sq.avail_idx % kQueueSize)), &head, 2);
  std::atomic_thread_fence(std::memory_order_release);
  sq.avail_idx++;
  memcpy(st_mem(sq.avail + 2), &sq.avail_idx, 2);
  std::atomic_thread_fence(std::memory_order_seq_cst);
  if (notify)
    st_reg_write(REG_QUEUE_NOTIFY, 16, sq.index);
  return head;
}

bool CVirtioPci::st_wait_used(StQueue &sq, u32 &id, u32 &len, int timeout_ms) {
  for (int t = 0; t < timeout_ms; ++t) {
    u16 idx;
    memcpy(&idx, st_mem(sq.used + 2), 2);
    if (idx != sq.used_seen) {
      std::atomic_thread_fence(std::memory_order_acquire);
      const u32 e = sq.used + 4 + 8 * (sq.used_seen % kQueueSize);
      memcpy(&id, st_mem(e), 4);
      memcpy(&len, st_mem(e + 4), 4);
      sq.used_seen++;
      return true;
    }
    st_wait_ms(1);
  }
  return false;
}

// --- saved state -----------------------------------------------------------

static u32 virtio_magic1 = 0x71A71001;
static u32 virtio_magic2 = 0x100171A7;

int CVirtioPci::SaveState(FILE *f) {
  int res;
  if ((res = CPCIDevice::SaveState(f)))
    return res;
  std::lock_guard<std::mutex> work(m_work);
  std::lock_guard<std::mutex> lk(m_mx);
  long ss = sizeof(m_state);
  long cs = (long)m_config.size();
  fwrite(&virtio_magic1, sizeof(u32), 1, f);
  fwrite(&ss, sizeof(long), 1, f);
  fwrite(&m_state, sizeof(m_state), 1, f);
  fwrite(&cs, sizeof(long), 1, f);
  fwrite(m_config.data(), 1, m_config.size(), f);
  fwrite(&virtio_magic2, sizeof(u32), 1, f);
  printf("%s: %d bytes saved.\n", devid_string, (int)(ss + cs));
  return 0;
}

int CVirtioPci::RestoreState(FILE *f) {
  int res;
  if ((res = CPCIDevice::RestoreState(f)))
    return res;
  std::lock_guard<std::mutex> work(m_work);
  std::lock_guard<std::mutex> lk(m_mx);
  long ss, cs;
  u32 m1, m2;
  if (fread(&m1, sizeof(u32), 1, f) != 1 || m1 != virtio_magic1 ||
      fread(&ss, sizeof(long), 1, f) != 1 || ss != sizeof(m_state) ||
      fread(&m_state, sizeof(m_state), 1, f) != 1 ||
      fread(&cs, sizeof(long), 1, f) != 1 || cs != (long)m_config.size() ||
      fread(m_config.data(), 1, m_config.size(), f) != m_config.size() ||
      fread(&m2, sizeof(u32), 1, f) != 1 || m2 != virtio_magic2) {
    printf("%s: saved state does not match.\n", devid_string);
    return -1;
  }
  update_irq();
  kick();
  printf("%s: %d bytes restored.\n", devid_string, (int)(ss + cs));
  return 0;
}
