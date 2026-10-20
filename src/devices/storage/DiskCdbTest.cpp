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
 * data and lengths with what SCSI-2, SPC-3, SBC-3 and MMC-3 say. A
 * command that throws (a FAILURE, which would stop the machine) fails its
 * check.
 *
 * usage: alphabox cdbtest <work directory> [-v]
 * The images are created in the work directory and removed at the end.
 **/

#include "Disk.hpp"
#include "DiskFile.hpp"
#include "DiskRam.hpp"
#include "SCSIBus.hpp"
#include "StdAfx.hpp"
#include "Sym53C8xx.hpp"
#include "System.hpp"

#include "Sym53C8xxRegs.hpp"

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
        bus->release_bus();
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
  CSystem *sys;
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

/// DISK-08: MODE DATA LENGTH and DBD.
void t_mode_length(Rig &r) {
  group("DISK-08 MODE DATA LENGTH, DBD");
  CCdbTestAdapter *a = r.a;
  Res m = a->run(0, {0x1a, 0, 0x08, 0, 255, 0});
  check(m.good() && m.in.size() >= 4 && m.in[0] == 3 + 8 + 20,
        "MODE SENSE(6) caching page: length 31", "%d",
        m.in.empty() ? -1 : m.in[0]);
  Res d = a->run(0, {0x1a, 0x08, 0x08, 0, 255, 0});
  check(d.good() && d.in.size() >= 6 && d.in[0] == 3 + 20 && d.in[3] == 0 &&
            d.in[4] == 0x08,
        "MODE SENSE(6) with DBD: no block descriptor");
  Res m10 = a->run(0, {0x5a, 0, 0x0a, 0, 0, 0, 0, 1, 0, 0});
  check(m10.good() && m10.in.size() >= 8 && be16(m10.in, 0) == 6 + 8 + 8,
        "MODE SENSE(10) control page: length 22", "%d",
        m10.in.size() >= 2 ? be16(m10.in, 0) : -1);
  Res d10 = a->run(0, {0x5a, 0x08, 0x0a, 0, 0, 0, 0, 1, 0, 0});
  check(d10.good() && d10.in.size() >= 10 && be16(d10.in, 0) == 6 + 8 &&
            be16(d10.in, 6) == 0 && d10.in[8] == 0x0a,
        "MODE SENSE(10) with DBD: no block descriptor");
}

/// DISK-09: INQUIRY lengths.
void t_inquiry_len(Rig &r) {
  group("DISK-09 INQUIRY lengths");
  CCdbTestAdapter *a = r.a;
  Res q = a->run(0, {0x12, 0, 0, 0, 36, 0});
  check(q.good() && q.in.size() >= 5 && q.in[4] == 31, "ADDITIONAL LENGTH 31",
        "%d", q.in.size() > 4 ? q.in[4] : -1);
  Res q2 = a->run(0, {0x12, 0, 0, 1, 0, 0});
  check(q2.good() && q2.in.size() >= 36, "allocation 256 in two bytes", "%s",
        q2.why().c_str());
}

/// DISK-10: a logical unit the target does not have.
void t_lun(Rig &r) {
  group("DISK-10 LUN");
  CCdbTestAdapter *a = r.a;
  expect_cc(r, 0, a->run(0, {0, 0, 0, 0, 0, 0}, {}, {0x81}),
            "TEST UNIT READY to LUN 1", 5, 0x25, 0, {0x81});
  expect_cc(r, 0, a->run(0, cdb10(0x25, 0, 0), {}, {0x81}),
            "READ CAPACITY to LUN 1", 5, 0x25, 0, {0x81});
  Res q = a->run(0, {0x12, 0, 0, 0, 36, 0}, {}, {0x81});
  check(q.good() && q.in.size() >= 1 && q.in[0] == 0x7f,
        "INQUIRY to LUN 1: peripheral qualifier 3");
  Res rs = a->run(0, {0x03, 0, 0, 0, 18, 0}, {}, {0x81});
  check(rs.good() && rs.in.size() >= 14 && (rs.in[2] & 0x0f) == 5 &&
            rs.in[12] == 0x25,
        "REQUEST SENSE to LUN 1: GOOD, LOGICAL UNIT NOT SUPPORTED");
  // With IDENTIFY, the CDB's LUN bits are ignored (SCSI-2 7.2.2); they are
  // RDPROTECT in a READ(10) from SBC-2 on.
  Res rd = a->run(0, cdb10(0x28, 0, 1, 0x20));
  check(rd.good() && blocks_ok(rd.in, 0, 1, 512),
        "READ(10) with byte 1 bit 5 after IDENTIFY: read");
  // Without IDENTIFY, they name the unit.
  Res nl = a->run(0, {0x00, 0x20, 0, 0, 0, 0}, {}, {0x08});
  check(nl.cc(), "TEST UNIT READY with CDB LUN 1 and no IDENTIFY: CHECK", "%s",
        nl.why().c_str());
  a->sense(0);
}

