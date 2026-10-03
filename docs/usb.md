# USB

Alphabox has three USB host controllers, four kinds of device to put on
them, and the test hooks to make a guest's USB driver prove itself.

| | Controller | Speed | Ports | Guest drivers |
| --- | --- | --- | --- | --- |
| `ali_usb` (pci0.19) | the ALi M1543C's own OHCI 1.0a function, on every ES40 | USB 1.1 (12 Mb/s) | 3 | Windows 2000 and Whistler (AXP64) ship `openhci.sys`, `usbhub.sys`, `usbstor.sys`, `hidusb.sys`, `mouhid.sys` |
| `ehci` (an add-in card) | a NEC uPD720101: EHCI 1.0 (function 2, 1033:00e0, class 0C0320) with two OHCI 1.0a companions (functions 0 and 1, 1033:0035, class 0C0310) | USB 2.0 (480 Mb/s); full speed through the companions | 4 | EHCI: none shipped for any Alpha Windows; `nadaehci.sys`, written for this card with the nada compiler (a separate project), drives it on Windows 2000. The companions: Windows' own `openhci.sys` |
| `uss344` (the ES47's on-board USB, hose 2 slot 3) | an Agere (Lucent) USS-344 QuadraBus: four OHCI 1.0a functions (11C1:5803, class 0C0310, all on INTA), one root hub port each -- the `ehci` card's companions without the EHCI | USB 1.1 (12 Mb/s) | 4 | the ES47 console lists usba..usbd; OpenVMS 8.4 configures OHA0..OHC0; Windows' `openhci.sys` binds to any OHCI |

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
                               // (or "audio": a USB speaker)
}

