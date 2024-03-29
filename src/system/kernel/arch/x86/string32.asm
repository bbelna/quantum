[BITS 32]
SECTION .text

global strcpy
strcpy:
  push ebp
  mov ebp, esp
  push edi
  push esi

  mov esi, [ebp+12]
  mov edi, [ebp+8]

L1:
  lodsb
  stosb
  test al, al
  jne L1
  
  mov eax, [ebp+8]
  pop esi
  pop edi
  pop ebp
  ret

global strncpy
strncpy:
  push ebp
  mov ebp, esp
  push edi
  push esi

  mov ecx, [ebp+16]
  mov esi, [ebp+12]
  mov edi, [ebp+8]

L2:
  dec ecx
  js L3
  lodsb
  stosb
  test al, al
  jne L1
  rep
  stosb

L3:
  mov eax, [ebp+8]
  pop esi
  pop edi
  pop ebp
  ret

SECTION .note.GNU-stack noalloc noexec nowrite progbits