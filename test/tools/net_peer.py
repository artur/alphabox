#!/usr/bin/env python3
"""A network for an emulated NIC on the UDP backend: ARP, BOOTP and TFTP.

The NIC's config block:
    type = "udp";
    udp_local = "127.0.0.1:<nic port>";
    udp_remote = "127.0.0.1:<peer port>";

  net_peer.py --listen <peer port> --nic <nic port> [--file IMAGE | --halt-kb N]
              [--server-ip 10.0.2.2] [--client-ip 10.0.2.15]
              [--boot-name boot.img] [--log FILE] [--timeout S]

Every Ethernet frame is one UDP datagram (QEMU "-netdev dgram" framing).
The peer answers ARP for its address, BOOTP requests with the client
address and --boot-name, and TFTP read requests (any name) with the boot
file, negotiating blksize when asked. --halt-kb N serves an N KB image
whose first instruction is CALL_PAL HALT, so an SRM "boot ewa0"/"boot eia0"
downloads it, runs it and reports "HALT instruction executed".

Prints one line per exchange and a summary at exit (on --timeout or when
the transfer is complete and --exit-after-transfer is given).
"""
import argparse
import signal
import socket
import struct
import sys
import time

SERVER_MAC = bytes.fromhex("020000000001")
BROADCAST = b"\xff" * 6


