#pragma once

#include <quantum/config.h>
#include <quantum/stddef.h>
#include <quantum/stdarg.h>

int kputs(const char*);
int kputchar(int);
int kprintf(const char*, ...);
int koutput_init(void);
int kvprintf(char const *fmt, void (*func) (int, void *), void *arg, int radix, va_list ap);
