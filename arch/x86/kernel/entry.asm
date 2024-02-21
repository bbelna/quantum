[BITS 32]
SECTION .mboot
global start
start:
  jmp stublet

; This part MUST be 4byte aligned, so we solve that issue using 'ALIGN 4'
ALIGN 4
mboot:
  ; Multiboot macros to make a few lines more readable later
  MULTIBOOT_PAGE_ALIGN    equ 1<<0
  MULTIBOOT_MEMORY_INFO   equ 1<<1
  MULTIBOOT_HEADER_MAGIC  equ 0x1BADB002
  MULTIBOOT_HEADER_FLAGS  equ MULTIBOOT_PAGE_ALIGN | MULTIBOOT_MEMORY_INFO
  MULTIBOOT_CHECKSUM      equ -(MULTIBOOT_HEADER_MAGIC + MULTIBOOT_HEADER_FLAGS)

  ; This is the GRUB Multiboot header. A boot signature
  dd MULTIBOOT_HEADER_MAGIC
  dd MULTIBOOT_HEADER_FLAGS
  dd MULTIBOOT_CHECKSUM
  dd 0, 0, 0, 0, 0 ; address fields

SECTION .text
ALIGN 4
stublet:
; initialize stack pointer.
  mov esp, default_stack_pointer
; initialize cpu features
  call cpu_init
; interpret multiboot information
  extern multiboot_init
  push ebx
  call multiboot_init
  add esp, 4

; jump to the boot processors's C code
  extern main
  call main
  jmp $

global cpu_init
cpu_init:
  mov eax, cr0
; enable caching, disable paging and fpu emulation
  and eax, 0x1ffffffb
; ...and turn on FPU exceptions
  or eax, 0x22
  mov cr0, eax
; clears the current pgd entry
  xor eax, eax
  mov cr3, eax
; at this stage, we disable the SSE support
  mov eax, cr4
  and eax, 0xfffbf9ff
  mov cr4, eax
  ret

; Here is the definition of our stack. Remember that a stack actually grows
; downwards, so we declare the size of the data before declaring
; the identifier 'default_stack_pointer'
SECTION .data
  resb 8192               ; This reserves 8KBytes of memory here
default_stack_pointer:

SECTION .note.GNU-stack noalloc noexec nowrite progbits