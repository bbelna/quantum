//! Fixed-size bitmap over borrowed `u64` storage.
//!
//! This is the primitive used by the address-space range allocator and any
//! other subsystem that needs dense boolean state. It borrows its backing store
//! rather than owning it so the caller decides where the bits live — static
//! memory at boot, a pre-mapped page later, or the stack in tests.
//!
//! Bits are packed little-endian-within-word: bit `i` lives in
//! `words[i / 64]` at position `1 << (i % 64)`.

/// Number of bits per backing word.
const BITS_PER_WORD: usize = u64::BITS as usize;

/// A fixed-capacity bitmap over caller-provided `u64` storage.
///
/// Capacity is `words.len() * 64`. Out-of-range indices are a programmer
/// error and panic in debug, matching `core`'s slice-indexing conventions.
pub struct Bitmap<'a> {
  words: &'a mut [u64],
}

impl<'a> Bitmap<'a> {
  /// Wrap the provided storage. The storage is **not** cleared; the caller
  /// decides the initial bit state.
  #[must_use]
  pub fn new(words: &'a mut [u64]) -> Self {
    Self { words }
  }

  /// Total number of bits this bitmap can address.
  #[must_use]
  pub fn len(&self) -> usize {
    self.words.len() * BITS_PER_WORD
  }

  /// `true` if the bitmap has zero capacity.
  #[must_use]
  pub fn is_empty(&self) -> bool {
    self.words.is_empty()
  }

  /// Read bit `i`.
  #[must_use]
  pub fn get(&self, i: usize) -> bool {
    let (w, b) = split(i);

    (self.words[w] >> b) & 1 == 1
  }

  /// Set bit `i` to 1.
  pub fn set(&mut self, i: usize) {
    let (w, b) = split(i);

    self.words[w] |= 1u64 << b;
  }

  /// Clear bit `i` to 0.
  pub fn clear(&mut self, i: usize) {
    let (w, b) = split(i);

    self.words[w] &= !(1u64 << b);
  }
}

#[inline]
fn split(i: usize) -> (usize, u32) {
  #[expect(clippy::cast_possible_truncation, reason = "i % 64 fits in u32")]
  (i / BITS_PER_WORD, (i % BITS_PER_WORD) as u32)
}

#[cfg(test)]
mod tests {
  use super::*;

  #[test]
  fn len_reflects_backing_storage() {
    let mut storage = [0u64; 4];
    let bm = Bitmap::new(&mut storage);

    assert_eq!(bm.len(), 256);
    assert!(!bm.is_empty());
  }

  #[test]
  fn set_then_get_round_trips() {
    let mut storage = [0u64; 2];
    let mut bm = Bitmap::new(&mut storage);

    for i in [0, 1, 63, 64, 127] {
      assert!(!bm.get(i));

      bm.set(i);

      assert!(bm.get(i));
    }
  }

  #[test]
  fn clear_undoes_set() {
    let mut storage = [u64::MAX; 1];
    let mut bm = Bitmap::new(&mut storage);

    bm.clear(17);

    assert!(!bm.get(17));
    assert!(bm.get(16));
    assert!(bm.get(18));
  }

  #[test]
  fn bits_are_independent_across_word_boundary() {
    let mut storage = [0u64; 2];
    let mut bm = Bitmap::new(&mut storage);

    bm.set(63);
    bm.set(64);

    assert!(bm.get(63));
    assert!(bm.get(64));
    assert!(!bm.get(62));
    assert!(!bm.get(65));
  }
}
