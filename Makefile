# Belna Operating System Makefile

TOPDIR = $(shell pwd)
ARCH = x86
NAME = bnix
ISO_NAME = BelnaOS
KERNDIRS = libkernel kernel arch/$(ARCH)/kernel
SUBDIRS = $(KERNDIRS)

CC_FOR_TARGET = cc
CXX_FOR_TARGET = c++
GCC_FOR_TARGET = gcc
CPP_FOR_TARGET = cpp
AR_FOR_TARGET = ar
AS_FOR_TARGET = as
LD_FOR_TARGET = ld
NM_FOR_TARGET = nm
OBJDUMP_FOR_TARGET = objdump
OBJCOPY_FOR_TARGET = objcopy
RANLIB_FOR_TARGET = ranlib
STRIP_FOR_TARGET = strip
READELF_FOR_TARGET = readelf
NASM = nasm

NASMFLAGS = -felf32 -g
INCLUDE = -I$(TOPDIR)/include -I$(TOPDIR)/arch/$(ARCH)/include
# Compiler options for final code
CFLAGS = -g -m32 -march=i586 -Wall -O2 -fno-builtin -fstrength-reduce -fomit-frame-pointer -finline-functions -nostdinc $(INCLUDE) -fno-stack-protector
# Compiler options for debugging
#CFLAGS = -g -O -m32 -march=i586 -Wall -fno-builtin -DWITH_FRAME_POINTER -nostdinc $(INCLUDE) -fno-stack-protector
AR = ar
ARFLAGS = rsv
RM = rm -rf
LDFLAGS = -T link.ld -z max-page-size=4096 --defsym __BUILD_DATE=$(shell date +'%Y%m%d') --defsym __BUILD_TIME=$(shell date +'%H%M%S')
STRIP_DEBUG = --strip-debug
KEEP_DEBUG = --only-keep-debug

# Prettify output
V = 0
ifeq ($V,0)
  Q = @
  P = > /dev/null
endif

default: all

all: $(NAME).elf

kernel:
  $Q$(LD_FOR_TARGET) $(LDFLAGS) -o $(NAME).elf $^
  @echo [OBJCOPY] $(NAME).sym
  $Q$(OBJCOPY_FOR_TARGET) $(KEEP_DEBUG) $(NAME).elf $(NAME).sym
  @echo [OBJCOPY] $(NAME).elf
  $Q$(OBJCOPY_FOR_TARGET) $(STRIP_DEBUG) $(NAME).elf

clean:
  $Q$(RM) $(NAME).elf $(NAME).sym *~
  @echo Cleaned.

veryclean: clean
  $Q$(RM) qemu-vlan0.pcap
  @echo Very cleaned

qemu: $(NAME).elf
  qemu-system-i386 -monitor stdio -smp 2 -net nic,model=rtl8139 -net user,hostfwd=tcp::12345-:7 -kernel $(NAME).elf

qemu-dbg: $(NAME).elf
  qemu-system-i386 -monitor stdio -s -S -smp 2 -net nic,model=rtl8139 -net user,hostfwd=tcp::12345-:7 -kernel $(NAME).elf

doc:
  @doxygen
  @echo Create documentation...

test:
  @echo "Nothing to test"

%.o : %.c
  @echo [CC] $@
  $Q$(CC_FOR_TARGET) -c -D__KERNEL__ $(CFLAGS) -o $@ $<
  @echo [DEP] $*.dep
  $Q$(CC_FOR_TARGET) -MF $*.dep -MT $*.o -MM $(CFLAGS) $<

%.o : %.asm
  @echo [ASM] $@
  $Q$(NASM) $(NASMFLAGS) -o $@ $<

%.o : %.s
  @echo [ASM] $@
  $Q$(NASM) $(NASMFLAGS) -o $@ $<

.PHONY: default all clean emu gdb newlib tools test
 
include $(addsuffix /Makefile,$(SUBDIRS))
