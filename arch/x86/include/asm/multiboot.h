#pragma once

#include <quantum/stddef.h>

typedef uint16_t multiboot_uint16_t;
typedef uint32_t multiboot_uint32_t;
typedef uint64_t multiboot_uint64_t;

struct multiboot_aout_symbol_table {
  multiboot_uint32_t tabsize;
  multiboot_uint32_t strsize;
  multiboot_uint32_t addr;
  multiboot_uint32_t reserved;
};
typedef struct multiboot_aout_symbol_table multiboot_aout_symbol_table_t;

struct multiboot_elf_section_header_table {
  multiboot_uint32_t num;
  multiboot_uint32_t size;
  multiboot_uint32_t addr;
  multiboot_uint32_t shndx;
};
typedef struct multiboot_elf_section_header_table multiboot_elf_section_header_table_t;

struct multiboot_info {
  multiboot_uint32_t flags;
  multiboot_uint32_t mem_lower;
  multiboot_uint32_t mem_upper;
  multiboot_uint32_t boot_device;
  multiboot_uint32_t cmdline;
  multiboot_uint32_t mods_count;
  multiboot_uint32_t mods_addr;
  multiboot_uint32_t mmap_length;
  multiboot_uint32_t mmap_addr;
  multiboot_uint32_t drives_length;
  multiboot_uint32_t drives_addr;
  multiboot_uint32_t config_table;
  multiboot_uint32_t boot_loader_name;
  multiboot_uint32_t apm_table;
  multiboot_uint32_t vbe_control_info;
  multiboot_uint32_t vbe_mode_info;
  multiboot_uint16_t vbe_mode;
  multiboot_uint16_t vbe_interface_seg;
  multiboot_uint16_t vbe_interface_off;
  multiboot_uint16_t vbe_interface_len;
  union {
    multiboot_aout_symbol_table_t aout_sym;
    multiboot_elf_section_header_table_t elf_sec;
  } u;
};

typedef struct multiboot_info multiboot_info_t;

struct multiboot_mmap_entry {
  multiboot_uint32_t size;
  multiboot_uint64_t addr;
  multiboot_uint64_t len;
#define MULTIBOOT_MEMORY_AVAILABLE 1
#define MULTIBOOT_MEMORY_RESERVED 2
  multiboot_uint32_t type;
} __attribute__((packed));
typedef struct multiboot_mmap_entry multiboot_memory_map_t;

struct multiboot_mod_list {
  multiboot_uint32_t mod_start;
  multiboot_uint32_t mod_end;
  multiboot_uint32_t cmdline;
  multiboot_uint32_t pad;
};
typedef struct multiboot_mod_list multiboot_module_t;

extern multiboot_info_t* mb_info;