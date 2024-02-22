#include <quantum/stddef.h>
#include <quantum/stdio.h>
#include <quantum/string.h>
#include <quantum/cpu.h>

extern const void kernel_start;
extern const void kernel_end;
extern const void bss_start;
extern const void bss_end;
extern char __BUILD_DATE;
extern char __BUILD_TIME;

static int init(void) {
  memset((void*)&bss_start, 0x00, ((size_t)&bss_end - (size_t)&bss_start));
  koutput_init();
}

int main(void) {
  init();
  kprintf("\nHello World!\n");

  // main kernel loop
  while(1) { HALT; }

  return 0;
}