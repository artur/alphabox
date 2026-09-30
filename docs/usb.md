# USB

Alphabox has two USB host controllers, three kinds of device to put on
them, and the test hooks to make a guest's USB driver prove itself.

| | Controller | Speed | Ports | Guest drivers |
| --- | --- | --- | --- | --- |
| `ali_usb` (pci0.19) | the ALi M1543C's own OHCI 1.0a function, on every ES40 | USB 1.1 (12 Mb/s) | 3 | Windows 2000 and Whistler (AXP64) ship `openhci.sys`, `usbhub.sys`, `usbstor.sys`, `hidusb.sys`, `mouhid.sys` |
| `ehci` (an add-in card) | EHCI 1.0, presenting itself as the EHCI function of a NEC uPD720101 (1033:00e0, class 0C0320) | USB 2.0 (480 Mb/s) | 4 | none shipped for any Alpha Windows; `nadaehci.sys`, written for this card with the nada compiler (a separate project), drives it on Windows 2000 |

## Configuration

```
pci0.19 = ali_usb              // the built-in USB 1.1 controller
{
  port1 = "tablet";            // an absolute pointer: see below
  disk2.0 = file               // a USB disk on port 2
  {
    file = "usb.img";
  }
  port3 = "host:2109:8888";    // a real host device, passed through
}

pci0.3 = ehci                  // a USB 2.0 card in a free slot
{
  disk1.0 = file { file = "usb2.img"; }
  port2 = "tablet";
}
```

A port takes one device. Disks are named `disk<port>.0`, with the port
counted from 1; `port<n>` takes `tablet` or `host:vvvv:pppp`. The sample
[`es40.cfg`](../es40.cfg) documents every value.

## Devices

**Tablet.** A HID pointer with absolute coordinates (0..32767 on each axis,
three buttons, a wheel). In the SDL window the guest's cursor simply goes
where the host's is: no mouse capture, no acceleration to fight, and the
host cursor is hidden over the window. On an EHCI port it is a USB 2.0
device polled every millisecond. Headless runs place it with
`tablet:X:Y[:B]` key-pipe tokens ([headless.md](headless.md)).

**Mass storage.** Any disk image (`file`, `device` or `ramdisk`, as on the
IDE and SCSI controllers) as a Bulk-Only Transport device with the SCSI
transparent command set -- the standard USB stick. The SCSI commands are
the same engine the IDE ATAPI path and the SCSI adapters use. On an EHCI
port it is USB 2.0 (512-byte bulk packets, a device qualifier). Each disk
has its own serial number, so two copies of one image are two devices.

**Host passthrough** (`host:vvvv:pppp`, builds with libusb; `alphabox
--version` lists it). A real device of the host, handed to the guest:
control, bulk and interrupt transfers, submitted asynchronously, so the
controller's frame never waits on the hardware -- a transfer is NAKed until
it completes, as a real device does while busy. SET_ADDRESS stays in the
emulator; SET_CONFIGURATION and SET_INTERFACE go through libusb, which
claims the interfaces. On the OHCI a high-speed device's descriptors are
rewritten to what a full-speed port allows (64-byte packets, intervals in
frames); on EHCI they pass unchanged.

Passthrough limits worth knowing:

- Isochronous endpoints (audio, webcams) are refused.
- On macOS the host keeps interfaces its own drivers hold -- keyboards,
  mice, mass storage, audio -- so such a device enumerates in the guest
  but cannot be used, unless alphabox runs as root. Some devices also need
  an entitlement or an unlocked Mac before a user program may open them at
  all (`ioreg` shows `UsbUserClientEntitlementRequired`).
- Verified on hardware so far: enumeration and control transfers. Bulk and
  interrupt pipes are verified through the emulated equivalent below, not
  yet against a physical device.

## How the controllers work

Both run their schedule on a thread of their own. The OHCI walks its
periodic, control and bulk lists each 1 ms frame; the EHCI walks its
asynchronous ring every 125 us microframe while it has recently moved data
(a driver queues work with plain memory writes, so the card has to look),
and each frame otherwise. Either is woken at once when a device finishes a
transfer, or when the driver fills a list. Consecutive bulk TDs on an OHCI
endpoint go to the device as one transfer, as a stream of packets would.

Interrupts follow the hardware: the ALi function's goes through the
bridge's USBIR routing byte to an ISA IRQ, as on the real chip; the EHCI
card's is PCI INTA. Both controllers reset with the machine and with a PCI
bus reset.

The EHCI card has no companion controllers: only a high-speed device keeps
its port. A full- or low-speed one is left for a companion that is not
there, as the specification has it. Isochronous transfer descriptors (iTD,
siTD, and isochronous OHCI TDs) are not implemented.

Device state -- addresses, configuration -- is not in a saved snapshot:
after a restore every device is shown to the guest as reconnected, and its
driver enumerates it again.

## Speed

Reading a 16 MB file from a USB disk in a Windows 2000 guest in 64 KB
`ReadFile` calls, timed by the guest's clock (the nada project's EHCI test
harness, same image for both rows, two runs each; not perf_ab
measurements):

| Path | 16 MB |
| --- | --- |
| OHCI, Windows' own drivers | ~80 ms |
| EHCI, `nadaehci.sys` | ~110 ms |

Both depend on the idle pacer taking an interrupt the moment it is raised:
before that was fixed, every interrupt that arrived while Windows idled
waited for the next timer tick, and the EHCI read took a second
([performance.md](performance.md#an-interrupt-that-waited-for-the-timer)).

## Testing a guest's USB driver

Everything below is documented in [headless.md](headless.md):

- `ALPHABOX_EHCI_SELFTEST=1` drives the EHCI card from inside the emulator
  at start-up -- port reset, enumeration, a Bulk-Only read of sector 0 --
  and prints PASS or FAIL. No guest driver needed.
- `usb:detach`, `usb:attach`, `usb:stall`, `usb:phase` and `usb:nak`
  tokens inject faults on a port while a guest runs: surprise removal, a
  halted endpoint, a Bulk-Only phase error, a device that stops answering.
- `ALPHABOX_USB_ASYNC_US=<us>` gives the emulated disk the timing of a
  device behind libusb, to exercise the passthrough path with data that can
  be checked.
- `test/tools/usb_bench.sh` times the 16 MB read and checks the copy
  written back byte for byte, optionally firing faults during it.
- `ALPHABOX_USBTRACE=1` logs register writes, every control request a
  device serves, each Bulk-Only command, and the EHCI card's per-transfer
  timeline (pickup, retirement, interrupt level, the driver's status reads).

Windows 2000's own drivers recover from an injected STALL and from surprise
removal; after a phase error its hub driver resets and restores the device
in a loop without retrying the command. `nadaehci.sys` recovers from all of
them.
