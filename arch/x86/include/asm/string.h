#pragma once

#include <bnix/stddef.h>

#ifdef HAVE_ARCH_MEMCPY
inline static void* memcpy(void* dest, const void* src, size_t count) {
  size_t i, j, k;
  if (BUILTIN_EXPECT(!dest || !src, 0)) return dest;
#ifdef CONFIG_X86_32
  asm volatile(
    "cld; rep movsl\n\t"
    "movl %4, %%ecx\n\t" 
    "andl $3, %%ecx\n\t"
    "rep movsb\n\t" 
    : "=&c"(i), "=&D"(j), "=&S"(k) 
    : "0"(count/4), "g"(count), "1"(dest), "2"(src) : "memory","cc"
  );
#elif defined(CONFIG_X86_64)
  asm volatile(
    "cld; rep movsq\n\t"
    "movq %4, %%rcx\n\t"
    "andq $7, %%rcx\n\t"
    "rep movsb\n\t"
    : "=&c"(i), "=&D"(j), "=&S"(k)
    : "0"(count/8), "g"(count), "1"(dest), "2"(src) : "memory","cc"
  );
#endif
  return dest;
}
#endif

#ifdef HAVE_ARCH_MEMSET
inline static void* memset(void* dest, int val, size_t count) {
  size_t i, j;
  if (BUILTIN_EXPECT(!dest, 0)) return dest;
  asm volatile(
    "cld; rep stosb"
    : "=&c"(i), "=&D"(j)
    : "a"(val), "1"(dest), "0"(count) : "memory","cc"
  );
  return dest;
}
#endif

#ifdef HAVE_ARCH_STRLEN
inline static size_t strlen(const char* str) {
  size_t len = 0;
  size_t i, j;
  if (BUILTIN_EXPECT(!str, 0)) return len;
#ifdef CONFIG_X86_32
  asm volatile(
    "not %%ecx; cld; repne scasb; not %%ecx; dec %%ecx"
    : "=&c"(len), "=&D"(i), "=&a"(j)
    : "2"(0), "1"(str), "0"(len)
    : "memory","cc"
  );
#elif defined(CONFIG_X86_64)
  asm volatile(
    "not %%rcx; cld; repne scasb; not %%rcx; dec %%rcx"
    : "=&c"(len), "=&D"(i), "=&a"(j)
    : "2"(0), "1"(str), "0"(len)
    : "memory","cc"
  );
#endif
  return len;
}
#endif

#ifdef HAVE_ARCH_STRNCPY
char* strncpy(char* dest, const char* src, size_t n);
#endif

#ifdef HAVE_ARCH_STRCPY
char* strcpy(char* dest, const char* src);
#endif