pci0.3 = ehci                  // a USB 2.0 card in a free slot
{
  disk1.0 = file { file = "usb2.img"; }
  port2 = "tablet";
  companions = true;           // the default; false: the EHCI alone
}
```

A port takes one device. Disks are named `disk<port>.0`, with the port
counted from 1; `port<n>` takes `tablet`, `audio` (not on a card without
companions) or `host:vvvv:pppp`. The sample
[`es40.cfg`](../es40.cfg) documents every value.

## Devices

**Tablet.** A HID pointer with absolute coordinates (0..32767 on each axis,
three buttons, a wheel). In the SDL window the guest's cursor simply goes
where the host's is: no mouse capture, no acceleration to fight, and the
host cursor is hidden over the window. On an EHCI card without companions
it is a USB 2.0 device polled every millisecond; everywhere else it is the
full-speed device a real tablet is (on the card: its companion's). Headless runs place it with
`tablet:X:Y[:B]` key-pipe tokens ([headless.md](headless.md)).

**Mass storage.** Any disk image (`file`, `device` or `ramdisk`, as on the
IDE and SCSI controllers) as a Bulk-Only Transport device with the SCSI
transparent command set -- the standard USB stick. The SCSI commands are
the same engine the IDE ATAPI path and the SCSI adapters use. On an EHCI
port it is USB 2.0 (512-byte bulk packets, a device qualifier). Each disk
has its own serial number, so two copies of one image are two devices.

**Speaker** (`audio`: a full-speed device, so on the EHCI card it is
served by a companion, and a card with `companions = false` refuses it). A USB Audio
Class 1.0 speaker: an input terminal fed by the streaming interface, a
feature unit with master mute and volume (-60 to 0 dB), a speaker output
terminal. Alternate setting 1 of the streaming interface has an adaptive
isochronous OUT endpoint taking 16-bit stereo PCM at 48 or 44.1 kHz, chosen
by the host with SET_CUR on the endpoint's sampling frequency control.
Windows 2000 installs its own `usbaudio.sys` for it with nothing supplied
(from its driver cache), and it becomes the preferred playback device,
"USB Audio Device". What the guest sends plays on the host's default audio
device through SDL, at the guest's volume and mute;
`ALPHABOX_USBAUDIO_WAV=<file>` also writes the stream, as sent, to a WAV
file (a headless run with `SDL_AUDIO_DRIVER=dummy` keeps it off the
speakers). Windows 2000's mixer resamples everything to 48 kHz.

**Host passthrough** (`host:vvvv:pppp`, builds with libusb; `alphabox
--version` lists it). A real device of the host, handed to the guest:
control, bulk and interrupt transfers, submitted asynchronously, so the
controller's frame never waits on the hardware -- a transfer is NAKed until
it completes, as a real device does while busy. Isochronous endpoints
stream through libusb isochronous transfers, on the OHCI and on EHCI iTDs
alike: each OUT packet the guest sends is submitted at once (with up to 32
in flight; beyond that it is refused), and an IN endpoint the guest reads
is kept streaming -- four transfers of eight packets in flight -- into a
queue its reads take packets from. An isochronous pipe has no NAK, so a
packet that is not there when the guest's (micro)frame wants it is reported
as a real controller reports a packet it could not move in time: on the
OHCI BufferOverrun (IN) or BufferUnderrun (OUT) in the packet's status
word, on an iTD Data Buffer Error. SET_ADDRESS stays in the emulator;
SET_CONFIGURATION and SET_INTERFACE go through libusb, which claims the
interfaces; the device's configuration is only changed when the guest asks
for a different one, and never to 0 (unconfigured), so interfaces the host's
own drivers hold on a composite device are left alone. On the OHCI a high-speed device's descriptors are
rewritten to what a full-speed port allows (64-byte packets, intervals in
frames); on EHCI they pass unchanged.

Passthrough limits worth knowing:

- A high-speed isochronous device (a USB 2.0 webcam) needs the EHCI: a
  full-speed controller cannot carry its bandwidth.
- On macOS the host keeps interfaces its own drivers hold -- keyboards,
  mice, mass storage, audio -- so such a device enumerates in the guest
  but cannot be used, unless alphabox runs as root. Some devices also need
  an entitlement or an unlocked Mac before a user program may open them at
  all (`ioreg` shows `UsbUserClientEntitlementRequired`).
- Verified on hardware so far: enumeration and control transfers. With an
  ESP32-S3's built-in USB JTAG/serial unit (303a:1001) on `ali_usb`,
  Windows 2000 read every descriptor through passthrough (all requests
  answered, the 98-byte configuration included) and offered the device in
  Found New Hardware, while macOS kept its hold on the device's serial
  interfaces. On the host, libusb bulk transfers to the same device's free
  JTAG interface (interface 2) read the chip's IDCODE, 0x120034e5, without
  root. Bulk and interrupt pipes from a guest are verified through the
  emulated equivalent below, not yet against a physical device: Windows 2000
  has no driver for the JTAG interface (one is being written with nada).
- A composite device whose class is EF/02/01 (interface association, as
  the ESP32's) is not split into its interfaces by Windows 2000, whose hub
  driver only splits devices of class 0: the guest sees one device, and
  its driver selects the configuration itself.
- Isochronous passthrough is **not verified against any physical device**.
  The host devices it would need are out of reach here: macOS's audio
  driver holds a USB audio device's interfaces, and alphabox does not run
  as root (a C-Media USB audio device, 0d8c:0014, full speed, is the
  root-only test path for it on this host). What is verified is the
  controller's side, through the test shim (`ALPHABOX_USB_ASYNC_US`),
  which moves isochronous packets with libusb's timing: Windows 2000
  playing chord.wav on the USB speaker behind the shim (500 us), on the
  ALi's OHCI and on the EHCI card's companion, captures a stream
  byte-for-byte identical to the one captured without the shim; with a
  100 ms latency the pipeline fills, 287 packets are reported as
  BufferUnderrun and the guest carries on; and the EHCI self-test's
  isochronous loopback behind the shim (250 us to 2 ms) returns every byte
  sent, in order, with the transactions that found nothing ready ending in
  Data Buffer Error. The libusb calls themselves (`libusb_fill_iso_transfer`
  and the stream's resubmission) have not run against hardware.

## How the controllers work

Both run their schedule on a thread of their own. The OHCI walks its
periodic, control and bulk lists each 1 ms frame; the EHCI walks its
asynchronous ring every 125 us microframe while it has recently moved data
(a driver queues work with plain memory writes, so the card has to look),
and each frame otherwise. Either is woken at once when a device finishes a
transfer, or when the driver fills a list. Consecutive bulk TDs on an OHCI
endpoint go to the device as one transfer, as a stream of packets would.

The card's two companions are the same OHCI engine as the ALi function
(`COhci`), two ports each: card ports 1-2 belong to function 0, 3-4 to
function 1 (HCSPARAMS N_CC = 2, N_PCC = 2). A port belongs to the EHCI or
to its companion, as EHCI 1.0 section 4.2 has it: while CONFIGFLAG is 0 --
after any reset, and for good under a guest with no EHCI driver, such as
Windows 2000 -- every port is the companions', and the guest's OHCI driver
uses its devices at full speed. An EHCI driver setting CONFIGFLAG takes
every port; a device that is not high speed then stays disabled after the
EHCI's port reset, and the driver hands the port back by setting
PORT_OWNER (clearing it, or CONFIGFLAG going to 0, routes it again). Both
controllers see a connect change whenever a port moves. The device belongs
to the port and follows it. `companions = false;` makes the card the EHCI
function alone (function 0, as before): only a high-speed device keeps its
port there, a full- or low-speed one being left for a companion that is
not there.

Interrupts follow the hardware: the ALi function's goes through the
bridge's USBIR routing byte to an ISA IRQ, as on the real chip; on the card
the companions are PCI INTA and INTB and the EHCI INTC (INTA without
companions). All the controllers reset with the machine and with a PCI bus
reset.

Isochronous transfers run on the OHCI -- the ALi function and the card's
companions alike (OHCI 1.0a 4.3.2): each isochronous
TD covers up to eight consecutive frames from its StartingFrame, one packet
a frame. In each frame the controller moves the packet that frame is due
for, writes its packet status word (condition code, and for IN the size
received) over the packet's offset, and retires the TD to the done queue
after its last packet. A TD queued too late for all its frames is retired
with DataOverrun; one whose first frame is still to come waits. If the
frame thread oversleeps, the frames it missed (up to 32) are run in order
when it wakes, so a stream does not lose packets to host scheduling. A
device takes packets through `CUsbDevice::iso_transfer`.

The EHCI runs high-speed isochronous transfers too (EHCI 1.0 3.3, 4.7):
an iTD in the periodic frame list carries the eight transactions of its
frame, one a microframe, each up to Mult packets of the endpoint's size
(up to 3072 bytes) from its buffer page and offset, continuing into the
next page. Each active transaction is run and written back inactive with
its status -- for IN with the length received: Transaction Error when no
device answers, Babble for an IN packet longer than the host allowed, Data
Buffer Error when the data was not there in time (a device behind libusb).
IOC raises USBINT, an error USBERRINT. The thread's unit is the frame, so a
frame's eight transactions run together, in order; frames it oversleeps are
run one by one, as on the OHCI. siTDs -- split transactions for full-speed
devices behind a high-speed hub -- are passed over: the card has no such
hub, and its full-speed devices go to the companions. No Alpha Windows
driver uses high-speed isochronous endpoints; the iTDs are verified by the
self-test below, against a loopback device only it plugs in.

A saved machine (a snapshot: `ALPHABOX_SNAPSHOT`/`ALPHABOX_RESTORE`, see
`test/tools/nt_snap.sh`) holds the USB as the guest left it: the
controllers' registers, the OHCI frame numbers and done queues (TDs retired
but not yet handed back), and each emulated device's own state -- address,
configuration, the control transfer under way, the tablet's HID settings,
the disk's Bulk-Only stage with a command's data (the disk's SCSI side is
in the disk's saved state), the speaker's stream settings. After a restore
the guest carries on: a copy to the USB disk that was under way when the
snapshot was taken finishes, a sound keeps playing. A device behind libusb
(and the `ALPHABOX_USB_ASYNC_US` test shim) cannot be saved -- its other
half is hardware, with transfers in flight -- so after a restore it is
reset and shown to the guest as reconnected, and its driver enumerates it
again (Windows 2000 then reports an unsafe removal). So is every device of
a state file saved before the devices were (it restores as before).

`test/tools/usb_snap.sh` checks it in a Windows 2000 guest, on the ALi's
OHCI or on the EHCI card's companions: tablet, disk and speaker attached,
the tablet placing the cursor within a pixel before and after, the
snapshot taken (`BUSY=1`) while the guest copies to the disk and plays a
sound, then a copy compared with `fc /b` and on the image, and a sound
played. Before the devices' state was saved, the same run left the guest
with an "Unsafe Removal of Device" dialog, the copies that were under way
missing from the disk, and (on the card) the speaker gone.

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
  and, with companions, the port routing: CONFIGFLAG handing every port
  over and back, and a full-speed device (the tablet, or a probe on a free
  port) left disabled by the EHCI's port reset, handed over with
  PORT_OWNER and enumerated by its companion. Then isochronous iTDs,
  against a high-speed loopback device plugged into a free port: eight OUT
  transactions of different lengths (up to three packets, across buffer
  pages) come back through eight IN transactions with their data and
  lengths, IOC raises USBINT, and an iTD for a missing device ends in a
  transaction error. The disk's Bulk-Only recovery is checked too: a
  phase error, reset recovery, the next command. Prints PASS or FAIL. No
  guest driver needed.
- `usb:detach`, `usb:attach`, `usb:stall`, `usb:phase` and `usb:nak`
  tokens inject faults on a port while a guest runs: surprise removal, a
  halted endpoint, a Bulk-Only phase error, a device that stops answering.
- `ALPHABOX_USB_ASYNC_US=<us>` gives the emulated disk and speaker (and
  the self-test's isochronous loopback) the timing of a device behind
  libusb, isochronous streams included, to exercise the passthrough path
  with data that can be checked.
- `test/tools/usb_bench.sh` times the 16 MB read and checks the copy
  written back byte for byte, optionally firing faults during it.
- `test/tools/usb_audio.sh` plays a sound on the USB speaker and compares
  the capture with the source (`test/tools/wav_compare.py`: alignment,
  envelope and waveform correlation); `test/tools/usb_snap.sh` takes the
  devices through a snapshot.
- `ALPHABOX_USBTRACE=1` logs register writes, the port status the driver
  reads, every control request a device serves, each Bulk-Only command,
  TDs retired with an error, isochronous TDs retired late and packets not
  moved in time, and
  the EHCI card's per-transfer timeline (pickup, retirement, interrupt
  level, the driver's status reads).
- `ALPHABOX_USBAUDIO_WAV=<file>` records what the guest plays on the USB
  speaker, to compare with the file it played.

Windows 2000's own drivers recover from an injected STALL and from surprise
removal. `nadaehci.sys` recovers from all of them.

### A phase error under Windows 2000: Windows, not the device

After an injected `usb:phase` on the OHCI, Windows 2000 (build 2128) resets
and restores the disk in a loop and never retries the command. The Bulk-Only
specification (BOT 1.0, 5.3.3 and 6.6) wants something else: on a CSW with
status 2 the host performs a *reset recovery* -- the class request
Bulk-Only Mass Storage Reset (21 FF), then CLEAR_FEATURE(ENDPOINT_HALT) on
the bulk IN and the bulk OUT endpoint -- after which the device takes the
next CBW. Traced with `ALPHABOX_USBTRACE=1` (usb_bench.sh with
`FAULTS="1:usb:phase:2"`, which also logs the port status the driver reads
and every TD retired with an error), what Windows does instead is:

1. it collects the CSW (status 2, residue 0) and reads the port status
   (0x103: connected, enabled, powered);
2. it takes the bulk endpoints off the schedule and resets the port --
   never the class reset, never CLEAR_FEATURE;
3. usbhub re-enumerates: GET_DESCRIPTOR(device, 64) at address 0, a second
   port reset, SET_ADDRESS(3), GET_DESCRIPTOR(device), GET_DESCRIPTOR
   (configuration, 9 bytes), SET_CONFIGURATION(1), and puts two bulk EDs
   back on the list;
4. it reads the port status twice (0x103) and goes back to step 2.

Every request in the loop is answered at once and successfully by the
device, every port status it reads is connected and enabled, and no TD
ends in an error (none traced) -- nothing on the bus gives it a reason. No
CBW is sent between the cycles. One run looped 3280 times in 100 s until
stopped; another stopped after 141 cycles (8.5 s), failing the command,
and the copy that issued it ended with an error. The injected phase error
is on one CSW only (one status-2 CSW in each trace): the loop is not the
fault being raised again.

The device side of the recovery Windows skips is checked by the EHCI
self-test: a phase-error CSW, the class reset and both CLEAR_FEATUREs, then
a READ(10) with a new tag answered with its data and a good CSW carrying
that tag; and a class reset in the middle of a command's data stage,
followed by a new command that runs normally. Windows 2000's behaviour is
its own; nothing here is changed for it.
