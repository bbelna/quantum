/*
 * Copyright (c) 2022, Brandon Alex Belna
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *    * Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *    * Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *    * Neither the name of the University nor the names of its contributors
 *      may be used to endorse or promote products derived from this
 *      software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

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
/** @brief Standard string length
 *
 * This function computed the length of the given null terminated string
 * just like the strlen functions you are used to.
 *
 * @return 
 * - The length of the string
 * - 0 if str is a NULL pointer
 */
inline static size_t strlen(const char* str)
{
	size_t len = 0;
	size_t i, j;

	if (BUILTIN_EXPECT(!str, 0))
		return len;

#ifdef CONFIG_X86_32
	asm volatile("not %%ecx; cld; repne scasb; not %%ecx; dec %%ecx"
		: "=&c"(len), "=&D"(i), "=&a"(j)
		: "2"(0), "1"(str), "0"(len)
		: "memory","cc");
#elif defined(CONFIG_X86_64)
	asm volatile("not %%rcx; cld; repne scasb; not %%rcx; dec %%rcx"
		: "=&c"(len), "=&D"(i), "=&a"(j)
		: "2"(0), "1"(str), "0"(len)
		: "memory","cc");
#endif

	return len;
}
#endif

#ifdef HAVE_ARCH_STRNCPY
/** @brief Copy string with maximum of n byte length
 *
 * @param dest Destination string pointer
 * @param src Source string pointer
 * @param n maximum number of bytes to copy
 */
char* strncpy(char* dest, const char* src, size_t n);
#endif

#ifdef HAVE_ARCH_STRCPY
/** @brief Copy string
 *
 * Note that there is another safer variant of this function: strncpy.\n
 * That one could save you from accidents with buffer overruns.
 *
 * @param dest Destination string pointer
 * @param src Source string pointer
 */
char* strcpy(char* dest, const char* src);
#endif
