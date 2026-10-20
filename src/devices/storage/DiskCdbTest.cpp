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
 * `alphabox cdbtest`: the SCSI target's conformance test.
 *
 * Builds disks (an image file, a read-only image, a RAM disk, a CD-ROM
 * ISO) on a SCSI bus of their own, with no guest and no controller model,
 * and drives them through the bus protocol every controller uses
 * (CSCSIDevice: arbitrate, select, then one transfer per phase), as a host
 * adapter would: IDENTIFY in MESSAGE OUT, the CDB, the data, the status
 * and the COMMAND COMPLETE message. Each check compares status, sense,
 * data and lengths with what SPC-3, SBC-2 and MMC-3 say. A command that
 * throws (a FAILURE, which would stop the machine) fails its check.
 *
 * usage: alphabox cdbtest <work directory> [-v]
 * The images are created in the work directory and removed at the end.
 **/

#include "Disk.hpp"
#include "DiskFile.hpp"
#include "DiskRam.hpp"
#include "SCSIBus.hpp"
#include "StdAfx.hpp"
#include "System.hpp"

#include <cstdarg>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace {

int g_pass = 0;
int g_fail = 0;
bool g_verbose = false;
std::string g_group;

void check(bool ok, const char *name, const char *fmt = nullptr, ...) {
  if (ok) {
    g_pass++;
    if (g_verbose)
      printf("  PASS %s: %s\n", g_group.c_str(), name);
    return;
  }
  g_fail++;
  printf("  FAIL %s: %s", g_group.c_str(), name);
  if (fmt) {
    va_list ap;
    va_start(ap, fmt);
    printf(": ");
    vprintf(fmt, ap);
    va_end(ap);
  }
  printf("\n");
}

/// What one command did on the bus.
struct Res {
  bool selected = false;
  int status = -1;        ///< the status byte; -1: no STATUS phase
  std::vector<u8> in;     ///< DATA IN bytes
  size_t out_taken = 0;   ///< DATA OUT bytes the target took
  std::vector<u8> msg_in; ///< MESSAGE IN bytes
  bool data_before_status = false;
  std::string error; ///< what would have stopped the machine

  bool good() const { return selected && error.empty() && status == 0; }
  bool cc() const { return selected && error.empty() && status == 2; }
  std::string why() const {
    char s[160];
    snprintf(s, sizeof(s), "selected %d status %d in %zu out %zu%s%s", selected,
             status, in.size(), out_taken,
             error.empty() ? "" : " error: ", error.c_str());
    return s;
  }
};

struct Sense {
  int key = -1;
  int asc = -1;
  int ascq = -1;
  u32 info = 0;
};

/**
 * The initiator: a host adapter with one bus, ID 7, that runs one command
 * at a time to completion.
 **/
class CCdbTestAdapter : public CSystemComponent,
                        public CDiskController,
                        public CSCSIDevice {
public:
  CCdbTestAdapter(CConfigurator *cfg, CSystem *sys)
      : CSystemComponent(cfg, sys), CDiskController(1, 16) {
    bus = new CSCSIBus(cfg, sys);
    scsi_register(0, bus, 7);
  }
  int SaveState(FILE *) override { return 0; }
  int RestoreState(FILE *) override { return 0; }

  /// Run one command. \a msgs is what the initiator sends in MESSAGE OUT
  /// (IDENTIFY for LUN 0 by default); \a out the DATA OUT bytes.
  Res run(int target, const std::vector<u8> &cdb,
          const std::vector<u8> &out = {},
          const std::vector<u8> &msgs = {0x80}) {
    Res r;
    try {
      if (scsi_get_phase(0) != SCSI_PHASE_FREE)
        bus->reset_bus();
      if (!scsi_arbitrate(0)) {
        r.error = "arbitration lost";
        return r;
      }
      if (!scsi_select(0, target)) {
        if (scsi_get_phase(0) == SCSI_PHASE_ARBITRATION)
          scsi_free(0);
        return r;
      }
      r.selected = true;
      size_t out_pos = 0;
      bool msgs_sent = false;
      for (int guard = 0; guard < 100000; guard++) {
        const int phase = scsi_get_phase(0);
        if (phase == SCSI_PHASE_FREE)
          return r;
        size_t n = scsi_expected_xfer(0);
        switch (phase) {
        case SCSI_PHASE_MSG_OUT: {
          // The whole message stream once; NO OPERATION if asked again.
          const std::vector<u8> nop = {0x08};
          const std::vector<u8> &m = msgs_sent || msgs.empty() ? nop : msgs;
          msgs_sent = true;
          memcpy(scsi_xfer_ptr(0, m.size()), m.data(), m.size());
          scsi_xfer_done(0);
          break;
        }
        case SCSI_PHASE_COMMAND:
          memcpy(scsi_xfer_ptr(0, cdb.size()), cdb.data(), cdb.size());
          scsi_xfer_done(0);
          break;
        case SCSI_PHASE_DATA_IN: {
          if (!n) {
            r.error = "DATA IN with nothing to transfer";
            return r;
          }
          const u8 *p = (const u8 *)scsi_xfer_ptr(0, n);
          r.in.insert(r.in.end(), p, p + n);
          scsi_xfer_done(0);
          break;
        }
        case SCSI_PHASE_DATA_OUT: {
          if (!n) {
            r.error = "DATA OUT with nothing to transfer";
            return r;
          }
          u8 *p = (u8 *)scsi_xfer_ptr(0, n);
          for (size_t i = 0; i < n; i++)
            p[i] = out_pos + i < out.size() ? out[out_pos + i] : 0;
          out_pos += n;
          r.out_taken += n;
          scsi_xfer_done(0);
          break;
        }
        case SCSI_PHASE_STATUS: {
          if (n) {
            r.status = *(const u8 *)scsi_xfer_ptr(0, n);
            if (r.status == 2 && !r.in.empty())
              r.data_before_status = true;
          }
          scsi_xfer_done(0);
          break;
        }
        case SCSI_PHASE_MSG_IN: {
          const u8 *p = (const u8 *)scsi_xfer_ptr(0, n);
          r.msg_in.insert(r.msg_in.end(), p, p + n);
          scsi_xfer_done(0);
          break;
        }
        default:
          r.error = "bus in phase " + std::to_string(phase);
          return r;
        }
      }
      r.error = "the target never released the bus";
    } catch (CException &e) {
      r.error = e.displayText();
    } catch (std::exception &e) {
      r.error = e.what();
    }
    return r;
  }

  Sense sense(int target, const std::vector<u8> &msgs = {0x80}) {
    Sense s;
    Res r = run(target, {0x03, 0, 0, 0, 18, 0}, {}, msgs);
    if (r.good() && r.in.size() >= 14) {
      s.key = r.in[2] & 0x0f;
      s.asc = r.in[12];
      s.ascq = r.in[13];
      s.info = (u32(r.in[3]) << 24) | (u32(r.in[4]) << 16) |
               (u32(r.in[5]) << 8) | r.in[6];
    }
    return s;
  }

  CSCSIBus *bus;
};

