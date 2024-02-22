#pragma once

#include <quantum/config.h>
#if CONFIG_X86_32
#include <arch/x86/stddef.h>
#endif

#define VIDEO_MEM_ADDR      0xB8000
#define NORETURN          __attribute__((noreturn))
#define STDCALL           __attribute__((stdcall))
#define NULL              ((void*) 0)
#define per_core(name)    name
#define CORE_ID           0

#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define BYTE_ORDER BIG_ENDIAN
#else
#define BYTE_ORDER LITTLE_ENDIAN
#endif

#ifdef __GNUC__
#define BUILTIN_EXPECT(exp, b)  __builtin_expect((exp), (b))
#else
#define BUILTIN_EXPECT(exp, b)  (exp)
#endif
