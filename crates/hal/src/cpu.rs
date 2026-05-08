//! CPU control contract.
//!
//! Minimum surface an arch must expose so core code can idle, spin, and pause
//! without depending on any specific ISA.

/// Per-CPU control primitives every arch implementation must provide.
///
/// All methods are associated (not `&self`) because the CPU is a singleton from
/// the kernel's perspective — we dispatch through a type, not an instance. This
/// keeps call sites cheap (no indirection) and lets the optimizer inline across
/// the boundary.
pub trait Cpu {
  /// Halt the CPU until the next interrupt, then return.
  ///
  /// On x86 this is `HLT`; on `AArch64` this is `WFI`. Used by the idle loop
  /// and anywhere the kernel wants to wait for external events without
  /// burning power.
  fn halt();

  /// Hint to the CPU that we are in a spin-wait.
  ///
  /// Implementations should emit a lightweight pause/yield (e.g. x86 `PAUSE`,
  /// `AArch64` `YIELD`). This is a performance hint, not a correctness
  /// requirement — a no-op impl is legal.
  fn pause();
}