/// DISK-01: writes to write-protected media, and the WP bit.
void t_write_protect(Rig &r) {
  group("DISK-01 write protect");
  CCdbTestAdapter *a = r.a;
  const std::vector<u8> before = read_file(path("ro.img"), 0, 4096);
  expect_cc(r, 1, a->run(1, cdb10(0x2a, 0, 8), std::vector<u8>(4096, 0xee)),
            "WRITE(10) to a read-only image", 7, 0x27, 0);
  check(read_file(path("ro.img"), 0, 4096) == before,
        "the read-only image is unchanged");
  expect_cc(r, 1, a->run(1, {0x0a, 0, 0, 0, 1, 0}, std::vector<u8>(512)),
            "WRITE(6) to a read-only image", 7, 0x27, 0);
  Res rw = a->run(1, cdb10(0x2a, 0, 1), std::vector<u8>(512));
  check(rw.out_taken == 0, "no DATA OUT for a refused write", "%zu",
        rw.out_taken);
  a->sense(1);
  expect_cc(r, 4, a->run(4, cdb10(0x2a, 0, 1), std::vector<u8>(512, 0xee)),
            "WRITE(10) to a read-only RAM disk", 7, 0x27, 0);
  Res rr = a->run(4, cdb10(0x28, 0, 1));
  check(rr.good() && rr.in.size() == 512 && rr.in[0] != 0xee,
        "the read-only RAM disk is unchanged");
  // A CD-ROM has no WRITE commands (SCSI-2 14.2).
  expect_cc(r, 3, a->run(3, cdb10(0x2a, 0, 1), std::vector<u8>(2048)),
            "WRITE(10) to a CD-ROM", 5, 0x20, 0);
  Res cdm = a->run(3, {0x1a, 0, 0x3f, 0, 255, 0});
  check(cdm.good() && cdm.in.size() > 2 && cdm.in[2] == 0,
        "MODE SENSE(6) on a CD-ROM: device-specific byte reserved, 0");
  Res m6 = a->run(1, {0x1a, 0, 0x3f, 0, 255, 0});
  check(m6.good() && m6.in.size() > 2 && (m6.in[2] & 0x80),
        "MODE SENSE(6): WP set on a read-only disk");
  Res m10 = a->run(1, {0x5a, 0, 0x3f, 0, 0, 0, 0, 1, 0, 0});
  check(m10.good() && m10.in.size() > 3 && (m10.in[3] & 0x80),
        "MODE SENSE(10): WP set on a read-only disk");
  Res w6 = a->run(0, {0x1a, 0, 0x3f, 0, 255, 0});
  check(w6.good() && w6.in.size() > 2 && !(w6.in[2] & 0x80),
        "MODE SENSE(6): WP clear on a writable disk");
}

/// DISK-15: transfers above 256 KiB.
void t_big(Rig &r) {
  group("DISK-15 large transfers");
  CCdbTestAdapter *a = r.a;
  for (u16 n : {512, 513, 1024, 2048, 8192}) {
    char name[64];
    Res rd = a->run(0, cdb10(0x28, 1000, n));
    snprintf(name, sizeof(name), "READ(10) of %u blocks", n);
    check(rd.good() && blocks_ok(rd.in, 1000, n, 512), name, "%s",
          rd.why().c_str());
  }
  std::vector<u8> w(2048 * 512);
  for (int i = 0; i < 2048; i++)
    fill_block(&w[i * 512], 1000 + i, 512);
  Res wr = a->run(0, cdb10(0x2a, 4000, 2048), w);
  check(wr.good() && wr.out_taken == w.size(), "WRITE(10) of 2048 blocks", "%s",
        wr.why().c_str());
  Res back = a->run(0, cdb10(0x28, 4000, 2048));
  check(back.good() && back.in == w, "it reads back");
  r.disk->flush();
  check(read_file(path("disk.img"), 4000 * 512, w.size()) == w,
        "it reached the image");

  // A state file written before the transfer state was appended (24 bytes
  // shorter) still restores.
  FILE *f = fopen(path("disk.state").c_str(), "wb+");
  r.disk->SaveState(f);
  fseek(f, 0, SEEK_END);
  std::vector<u8> st((size_t)ftell(f));
  fseek(f, 0, SEEK_SET);
  st.resize(fread(st.data(), 1, st.size(), f));
  fclose(f);
  long ss = 0;
  memcpy(&ss, &st[4], sizeof(long));
  const size_t data_at = 4 + sizeof(long);
  std::vector<u8> old(st.begin(), st.begin() + data_at + ss - 24);
  old.insert(old.end(), st.end() - 4, st.end());
  ss -= 24;
  memcpy(&old[4], &ss, sizeof(long));
  f = fopen(path("disk.state").c_str(), "wb+");
  fwrite(old.data(), 1, old.size(), f);
  fseek(f, 0, SEEK_SET);
  const int rc = r.disk->RestoreState(f);
  fclose(f);
  remove(path("disk.state").c_str());
  Res after = a->run(0, cdb10(0x28, 1000, 600));
  check(rc == 0 && after.good() && blocks_ok(after.in, 1000, 600, 512),
        "a state file from before the transfer state restores");
}

