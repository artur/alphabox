# Paravirtual devices: virtio-blk and virtio-net

Alphabox offers two paravirtual PCI devices for guests with a driver for
them: a block device and an Ethernet NIC, both speaking the **legacy
virtio-pci interface** -- the virtio 0.9.5 register layout, as the OASIS
*Virtual I/O Device (VIRTIO) Version 1.0* specification describes it in its
"Legacy Interface" sections (2.4.2 split virtqueue layout, 4.1.4.8 legacy
PCI registers, 5.1 network device, 5.2 block device). No Alpha operating
system ships a driver: this page is the reference for writing one (the
Windows 2000 drivers are nada's). Everything here is what the code in
`src/devices/pci/virtio/` does; where it chose among what the specification
allows, the choice is stated.

## Configuration

```
pci0.3 = virtio_blk
{
  disk0.0 = file              // or device / ramdisk: any disk, as elsewhere
  {
    file = "data.img";
    read_only = false;        // true: the RO feature, writes fail
  }
}

pci0.4 = virtio_net
{
  type = "udp";               // null, udp, tap (Linux) or pcap (default),
  udp_local = "127.0.0.1:5001";  // with that backend's keys, exactly as
  udp_remote = "127.0.0.1:5002"; // for the Tulip and Intel NICs
  //mac = "08-00-2B-E5-40-10";   // default: the shared NIC default
}
```

Any free slot works (the ES40's `pci0.3`-`pci0.6`, `pci1.x`, or behind a
bridge). The `virtio_blk` takes exactly one disk, `disk0.0`. The
`virtio_net` reads `mac` and the backend keys (`type`, `adapter`,
`udp_local`, `udp_remote`, `host_ip`, `bridge`, `uplink`, `tap_create`); see
the NIC section of the sample `es40.cfg`.

The ES40 SRM console does not know the devices: `show config` lists them by
ID only (`10011AF4/00021AF4` and `10001AF4/00011AF4`), assigns their I/O BAR
and interrupt line, and otherwise leaves them alone.

## PCI identity

| | virtio-blk | virtio-net |
|---|---|---|
| Vendor / device | 1AF4 / **1001** | 1AF4 / **1000** |
| Revision | 00 (legacy) | 00 (legacy) |
| Class | 01 80 00 (mass storage, other) | 02 00 00 (Ethernet) |
| Subsystem vendor / ID | 1AF4 / **0002** (block) | 1AF4 / **0001** (network) |
| BAR 0 | I/O, 64 bytes | I/O, 64 bytes |
| Other BARs, expansion ROM, capabilities | none | none |
| Interrupt pin | INTA | INTA |

The subsystem ID is the virtio device type; a driver matching on vendor
1AF4 and device 1000/1001 finds the device, the subsystem ID confirms what
it is. There is no MSI-X (so the device configuration starts at 0x14) and no
modern (virtio 1.0) capability list: this is a legacy-only device.

## Registers (BAR 0, I/O space)

Little-endian, as the Alpha is. Access each field with its own width
(byte, word, longword); device configuration may be read a byte at a time.

| Offset | Size | Name | Access |
|---|---|---|---|
| 0x00 | 32 | Host (device) features | R |
| 0x04 | 32 | Guest (driver) features | R/W: only offered bits stick |
| 0x08 | 32 | Queue address: page frame number, 4096-byte pages, of the selected queue | R/W |
| 0x0C | 16 | Queue size of the selected queue: 256, or 0 for a queue that does not exist | R |
| 0x0E | 16 | Queue select | R/W |
| 0x10 | 16 | Queue notify: write the queue's index | W |
| 0x12 | 8 | Device status | R/W: 0 resets |
| 0x13 | 8 | ISR status: bit 0 queue, bit 1 configuration; **reading clears it** and drops INTA | R |
| 0x14 | ... | Device-specific configuration (below) | R (net MAC: R/W) |

Device status bits: 1 ACKNOWLEDGE, 2 DRIVER, 4 DRIVER_OK, 0x80 FAILED (the
legacy interface has no FEATURES_OK step). Writing **0 resets** the device:
features, every queue address, the ISR and the status go back to 0 and the
interrupt drops. A reset waits for a request the device is working on to
finish; it never interrupts one half-way.

Writing a queue address resets that queue's indices (the device's next
available entry and the used index both start at 0); writing 0 disables the
queue. The device works on a queue whenever its address is non-zero -- it
does not wait for DRIVER_OK (the network device's receive path does, see
below).