/// A configuration node: \a text is the already stripped "k=v;" form.
CConfigurator *node(CConfigurator *parent, const char *name, const char *value,
                    const std::string &text) {
  char *t = strdup(text.c_str());
  return new CConfigurator(parent, strdup(name), strdup(value), t, strlen(t));
}

std::string g_dir;

std::string path(const char *name) { return g_dir + "/" + name; }

/// Each block of a test image holds its LBA in the first eight bytes, then
/// a byte pattern of its own.
void fill_block(u8 *p, u64 lba, size_t bs) {
  for (size_t i = 0; i < bs; i++)
    p[i] = u8(lba * 7 + i * 13 + 1);
  for (int i = 0; i < 8; i++)
    p[i] = u8(lba >> (8 * i));
}

bool make_image(const std::string &file, u64 blocks, size_t bs) {
  FILE *f = fopen(file.c_str(), "wb");
  if (!f)
    return false;
  std::vector<u8> b(bs);
  for (u64 lba = 0; lba < blocks; lba++) {
    fill_block(b.data(), lba, bs);
    if (fwrite(b.data(), 1, bs, f) != bs) {
      fclose(f);
      return false;
    }
  }
  return fclose(f) == 0;
}

std::vector<u8> read_file(const std::string &file, off_t_large at, size_t len) {
  std::vector<u8> v(len);
  FILE *f = fopen_large(file.c_str(), "rb");
  if (!f)
    return {};
  fseek_large(f, at, SEEK_SET);
  v.resize(fread(v.data(), 1, len, f));
  fclose(f);
  return v;
}

bool blocks_ok(const std::vector<u8> &data, u64 lba, size_t count, size_t bs) {
  if (data.size() != count * bs)
    return false;
  std::vector<u8> b(bs);
  for (size_t i = 0; i < count; i++) {
    fill_block(b.data(), lba + i, bs);
    if (memcmp(&data[i * bs], b.data(), bs) != 0)
      return false;
  }
  return true;
}

std::vector<u8> cdb10(u8 op, u32 lba, u16 len, u8 b1 = 0) {
  return {op,      b1, u8(lba >> 24), u8(lba >> 16), u8(lba >> 8),
          u8(lba), 0,  u8(len >> 8),  u8(len),       0};
}

std::vector<u8> cdb12(u8 op, u32 lba, u32 len) {
  return {op,
          0,
          u8(lba >> 24),
          u8(lba >> 16),
          u8(lba >> 8),
          u8(lba),
          u8(len >> 24),
          u8(len >> 16),
          u8(len >> 8),
          u8(len),
          0,
          0};
}

std::vector<u8> cdb16(u8 op, u64 lba, u32 len) {
  std::vector<u8> c(16, 0);
  c[0] = op;
  for (int i = 0; i < 8; i++)
    c[2 + i] = u8(lba >> (56 - 8 * i));
  for (int i = 0; i < 4; i++)
    c[10 + i] = u8(len >> (24 - 8 * i));
  return c;
}

u32 be32(const std::vector<u8> &v, size_t at) {
  return (u32(v[at]) << 24) | (u32(v[at + 1]) << 16) | (u32(v[at + 2]) << 8) |
         v[at + 3];
}
u16 be16(const std::vector<u8> &v, size_t at) {
  return u16((v[at] << 8) | v[at + 1]);
}

