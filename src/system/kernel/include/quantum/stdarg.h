#pragma once

typedef __builtin_va_list va_list;

#define va_start        __builtin_va_start
#define va_arg          __builtin_va_arg
#define va_end          __builtin_va_end
#define va_copy         __builtin_va_copy