## Virtqueues

Split virtqueues in the legacy layout, 256 entries each, starting at
`PFN * 4096`:

| Part | Offset | Size |
|---|---|---|
| Descriptor table | 0 | 16 x 256 = 4096 |
| Available ring: flags, idx, ring[256], used_event | 4096 | 6 + 2 x 256 = 518 |
| Used ring: flags, idx, ring[256] of {u32 id, u32 len}, avail_event | **8192** (next 4096 boundary) | 6 + 8 x 256 = 2054 |

Three contiguous 4 KB pages per queue (12 KB), physically contiguous as the
bus sees them. Descriptors are `{u64 addr; u32 len; u16 flags; u16 next}`,
flags NEXT 1, WRITE 2 (device writes this buffer), INDIRECT 4.

**Addresses are PCI bus addresses.** The queue PFN and every descriptor
address are what the device puts on the bus, through the Tsunami's DMA
windows -- on Windows, the logical addresses the HAL gives
(`HalGetAdapter`/`AllocateCommonBuffer`, `MapTransfer`, or a miniport's
`ScsiPortGetPhysicalAddress`/`NdisMAllocateSharedMemory`), never raw
physical addresses. They must lie below 4 GB (the device is a 32-bit PCI
master); a descriptor beyond reach fails its request. The ES40 SRM console
leaves a direct-mapped window at bus 0x80000000 = memory 0 (see the late
self-test below), but a driver must not assume it.

What the device does with the rings:

- It reads the available `idx`, then the ring entries and descriptors
  (with a read barrier between), takes chains in order, follows `NEXT`, and
  follows one level of `INDIRECT` table (`VIRTIO_RING_F_INDIRECT_DESC`,
  bit 28, is offered by both devices). An indirect descriptor must not also
  have NEXT, and its length must be a multiple of 16.
- It writes the used element, then (after a write barrier) the used `idx`,
  one element per chain, in the order the chains were taken.
- After a batch it re-reads the available ring's `flags`: unless
  `VRING_AVAIL_F_NO_INTERRUPT` (1) is set, it sets ISR bit 0 and asserts
  INTA. `VIRTIO_RING_F_EVENT_IDX` is **not** offered: used_event/avail_event
  are ignored, and the device never sets `VRING_USED_F_NO_NOTIFY` -- notify
  on every submission (it costs one I/O write).
- A malformed chain (head out of range, a loop, a descriptor out of reach, a
  bad indirect table) is reported once on the emulator console and
  completed with used length 0.

## Interrupts

One level-triggered INTA. It is asserted while the ISR is non-zero; reading
the ISR returns its bits, clears them and deasserts INTA. The ISR read is
the acknowledgement: a shared-interrupt ISR reads it, returns "not mine" on
0, else schedules its DPC. Bit 1 (configuration change) is never raised by
these devices today (the link is always up, the capacity fixed).

Before the console has assigned the interrupt line (PCI 0x3C still 0xFF),
the device does not drive the interrupt at all; on the ES40 the line the
console assigns is what `show config` / Windows' resource list reports.

## virtio-blk (queue 0)

Features offered (host features register):

| Bit | Name | |
|---|---|---|
| 1 | SIZE_MAX | largest segment: 65536 bytes |
| 2 | SEG_MAX | most data segments per request: 254 |
| 4 | GEOMETRY | cylinders/heads/sectors of the image |
| 5 | RO | only when the disk is read-only (`read_only`, or a CD-ROM) |
| 6 | BLK_SIZE | the image's block size (512; 2048 for a `cdrom` image) |
| 9 | FLUSH | the FLUSH request (a.k.a. WCE in old headers) |
| 28 | RING_INDIRECT_DESC | |