/// Feature: the 64-bit LBA family.
void t_lba64(Rig &r) {
  group("64-bit LBA");
  CCdbTestAdapter *a = r.a;
  Res c16 = a->run(0, {0x9e, 0x10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 32, 0, 0});
  check(c16.good() && c16.in.size() == 32 && be32(c16.in, 0) == 0 &&
            be32(c16.in, 4) == kDiskBlocks - 1 && be32(c16.in, 8) == 512,
        "READ CAPACITY(16)", "%s", c16.why().c_str());
  Res rd = a->run(0, cdb16(0x88, 200, 5));
  check(rd.good() && blocks_ok(rd.in, 200, 5, 512), "READ(16)");
  std::vector<u8> w(512, 0x77);
  expect_good(a->run(0, cdb16(0x8a, 210, 1), w), "WRITE(16)");
  Res b = a->run(0, cdb10(0x28, 210, 1));
  check(b.good() && b.in == w, "WRITE(16) reads back");
  expect_cc(r, 0, a->run(0, cdb16(0x88, 0xffffffffffffffffull, 2)),
            "READ(16) at 2^64-1", 5, 0x21, 0);
  expect_cc(r, 0, a->run(0, cdb16(0x88, 0x100000000ull, 1)),
            "READ(16) above 2^32", 5, 0x21, 0);
  expect_good(a->run(0, cdb16(0x8f, 0, 8)), "VERIFY(16)");

  // A disk past 2 TiB: a sparse image of 2^32 + 8 blocks, which costs the
  // host a few blocks.
  const u64 big = 0x100000008ull;
  FILE *f = fopen(path("big.img").c_str(), "wb");
  const bool made =
      f && fseek_large(f, (off_t_large)(big * 512 - 1), SEEK_SET) == 0 &&
      fputc(0, f) != EOF;
  if (f)
    fclose(f);
  check(made, "a sparse 2 TiB image");
  if (!made)
    return;
  CDisk *d = file_disk(r, r.sys, 6, "file=\"" + path("big.img") + "\";");
  Res c10 = a->run(6, cdb10(0x25, 0, 0));
  check(c10.good() && c10.in.size() == 8 && be32(c10.in, 0) == 0xffffffff,
        "READ CAPACITY(10) above 2^32 blocks: FFFFFFFFh");
  Res c16b = a->run(6, {0x9e, 0x10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 32, 0, 0});
  check(c16b.good() && c16b.in.size() == 32 && be32(c16b.in, 0) == 1 &&
            be32(c16b.in, 4) == 7,
        "READ CAPACITY(16): last LBA 2^32 + 7");
  std::vector<u8> hi(1024, 0x5c);
  expect_good(a->run(6, cdb16(0x8a, 0x100000002ull, 2), hi),
              "WRITE(16) above 2^32");
  Res hb = a->run(6, cdb16(0x88, 0x100000002ull, 2));
  check(hb.good() && hb.in == hi, "READ(16) above 2^32 reads it back");
  expect_cc(r, 6, a->run(6, cdb16(0x88, big - 1, 2)),
            "READ(16) across the end of the 2 TiB disk", 5, 0x21, 0);
  Res c8 = a->run(6, {0x9e, 0x10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 8, 0, 0});
  check(c8.good() && c8.in.size() == 8, "READ CAPACITY(16), allocation 8");
  expect_cc(r, 6,
            a->run(6, {0x9e, 0x11, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 32, 0, 0}),
            "SERVICE ACTION IN(16), another service action", 5, 0x24, 0);
  (void)d;
  remove(path("big.img").c_str());
}

/// Feature: VPD pages 0x83 and 0xB0.
void t_vpd(Rig &r) {
  group("VPD pages");
  CCdbTestAdapter *a = r.a;
  Res v0 = a->run(0, {0x12, 1, 0x00, 0, 255, 0});
  bool has83 = false, hasb0 = false;
  for (size_t i = 4; i < v0.in.size() && i < 4u + v0.in[3]; i++) {
    has83 |= v0.in[i] == 0x83;
    hasb0 |= v0.in[i] == 0xb0;
  }
  check(has83 && hasb0 && v0.in[3] == 4 && v0.in[4] == 0 && v0.in[5] == 0x80 &&
            v0.in[6] == 0x83 && v0.in[7] == 0xb0,
        "page 0 lists 0x00, 0x80, 0x83 and 0xB0, in order");
  Res v83 = a->run(0, {0x12, 1, 0x83, 0, 255, 0});
  const std::string id =
      std::string("DEC     RZ58     (C) DEC") + r.disk->get_serial();
  check(v83.good() && v83.in.size() >= 8 + id.size() && v83.in[1] == 0x83 &&
            v83.in[3] == 4 + id.size() && v83.in[4] == 0x02 &&
            v83.in[5] == 0x01 && v83.in[7] == id.size() &&
            memcmp(&v83.in[8], id.data(), id.size()) == 0,
        "page 0x83: one T10 vendor ID designator, vendor, product, serial",
        "%s", v83.why().c_str());
  Res vb0 = a->run(0, {0x12, 1, 0xb0, 0, 255, 0});
  bool zeros = vb0.in.size() >= 64;
  for (size_t i = 4; zeros && i < 64; i++)
    zeros = vb0.in[i] == 0;
  check(vb0.good() && vb0.in[1] == 0xb0 && be16(vb0.in, 2) == 0x3c && zeros,
        "page 0xB0: length 3Ch, no limits reported", "%s", vb0.why().c_str());
  Res c0 = a->run(3, {0x12, 1, 0x00, 0, 255, 0});
  check(c0.good() && c0.in.size() > 6 && c0.in[3] == 3 && c0.in[6] == 0x83,
        "a CD-ROM lists 0x00, 0x80, 0x83");
  Res c83 = a->run(3, {0x12, 1, 0x83, 0, 255, 0});
  check(c83.good() && c83.in.size() > 32 &&
            memcmp(&c83.in[8], "DEC     RRD42   (C) DEC ", 24) == 0,
        "a CD-ROM's page 0x83");
  expect_cc(r, 3, a->run(3, {0x12, 1, 0xb0, 0, 255, 0}),
            "no block limits page on a CD-ROM", 5, 0x24, 0);
  Res u83 = a->run(5, {0x12, 1, 0x83, 0, 255, 0});
  check(u83.good() && std::string(r.emptycd->get_serial()) !=
                          std::string(r.cd->get_serial()),
        "every drive its own identifier");
}