/// The machine under test: disks on one adapter's bus.
struct Rig {
  CCdbTestAdapter *a;
  CConfigurator *acfg;
  CDisk *disk = nullptr;    ///< id 0: 8 MB image, read-write
  CDisk *ro = nullptr;      ///< id 1: 1 MB image, read-only
  CDisk *ram = nullptr;     ///< id 2: 4 MB RAM disk
  CDisk *cd = nullptr;      ///< id 3: CD-ROM, a 1000-sector ISO
  CDisk *ramro = nullptr;   ///< id 4: 1 MB RAM disk, read-only
  CDisk *emptycd = nullptr; ///< id 5: CD-ROM drive, no media
};

const u64 kDiskBlocks = 16384; // 8 MB
const u64 kRoBlocks = 2048;    // 1 MB
const u64 kRamBlocks = 8192;   // 4 MB
const u64 kCdBlocks = 1000;

CDisk *file_disk(Rig &r, CSystem *sys, int id, const std::string &cfgtext) {
  char name[16];
  snprintf(name, sizeof(name), "disk0.%d", id);
  CConfigurator *c = node(r.acfg, name, "file", cfgtext);
  CDisk *d = new CDiskFile(c, sys, r.a, 0, id);
  d->scsi_register(0, r.a->bus, id);
  return d;
}

CDisk *ram_disk(Rig &r, CSystem *sys, int id, const std::string &cfgtext) {
  char name[16];
  snprintf(name, sizeof(name), "disk0.%d", id);
  CConfigurator *c = node(r.acfg, name, "ramdisk", cfgtext);
  CDisk *d = new CDiskRam(c, sys, r.a, 0, id);
  d->scsi_register(0, r.a->bus, id);
  return d;
}

// ---------------------------------------------------------------------------
//  Checks
// ---------------------------------------------------------------------------

void expect_good(const Res &r, const char *name) {
  check(r.good(), name, "%s", r.why().c_str());
}

/// CHECK CONDITION with no data phase, and REQUEST SENSE gives key/asc/ascq.
void expect_cc(Rig &rig, int id, const Res &r, const char *name, int key,
               int asc, int ascq, const std::vector<u8> &msgs = {0x80}) {
  if (!r.cc()) {
    check(false, name, "expected CHECK CONDITION, %s", r.why().c_str());
    return;
  }
  Sense s = rig.a->sense(id, msgs);
  check(s.key == key && s.asc == asc && (ascq < 0 || s.ascq == ascq), name,
        "sense %x/%02x/%02x, expected %x/%02x/%02x", s.key, s.asc, s.ascq, key,
        asc, ascq);
}

void group(const char *name) {
  g_group = name;
  if (g_verbose)
    printf("%s\n", name);
}

/// Commands every target answers, on the read-write image.
void t_basic(Rig &r) {
  group("basic");
  CCdbTestAdapter *a = r.a;
  expect_good(a->run(0, {0x00, 0, 0, 0, 0, 0}), "TEST UNIT READY");

  Res q = a->run(0, {0x12, 0, 0, 0, 36, 0});
  expect_good(q, "INQUIRY");
  check(q.in.size() == 36 && q.in[0] == 0x00 &&
            memcmp(&q.in[8], "DEC     RZ58", 12) == 0,
        "INQUIRY data: direct access, DEC RZ58");

  Res v0 = a->run(0, {0x12, 1, 0x00, 0, 255, 0});
  expect_good(v0, "INQUIRY page 0x00");
  Res v80 = a->run(0, {0x12, 1, 0x80, 0, 255, 0});
  expect_good(v80, "INQUIRY page 0x80");
  const std::string serial = r.disk->get_serial();
  check(v80.in.size() >= 4 + serial.size() && v80.in[1] == 0x80 &&
            v80.in[3] == serial.size() &&
            memcmp(&v80.in[4], serial.data(), serial.size()) == 0,
        "INQUIRY page 0x80 is the disk's serial");
  check(serial == "ES40EM00000", "serial of disk0.0", "%s", serial.c_str());
  check(std::string(r.cd->get_serial()) == "ES40EM00003", "serial of disk0.3",
        "%s", r.cd->get_serial());

  Res cap = a->run(0, cdb10(0x25, 0, 0));
  expect_good(cap, "READ CAPACITY(10)");
  check(cap.in.size() == 8 && be32(cap.in, 0) == kDiskBlocks - 1 &&
            be32(cap.in, 4) == 512,
        "READ CAPACITY(10) data", "%u blocks of %u",
        cap.in.size() == 8 ? be32(cap.in, 0) : 0,
        cap.in.size() == 8 ? be32(cap.in, 4) : 0);

  Res rd = a->run(0, cdb10(0x28, 100, 4));
  expect_good(rd, "READ(10)");
  check(blocks_ok(rd.in, 100, 4, 512), "READ(10) data");

  Res rd6 = a->run(0, {0x08, 0, 0, 10, 0, 0}); // 0 blocks means 256
  expect_good(rd6, "READ(6) of 256 blocks");
  check(blocks_ok(rd6.in, 10, 256, 512), "READ(6) data");

  Res rd12 = a->run(0, cdb12(0xa8, 300, 3));
  expect_good(rd12, "READ(12)");
  check(blocks_ok(rd12.in, 300, 3, 512), "READ(12) data");

  std::vector<u8> w(2 * 512);
  fill_block(&w[0], 9000, 512);
  fill_block(&w[512], 9001, 512);
  Res wr = a->run(0, cdb10(0x2a, 50, 2), w);
  expect_good(wr, "WRITE(10)");
  check(wr.out_taken == 1024, "WRITE(10) takes 1024 bytes", "%zu",
        wr.out_taken);
  Res back = a->run(0, cdb10(0x28, 50, 2));
  check(back.good() && back.in == w, "WRITE(10) reads back");
  r.disk->flush();
  check(read_file(path("disk.img"), 50 * 512, 1024) == w,
        "WRITE(10) reaches the image");

  Res wr6 = a->run(0, {0x0a, 0, 0, 60, 1, 0}, std::vector<u8>(512, 0x5a));
  expect_good(wr6, "WRITE(6)");
  Res wr12 = a->run(0, cdb12(0xaa, 61, 1), std::vector<u8>(512, 0xa5));
  expect_good(wr12, "WRITE(12)");
  Res b2 = a->run(0, cdb10(0x28, 60, 2));
  check(b2.good() && b2.in.size() == 1024 && b2.in[0] == 0x5a &&
            b2.in[511] == 0x5a && b2.in[512] == 0xa5 && b2.in[1023] == 0xa5,
        "WRITE(6) and WRITE(12) read back");

  expect_good(a->run(0, cdb10(0x2f, 0, 16)), "VERIFY(10)");
  expect_good(a->run(0, cdb10(0x35, 0, 0)), "SYNCHRONIZE CACHE(10)");
  expect_good(a->run(0, cdb10(0x2b, 1000, 0)), "SEEK(10)");

  Sense s = a->sense(0);
  check(s.key == 0 && s.asc == 0, "REQUEST SENSE after GOOD: NO SENSE",
        "%x/%02x/%02x", s.key, s.asc, s.ascq);

  expect_cc(r, 0, a->run(0, {0xc5, 0, 0, 0, 0, 0}), "unknown opcode", 5, 0x20,
            0);
  expect_cc(r, 0, a->run(0, cdb10(0x28, kDiskBlocks, 1)),
            "READ(10) past the end", 5, 0x21, 0);
  expect_cc(r, 0, a->run(0, cdb10(0x28, kDiskBlocks - 1, 2)),
            "READ(10) across the end", 5, 0x21, 0);
  expect_cc(r, 0, a->run(0, cdb10(0x2a, kDiskBlocks, 1)),
            "WRITE(10) past the end", 5, 0x21, 0);
  expect_cc(r, 0, a->run(0, {0x1a, 0, 0x07, 0, 255, 0}),
            "MODE SENSE of a page the disk lacks", 5, 0x24, 0);
}

