/* Alphabox: the AlphaServer ES40 emulator
 *
 * Hypervisor.framework runtime: runs this process's own code at EL1 inside a
 * VM, on a vCPU per calling thread, with the same virtual addresses it has
 * outside. See docs/hypervisor.md ("The build") for the design.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */
#pragma once

#ifdef ALPHABOX_HVF

#include <cstddef>
#include <cstdint>

namespace hv {

// What the runtime did, for the self-test and the traces.
struct Stats {
  uint64_t entries = 0;        // hv_vcpu_run calls
  uint64_t pages_direct = 0;   // host pages mapped in place (IPA == VA)
  uint64_t pages_copied = 0;   // pages copied into the arena (text, cache)
  uint64_t stage1_faults = 0;  // EL1 translation faults resolved by the host
  uint64_t syscalls = 0;       // SVCs performed on the guest's behalf
  uint64_t irqs = 0;
  uint64_t escapes = 0;        // calls performed outside on the VM's behalf
  uint64_t by_ec[64] = {};     // exits by exception class
  uint64_t el1_ec[64] = {};    // what our own EL1 vector saw
  uint64_t el1_far = 0, el1_pc = 0; // and the last one's address
  uint64_t sysreg[8] = {};     // trapped system registers, by op0/op1 bucket
  uint64_t last_sysreg = 0;    // the most recent trapped register's encoding
  uint64_t last_pc = 0;
  uint64_t other_exits = 0;
};

// One-time VM creation (idempotent). Returns false, with the reason printed,
// when Hypervisor.framework is unavailable or the entitlement is missing.
bool init();

// Whether init() succeeded.
bool available();

// ALPHABOX_HV=1: the CPU threads run their loop inside the VM. enable()
// after a successful init(); enabled() is what the CPU asks.
void enable();
bool enabled();

// Run fn(arg) inside the VM on this thread's vCPU (created on first use) and
// return its result. Everything fn touches is mapped on demand; the call
// returns when fn returns or when the VM is cancelled (then ~0ULL).
uint64_t call(uint64_t (*fn)(void *), void *arg);

// Cancel the vCPU running on `thread_id` (from hv_vcpu_id()) from another
// thread: its call() returns ~0ULL at the next exit.
void cancel_all();

const Stats &stats();

// Memory both sides share for real: allocated through the framework's own
// allocator and mapped into the VM at its own address. This is the ONLY
// memory the VM and the outside world hold in common -- see the mapping
// rule in HvRuntime.cpp. Returns nullptr on failure; freed only at exit.
void *alloc(size_t size);

// Run fn(arg) OUTSIDE the VM and return its result. Called from code that
// may be running at EL1: it leaves the VM, the host thread performs the
// call with the real process memory under it, and the VM resumes. Called
// from ordinary host code it is just fn(arg). This is how the parts that
// must stay outside -- the device models, the allocator, anything that
// takes a lock the device threads also take -- are reached from inside.
// `arg` must be memory both sides see: the caller's own stack qualifies
// when the caller is inside (the vCPU stack is alloc() memory), as does
// anything from alloc().
uint64_t escape(uint64_t (*fn)(void *), void *arg);

// Whether this thread is currently executing inside the VM.
bool inside();

// How a host page would be treated. Only memory from alloc() above is ever
// passed to hv_vm_map; every other page the VM touches is copied, whatever
// this says. The classification survives as a diagnostic (hvtest
// --regions) and to tell a readable page from an unreadable one.
enum class PageClass { kAnonymous, kCopy, kUnmapped };
PageClass classify(uint64_t page, char *why, size_t why_len);

// List every region of this process with its classification and stop:
// no VM is created. What `alphabox hvtest --regions` prints.
void dump_regions();

} // namespace hv

#endif // ALPHABOX_HVF
