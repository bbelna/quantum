#pragma once

#define CONFIG_X86_32

typedef unsigned long size_t;
typedef long ptrdiff_t;
typedef long ssize_t;
typedef long off_t;

typedef unsigned long long uint64_t;
typedef long long int64_t;
typedef unsigned int uint32_t;
typedef int int32_t;
typedef unsigned short uint16_t;
typedef short int16_t;
typedef unsigned char uint8_t;
typedef char int8_t;
typedef unsigned short wchar_t;

struct register_state {
  uint32_t edi;
  uint32_t esi;
  uint32_t ebp;
  uint32_t esp; 
  uint32_t ebx;
  uint32_t edx;
  uint32_t ecx;
  uint32_t eax;
  uint32_t eflags;
  uint32_t eip;
};