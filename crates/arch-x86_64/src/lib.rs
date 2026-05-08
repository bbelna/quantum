//! `x86_64` implementation of the Quantum HAL contracts.
//!
//! This crate only builds on `x86_64` targets. Everything else is silently
//! empty so the workspace remains buildable on non-x86 hosts — kernel binaries
//! always link this conditionally via the top-level `quantum-kernel` crate.
//!
//! # Design notes
//!
//! * No inline assembly here yet; we reach for the `core::arch::x86_64`
//!   intrinsics where available to keep things portable-within-nightly.
//! * The HAL impl is the *only* place in the tree that is allowed to know it
//!   is running on `x86_64`. Core code must never ask.

#![no_std]

#[cfg(target_arch = "x86_64")]
mod cpu;

#[cfg(target_arch = "x86_64")]
pub use cpu::X86_64Cpu;
