//! Arch-agnostic core logic for the Quantum kernel.
//!
//! Everything in this crate is `#![no_std]` and ISA-independent. Arch-specific
//! behavior is reached via traits defined in [`quantum_hal`]. This means every
//! module here is directly testable on the host (`cargo test`) without needing
//! a custom target or QEMU.
//!
//! # Guarantees
//!
//! * No `unsafe` that relies on hardware state. If a routine would need to poke
//!   MSRs, page tables, or interrupt controllers, it belongs in a HAL trait
//!   method and not in this crate.
//! * No `#[cfg(target_arch = ...)]`. Call sites are arch-free; swaps happen at
//!   the kernel binary link layer.

#![no_std]

pub mod bitmap;
