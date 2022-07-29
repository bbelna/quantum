#pragma once

#include <asm/stddef.h>

inline static uint8_t inportb(uint16_t _port) {
  uint8_t rv;
  asm volatile("inb %1, %0":"=a"(rv):"dN"(_port));
  return rv;
}

inline static wchar_t inportw(uint16_t _port) {
  wchar_t rv;
  asm volatile("inw %1, %0":"=a"(rv):"dN"(_port));
  return rv;
}

inline static uint32_t inportl(uint16_t _port) {
  uint32_t rv;
  asm volatile("inl %1, %0":"=a"(rv):"dN"(_port));
  return rv;
}