(SIZE_MAX and SEG_MAX are advice for the driver: the device itself takes a
request of any size and any number of segments that fit its chain.)

Configuration at 0x14 (24 bytes, little-endian):

| Offset from 0x14 | Size | Field |
|---|---|---|
| 0x00 | 64 | capacity, in 512-byte sectors |
| 0x08 | 32 | size_max |
| 0x0C | 32 | seg_max |
| 0x10 | 16 + 8 + 8 | geometry: cylinders, heads, sectors |
| 0x14 | 32 | blk_size |

Requests: one chain = a 16-byte header the device reads
`{u32 type; u32 reserved; u64 sector}`, the data, and **one status byte the
device writes, the last writable byte of the chain**. Sectors are always
512 bytes, whatever blk_size says. The header, data and status may be split
across descriptors any way (the device gathers them), but device-readable
buffers must come before device-writable ones.

| Type | | Data | Used length |
|---|---|---|---|
| 0 IN | read | writable, a multiple of 512 | data + 1 |
| 1 OUT | write | readable, a multiple of 512 | 1 |
| 4 FLUSH | flush the image to the host | none | 1 |
| 8 GET_ID | device ID | writable, 20 bytes: the disk's serial number (`serial_number`, default `ES40EM00000`), NUL-padded, not terminated if it fills 20 | bytes written + 1 |
| other | | | 1, status UNSUPP |

Status: 0 OK, 1 IOERR (out of range, not a multiple of 512, a read-only
disk written, a host I/O error), 2 UNSUPP. Requests complete in order, one
at a time, on the device's own thread; the used ring is written as each
finishes, the interrupt after the batch.

## virtio-net (queue 0 receive, queue 1 transmit)

Features offered: MAC (5), STATUS (16), RING_INDIRECT_DESC (28). Nothing
else: no checksum or segmentation offload, no mergeable receive buffers, no
control queue, no multiqueue.

Configuration at 0x14 (8 bytes): `mac[6]`, then `u16 status` with bit 0
LINK_UP -- always 1. The MAC is the configured `mac`, else the shared NIC
default `08-00-2B-E5-40-nn` (nn counts the NICs in configuration order). A
driver may write the MAC bytes (legacy style); the host-side filter follows.

Every buffer, both directions, starts with the legacy 10-byte
`struct virtio_net_hdr` `{u8 flags; u8 gso_type; u16 hdr_len; u16 gso_size;
u16 csum_start; u16 csum_offset}`; since no offload is negotiated the driver
sends it zeroed, and the device writes it zeroed.

- **Transmit (queue 1)**: a chain of header + Ethernet frame (any split;
  the device skips the first 10 bytes and sends the rest as one frame,
  without the FCS). Used length 0.
- **Receive (queue 0)**: post device-writable buffers of at least
  10 + 1514 = 1524 bytes (a buffer may be several descriptors). The device
  writes the header and one frame, and puts `10 + frame length` in the used
  ring. A frame that does not fit the buffer is dropped (used length 0, one
  console message). Frames are delivered only while DRIVER_OK is set and
  queue 0 has an address; before that they are discarded. When the driver
  has no buffer posted, one frame waits in the device and the rest in the
  host backend; posting buffers and notifying queue 0 lets them in (the
  device also looks every 2 ms).
- Filtering: with no control queue the device delivers its own address,
  broadcast and all multicast (the pcap backend filters to that; UDP and
  TAP deliver whatever arrives). The driver does its own multicast
  filtering if it wants any.

## Self-test: a reference driver sequence