/// DISK-14: a bus reset resets the targets.
void t_bus_reset(Rig &r) {
  group("DISK-14 bus reset");
  CCdbTestAdapter *a = r.a;
  expect_good(a->run(3, {0x1e, 0, 0, 0, 1, 0}), "PREVENT MEDIUM REMOVAL");
  a->bus->reset_bus();
  check(!r.cd->media_locked(), "the reset unlocks the drive");
  expect_cc(r, 3, a->run(3, {0, 0, 0, 0, 0, 0}),
            "first command after the reset: UNIT ATTENTION", 6, 0x29, -1);
  expect_good(a->run(3, {0, 0, 0, 0, 0, 0}), "then GOOD");
  expect_cc(r, 0, a->run(0, {0, 0, 0, 0, 0, 0}),
            "the disk reports the reset too", 6, 0x29, 0);
  expect_good(a->run(0, {0, 0, 0, 0, 0, 0}), "and then works");
  // INQUIRY does not take the unit attention; REQUEST SENSE does.
  a->bus->reset_bus();
  Res q = a->run(0, {0x12, 0, 0, 0, 36, 0});
  Sense s = a->sense(0);
  check(q.good() && s.key == 6 && s.asc == 0x29,
        "INQUIRY works through it, REQUEST SENSE reports it");
  expect_good(a->run(0, {0, 0, 0, 0, 0, 0}), "and clears it");
  // MODE SELECT's block length returns to the power-on value.
  std::vector<u8> d = {0, 0, 0, 8, 0, 0, 0, 0, 0, 0, 4, 0};
  expect_good(a->run(0, {0x15, 0x10, 0, 0, 12, 0}, d), "block length 1024");
  a->bus->reset_bus();
  a->sense(0);
  Res cap = a->run(0, cdb10(0x25, 0, 0));
  check(cap.good() && be32(cap.in, 4) == 512, "512 again after the reset");
  // BUS DEVICE RESET: the target resets itself and frees the bus.
  a->sense(3); // the CD has the resets above pending
  expect_good(a->run(3, {0x1e, 0, 0, 0, 1, 0}), "PREVENT again");
  Res bdr = a->run(3, {0, 0, 0, 0, 0, 0}, {}, {0x80, 0x0c});
  check(bdr.selected && bdr.error.empty() && bdr.status == -1 &&
            a->bus->get_phase() == SCSI_PHASE_FREE && !r.cd->media_locked(),
        "BUS DEVICE RESET: BUS FREE, no status, the drive unlocked", "%s",
        bdr.why().c_str());
  expect_cc(r, 3, a->run(3, {0, 0, 0, 0, 0, 0}), "then a unit attention", 6,
            0x29, 0);
  // ABORT: BUS FREE, nothing else.
  Res ab = a->run(0, cdb10(0x28, 0, 1), {}, {0x80, 0x06});
  check(ab.selected && ab.error.empty() && ab.status == -1 && ab.in.empty() &&
            a->bus->get_phase() == SCSI_PHASE_FREE,
        "ABORT: BUS FREE, no data, no status");
  expect_good(a->run(0, {0, 0, 0, 0, 0, 0}), "no unit attention after ABORT");
}

