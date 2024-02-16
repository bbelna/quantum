# Constant part of the script
symbol-file bnix.elf
target remote localhost:1234

set architecture i386
break main
continue