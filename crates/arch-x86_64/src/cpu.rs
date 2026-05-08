//! `x86_64` [`Cpu`] implementation.
//!
//! `HLT` and `PAUSE` semantics are documented in the Intel SDM, Vol. 2A/2B.
//! `HLT` requires ring 0; calling it from user mode will #GP. `PAUSE` is
//! always safe and is the canonical spin-wait hint since the P4.

use core::arch::asm;

use quantum_hal::Cpu;

/// Zero-sized type carrying the `x86_64` [`Cpu`] implementation.
pub struct X86_64Cpu;

impl Cpu for X86_64Cpu {
  fn halt() {
    // SAFETY: `hlt` has no memory effects and no operands; the only
    // requirement is ring 0, which the kernel always satisfies.
    unsafe { asm!("hlt", options(nomem, nostack, preserves_flags)) }
  }

  fn pause() {
    // SAFETY: `pause` is a hint with no architectural side effects.
    unsafe { asm!("pause", options(nomem, nostack, preserves_flags)) }
  }
}