/// MODE SENSE pages the disk has.
void t_mode_sense(Rig &r) {
  group("mode sense");
  CCdbTestAdapter *a = r.a;
  const u8 pages[] = {0x01, 0x03, 0x04, 0x05, 0x08, 0x0a, 0x1c, 0x3f};
  for (u8 p : pages) {
    char name[64];
    snprintf(name, sizeof(name), "MODE SENSE(6) page %02x", p);
    expect_good(a->run(0, {0x1a, 0, p, 0, 255, 0}), name);
    snprintf(name, sizeof(name), "MODE SENSE(10) page %02x", p);
    expect_good(a->run(0, {0x5a, 0, p, 0, 0, 0, 0, 1, 0, 0}), name);
    snprintf(name, sizeof(name), "MODE SENSE(6) changeable page %02x", p);
    expect_good(a->run(0, {0x1a, 0, u8(0x40 | p), 0, 255, 0}), name);
  }
  Res m = a->run(0, {0x1a, 0, 0x08, 0, 255, 0});
  check(m.in.size() >= 32 && m.in[3] == 8 && m.in[12] == 0x08 &&
            m.in[13] == 0x12,
        "MODE SENSE(6) caching page after one block descriptor");
  check(m.in.size() >= 12 && m.in[9] == 0 && m.in[10] == 2 && m.in[11] == 0,
        "MODE SENSE block descriptor: 512-byte blocks");
}

/// The CD-ROM drive.
void t_cdrom(Rig &r) {
  group("cdrom");
  CCdbTestAdapter *a = r.a;
  Res q = a->run(3, {0x12, 0, 0, 0, 36, 0});
  check(q.good() && q.in.size() == 36 && q.in[0] == 0x05 && q.in[1] == 0x80 &&
            memcmp(&q.in[8], "DEC     RRD42", 13) == 0,
        "INQUIRY: removable CD-ROM, DEC RRD42");
  Res cap = a->run(3, cdb10(0x25, 0, 0));
  check(cap.good() && cap.in.size() == 8 && be32(cap.in, 0) == kCdBlocks - 1 &&
            be32(cap.in, 4) == 2048,
        "READ CAPACITY: 2048-byte blocks");
  Res rd = a->run(3, cdb10(0x28, 17, 2));
  check(rd.good() && blocks_ok(rd.in, 17, 2, 2048), "READ(10) of two sectors");

  // READ TOC, format 0, LBA form: track 1 at 0, the lead-out at the end.
  Res toc = a->run(3, {0x43, 0, 0, 0, 0, 0, 0, 0, 255, 0});
  expect_good(toc, "READ TOC format 0");
  check(toc.in.size() >= 20 && be16(toc.in, 0) == 18 && toc.in[2] == 1 &&
            toc.in[3] == 1 && toc.in[6] == 1 && be32(toc.in, 8) == 0 &&
            toc.in[14] == 0xaa && be32(toc.in, 16) == kCdBlocks,
        "READ TOC format 0: track 1 at LBA 0, lead-out at the capacity");
  Res tocaa = a->run(3, {0x43, 0, 0, 0, 0, 0, 0xaa, 0, 255, 0});
  check(tocaa.good() && tocaa.in.size() >= 12 && be16(tocaa.in, 0) == 10 &&
            tocaa.in[6] == 0xaa,
        "READ TOC from track 0xAA: the lead-out only");

  // PREVENT MEDIUM REMOVAL stops an eject; ALLOW lets it through.
  expect_good(a->run(3, {0x1e, 0, 0, 0, 1, 0}), "PREVENT MEDIUM REMOVAL");
  check(r.cd->media_locked(), "the drive is locked");
  expect_cc(r, 3, a->run(3, {0x1b, 0, 0, 0, 0x02, 0}),
            "START STOP UNIT eject while locked", 5, 0x53, 2);
  expect_good(a->run(3, {0x1e, 0, 0, 0, 0, 0}), "ALLOW MEDIUM REMOVAL");
  check(!r.cd->media_locked(), "the drive is unlocked");

  // The empty drive.
  expect_cc(r, 5, a->run(5, {0x00, 0, 0, 0, 0, 0}),
            "TEST UNIT READY on an empty drive", 2, 0x3a, 1);
  expect_good(a->run(5, {0x12, 0, 0, 0, 36, 0}), "INQUIRY on an empty drive");
}

