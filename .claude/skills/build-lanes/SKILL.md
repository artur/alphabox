---
name: build-lanes
description: Configure, build and sanity-check Alphabox's build lanes (interpreter, JIT, JIT_VERIFY, JIT_STATS/JIT_REGPROF, x86-64 via Rosetta, headless) and check that every commit of a series builds. Use before committing C++ changes, when adding or reconfiguring a lane, or when a lane's results look suspicious.
---

# Build lanes

One source tree, many CMake build directories ("lanes"), each testing a
different configuration. Sources are collected by `file(GLOB)`: re-run the
configure step after adding a `.cpp`. Builds default to Release when no
`CMAKE_BUILD_TYPE` is given (single-config generators).

## Prerequisites

- asmjit for JIT lanes: plain clone (gitignored) in `third_party/asmjit` at
  pin `0bd5787b54b575ed94bf32ac452153b34385c514`.
- SDL3: system package when found, else the `third_party/SDL` submodule
  (`git submodule update --init`). `-DDISABLE_SDL=yes` builds headless.

## Lanes

Minimum for any change: an interpreter lane, a JIT lane and a headless lane
must compile. The set kept configured on the macOS/arm64 development host:

| Lane | Options | Use |
|---|---|---|
| `build` | `DISABLE_SDL=yes` | interpreter, headless |
| `build-default` | (none) | interpreter with SDL |
| `build-jit` | `ES40_DISABLE_ASMJIT=OFF` | AArch64 JIT with SDL: guest boots |
| `build-jit-verify` | JIT, `-DJIT_VERIFY`, headless | compiled blocks checked against the interpreter (expect 0 mismatches) |
| `build-jit-verify-sdl` | JIT, `-DJIT_VERIFY` | the same with a display |
| `build-jit-verify-x64` | JIT, `-DJIT_VERIFY`, `CMAKE_OSX_ARCHITECTURES=x86_64`, headless | x86-64 emitter under verify (Rosetta) |
| `build-jit-x64` | JIT, x86_64, headless | x86-64 emitter without verify: the only lane running its chain gates |
| `build-jit-stats` | JIT, `-DJIT_STATS`, headless | cold-path, bail and throughput counters |
| `build-jit-stats-sdl` | JIT, `-DJIT_STATS` | MIPS benchmarks on Windows guests |
| `build-jit-regprof-sdl` | JIT, `-DJIT_STATS -DJIT_REGPROF` | exec-weighted register/expansion reports |
| `build-dbgflags-arm64` / `-x86_64` | JIT, `-DJIT_STATS -DJIT_REGPROF -DJIT_DISASM -DJIT_TRACES`, headless | keeps the rarely built debug code compiling (the trace tier is dormant) |

What a verify lane cannot see: the chain gates and the inline data-page-cache
fast paths are compiled out under `JIT_VERIFY`, so changes there need guest
boots (and `build-jit-x64` for the x86-64 side). `ALPHABOX_JIT_FPTEST=1` on a
verify build self-tests the inline IEEE FP ops.

## Configure (bash, not zsh)

The Bash tool runs zsh, where an unquoted `$opts` is ONE argument: a
"JIT lane" configured as `cmake -S . -B dir $opts` silently builds an
interpreter. Use a bash script with arrays:

```bash
#!/bin/bash
opts=(-DCMAKE_BUILD_TYPE=Release -DES40_DISABLE_ASMJIT=OFF -DDISABLE_SDL=yes
      "-DCMAKE_CXX_FLAGS=-DJIT_VERIFY")
cmake -S . -B build-jit-verify "${opts[@]}"
```

Then check what was actually configured, before trusting any result:

```bash
grep -E '^(ES40_DISABLE_ASMJIT|DISABLE_SDL|CMAKE_CXX_FLAGS|CMAKE_OSX_ARCHITECTURES|CMAKE_BUILD_TYPE):' build-jit-verify/CMakeCache.txt
grep -rl ES40_JIT build-jit-verify/CMakeFiles/*/flags.make   # JIT lanes must define it
```

## Build every lane

```bash
test/tools/build_lanes.sh                 # every configured build*/ directory
test/tools/build_lanes.sh build build-jit # just these
```

One `rc=0` / `FAILED` line per lane (first errors shown, full log in
`<lane>/build_lanes.log`); exit status 0 only when every lane built.

Don't rebuild a lane whose binary a running test uses (the linker replaces
the file under it): copy the binary first, or wait.

## Every commit of a series builds

Before fast-forwarding a branch, build each intermediate commit in a
separate worktree with its own lanes:

```bash
git worktree add ../alphabox-wt <base>           # once; configure lanes inside
WT=../alphabox-wt test/tools/build_revs.sh <sha1> <sha2> ...
```

A detached checkout refuses while the worktree has local edits; compare
them with the target first (`git diff <rev> --stat`) and discard only what
matches.

## Formatting

Repo style is clang-format (LLVM). Format only the lines you changed:

```bash
git clang-format --binary <clang-format> --diff HEAD -- src
```

`clang-format-14` matches the repo's formatting exactly; newer versions
(e.g. Homebrew LLVM on macOS) differ in a few spots such as `){};` vs
`) {};`. When they disagree on untouched style, keep the existing form.