/// DISK-19: REPORT SUPPORTED OPERATION CODES.
void t_rsoc(Rig &r) {
  group("DISK-19 RSOC");
  CCdbTestAdapter *a = r.a;
  Res all = a->run(0, {0xa3, 0x0c, 0, 0, 0, 0, 0, 0, 0x10, 0, 0, 0});
  expect_good(all, "all commands");
  auto listed = [&](u8 op) {
    for (size_t i = 4; i + 8 <= all.in.size(); i += 8)
      if (all.in[i] == op)
        return be16(all.in, i + 6);
    return u16(0);
  };
  check(listed(0x08) == 6 && listed(0x0a) == 6 && listed(0x1b) == 6 &&
            listed(0x1e) == 6 && listed(0x15) == 6 && listed(0x55) == 10,
        "READ(6), WRITE(6), START STOP, PREVENT, MODE SELECT listed");
  check(listed(0x3e) == 10, "READ LONG is a 10-byte CDB", "%u", listed(0x3e));
  Res one = a->run(0, {0xa3, 0x0c, 0x01, 0x08, 0, 0, 0, 0, 0x10, 0, 0, 0});
  check(one.good() && one.in.size() >= 2 && (one.in[1] & 7) == 3,
        "READ(6) supported (one command)");
  Res sa = a->run(0, {0xa3, 0xec, 0, 0, 0, 0, 0, 0, 0x10, 0, 0, 0});
  check(sa.good(), "the service action is the low 5 bits");
  bool rc16 = false;
  for (size_t i = 4; i + 8 <= all.in.size(); i += 8)
    rc16 |= all.in[i] == 0x9e && be16(all.in, i + 2) == 0x10 &&
            (all.in[i + 5] & 1) && be16(all.in, i + 6) == 16;
  check(rc16, "READ CAPACITY(16) listed with SERVACTV");
  Res one16 =
      a->run(0, {0xa3, 0x0c, 0x02, 0x9e, 0x00, 0x10, 0, 0, 0x10, 0, 0, 0});
  check(one16.good() && one16.in.size() == 20 && (one16.in[1] & 7) == 3 &&
            one16.in[4] == 0x9e && one16.in[5] == 0x10,
        "READ CAPACITY(16) supported (one service action)");
  Res cd = a->run(3, {0xa3, 0x0c, 0, 0, 0, 0, 0, 0, 0x10, 0, 0, 0});
  bool cd_write = false, cd_toc = false;
  for (size_t i = 4; i + 8 <= cd.in.size(); i += 8) {
    cd_write |= cd.in[i] == 0x2a;
    cd_toc |= cd.in[i] == 0x43;
  }
  check(cd.good() && cd_toc && !cd_write,
        "a CD-ROM lists READ TOC and not WRITE(10)");
}

/// DISK-18: a RAM disk's contents in the state file.
void write_text(const std::string &file, const std::string &text) {
  FILE *f = fopen(file.c_str(), "w");
  if (f) {
    fputs(text.c_str(), f);
    fclose(f);
  }
}

/// DISK-17: BIN/CUE images, one file per track and one file for all.
void t_bincue(Rig &r) {
  group("DISK-17 BIN/CUE");
  CCdbTestAdapter *a = r.a;
  // Two files: 100 cooked data sectors, then 30 raw audio sectors.
  make_image(path("t1.bin"), 100, 2048);
  {
    FILE *f = fopen(path("t2.bin").c_str(), "wb");
    std::vector<u8> s(2352);
    for (int i = 0; i < 30; i++) {
      for (size_t j = 0; j < s.size(); j++)
        s[j] = u8(i + j * 3);
      fwrite(s.data(), 1, s.size(), f);
    }
    fclose(f);
  }
  write_text(path("two.cue"), "FILE \"t1.bin\" BINARY\n"
                              "  TRACK 01 MODE1/2048\n"
                              "    INDEX 01 00:00:00\n"
                              "FILE \"t2.bin\" BINARY\n"
                              "  TRACK 02 AUDIO\n"
                              "    INDEX 01 00:00:00\n");
  // One file of raw MODE1/2352 sectors, user data at byte 16.
  {
    FILE *f = fopen(path("raw.bin").c_str(), "wb");
    std::vector<u8> s(2352, 0xee);
    for (int i = 0; i < 20; i++) {
      fill_block(&s[16], i, 2048);
      fwrite(s.data(), 1, s.size(), f);
    }
    fclose(f);
  }
  write_text(path("raw.cue"), "FILE \"raw.bin\" BINARY\n"
                              "  TRACK 01 MODE1/2352\n"
                              "    INDEX 01 00:00:00\n");
  CDisk *two =
      file_disk(r, r.sys, 8, "file=\"" + path("two.cue") + "\";cdrom=true;");
  CDisk *raw =
      file_disk(r, r.sys, 9, "file=\"" + path("raw.cue") + "\";cdrom=true;");
  (void)two;
  (void)raw;
  a->sense(8);
  a->sense(9);

  Res cap = a->run(8, cdb10(0x25, 0, 0));
  check(cap.good() && be32(cap.in, 0) == 129,
        "two files: 130 sectors, the second file after the first", "%u",
        cap.in.size() == 8 ? be32(cap.in, 0) : 0);
  Res toc = a->run(8, {0x43, 0, 0, 0, 0, 0, 0, 0, 255, 0});
  check(toc.good() && toc.in.size() >= 28 && toc.in[3] == 2 && toc.in[6] == 1 &&
            be32(toc.in, 8) == 0 && toc.in[14] == 2 &&
            be32(toc.in, 16) == 100 && toc.in[22] == 0xaa &&
            be32(toc.in, 24) == 130,
        "two files: TOC track 1 at 0, track 2 at 100, lead-out at 130");
  // In MSF, binary: 100 + 150 frames is 00:03:25, 130 + 150 is 00:03:55.
  Res tm = a->run(8, {0x43, 0x02, 0, 0, 0, 0, 0, 0, 255, 0});
  check(tm.good() && tm.in.size() >= 28 && tm.in[9] == 0 && tm.in[10] == 2 &&
            tm.in[11] == 0 && tm.in[17] == 0 && tm.in[18] == 3 &&
            tm.in[19] == 25 && tm.in[25] == 0 && tm.in[26] == 3 &&
            tm.in[27] == 55,
        "two files: TOC in MSF, 00:02:00, 00:03:25, 00:03:55");
  Res d5 = a->run(8, cdb10(0x28, 5, 2));
  check(d5.good() && blocks_ok(d5.in, 5, 2, 2048), "two files: a data read");
  Res au = a->run(8, cdb10(0x28, 101, 1));
  check(au.good() && au.in.size() == 2048 && au.in[0] == 1 && au.in[1] == 4,
        "two files: the second file's sector 1 at LBA 101");
  Res rc = a->run(9, cdb10(0x25, 0, 0));
  check(rc.good() && be32(rc.in, 0) == 19, "raw MODE1/2352: 20 sectors");
  Res rd = a->run(9, cdb10(0x28, 3, 4));
  check(rd.good() && blocks_ok(rd.in, 3, 4, 2048),
        "raw MODE1/2352: the user data of each sector");
  for (const char *n : {"t1.bin", "t2.bin", "raw.bin", "two.cue", "raw.cue"})
    remove(path(n).c_str());
}

