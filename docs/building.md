# Building Alphabox

There are no binary packages of this project. The packages in
[T2 SDE](http://t2sde.org/packages/axpbox) and
[openSUSE](https://build.opensuse.org/package/show/Emulators/axpbox) are
built from [lenticularis39/axpbox](https://github.com/lenticularis39/axpbox),
not from this repository.

## What you need

CMake and a C++17 compiler. Everything else is optional:

| Dependency | What it gives | Without it |
| --- | --- | --- |
| pcap | networking | no networking; `-DDISABLE_PCAP=yes` skips the search |
| SDL3 | the graphical console | headless only; `-DDISABLE_SDL=yes` builds that on purpose |
| libusb 1.0 | a host USB device passed through to the guest | no passthrough; `-DDISABLE_LIBUSB=yes` skips the search |
| asmjit | the JIT | the interpreter only (see [JIT](#jit)) |

SDL3 is bundled as a submodule and linked statically when no system SDL3
is found:

```
git clone --recurse-submodules https://github.com/artur/alphabox
cd alphabox
# existing clone: git submodule update --init
```

Without `-DCMAKE_BUILD_TYPE` the build defaults to Release. `alphabox --version`
prints the version, the commit and the compiled-in features.

Sources are collected at *configure* time. After adding, moving or deleting a
source file, re-run `cmake -S . -B <build-dir>`; `cmake --build` alone keeps
the old file list.

## Linux

```
sudo apt install build-essential cmake libpcap-dev
# only needed when building the bundled SDL3 (skip for headless):
sudo apt install libx11-dev libxext-dev libxrandr-dev libxcursor-dev \
                 libxi-dev libxtst-dev libxfixes-dev libxss-dev libxkbcommon-dev libwayland-dev libegl-dev

cmake -S . -B build
cmake --build build -j$(nproc)
```

The binary is `build/alphabox`.

- Headless build: add `-DDISABLE_SDL=yes -DDISABLE_X11=yes`.
- Without networking: add `-DDISABLE_PCAP=yes`.

To use a NIC with the pcap backend without running as root, grant capture
permission once:

```
sudo setcap cap_net_raw,cap_net_admin+eip ./build/alphabox
```

## macOS

```
brew install cmake sdl3
cmake -S . -B build
cmake --build build -j$(sysctl -n hw.ncpu)
```

## Windows

You need Visual Studio 2022 with "Desktop development with C++" and CMake.
SDL3 needs no separate install. Networking needs Npcap:

- at build time, the [npcap SDK](https://npcap.com/dist/npcap-sdk-1.13.zip),
  unzipped to e.g. `C:\pcap`;
- at run time, [Npcap](https://npcap.com/#download) installed. Without it the
  emulator still starts, just without networking.

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 ^
      -DPCAP_INCLUDE_DIR=C:/pcap/Include ^
      -DPCAP_LIBRARY=C:/pcap/Lib/x64/wpcap.lib
cmake --build build --config Release
```

The binary is `build\Release\alphabox.exe`.

## JIT

The JIT translates Alpha code to host code with asmjit, and has emitters
for **x86-64 and AArch64** (Apple Silicon, arm64 Linux). Clone asmjit at
the pinned revision and enable the JIT:

```
git clone https://github.com/asmjit/asmjit third_party/asmjit
git -C third_party/asmjit checkout 0bd5787b54b575ed94bf32ac452153b34385c514
cmake -S . -B build-jit -DES40_DISABLE_ASMJIT=OFF
cmake --build build-jit -j$(nproc)
```

Diagnostic JIT builds, used while developing:

| Flag | Effect |
| --- | --- |
| `-DCMAKE_CXX_FLAGS="-DJIT_VERIFY"` | Re-runs every compiled block in the interpreter and reports any difference; expect `0 mismatches`. |
| the same, run with `ALPHABOX_JIT_FPTEST=1` | Also self-tests the inline IEEE floating-point ops against the interpreter (about 8.5 M cases). |
| `-DCMAKE_CXX_FLAGS="-DJIT_STATS"` | Adds throughput, cold-path and bail counters. |
| `-DCMAKE_CXX_FLAGS="-DJIT_STATS -DJIT_REGPROF"` | Adds the register and hot-path profile. |
| `-DCMAKE_CXX_FLAGS="-DJIT_DISASM"` | Lists every compiled block. Very noisy. |

Verify builds compile every block on its first run, for maximum coverage.
`src/common/config_debug.hpp` lists the other debug flags.

## The Hypervisor.framework runtime

`-DALPHABOX_HVF=ON` (macOS on Apple silicon, with the JIT enabled) builds
the experimental runtime that runs the processor's loop inside a VM, and
the `alphabox hvtest` and `alphabox hvprobe` subcommands. The build signs
the binary ad hoc with the hypervisor entitlement. See
[hypervisor.md](hypervisor.md).

## Build configurations kept working

Every change must compile in at least:

- an interpreter build with the SDL3 GUI;
- a JIT build;
- a headless build (`-DDISABLE_SDL=yes`).

`test/tools/build_lanes.sh` builds every configured `build*/` directory; see
[development.md](development.md).
