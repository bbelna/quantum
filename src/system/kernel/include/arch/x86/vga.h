#pragma once

#include <quantum/stddef.h>

void vga_init(void);
void vga_clear(void);
int vga_puts(const char* text);
int vga_putchar(unsigned char c);
void vga_cls();