#pragma once

#include <arch/x86/stddef.h>

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

inline static void outportb(uint16_t _port, uint8_t _data) {
  asm volatile("outb %1, %0"::"dN"(_port), "a"(_data));
}

inline static void outportw(uint16_t _port, uint16_t _data) {
  asm volatile("outw %1, %0"::"dN"(_port), "a"(_data));
}

inline static void outportl(uint16_t _port, uint32_t _data) {
  asm volatile("outl %1, %0"::"dN"(_port), "a"(_data));
}