void t_ram_state(Rig &r) {
  group("DISK-18 RAM disk state");
  CCdbTestAdapter *a = r.a;
  a->sense(2); // a unit attention from the bus resets above
  std::vector<u8> w(512, 0x3c);
  expect_good(a->run(2, cdb10(0x2a, 123, 1), w), "write a pattern");
  FILE *f = fopen(path("ram.state").c_str(), "wb+");
  r.ram->SaveState(f);
  a->run(2, cdb10(0x2a, 123, 1), std::vector<u8>(512, 0));
  fseek(f, 0, SEEK_SET);
  int rc = r.ram->RestoreState(f);
  fclose(f);
  remove(path("ram.state").c_str());
  Res rd = a->run(2, cdb10(0x28, 123, 1));
  check(rc == 0 && rd.good() && rd.in == w, "contents survive a restore");
}

/// DISK-05: READ TOC formats.
void t_toc_formats(Rig &r) {
  group("DISK-05 READ TOC formats");
  CCdbTestAdapter *a = r.a;
  Res s1 = a->run(3, {0x43, 0, 1, 0, 0, 0, 0, 0, 255, 0});
  check(s1.good() && s1.in.size() >= 12 && be16(s1.in, 0) == 10 &&
            s1.in[2] == 1 && s1.in[3] == 1 && s1.in[6] == 1 &&
            be32(s1.in, 8) == 0,
        "READ TOC format 1: session 1, track 1 at LBA 0", "%s",
        s1.why().c_str());
  for (int f = 2; f <= 5; f++) {
    char name[40];
    snprintf(name, sizeof(name), "READ TOC format %d: refused", f);
    expect_cc(r, 3, a->run(3, {0x43, 0, u8(f), 0, 0, 0, 0, 0, 255, 0}), name, 5,
              0x24, 0);
  }
  expect_cc(r, 3, a->run(3, {0x43, 0, 0, 0, 0, 0, 2, 0, 255, 0}),
            "READ TOC from track 2 of a one-track disc", 5, 0x24, 0);
}

/// DISK-16: READ TOC in MSF form.
void t_toc_msf(Rig &r) {
  group("DISK-16 READ TOC MSF");
  CCdbTestAdapter *a = r.a;
  Res t = a->run(3, {0x43, 0x02, 0, 0, 0, 0, 0, 0, 255, 0});
  bool ok = t.good() && t.in.size() >= 20;
  check(ok && t.in[9] == 0 && t.in[10] == 2 && t.in[11] == 0,
        "track 1 at 00:02:00", "%02x:%02x:%02x", ok ? t.in[9] : 0,
        ok ? t.in[10] : 0, ok ? t.in[11] : 0);
  // 1000 + 150 frames = 15 s 25 frames.
  check(ok && t.in[17] == 0 && t.in[18] == 15 && t.in[19] == 25,
        "lead-out at 00:15:25", "%02x:%02x:%02x", ok ? t.in[17] : 0,
        ok ? t.in[18] : 0, ok ? t.in[19] : 0);
  Res s1 = a->run(3, {0x43, 0x02, 1, 0, 0, 0, 0, 0, 255, 0});
  check(s1.good() && s1.in.size() >= 12 && s1.in[9] == 0 && s1.in[10] == 2 &&
            s1.in[11] == 0,
        "format 1 (session info) in MSF: 00:02:00");
}

// ---------------------------------------------------------------------------
//  The 53C8xx SCRIPTS processor
// ---------------------------------------------------------------------------

