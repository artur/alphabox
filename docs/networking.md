# Networking

Every NIC connects to the host through a back end chosen with `type`
(see [configuration.md](configuration.md#networking) for the list). This
page is about `type = "nat"`, the one that needs no privileges and no host
interface: the guest sits behind a small router that Alphabox provides,
and reaches the internet and the host the way a guest in QEMU's
`-netdev user` does. It is built on libslirp, the library QEMU uses.

## What the guest gets

A NIC on `type = "nat"` finds, on its virtual wire:

| Address | What |
| --- | --- |
| `10.0.2.0/24` | the guest network (`net`) |
| `10.0.2.2` | the router, and the host: TCP and UDP to it reach the host's own services, including those bound to `127.0.0.1` (`host`) |
| `10.0.2.3` | a DNS forwarder to the host's resolver (`dns`) |
| `10.0.2.15` | the one address leased to the guest (`dhcp_start`) |

A DHCP server answers on the wire, and so does a plain BOOTP server (no
option 53), which is what the SRM console sends. Both hand out
`10.0.2.15`, router `10.0.2.2`, DNS `10.0.2.3`, and name the TFTP server
`10.0.2.2` and the `bootfile`. The reply goes to the client's own hardware
address unless the client asked for a broadcast, because the console's
receive filter holds its station address and nothing else.

TFTP is served from `tftp_root` on `10.0.2.2`. TCP and UDP from the guest
are relayed through ordinary host sockets, so the guest can use any host
the host can. ICMP echo is relayed too: on macOS and Linux `ping` to an
internet address is answered by that address (a host that does not answer
gives the guest no answer either).

## Configuration

```
pci0.4 = dec21143
{
  type = "nat";
  tftp_root = "tftp";                      // optional
  bootfile = "boot.img";                   // optional
  hostfwd = "tcp:127.0.0.1:2222-:22";      // optional
}
```

All keys are optional; the sample `es40.cfg` documents each.

| Key | Default | Meaning |
| --- | --- | --- |
| `net` | `10.0.2.0/24` | guest network, address and prefix length (8 to 30) |
| `host` | `10.0.2.2` | the router and host address |
| `dns` | `10.0.2.3` | the DNS forwarder |
| `dhcp_start` | `10.0.2.15` | the address leased to the guest |
| `hostname` | none | the name offered in DHCP |
| `tftp_root` | none | directory the guest reads over TFTP |
| `bootfile` | none | the file named in BOOTP and DHCP replies |
| `hostfwd` | none | forwards into the guest, comma separated |
| `restrict` | false | the guest reaches only the addresses above, nothing outside |
| `host_loopback` | true | false: `10.0.2.2` no longer reaches the host's `127.0.0.1` services |
| `ipv6` | false | IPv6 on the virtual network (`fec0::/64`); off, because the guests of this era have none |

`host`, `dns` and `dhcp_start` must be three different host addresses
inside `net`. A bad value is refused at startup with a message naming it.

The keys are the same on every NIC: the Tulips, the Intel parts and
`virtio_net`. Each NIC with `type = "nat"` has a network of its own, with
the same addresses; two NICs do not see each other.

## Reaching the guest

Nothing outside can start a connection to the guest unless it is
forwarded. `hostfwd` takes rules of the form

```
proto:[hostaddr]:hostport-[guestaddr]:guestport
```

with `proto` `tcp` or `udp`, separated by commas. The host address
defaults to `127.0.0.1`, so a forward is not offered to the network unless
you write `0.0.0.0` (or an interface address) there; the guest address
defaults to `dhcp_start`. To log in to a guest that runs sshd:

```
hostfwd = "tcp:127.0.0.1:2222-:22";
```

and from the host `ssh -p 2222 user@127.0.0.1`. The host port must be
free: a rule whose port is in use stops the start with a message.

## Network booting from the console

Point `tftp_root` at a directory and name the file in `bootfile`. The SRM
console tries MOP first; set the protocol once, then boot:

```
P00>>> set ewa0_protocols bootp
P00>>> boot ewa0
```

`test/tools/srm_probe.sh` does this with `NAT=1 NIC=dec21143` (see
[development.md](development.md)).

## Measured

On the development Mac (JIT build, Apple silicon), a Gentoo guest on a
`dec21143` with `type = "nat"` got its lease from dhcpcd at boot
(`10.0.2.15`, router `10.0.2.2`, DNS `10.0.2.3`), pinged `10.0.2.2`,
`10.0.2.3` and `1.1.1.1` (about 11 ms), resolved names through the
forwarder, fetched 20 MB from a web server on the host through `10.0.2.2`
with a matching SHA-256 (about 77 MB/s as the guest's clock saw it; 600 MB
to `/dev/null` in 11 s of host time, about 55 MB/s), fetched 20 MB of a
Gentoo stage 3 over HTTPS from `distfiles.gentoo.org` with a matching
SHA-256 (13.7 MB/s), and accepted an sshd connection from the host
through `hostfwd`. The console booted a 64 KB image over BOOTP and TFTP.
These are single runs, not a benchmark.

## Limits

- One lease address per network, and one guest per NIC.
- Frames are Ethernet II; IPv6 is off by default and the older guests have
  none. A name lookup through `10.0.2.3` can still return AAAA records, so
  a guest without IPv6 should ask for IPv4 (`curl -4`, `getent ahostsv4`).
- Broadcasts and multicasts do not leave the virtual network, so Windows
  network neighbourhood, mDNS and the like find nothing.
- ICMP echo to the internet depends on the host allowing unprivileged
  ICMP sockets; the tests above pass on macOS.
- The queue of frames waiting for the guest holds 512; beyond that frames
  are dropped (TCP retransmits, as it would on a congested link).
- Not available on Windows hosts, and in builds without libslirp (the type
  is then refused at startup and by `alphabox --check`).
- libslirp's `enable_emu` (the FTP and IRC helpers) is never switched on.

## Building

libslirp is found with pkg-config. Install it with `brew install libslirp`
or `apt install libslirp-dev libglib2.0-dev` (the Ubuntu 24.04 package
needs glib's headers installed alongside) and configure again;
`alphabox --version` lists `libslirp` when it is in. `-DDISABLE_SLIRP=yes`
builds without it. A lane for another architecture than the installed
library (the x86-64 lane on an arm64 Mac) does not find a usable one and
builds without. libslirp is not bundled, and links glib dynamically. It
builds against libslirp 4.7 (Ubuntu 24.04) and 4.9 (Homebrew).
