#pragma once

#include <quantum/stddef.h>

inline static uint64_t rdtsc(void) {
  uint64_t x;
  asm volatile("rdtsc" : "=A" (x));
  return x;
}

inline static void flush_cache(void) {
  asm volatile("wbinvd" : : : "memory");
}

inline static void invalid_cache(void) {
  asm volatile("invd");
}

inline static void mb(void) { asm volatile("mfence" ::: "memory"); }
inline static void rmb(void) { asm volatile("lfence" ::: "memory"); }
inline static void wmb(void) { asm volatile("sfence" ::: "memory"); }

#define NOP     asm volatile("nop")
#define PAUSE   asm volatile("pause")
#define HALT    asm volatile("hlt")