/// The RAM disk.
void t_ram(Rig &r) {
  group("ramdisk");
  CCdbTestAdapter *a = r.a;
  Res cap = a->run(2, cdb10(0x25, 0, 0));
  check(cap.good() && cap.in.size() == 8 && be32(cap.in, 0) == kRamBlocks - 1,
        "READ CAPACITY");
  std::vector<u8> w(8 * 512);
  for (int i = 0; i < 8; i++)
    fill_block(&w[i * 512], 70 + i, 512);
  expect_good(a->run(2, cdb10(0x2a, 70, 8), w), "WRITE(10)");
  Res rd = a->run(2, cdb10(0x28, 70, 8));
  check(rd.good() && blocks_ok(rd.in, 70, 8, 512), "READ(10) reads it back");
  expect_cc(r, 2, a->run(2, cdb10(0x28, kRamBlocks, 1)),
            "READ(10) past the end", 5, 0x21, 0);
}

/// Messages a target is sent.
void t_messages(Rig &r) {
  group("messages");
  CCdbTestAdapter *a = r.a;
  // SDTR: the disk answers with the same period and offset.
  Res sd = a->run(0, {0x00, 0, 0, 0, 0, 0}, {},
                  {0x80, 0x01, 0x03, 0x01, 0x19, 0x08});
  expect_good(sd, "TEST UNIT READY after SDTR");
  check(sd.msg_in.size() >= 5 && sd.msg_in[0] == 0x01 && sd.msg_in[1] == 0x03 &&
            sd.msg_in[2] == 0x01 && sd.msg_in[3] == 0x19 &&
            sd.msg_in[4] == 0x08,
        "SDTR is answered in kind");
  // WDTR: the disk is narrow and says so.
  Res wd = a->run(0, {0x00, 0, 0, 0, 0, 0}, {}, {0x80, 0x01, 0x02, 0x03, 0x01});
  expect_good(wd, "TEST UNIT READY after WDTR");
  check(wd.msg_in.size() >= 4 && wd.msg_in[0] == 0x01 && wd.msg_in[1] == 0x02 &&
            wd.msg_in[2] == 0x03 && wd.msg_in[3] == 0x00,
        "WDTR is answered 8-bit");
  // A tag message is taken.
  expect_good(a->run(0, {0x00, 0, 0, 0, 0, 0}, {}, {0x80, 0x20, 0x05}),
              "TEST UNIT READY with a SIMPLE QUEUE TAG");
  // An unknown message is rejected.
  Res rj = a->run(0, {0x00, 0, 0, 0, 0, 0}, {}, {0x80, 0x05});
  check(rj.good() && !rj.msg_in.empty() && rj.msg_in[0] == 0x07,
        "an unknown message gets MESSAGE REJECT", "%s", rj.why().c_str());
}

/// The ATAPI form: no message phases, as IDE and USB use the same code.
void t_atapi(CSystem *sys, CConfigurator *root) {
  group("atapi");
  CConfigurator *c = node(root, "ide9", "cdbtest", "");
  CCdbTestAdapter *a = new CCdbTestAdapter(c, sys);
  CConfigurator *dc =
      node(c, "disk0.0", "file", "file=\"" + path("cd.iso") + "\";cdrom=true;");
  CDisk *d = new CDiskFile(dc, sys, a, 0, 0);
  d->scsi_register(0, a->bus, 0);
  d->set_atapi_mode();
  Res q = a->run(0, {0x12, 0, 0, 0, 36, 0, 0, 0, 0, 0, 0, 0}, {}, {});
  check(q.good() && q.in.size() == 36 && q.in[0] == 0x05 && q.msg_in.empty(),
        "INQUIRY, no message phases", "%s", q.why().c_str());
  Res rd = a->run(0, {0x28, 0, 0, 0, 0, 5, 0, 0, 3, 0, 0, 0}, {}, {});
  check(rd.good() && blocks_ok(rd.in, 5, 3, 2048), "READ(10)");
  Res bad = a->run(0, {0x28, 0, 0, 0, 0x10, 0, 0, 0, 1, 0, 0, 0}, {}, {});
  check(bad.cc() && d->sense_key() == 5,
        "READ(10) past the end: CHECK CONDITION, sense key readable",
        "%s key %d", bad.why().c_str(), d->sense_key());
}

