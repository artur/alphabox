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

/* Unit test of the NAT network back end (src/devices/net/NetworkNat.cpp,
 * on libslirp), driven the way a NIC drives it: Ethernet frames in through
 * send(), frames out of receive(), built by hand as a guest's IP stack
 * would. DHCP, a plain BOOTP request (no option 53, what SRM sends), TFTP,
 * TCP to a listener on the host's 127.0.0.1 through the NAT's host address,
 * a connection from the host into the guest through hostfwd, a guest-bound
 * frame lost on the way and recovered by slirp's retransmission, and the
 * bounded queue. DNS and a real web server need the internet and run only
 * with ALPHABOX_NAT_TEST_NET=1. Built only with libslirp, as `nat_test`;
 * exit status 0 when every check passes.
 */

#include "Configurator.hpp"
#include "NetworkNat.hpp"
#include "StdAfx.hpp"

#include <arpa/inet.h>
#include <chrono>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

// ---- stubs for what the back end's init() needs from the configuration

char *CConfigurator::get_text_value(const char *, const char *def) {
  return (char *)def;
}
bool CConfigurator::get_bool_value(const char *, bool def) { return def; }

// ---- the harness

static int checks = 0, failures = 0;
#define CHECK(c, ...)                                                          \
  do {                                                                         \
    checks++;                                                                  \
    if (!(c)) {                                                                \
      failures++;                                                              \
      printf("FAIL ");                                                         \
      printf(__VA_ARGS__);                                                     \
      printf("\n");                                                            \
    }                                                                          \
  } while (0)

typedef std::vector<u8> Frame;

