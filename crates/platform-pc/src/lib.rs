//! PC-class platform glue for the Quantum kernel.
//!
//! "PC" here means the IBM-PC-descended platform: legacy 8259 / APIC-LAPIC /
//! IOAPIC interrupt routing, ACPI for firmware tables, PIO and MMIO for
//! peripherals. This crate is arch-agnostic in *principle* (a PC can be
//! `x86_64`, i686, or in theory `AArch64`-SBSA), but in practice we pair it
//! with `quantum-arch-x86_64` for v1.
//!
//! Currently empty — platform pieces land here as the kernel grows to need
//! them (first target: serial console for debug output).

#![no_std]
