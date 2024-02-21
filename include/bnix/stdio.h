#pragma once

#include <bnix/config.h>
#include <bnix/stddef.h>
#include <bnix/stdarg.h>

int kputs(const char*);
int kputchar(int);
int kprintf(const char*, ...);
int koutput_init(void);
int kvprintf(char const *fmt, void (*func) (int, void *), void *arg, int radix, va_list ap);