/// DISK-12 and DISK-13: selections nobody answers.
void t_selection(Rig &r) {
  group("DISK-12/13 selection");
  CCdbTestAdapter *a = r.a;
  for (int id : {7, 6, 16, 100, 255, -1}) {
    Res t = a->run(id, {0, 0, 0, 0, 0, 0});
    char name[64];
    snprintf(name, sizeof(name), "SELECT %d: time-out", id);
    check(!t.selected && t.error.empty(), name, "%s", t.why().c_str());
  }
  expect_good(a->run(0, {0, 0, 0, 0, 0, 0}), "the bus works afterwards");
}

/// DISK-11: malformed messages.
void t_bad_messages(Rig &r) {
  group("DISK-11 messages");
  CCdbTestAdapter *a = r.a;
  const std::vector<std::vector<u8>> streams = {
      {0x80, 0x01, 0x00, 0x01},             // SDTR, length 0
      {0x80, 0x01, 0xff, 0x01, 0x19},       // SDTR, length 255
      {0x80, 0x01, 0x03, 0x01},             // SDTR cut short
      {0x80, 0x01},                         // extended, no length
      {0x80, 0x01, 0x05, 0x04, 1, 2, 3, 4}, // PPR
  };
  int i = 0;
  for (const auto &m : streams) {
    Res t = a->run(0, {0, 0, 0, 0, 0, 0}, {}, m);
    char name[64];
    snprintf(name, sizeof(name), "message stream %d", i++);
    check(t.good() && t.msg_in.size() >= 1 && t.msg_in.size() < 300 &&
              t.msg_in[0] == 0x07,
          name, "%s msg_in %zu", t.why().c_str(), t.msg_in.size());
  }
}

/// DISK-02: the LBA range check in 64 bits.
void t_lba_wrap(Rig &r) {
  group("DISK-02 LBA wrap");
  CCdbTestAdapter *a = r.a;
  for (int id : {0, 2}) {
    for (u16 len : {1, 2, 3}) {
      char name[80];
      snprintf(name, sizeof(name), "READ(10) at 0xFFFFFFFF, %u blocks, id %d",
               len, id);
      expect_cc(r, id, a->run(id, cdb10(0x28, 0xffffffff, len)), name, 5, 0x21,
                0);
      snprintf(name, sizeof(name), "WRITE(10) at 0xFFFFFFFF, %u blocks, id %d",
               len, id);
      expect_cc(
          r, id,
          a->run(id, cdb10(0x2a, 0xffffffff, len), std::vector<u8>(len * 512)),
          name, 5, 0x21, 0);
    }
    char name[80];
    snprintf(name, sizeof(name), "READ(12) at 0xFFFFFFFF, 3 blocks, id %d", id);
    expect_cc(r, id, a->run(id, cdb12(0xa8, 0xffffffff, 3)), name, 5, 0x21, 0);
    snprintf(name, sizeof(name), "READ(12) of 0xFFFFFFFF blocks, id %d", id);
    expect_cc(r, id, a->run(id, cdb12(0xa8, 1, 0xffffffff)), name, 5, 0x21, 0);
    snprintf(name, sizeof(name), "SEEK(10) to 0x80000000, id %d", id);
    expect_cc(r, id, a->run(id, cdb10(0x2b, 0x80000000, 0)), name, 5, 0x21, 0);
    snprintf(name, sizeof(name), "VERIFY(10) at 0xFFFFFFFF, id %d", id);
    expect_cc(r, id, a->run(id, cdb10(0x2f, 0xffffffff, 2)), name, 5, 0x21, 0);
  }
}

/// DISK-03: MODE SELECT block lengths.
void t_mode_select_block(Rig &r) {
  group("DISK-03 MODE SELECT block length");
  CCdbTestAdapter *a = r.a;
  auto ms6 = [&](int id, u32 bl) {
    std::vector<u8> d = {0,           0,     0, 8, 0, 0, 0, 0, 0, u8(bl >> 16),
                         u8(bl >> 8), u8(bl)};
    return a->run(id, {0x15, 0x10, 0, 0, 12, 0}, d);
  };
  expect_cc(r, 0, ms6(0, 0), "block length 0", 5, 0x26, 0);
  expect_cc(r, 0, ms6(0, 3), "block length 3", 5, 0x26, 0);
  expect_cc(r, 0, ms6(0, 520), "block length 520", 5, 0x26, 0);
  expect_cc(r, 0, ms6(0, 8192), "block length 8192", 5, 0x26, 0);
  Res cap = a->run(0, cdb10(0x25, 0, 0));
  check(cap.good() && be32(cap.in, 4) == 512 &&
            be32(cap.in, 0) == kDiskBlocks - 1,
        "a refused block length changes nothing");
  expect_good(ms6(0, 512), "block length 512 on a disk");
  expect_good(ms6(3, 512), "block length 512 on a CD-ROM");
  Res cd = a->run(3, cdb10(0x25, 0, 0));
  check(cd.good() && be32(cd.in, 4) == 512 &&
            be32(cd.in, 0) == kCdBlocks * 4 - 1,
        "the CD-ROM then has 512-byte blocks");
  Res rd = a->run(3, cdb10(0x28, 4 * 17 + 1, 1));
  std::vector<u8> sector(2048);
  fill_block(sector.data(), 17, 2048);
  check(rd.good() && rd.in.size() == 512 &&
            memcmp(rd.in.data(), &sector[512], 512) == 0,
        "and reads the second quarter of sector 17 as block 69");
  expect_good(ms6(3, 2048), "block length 2048 on a CD-ROM");
}