static const u8 GMAC[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
static const u8 BCAST[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
static u8 SMAC[6]; // the NAT's MAC, learned from its frames
static CNetworkNat *nat;
static std::vector<Frame> frames; // everything the NAT sent the guest
static bool drop_next_tcp_data = false;
static int dropped_data = 0;

static long long now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

static u32 ip4(const char *s) {
  in_addr a;
  inet_pton(AF_INET, s, &a);
  return a.s_addr;
}

static u16 csum(const u8 *b, int n, u32 s) {
  for (int i = 0; i + 1 < n; i += 2)
    s += (b[i] << 8) | b[i + 1];
  if (n & 1)
    s += b[n - 1] << 8;
  while (s >> 16)
    s = (s & 0xffff) + (s >> 16);
  return (u16)~s;
}

static void to_nat(const u8 *f, int n) { nat->send(f, n); }

/// The guest answers an ARP request for its own address.
static void arp_reply(const Frame &q) {
  u8 f[42];
  memcpy(f, &q[6], 6);
  memcpy(f + 6, GMAC, 6);
  f[12] = 0x08, f[13] = 0x06;
  memcpy(f + 14, &q[14], 6);
  f[20] = 0, f[21] = 2;
  memcpy(f + 22, GMAC, 6);
  memcpy(f + 28, &q[38], 4);
  memcpy(f + 32, &q[22], 6);
  memcpy(f + 38, &q[28], 4);
  to_nat(f, 42);
}

/// Drain the NAT's output for `ms`, answering ARP and keeping the rest.
static void pump(int ms) {
  long long end = now_ms() + ms;
  do {
    const u8 *d;
    int n;
    while (nat->receive(&d, &n) == 1) {
      memcpy(SMAC, d + 6, 6);
      Frame f(d, d + n);
      if (n >= 42 && f[12] == 0x08 && f[13] == 0x06 && f[21] == 1) {
        arp_reply(f);
        continue;
      }
      // A TCP segment with data, lost on its way to the guest.
      if (drop_next_tcp_data && n > 54 && f[12] == 0x08 && f[23] == 6) {
        int hl = (f[14] & 15) * 4;
        int th = (f[14 + hl + 12] >> 4) * 4;
        int dl = ((f[16] << 8) | f[17]) - hl - th;
        if (dl > 0) {
          drop_next_tcp_data = false;
          dropped_data++;
          continue;
        }
      }
      frames.push_back(std::move(f));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  } while (now_ms() < end);
}

static int eth_ip(u8 *f, const u8 *dmac, u8 proto, u32 src, u32 dst,
                  int l4len) {
  memcpy(f, dmac, 6);
  memcpy(f + 6, GMAC, 6);
  f[12] = 0x08, f[13] = 0x00;
  u8 *ip = f + 14;
  memset(ip, 0, 20);
  ip[0] = 0x45;
  int tot = 20 + l4len;
  ip[2] = tot >> 8, ip[3] = tot;
  ip[8] = 64, ip[9] = proto;
  memcpy(ip + 12, &src, 4);
  memcpy(ip + 16, &dst, 4);
  u16 c = csum(ip, 20, 0);
  ip[10] = c >> 8, ip[11] = c;
  return 14 + tot;
}

static int udp_frame(u8 *f, const u8 *dmac, u32 src, u16 sp, u32 dst, u16 dp,
                     const void *d, int n) {
  u8 *u = f + 34;
  u[0] = sp >> 8, u[1] = sp, u[2] = dp >> 8, u[3] = dp;
  u[4] = (8 + n) >> 8, u[5] = 8 + n, u[6] = u[7] = 0;
  memcpy(u + 8, d, n);
  return eth_ip(f, dmac, 17, src, dst, 8 + n);
}

static int tcp_frame(u8 *f, u32 src, u16 sp, u32 dst, u16 dp, u32 seq, u32 ack,
                     u8 flags, const void *d, int n) {
  u8 *t = f + 34;
  memset(t, 0, 20);
  t[0] = sp >> 8, t[1] = sp, t[2] = dp >> 8, t[3] = dp;
  u32 s = htonl(seq), a = htonl(ack);
  memcpy(t + 4, &s, 4);
  memcpy(t + 8, &a, 4);
  t[12] = 5 << 4, t[13] = flags, t[14] = 0xff, t[15] = 0xff;
  if (n)
    memcpy(t + 20, d, n);
  u8 ph[12];
  memcpy(ph, &src, 4);
  memcpy(ph + 4, &dst, 4);
  ph[8] = 0, ph[9] = 6, ph[10] = (20 + n) >> 8, ph[11] = 20 + n;
  u32 sum = 0;
  for (int i = 0; i < 12; i += 2)
    sum += (ph[i] << 8) | ph[i + 1];
  u16 c = csum(t, 20 + n, sum);
  t[16] = c >> 8, t[17] = c;
  return eth_ip(f, SMAC, 6, src, dst, 20 + n);
}

/// Payload of the first UDP frame from `sport` to `dport` at or after
/// `from` (its index in *idx), or null.
static const u8 *find_udp(size_t from, u16 sport, u16 dport, int *len,
                          size_t *idx) {
  for (size_t i = from; i < frames.size(); i++) {
    const Frame &f = frames[i];
    if (f.size() < 42 || f[12] != 0x08 || f[13] != 0x00 || f[23] != 17)
      continue;
    const u8 *u = &f[14 + (f[14] & 15) * 4];
    if (((u[0] << 8) | u[1]) == sport && ((u[2] << 8) | u[3]) == dport) {
      *len = ((u[4] << 8) | u[5]) - 8;
      if (idx)
        *idx = i;
      return u + 8;
    }
  }
  return nullptr;
}

static int bootp_request(u8 *b, int dhcp_type) {
  memset(b, 0, 300);
  b[0] = 1, b[1] = 1, b[2] = 6;
  b[4] = 0x12, b[5] = 0x34, b[6] = 0x56, b[7] = dhcp_type ? 0x78 : 0x79;
  memcpy(b + 28, GMAC, 6);
  int o = 236;
  b[o++] = 99, b[o++] = 130, b[o++] = 83, b[o++] = 99;
  if (dhcp_type)
    b[o++] = 53, b[o++] = 1, b[o++] = (u8)dhcp_type;
  b[o++] = 255;
  return 300;
}

/// A guest TCP connection: the segments it needs to answer are read out of
/// `frames` from `next` on.
struct Tcp {
  u32 dst;
  u16 sport, dport;
  u32 iss = 1000, rcv = 0, sent = 0;
  size_t next;
  bool synack = false, fin = false, reset = false;
  std::string data; // received in order
  u32 gip = ip4("10.0.2.15");

  Tcp(u32 d, u16 dp, u16 sp) : dst(d), sport(sp), dport(dp) {
    next = frames.size();
    u8 f[1600];
    to_nat(f, tcp_frame(f, gip, sport, dst, dport, iss, 0, 0x02, nullptr, 0));
  }
  void send_data(const std::string &s) {
    u8 f[1600];
    to_nat(f, tcp_frame(f, gip, sport, dst, dport, iss + 1 + sent, rcv, 0x18,
                        s.data(), (int)s.size()));
    sent += (u32)s.size();
  }
  /// Answer what arrived since the last call.
  void step() {
    u8 f[1600];
    for (; next < frames.size(); next++) {
      const Frame &fr = frames[next];
      if (fr.size() < 54 || fr[12] != 0x08 || fr[13] != 0x00 || fr[23] != 6)
        continue;
      const u8 *t = &fr[14 + (fr[14] & 15) * 4];
      if (((t[2] << 8) | t[3]) != sport)
        continue;
      u32 seq;
      memcpy(&seq, t + 4, 4);
      seq = ntohl(seq);
      int hl = (t[12] >> 4) * 4;
      int dl = ((fr[16] << 8) | fr[17]) - (fr[14] & 15) * 4 - hl;
      u8 fl = t[13];
      if ((fl & 0x12) == 0x12 && !synack) {
        synack = true;
        rcv = seq + 1;
        to_nat(f, tcp_frame(f, gip, sport, dst, dport, iss + 1, rcv, 0x10,
                            nullptr, 0));
        continue;
      }
      if (fl & 0x04) {
        reset = true;
        return;
      }
      bool in_order = dl > 0 && seq == rcv;
      if (in_order) {
        data.append((const char *)t + hl, dl);
        rcv += dl;
      }
      if ((fl & 0x01) && seq + (dl > 0 ? dl : 0) == rcv && !fin) {
        fin = true;
        rcv++;
      }
      if (dl > 0 || (fl & 0x01))
        to_nat(f, tcp_frame(f, gip, sport, dst, dport, iss + 1 + sent, rcv,
                            0x10, nullptr, 0));
    }
  }
  /// Run until `done` or `ms` pass.
  template <class F> bool run(int ms, F done) {
    long long end = now_ms() + ms;
    while (now_ms() < end) {
      pump(10);
      step();
      if (done(*this))
        return true;
      if (reset)
        return false;
    }
    return false;
  }
};

// ---- the host side of the tests

/// A TCP listener on 127.0.0.1, port chosen by the system.
static int listen_loopback(int *port) {
  int s = socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in a = {};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  bind(s, (sockaddr *)&a, sizeof(a));
  socklen_t al = sizeof(a);
  getsockname(s, (sockaddr *)&a, &al);
  *port = ntohs(a.sin_port);
  listen(s, 4);
  return s;
}

static std::string pattern(size_t n) {
  std::string s(n, 0);
  u32 x = 12345;
  for (size_t i = 0; i < n; i++) {
    x = x * 1103515245u + 12345u;
    s[i] = (char)(x >> 16);
  }
  return s;
}

static const char *TFTP_DIR = "nat_test_tftp";

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);

  // A TFTP root with a 100 KB file.
  mkdir(TFTP_DIR, 0755);
  std::string image = pattern(102400);
  {
    FILE *fp = fopen((std::string(TFTP_DIR) + "/boot.img").c_str(), "wb");
    fwrite(image.data(), 1, image.size(), fp);
    fclose(fp);
  }

  // Options that cannot start a network are refused, not half-started.
  {
    NatOptions bad;
    bad.net = "10.0.2.0/33";
    CNetworkNat n;
    CHECK(!n.start("nat0", bad), "net with a 33 bit prefix is refused");
    NatOptions out;
    out.dhcp_start = "10.0.3.15";
    CNetworkNat n2;
    CHECK(!n2.start("nat0", out), "dhcp_start outside net is refused");
    NatOptions same;
    same.dns = same.host;
    CNetworkNat n3;
    CHECK(!n3.start("nat0", same), "dns = host is refused");
    NatOptions fwd;
    fwd.hostfwd = "tcp:127.0.0.1:2222";
    CNetworkNat n4;
    CHECK(!n4.start("nat0", fwd), "a hostfwd rule without '-' is refused");
    NatOptions fwd2;
    fwd2.hostfwd = "sctp::2222-:22";
    CNetworkNat n5;
    CHECK(!n5.start("nat0", fwd2), "a hostfwd rule for sctp is refused");
  }

  // The network under test, with a forward into the guest.
  int fwd_probe_port;
  {
    int s = listen_loopback(&fwd_probe_port);
    close(s);
  }
  NatOptions opt;
  opt.tftp_root = TFTP_DIR;
  opt.bootfile = "boot.img";
  opt.hostname = "es40";
  opt.hostfwd = "tcp:127.0.0.1:" + std::to_string(fwd_probe_port) + "-:22";
  nat = new CNetworkNat();
  if (!nat->start("nat0", opt)) {
    printf("FAIL the NAT back end did not start\n");
    return 1;
  }
  u8 f[1600], b[600];
  int n;
  size_t i;
  const u8 *r;

  // 1. DHCP DISCOVER, answered by an OFFER of the first lease address.
  n = udp_frame(f, BCAST, 0, 68, 0xffffffff, 67, b, bootp_request(b, 1));
  to_nat(f, n);
  pump(50);
  r = find_udp(0, 67, 68, &n, &i);
  int offer = 0;
  if (r)
    for (int o = 240; o + 2 < n; o += 2 + r[o + 1])
      if (r[o] == 53)
        offer = r[o + 2];
  CHECK(r && offer == 2, "DHCP DISCOVER is answered by an OFFER (type %d)",
        offer);
  CHECK(r && !memcmp(r + 16, "\x0a\x00\x02\x0f", 4), "the OFFER is 10.0.2.15");
  CHECK(!memcmp(&frames[i][0], GMAC, 6),
        "the OFFER goes to the client's hardware address, not broadcast");
  {
    // A client that asks for a broadcast reply gets one.
    size_t m = frames.size();
    bootp_request(b, 1);
    b[10] = 0x80;
    to_nat(f, udp_frame(f, BCAST, 0, 68, 0xffffffff, 67, b, 300));
    pump(50);
    size_t j;
    const u8 *rb = find_udp(m, 67, 68, &n, &j);
    CHECK(rb && !memcmp(&frames[j][0], BCAST, 6),
          "with the broadcast flag set the reply is a broadcast (%s, %02x)",
          rb ? "answered" : "no reply", rb ? frames[j][0] : 0);
  }

  // 2. A BOOTP request with no DHCP option, as SRM's boot ewa0 sends.
  size_t mark = frames.size();
  n = udp_frame(f, BCAST, 0, 68, 0xffffffff, 67, b, bootp_request(b, 0));
  to_nat(f, n);
  pump(50);
  r = find_udp(mark, 67, 68, &n, &i);
  CHECK(r != nullptr, "a plain BOOTP request is answered");
  if (r) {
    CHECK(!memcmp(r + 16, "\x0a\x00\x02\x0f", 4), "BOOTP yiaddr is 10.0.2.15");
    CHECK(!memcmp(r + 20, "\x0a\x00\x02\x02", 4),
          "BOOTP siaddr is 10.0.2.2 (the TFTP server)");
    CHECK(!strcmp((const char *)r + 108, "boot.img"),
          "BOOTP file is boot.img, got \"%s\"", (const char *)r + 108);
  }

  // 3. TFTP: read boot.img, 512 byte blocks, each acknowledged.
  {
    u8 rq[64];
    int q = 0;
    rq[q++] = 0, rq[q++] = 1;
    q += snprintf((char *)rq + q, 20, "boot.img") + 1;
    q += snprintf((char *)rq + q, 20, "octet") + 1;
    u32 gip = ip4("10.0.2.15"), sip = ip4("10.0.2.2");
    size_t from = frames.size();
    to_nat(f, udp_frame(f, SMAC, gip, 2000, sip, 69, rq, q));
    std::string got;
    int blk = 1;
    bool done = false;
    long long t0 = now_ms();
    while (!done && now_ms() - t0 < 10000) {
      pump(5);
      for (; from < frames.size(); from++) {
        const Frame &fr = frames[from];
        if (fr.size() < 46 || fr[23] != 17)
          continue;
        const u8 *u = &fr[34];
        if (((u[2] << 8) | u[3]) != 2000)
          continue;
        int dl = ((u[4] << 8) | u[5]) - 8;
        const u8 *d = u + 8;
        if (d[1] != 3 || ((d[2] << 8) | d[3]) != blk)
          continue;
        int sport = (u[0] << 8) | u[1];
        got.append((const char *)d + 4, dl - 4);
        u8 ack[4] = {0, 4, (u8)(blk >> 8), (u8)blk};
        to_nat(f, udp_frame(f, SMAC, gip, 2000, sip, sport, ack, 4));
        blk++;
        if (dl - 4 < 512)
          done = true;
      }
    }
    CHECK(done && got == image,
          "TFTP boot.img: %zu of %zu bytes in %d blocks, identical", got.size(),
          image.size(), blk - 1);
    printf("TFTP of %zu bytes took %lld ms\n", image.size(), now_ms() - t0);
  }

  // 4. TCP to a listener on the host's 127.0.0.1, through 10.0.2.2.
  {
    int port;
    int ls = listen_loopback(&port);
    std::thread echo([ls] {
      int c = accept(ls, nullptr, nullptr);
      char buf[256];
      ssize_t k = recv(c, buf, sizeof(buf), 0);
      if (k > 0)
        send(c, buf, k, 0);
      shutdown(c, SHUT_WR);
      recv(c, buf, sizeof(buf), 0);
      close(c);
    });
    Tcp t(ip4("10.0.2.2"), (u16)port, 41000);
    t.run(3000, [](Tcp &x) { return x.synack; });
    CHECK(t.synack, "TCP to 10.0.2.2:%d is accepted", port);
    t.send_data("hello from the guest\n");
    t.run(3000, [](Tcp &x) { return x.fin; });
    CHECK(t.data == "hello from the guest\n",
          "the host's 127.0.0.1 listener echoes it back, got \"%s\"",
          t.data.c_str());
    u8 rst[1600];
    to_nat(rst, tcp_frame(rst, t.gip, t.sport, t.dst, t.dport,
                          t.iss + 1 + t.sent, t.rcv, 0x11, nullptr, 0));
    pump(100);
    echo.join();
    close(ls);
  }

  // 5. The host connects into the guest through hostfwd.
  {
    int hs = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(fwd_probe_port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    fcntl(hs, F_SETFL, fcntl(hs, F_GETFL) | O_NONBLOCK);
    connect(hs, (sockaddr *)&a, sizeof(a));
    size_t from = frames.size();
    u32 gip = ip4("10.0.2.15"), peer = 0;
    u16 pport = 0;
    u32 gseq = 5000, pseq = 0;
    bool syn = false, established = false;
    std::string in;
    long long end = now_ms() + 5000;
    while (now_ms() < end && in.empty()) {
      pump(10);
      for (; from < frames.size(); from++) {
        const Frame &fr = frames[from];
        if (fr.size() < 54 || fr[23] != 6)
          continue;
        const u8 *t = &fr[34];
        if (((t[2] << 8) | t[3]) != 22)
          continue;
        u32 seq;
        memcpy(&seq, t + 4, 4);
        seq = ntohl(seq);
        if ((t[13] & 0x12) == 0x02 && !syn) {
          syn = true;
          memcpy(&peer, &fr[26], 4);
          pport = (t[0] << 8) | t[1];
          pseq = seq + 1;
          to_nat(f, tcp_frame(f, gip, 22, peer, pport, gseq, pseq, 0x12,
                              nullptr, 0));
          continue;
        }
        int hl = (t[12] >> 4) * 4;
        int dl = ((fr[16] << 8) | fr[17]) - (fr[14] & 15) * 4 - hl;
        if (dl > 0 && seq == pseq) {
          in.append((const char *)t + hl, dl);
          pseq += dl;
          to_nat(f, tcp_frame(f, gip, 22, peer, pport, gseq + 1, pseq, 0x10,
                              nullptr, 0));
        }
      }
      if (syn && !established) {
        // The host side's connect() has completed once the NAT acked our
        // SYN-ACK; send the data from the host.
        int err = 0;
        socklen_t el = sizeof(err);
        getsockopt(hs, SOL_SOCKET, SO_ERROR, &err, &el);
        if (!err) {
          established = true;
          send(hs, "ssh-banner?", 11, 0);
        }
      }
    }
    CHECK(syn, "a host connection to 127.0.0.1:%d reaches guest port 22",
          fwd_probe_port);
    CHECK(in == "ssh-banner?", "and its data arrives in the guest, got \"%s\"",
          in.c_str());
    close(hs);
    pump(50);
  }

  // 6. A guest-bound segment is lost: slirp retransmits it. The host sends
  // 40000 bytes and closes; the guest drops the first data segment.
  {
    int port;
    int ls = listen_loopback(&port);
    std::string payload = pattern(40000);
    std::thread src([ls, &payload] {
      int c = accept(ls, nullptr, nullptr);
      size_t off = 0;
      while (off < payload.size()) {
        ssize_t k = send(c, payload.data() + off, payload.size() - off, 0);
        if (k <= 0)
          break;
        off += k;
      }
      shutdown(c, SHUT_WR);
      char buf[16];
      recv(c, buf, sizeof(buf), 0);
      close(c);
    });
    dropped_data = 0;
    drop_next_tcp_data = true;
    long long t0 = now_ms();
    Tcp t(ip4("10.0.2.2"), (u16)port, 42000);
    bool ok = t.run(30000, [](Tcp &x) { return x.fin; });
    long long took = now_ms() - t0;
    CHECK(dropped_data == 1, "one data segment was dropped on the way");
    CHECK(ok && t.data == payload,
          "the transfer completes after the loss: %zu of %zu bytes",
          t.data.size(), payload.size());
    printf("a dropped segment cost %lld ms to recover (whole transfer)\n",
           took);
    u8 rst[1600];
    to_nat(rst, tcp_frame(rst, t.gip, t.sport, t.dst, t.dport, t.iss + 1, t.rcv,
                          0x11, nullptr, 0));
    pump(100);
    src.join();
    close(ls);
  }

  // 7. The queue towards the guest is bounded: a guest that never reads
  // loses frames rather than growing the queue. ICMP echo requests to the
  // NAT's own address each draw one reply.
  {
    pump(50); // let the queue empty
    unsigned long before = nat->dropped();
    for (int k = 0; k < 800; k++) {
      u8 icmp[8] = {8, 0, 0, 0, 0, 1, (u8)(k >> 8), (u8)k};
      u16 c = csum(icmp, 8, 0);
      icmp[2] = c >> 8, icmp[3] = c;
      memcpy(f + 34, icmp, 8);
      int len = eth_ip(f, SMAC, 1, ip4("10.0.2.15"), ip4("10.0.2.2"), 8);
      to_nat(f, len);
    }
    // Not reading yet: count what is queued by draining afterwards.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    size_t queued = 0;
    const u8 *d;
    int dl;
    while (nat->receive(&d, &dl) == 1)
      queued++;
    CHECK(queued <= CNetworkNat::QUEUE_LIMIT,
          "no more than %zu frames wait for the guest, found %zu",
          (size_t)CNetworkNat::QUEUE_LIMIT, queued);
    CHECK(nat->dropped() > before,
          "frames beyond the limit are counted as dropped (%lu)",
          nat->dropped() - before);
  }

  // 8. The internet, only on request.
  if (getenv("ALPHABOX_NAT_TEST_NET")) {
    u32 exip = 0;
    u8 q[64] = {0xab, 0xcd, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0};
    int o = 12;
    q[o++] = 7, memcpy(q + o, "example", 7), o += 7;
    q[o++] = 3, memcpy(q + o, "com", 3), o += 3;
    q[o++] = 0, q[o++] = 0, q[o++] = 1, q[o++] = 0, q[o++] = 1;
    mark = frames.size();
    to_nat(f, udp_frame(f, SMAC, ip4("10.0.2.15"), 5353, ip4("10.0.2.3"), 53, q,
                        o));
    r = nullptr;
    for (int k = 0; k < 100 && !r; k++) {
      pump(30);
      r = find_udp(mark, 53, 5353, &n, &i);
    }
    int an = r ? (r[6] << 8) | r[7] : 0;
    if (r && an)
      for (int p = o; p + 16 <= n; p++)
        if (r[p] == 0 && r[p + 1] == 1 && r[p + 2] == 0 && r[p + 3] == 1 &&
            r[p + 8] == 0 && r[p + 9] == 4) {
          memcpy(&exip, r + p + 10, 4);
          break;
        }
    CHECK(r && an > 0 && exip, "DNS answers example.com through 10.0.2.3");
    if (exip) {
      Tcp t(exip, 80, 43000);
      t.run(8000, [](Tcp &x) { return x.synack; });
      t.send_data("GET / HTTP/1.0\r\nHost: example.com\r\n\r\n");
      t.run(8000, [](Tcp &x) { return x.fin; });
      CHECK(t.data.compare(0, 7, "HTTP/1.") == 0,
            "an HTTP request to example.com:80 is answered (%zu bytes)",
            t.data.size());
    }
  }

  // Closing the back end stops its worker and releases everything.
  nat->close();
  delete nat;
  unlink((std::string(TFTP_DIR) + "/boot.img").c_str());
  rmdir(TFTP_DIR);

  printf("%d checks, %d failures\n", checks, failures);
  return failures != 0;
}
