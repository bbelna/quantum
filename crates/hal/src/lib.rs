//! Hardware abstraction contracts for the Quantum kernel.
//!
//! This crate defines the **trait contracts** that arch and platform crates must
//! implement to provide a working kernel build. It contains no implementations
//! and no arch-specific code — everything here is pure contract.
//!
//! # Design
//!
//! Quantum is modular: [`quantum-core`](../quantum_core/index.html) holds the
//! arch-agnostic logic and calls into these traits. An arch crate (e.g.
//! `quantum-arch-x86_64`) provides the concrete impls. The kernel binary crate
//! is the only place that picks which arch/platform gets linked in, so call
//! sites stay free of `#[cfg(target_arch = ...)]` sprinkles.
//!
//! See [`project_architecture_decisions.md`] in the project memory for the
//! rationale behind this split.
//!
//! [`project_architecture_decisions.md`]: https://github.com/bbelna/quantum

#![no_std]

pub mod cpu;

pub use cpu::Cpu;