/// DISK-04: MODE SENSE page control and the CD's page 5.
void t_mode_sense_pc(Rig &r) {
  group("DISK-04 MODE SENSE page control");
  CCdbTestAdapter *a = r.a;
  int stops = 0;
  for (int id : {0, 3}) {
    for (int p = 0; p < 0x40; p++) {
      for (int pc = 0; pc < 4; pc++) {
        Res m = a->run(id, {0x1a, 0, u8(pc << 6 | p), 0, 255, 0});
        Res m10 = a->run(id, {0x5a, 0, u8(pc << 6 | p), 0, 0, 0, 0, 1, 0, 0});
        if (!m.error.empty() || !m10.error.empty() ||
            (m.status != 0 && m.status != 2)) {
          if (stops++ < 3) {
            char name[80];
            snprintf(name, sizeof(name), "id %d page %02x PC %d", id, p, pc);
            check(false, name, "%s",
                  m.error.empty() ? m10.error.c_str() : m.error.c_str());
          }
        }
        a->sense(id);
      }
    }
  }
  check(stops == 0, "MODE SENSE(6)/(10) of every page and PC: no stop", "%d",
        stops);
  expect_good(a->run(0, {0x1a, 0, 0x88, 0, 255, 0}),
              "default values (PC 10b) of the caching page");
  Res d = a->run(0, {0x1a, 0, 0x88, 0, 255, 0});
  Res c = a->run(0, {0x1a, 0, 0x08, 0, 255, 0});
  check(d.in == c.in, "default values equal current values");
  expect_cc(r, 0, a->run(0, {0x1a, 0, 0xc8, 0, 255, 0}),
            "saved values (PC 11b)", 5, 0x39, 0);
  expect_cc(r, 3, a->run(3, {0x1a, 0, 0x05, 0, 255, 0}), "page 5 on a CD-ROM",
            5, 0x24, 0);
  Res m = a->run(0, {0x1a, 0, 0x07, 0, 255, 0});
  check(m.cc() && m.in.empty(), "no DATA IN before CHECK CONDITION", "%s",
        m.why().c_str());
  a->sense(0);
}

/// DISK-05: READ CAPACITY with PMI.
void t_capacity_pmi(Rig &r) {
  group("DISK-05 READ CAPACITY PMI");
  CCdbTestAdapter *a = r.a;
  Res pmi = a->run(0, {0x25, 0, 0, 0, 0, 0, 0, 0, 1, 0});
  check(pmi.good() && pmi.in.size() == 8 && be32(pmi.in, 0) == kDiskBlocks - 1,
        "READ CAPACITY with PMI", "%s", pmi.why().c_str());
  Res pmi2 = a->run(0, {0x25, 0, 0, 0, 0x10, 0, 0, 0, 1, 0});
  check(pmi2.good() && pmi2.in.size() == 8 &&
            be32(pmi2.in, 0) == kDiskBlocks - 1,
        "READ CAPACITY with PMI and an LBA on the disk");
  expect_cc(r, 0, a->run(0, cdb10(0x25, 16, 0)),
            "READ CAPACITY with an LBA and no PMI", 5, 0x24, 0);
  std::vector<u8> past = cdb10(0x25, kDiskBlocks, 0);
  past[8] = 1;
  expect_cc(r, 0, a->run(0, past), "READ CAPACITY with PMI past the end", 5,
            0x21, 0);
}

/// DISK-06: sense data lasts until the next command.
void t_sense_cleared(Rig &r) {
  group("DISK-06 sense cleared");
  CCdbTestAdapter *a = r.a;
  a->run(0, cdb10(0x28, kDiskBlocks, 1));
  expect_good(a->run(0, {0, 0, 0, 0, 0, 0}), "TEST UNIT READY after an error");
  Sense s = a->sense(0);
  check(s.key == 0 && s.asc == 0, "the next command clears the sense",
        "%x/%02x/%02x", s.key, s.asc, s.ascq);
  // SCSI-2 8.2.14: any command, INQUIRY included, clears it.
  a->run(0, cdb10(0x28, kDiskBlocks, 1));
  a->run(0, {0x12, 0, 0, 0, 36, 0});
  s = a->sense(0);
  check(s.key == 0 && s.asc == 0, "so does INQUIRY", "%x/%02x/%02x", s.key,
        s.asc, s.ascq);
  // REQUEST SENSE retrieves it once.
  a->run(0, cdb10(0x28, kDiskBlocks, 1));
  s = a->sense(0);
  Sense s2 = a->sense(0);
  check(s.key == 5 && s.asc == 0x21 && s2.key == 0,
        "REQUEST SENSE returns it, then NO SENSE");
  // The ATAPI path reads the sense key after the command that failed.
  a->run(0, cdb10(0x28, kDiskBlocks, 1));
  check(r.disk->sense_key() == 5, "sense_key() after the failed command");
  a->sense(0);
}