def checksum(data):
    if len(data) & 1:
        data += b"\0"
    s = sum(struct.unpack("!%dH" % (len(data) // 2), data))
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return ~s & 0xFFFF


def ip_bytes(text):
    return socket.inet_aton(text)


class Peer:
    def __init__(self, args, image):
        self.args = args
        self.image = image
        self.sip = ip_bytes(args.server_ip)
        self.cip = ip_bytes(args.client_ip)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", args.listen))
        self.nic = ("127.0.0.1", args.nic)
        self.log = open(args.log, "a") if args.log else None
        self.stats = {"frames_in": 0, "frames_out": 0, "arp": 0, "bootp": 0,
                      "tftp_rrq": 0, "tftp_blocks": 0, "tftp_done": 0}
        self.tftp = None  # current transfer

    def say(self, text):
        line = "%.3f %s" % (time.time() - self.t0, text)
        print(line, flush=True)
        if self.log:
            self.log.write(line + "\n")
            self.log.flush()

    def send_frame(self, dst, ethertype, payload):
        frame = dst + SERVER_MAC + struct.pack("!H", ethertype) + payload
        if len(frame) < 60:
            frame += b"\0" * (60 - len(frame))
        self.sock.sendto(frame, self.nic)
        self.stats["frames_out"] += 1

    def send_udp(self, dst_mac, dst_ip, sport, dport, data):
        udp = struct.pack("!HHHH", sport, dport, 8 + len(data), 0) + data
        hdr = struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + len(udp), 0, 0, 64,
                          17, 0, self.sip, dst_ip)
        hdr = hdr[:10] + struct.pack("!H", checksum(hdr)) + hdr[12:]
        self.send_frame(dst_mac, 0x0800, hdr + udp)

    # ARP
    def on_arp(self, frame):
        op = struct.unpack_from("!H", frame, 20)[0]
        sha, spa, tpa = frame[22:28], frame[28:32], frame[38:42]
        if op != 1 or tpa != self.sip:
            return
        self.stats["arp"] += 1
        self.say("ARP who-has %s from %s" % (socket.inet_ntoa(tpa), sha.hex(":")))
        reply = struct.pack("!HHBBH6s4s6s4s", 1, 0x0800, 6, 4, 2, SERVER_MAC,
                            self.sip, sha, spa)
        self.send_frame(sha, 0x0806, reply)

    # BOOTP
    def on_bootp(self, frame, data):
        if len(data) < 236 or data[0] != 1:
            return
        self.stats["bootp"] += 1
        xid = data[4:8]
        chaddr = data[28:34]
        self.say("BOOTP request xid %s from %s" % (xid.hex(), chaddr.hex(":")))
        reply = bytearray(300)
        reply[0:4] = b"\x02\x01\x06\x00"
        reply[4:8] = xid
        reply[16:20] = self.cip
        reply[20:24] = self.sip
        reply[28:34] = chaddr
        reply[44:44 + 16] = b"alphabox-peer".ljust(16, b"\0")
        name = self.args.boot_name.encode()
        reply[108:108 + len(name)] = name
        if data[236:240] == b"\x63\x82\x53\x63":
            opts = (b"\x63\x82\x53\x63" + b"\x01\x04" + ip_bytes("255.255.255.0") +
                    b"\x03\x04" + self.sip + b"\xff")
            reply[236:236 + len(opts)] = opts
        self.send_udp(chaddr, b"\xff\xff\xff\xff", 67, 68, bytes(reply))

    # TFTP
    def tftp_send_block(self):
        t = self.tftp
        start = (t["block"] - 1) * t["blksize"]
        chunk = self.image[start:start + t["blksize"]]
        pkt = struct.pack("!HH", 3, t["block"] & 0xFFFF) + chunk
        self.send_udp(t["mac"], t["ip"], t["sport"], t["cport"], pkt)
        t["sent_at"] = time.time()
        t["last_len"] = len(chunk)

    def on_tftp_request(self, frame, ip, sport, data):
        parts = data[2:].split(b"\0")
        name = parts[0].decode(errors="replace")
        opts = {}
        for i in range(2, len(parts) - 1, 2):
            opts[parts[i].decode().lower()] = parts[i + 1].decode()
        self.stats["tftp_rrq"] += 1
        self.say("TFTP RRQ %r mode %s options %s" % (name, parts[1].decode(), opts))
        blksize = 512
        t = {"mac": frame[6:12], "ip": ip[12:16], "cport": sport,
             "sport": 20000 + self.stats["tftp_rrq"], "block": 0,
             "blksize": blksize, "done": False}
        self.tftp = t
        if "blksize" in opts:
            t["blksize"] = max(8, min(int(opts["blksize"]), 1428))
            oack = struct.pack("!H", 6) + b"blksize\0" + str(t["blksize"]).encode() + b"\0"
            self.send_udp(t["mac"], t["ip"], t["sport"], sport, oack)
            t["sent_at"] = time.time()
        else:
            t["block"] = 1
            self.tftp_send_block()

    def on_tftp_ack(self, data):
        t = self.tftp
        block = struct.unpack_from("!H", data, 2)[0]
        if block != t["block"] & 0xFFFF:
            return  # duplicate
        if t["block"] > 0:
            self.stats["tftp_blocks"] += 1
            if t["last_len"] < t["blksize"]:
                t["done"] = True
                self.stats["tftp_done"] += 1
                self.say("TFTP transfer complete: %d blocks, %d bytes" %
                         (t["block"], len(self.image)))
                return
        t["block"] += 1
        self.tftp_send_block()

    def on_ip(self, frame):
        ip = frame[14:]
        if len(ip) < 28 or ip[9] != 17:
            return
        ihl = (ip[0] & 15) * 4
        sport, dport = struct.unpack_from("!HH", ip, ihl)
        data = ip[ihl + 8:]
        if dport == 67:
            self.on_bootp(frame, data)
        elif dport == 69 and data[:2] == b"\x00\x01":
            self.on_tftp_request(frame, ip, sport, data)
        elif self.tftp and dport == self.tftp["sport"]:
            op = struct.unpack_from("!H", data)[0]
            if op == 4:
                self.on_tftp_ack(data)
            elif op == 5:
                self.say("TFTP error from client: %r" % data[4:].split(b"\0")[0])

    def run(self):
        self.t0 = time.time()
        self.sock.settimeout(0.5)
        end = self.t0 + self.args.timeout
        while time.time() < end:
            try:
                frame, _ = self.sock.recvfrom(4096)
            except socket.timeout:
                frame = None
            if frame and len(frame) >= 14:
                self.stats["frames_in"] += 1
                ethertype = struct.unpack_from("!H", frame, 12)[0]
                if ethertype == 0x0806:
                    self.on_arp(frame)
                elif ethertype == 0x0800:
                    self.on_ip(frame)
            t = self.tftp
            if t and not t["done"] and time.time() - t["sent_at"] > 1.0:
                if t["block"] == 0:
                    t["sent_at"] = time.time()  # OACK lost: client retries
                else:
                    self.tftp_send_block()  # retransmit
            if t and t["done"] and self.args.exit_after_transfer:
                break
        self.say("summary " + " ".join("%s=%d" % kv for kv in self.stats.items()))
        return 0 if self.stats["tftp_done"] or not self.args.exit_after_transfer else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--listen", type=int, required=True)
    ap.add_argument("--nic", type=int, required=True)
    ap.add_argument("--file")
    ap.add_argument("--halt-kb", type=int, default=64)
    ap.add_argument("--server-ip", default="10.0.2.2")
    ap.add_argument("--client-ip", default="10.0.2.15")
    ap.add_argument("--boot-name", default="boot.img")
    ap.add_argument("--log")
    ap.add_argument("--timeout", type=float, default=300)
    ap.add_argument("--exit-after-transfer", action="store_true")
    args = ap.parse_args()
    if args.file:
        image = open(args.file, "rb").read()
    else:
        # CALL_PAL HALT (0x00000000) first, then a recognizable fill.
        # The odd tail makes the last TFTP block a short one, so the transfer
        # ends with an acknowledged block rather than an empty one.
        image = b"\0\0\0\0" + bytes((i * 7 + 3) & 0xFF for i in range(args.halt_kb * 1024 + 96))
    # SIGTERM ends the run the way --timeout does, summary included.
    signal.signal(signal.SIGTERM, lambda *_: (_ for _ in ()).throw(KeyboardInterrupt))
    peer = Peer(args, image)
    try:
        return peer.run()
    except KeyboardInterrupt:
        peer.say("summary " + " ".join("%s=%d" % kv for kv in peer.stats.items()))
        return 0


if __name__ == "__main__":
    sys.exit(main())