`ALPHABOX_VIRTIO_SELFTEST=1` makes each configured virtio device drive
itself through its own registers at start-up, as a minimal driver would,
with its rings and buffers in 1 MB of guest RAM (16 to 32 MB below the top,
one megabyte per device) addressed physically, before the console runs.
`ALPHABOX_VIRTIO_SELFTEST_DELAY=<s>` runs it `<s>` seconds later instead,
through the PCI DMA window the console has set up by then (the test looks
for a direct-mapped window reaching its megabyte and uses bus addresses, as
a real driver must). Each check prints a `%VIRTIO-I-SELFTEST` line, and
each device ends with PASS or FAIL. The code is `CVirtioBlk::selftest` and
`CVirtioNet::selftest`; the order it does things in is a working order for
a driver:

1. Write 0 to status (reset); read it back as 0.
2. Status = ACKNOWLEDGE | DRIVER.
3. Read host features; write the ones you use to guest features.
4. Read the device configuration (capacity / MAC and link status).
5. For each queue: write its index to queue select, read queue size (256;
   0 means no such queue), zero 12 KB of bus-contiguous memory, write its
   PFN (bus address / 4096).
6. Status = ACKNOWLEDGE | DRIVER | DRIVER_OK.
7. Per request: fill descriptors, put the head index in the available ring
   at `idx % 256`, write barrier, increment the available `idx`, write the
   queue index to queue notify.
8. On the interrupt: read ISR (clears it); for each queue, consume used
   entries from your last seen index up to the used `idx`.

The blk test (it needs a disk whose sector 0 ends in 55 AA, e.g. an MBR
image; it writes the **last** sector and puts it back -- still, run it on a
copy): features, capacity, GET_ID, IN of sector 0 (used length 513, ISR bit
0 and INTA, the ISR read clearing both), the same through an indirect
table, IN/OUT/IN of the last sector (or OUT refused with IOERR on a
read-only image), FLUSH, IN past the end (IOERR), an unknown type (UNSUPP),
`NO_INTERRUPT` honoured, and reset clearing the queue address.

The net test transmits a broadcast frame (ethertype 88B5, local
experimental) and waits for a frame to come back, so it needs a backend
that returns one: `type = "udp"` with `udp_local` and `udp_remote` the
**same** address is a loopback, or two `virtio_net` devices wired to each
other. It checks features, MAC, link status, transmit completion, the
received header (zeroed) and length (10 + 64), the frame's contents, the
interrupt and ISR, and reset.

Run it with the SRM probe (configuration from `EXTRA_CFG`):

```bash
cp lab/usbstor/usb.img lab/runs/vblk.img     # the test writes: use a copy
cat > /tmp/virtio.cfg <<'EOF'
  pci0.3 = virtio_blk { disk0.0 = file { file = "/abs/path/lab/runs/vblk.img"; } }
  pci0.4 = virtio_net { type = "udp"; udp_local = "127.0.0.1:23456";
                        udp_remote = "127.0.0.1:23456"; }
EOF
ALPHABOX_VIRTIO_SELFTEST=1 EXTRA_CFG=/tmp/virtio.cfg CMDS="show config" \
  test/tools/srm_probe.sh build/alphabox virtio
grep -a VIRTIO lab/runs/probe-virtio/alphabox.out
```

The late variant needs the console to be up and the emulator still running
when it fires, e.g. `ALPHABOX_VIRTIO_SELFTEST_DELAY=40 MEMBITS=28
CMDS="sleep 50|show config" CMD_TIMEOUT=90` (256 MB, so the test's
megabytes are far from the console's own memory).

`ALPHABOX_VIRTIO_TRACE=1` traces every register access, queue address,
notify and interrupt level change (`VIRTIO` lines) -- the first thing to
turn on when a driver and the device disagree.

## Not implemented

The modern (virtio 1.0, revision 1, capability-based) interface; MSI-X;
`EVENT_IDX`; the net control queue, offloads, mergeable buffers and
multiqueue; blk DISCARD / WRITE_ZEROES / topology / config write-back;
a configuration-change interrupt. Saved state (`SaveState`) keeps the
registers and queue indices, so a restored guest continues where it was.