/// DISK-07 and DISK-31: allocation lengths, and no data with CHECK CONDITION.
void t_alloc(Rig &r) {
  group("DISK-07 allocation length");
  CCdbTestAdapter *a = r.a;
  Res full = a->run(3, {0x46, 0, 0, 0, 0, 0, 0, 0x10, 0, 0});
  check(full.good() && full.in.size() > 8 &&
            be32(full.in, 0) + 4 == full.in.size(),
        "GET CONFIGURATION: the whole feature list, with status", "%s",
        full.why().c_str());
  for (u16 al : {0, 1, 4, 5, 8, 36, 255}) {
    char name[64];
    Res g = a->run(3, {0x46, 0, 0, 0, 0, 0, 0, u8(al >> 8), u8(al), 0});
    snprintf(name, sizeof(name), "GET CONFIGURATION, allocation %u", al);
    check(g.good() && g.in.size() == std::min<size_t>(al, full.in.size()) &&
              (al < 4 || be32(g.in, 0) == be32(full.in, 0)),
          name, "%s", g.why().c_str());
    Res sc =
        a->run(3, {0x42, 0x02, 0x40, 0x01, 0, 0, 0, u8(al >> 8), u8(al), 0});
    snprintf(name, sizeof(name), "READ SUB-CHANNEL, allocation %u", al);
    check(sc.good() && sc.in.size() == std::min<size_t>(al, 16), name, "%s",
          sc.why().c_str());
    Res ge = a->run(3, {0x4a, 1, 0, 0, 0x10, 0, 0, u8(al >> 8), u8(al), 0});
    snprintf(name, sizeof(name), "GET EVENT STATUS NOTIFICATION, allocation %u",
             al);
    check(ge.good() && ge.in.size() == std::min<size_t>(al, 8), name, "%s",
          ge.why().c_str());
  }
  Res ne = a->run(3, {0x4a, 1, 0, 0, 0x04, 0, 0, 0, 8, 0});
  check(ne.good() && ne.in.size() == 4 && ne.in[2] == 0x80,
        "GESN for a class the drive lacks: NEA, class 0");
  expect_cc(r, 3, a->run(3, {0x42, 0x02, 0x40, 0x07, 0, 0, 0, 0, 16, 0}),
            "READ SUB-CHANNEL of a reserved format", 5, 0x24, 0);
}

} // namespace

int main_cdbtest(int argc, char **argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc < 2) {
    printf("usage: alphabox cdbtest <work directory> [-v]\n");
    return 2;
  }
  g_dir = argv[1];
  g_verbose = argc > 2 && strcmp(argv[2], "-v") == 0;
  mkdir(g_dir.c_str(), 0755);
  chmod(path("ro.img").c_str(), 0644); // left by a run that crashed
  remove(path("ro.img").c_str());

  if (!make_image(path("disk.img"), kDiskBlocks, 512) ||
      !make_image(path("ro.img"), kRoBlocks, 512) ||
      !make_image(path("cd.iso"), kCdBlocks, 2048)) {
    printf("cdbtest: cannot create the images in %s\n", g_dir.c_str());
    return 2;
  }
  chmod(path("ro.img").c_str(), 0444);

  // The rest of the emulator prints as it builds the machine; the checks'
  // own lines start with two spaces and PASS or FAIL.
  char roottext[] = "memory.bits=26;";
  CConfigurator *root =
      new CConfigurator(nullptr, strdup("cdbtest"), strdup("cdbtest"), roottext,
                        strlen(roottext));
  CSystem *sys = new CSystem(root);

  Rig r;
  r.acfg = node(root, "scsi0", "cdbtest", "");
  r.a = new CCdbTestAdapter(r.acfg, sys);
  r.disk = file_disk(r, sys, 0, "file=\"" + path("disk.img") + "\";");
  r.ro =
      file_disk(r, sys, 1, "file=\"" + path("ro.img") + "\";read_only=true;");
  r.ram = ram_disk(r, sys, 2, "size=4194304;");
  r.cd = file_disk(r, sys, 3, "file=\"" + path("cd.iso") + "\";cdrom=true;");
  r.ramro = ram_disk(r, sys, 4, "size=1048576;read_only=true;");
  r.emptycd = file_disk(r, sys, 5, "cdrom=true;");

  printf("cdbtest: checks\n");
  t_basic(r);
  t_mode_sense(r);
  t_cdrom(r);
  t_ram(r);
  t_messages(r);
  t_atapi(sys, root);
  t_selection(r);
  t_bad_messages(r);
  t_lba_wrap(r);
  t_mode_select_block(r);
  t_mode_sense_pc(r);
  t_capacity_pmi(r);
  t_sense_cleared(r);
  t_alloc(r);

  printf("cdbtest: %d passed, %d failed\n", g_pass, g_fail);
  printf("%s\n", g_fail ? "FAIL" : "PASS");
  remove(path("disk.img").c_str());
  chmod(path("ro.img").c_str(), 0644);
  remove(path("ro.img").c_str());
  remove(path("cd.iso").c_str());
  fflush(stdout);
  // The machine is not torn down: its components' destructors expect a
  // running system. The process ends here.
  _exit(g_fail ? 1 : 0);
}
