#include <quantum/stddef.h>
#include <arch/x86/multiboot.h>

multiboot_info_t* mb_info __attribute__((section(".data"))) = NULL;

void multiboot_init(void* mb) {
  mb_info = (multiboot_info_t*)mb;
}