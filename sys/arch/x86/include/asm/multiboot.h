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