/// A 53C875 at pci0.3 of the test machine, with a disk at id 0 on its bus,
/// driven by SCRIPTS the test writes to memory and starts by writing DSP.
/// The chipset's DMA window 0 maps PCI addresses 0-1 GB onto memory.
struct SymRig {
  CSystem *sys;
  CSym53C8xx *sym;
  u32 next = 0x100000; ///< where the next SCRIPTS program goes

  u8 r8(u32 reg) { return (u8)sym->ReadMem_Bar(0, 1, reg, 8); }
  u32 r32(u32 reg) { return sym->ReadMem_Bar(0, 1, reg, 32); }
  void w8(u32 reg, u8 v) { sym->WriteMem_Bar(0, 1, reg, 8, v); }
  void w32(u32 reg, u32 v) { sym->WriteMem_Bar(0, 1, reg, 32, v); }
  void mw(u32 addr, u32 v) { sys->WriteMem(addr, 32, v, nullptr); }
  u32 mr(u32 addr) { return (u32)sys->ReadMem(addr, 32, nullptr); }

  /// Place a program (pairs of dwords) and return its address.
  u32 program(const std::vector<u32> &words) {
    const u32 at = next;
    for (size_t i = 0; i < words.size(); i++)
      mw(at + 4 * (u32)i, words[i]);
    next += 0x1000;
    return at;
  }

  /// Software reset (ISTAT SRST), then the registers a driver sets up.
  void reset() {
    w8(R_ISTAT, 0x40);
    w8(R_ISTAT, 0x00);
    w8(R_SCID, 0x47); // id 7, respond to reselection
    w8(R_DIEN, 0x7d);
    w8(R_SIEN0, 0x8f);
    w8(R_SIEN1, 0x05);
  }

  /// Start at `at` and wait until the SCRIPTS processor interrupts (ISTAT
  /// DIP or SIP), driving the main-thread timers as the system would.
  /// Returns ISTAT, or 0 on a time-out; `error` gets a SCRIPTS failure.
  u8 run(u32 at, std::string &error, int ms = 2000) {
    w32(R_DSP, at);
    return wait(error, ms);
  }
  u8 wait(std::string &error, int ms = 2000) {
    for (int i = 0; i < ms; i++) {
      try {
        sym->check_state();
      } catch (CException &e) {
        error = e.displayText();
        return 0;
      }
      const u8 istat = r8(R_ISTAT);
      if (istat & 0x03)
        return istat;
      usleep(1000);
    }
    return 0;
  }
};

// SCRIPTS instruction words.
u32 sel(int id, bool atn = true) {
  return (atn ? 0x41000000 : 0x40000000) | (u32(id) << 16);
}
u32 jump_when(int phase) { return 0x800b0000 | (u32(phase) << 24); }
const u32 kJump = 0x80080000;
const u32 kInt = 0x98080000;
u32 move_when(int phase, u32 count) {
  return (u32(0x08 | phase) << 24) | (count & 0xffffff);
}
const u32 kClearAck = 0x60000040;
const u32 kWaitDisconnect = 0x48000000;

/// The 53C875 runs a TEST UNIT READY to the disk at id 0, as a driver's
/// SCRIPTS would; and a selection nobody answers ends in a time-out.
void t_sym_basic(SymRig &s) {
  group("53C8xx SCRIPTS");
  std::string err;
  s.reset();
  // Data: identify, a TEST UNIT READY CDB, the status and message bytes.
  const u32 data = 0x200000;
  s.mw(data, 0x80);              // IDENTIFY
  s.mw(data + 0x10, 0x00000000); // TUR cdb bytes 0-3
  s.mw(data + 0x14, 0x00000000); // bytes 4-5
  s.mw(data + 0x20, 0xffffffff); // status
  s.mw(data + 0x24, 0xffffffff); // message in
  const u32 p = s.program({sel(0), 0, move_when(SCSI_PHASE_MSG_OUT, 1), data,
                           move_when(SCSI_PHASE_COMMAND, 6), data + 0x10,
                           move_when(SCSI_PHASE_STATUS, 1), data + 0x20,
                           move_when(SCSI_PHASE_MSG_IN, 1), data + 0x24,
                           kClearAck, 0, kWaitDisconnect, 0, kInt, 0x1234});
  const u8 istat = s.run(p, err);
  const u8 dstat = s.r8(R_DSTAT);
  check(err.empty() && (istat & 0x01) && (dstat & 0x04) &&
            s.r32(0x30) == 0x1234,
        "a TEST UNIT READY in SCRIPTS ends at its INT",
        "istat %02x dstat %02x "
        "dsps %08x %s",
        istat, dstat, s.r32(0x30), err.c_str());
  check((s.mr(data + 0x20) & 0xff) == 0 && (s.mr(data + 0x24) & 0xff) == 0,
        "status GOOD, COMMAND COMPLETE", "%08x %08x", s.mr(data + 0x20),
        s.mr(data + 0x24));

  // SELECT of the chip's own id, 7: a selection time-out (SIST1 STO) at
  // the next instruction that needs the target, never a stop.
  s.reset();
  const u32 q =
      s.program({sel(7), 0, jump_when(SCSI_PHASE_MSG_OUT), 0, kInt, 0x1});
  const u8 is2 = s.run(q, err);
  const u8 sist1 = s.r8(R_SIST1);
  check(err.empty() && (is2 & 0x02) && (sist1 & 0x04),
        "SELECT 7 (its own id): STO, no stop", "istat %02x sist1 %02x %s", is2,
        sist1, err.c_str());
}

/// SYM-03: a reserved Transfer Control opcode is an illegal instruction.
void t_sym_reserved(SymRig &s) {
  group("SYM-03 reserved opcode");
  std::string err;
  for (u32 opc = 4; opc < 8; opc++) {
    s.reset();
    const u32 p = s.program({0x80080000 | (opc << 27), 0, kInt, 0x1});
    const u8 istat = s.run(p, err);
    const u8 dstat = s.r8(R_DSTAT);
    char name[64];
    snprintf(name, sizeof(name), "Transfer Control opcode %u: DSTAT IID", opc);
    check(err.empty() && (istat & 0x01) && (dstat & 0x01) && !(dstat & 0x04) &&
              s.r32(R_DSP) == p + 8,
          name, "istat %02x dstat %02x dsp %08x %s", istat, dstat, s.r32(R_DSP),
          err.c_str());
  }
}

/// SYM-05: a table-indirect SELECT loads SCNTL3 and SXFER with the ID.
void t_sym_table_select(SymRig &s) {
  group("SYM-05 table-indirect SELECT");
  std::string err;
  s.reset();
  const u32 table = 0x210000;
  // SCNTL3 0x33, id 0, SXFER 0x2c (period and offset), byte 0 unused.
  s.mw(table, 0x33002c00);
  s.w32(R_DSA, table);
  s.w8(R_SCNTL3, 0);
  s.w8(R_SXFER, 0);
  const u32 q = s.program({0x43000000, 0, kInt, 0x2});
  const u8 istat = s.run(q, err);
  const u8 dstat = s.r8(R_DSTAT);
  check(err.empty() && (istat & 0x01) && (dstat & 0x04),
        "the table SELECT runs to the INT", "istat %02x dstat %02x %s", istat,
        dstat, err.c_str());
  check(s.r8(R_SCNTL3) == 0x33 && s.r8(R_SXFER) == 0x2c,
        "SCNTL3 and SXFER come from the table", "scntl3 %02x sxfer %02x",
        s.r8(R_SCNTL3), s.r8(R_SXFER));
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

  // The machine: an ES40 with 64 MB, no processors, and a 53C875 with a
  // disk for the SCRIPTS checks; the disks the CDB checks drive hang off
  // an adapter of the test's own. The rest of the emulator prints as it
  // builds the machine; the checks' own lines start with two spaces and
  // PASS or FAIL.
  if (!make_image(path("sym.img"), 2048, 512)) {
    printf("cdbtest: cannot create the images in %s\n", g_dir.c_str());
    return 2;
  }
  const std::string systext =
      "sys0 = tsunami { memory.bits = 26; pci0.3 = sym53c875 { disk0.0 = "
      "file { file = \"" +
      path("sym.img") + "\"; } } }";
  char *roottext = strdup(systext.c_str());
  CConfigurator *root =
      new CConfigurator(nullptr, strdup("cdbtest"), strdup("cdbtest"), roottext,
                        strlen(roottext));
  CSystem *sys = theSystem;
  SymRig sr;
  sr.sys = sys;
  sr.sym = nullptr;
  for (int i = 0; i < sys->component_count(); i++)
    if (CSym53C8xx *s = dynamic_cast<CSym53C8xx *>(sys->component(i)))
      sr.sym = s;
  // DMA window 0 of the Pchip: PCI 0-1 GB direct-mapped onto memory.
  sys->WriteMem(U64(0x80180000100), 64, 0x3ff00000, nullptr); // WSM0
  sys->WriteMem(U64(0x80180000200), 64, 0, nullptr);          // TBA0
  sys->WriteMem(U64(0x80180000000), 64, 1, nullptr);          // WSBA0
  sr.sym->start_threads();

  Rig r;
  r.sys = sys;
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
  t_mode_length(r);
  t_inquiry_len(r);
  t_lun(r);
  t_write_protect(r);
  t_big(r);
  t_lba64(r);
  t_vpd(r);
  t_bus_reset(r);
  t_rsoc(r);
  t_ram_state(r);
  t_bincue(r);
  t_toc_formats(r);
  t_toc_msf(r);
  if (sr.sym) {
    t_sym_basic(sr);
    t_sym_reserved(sr);
    t_sym_table_select(sr);
  } else
    check(false, "the 53C875 of the test machine");

  printf("cdbtest: %d passed, %d failed\n", g_pass, g_fail);
  printf("%s\n", g_fail ? "FAIL" : "PASS");
  remove(path("disk.img").c_str());
  chmod(path("ro.img").c_str(), 0644);
  remove(path("ro.img").c_str());
  remove(path("cd.iso").c_str());
  remove(path("sym.img").c_str());
  fflush(stdout);
  // The machine is not torn down: its components' destructors expect a
  // running system. The process ends here.
  _exit(g_fail ? 1 : 0);
}
