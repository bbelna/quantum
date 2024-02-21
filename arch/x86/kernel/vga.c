#include <quantum/string.h>
#include <asm/io.h>
#include <asm/vga.h>
#include <asm/stddef.h>

static uint16_t* textmemptr;
static int attrib = 0x0F;
static int csr_x = 0, csr_y = 0;

inline static uint16_t* memsetw(uint16_t* dest, uint16_t val, size_t count) {
  size_t i;
  if (BUILTIN_EXPECT(!dest, 0)) return dest;
  for (i = 0; i < count; i++) dest[i] = val;
  return dest;
}

static void scroll(void) {
  unsigned blank, temp;
  blank = 0x20 | (attrib << 8);
  if (csr_y >= 25) {
    temp = csr_y - 25 + 1;
    memcpy(
      textmemptr, textmemptr + temp * 80,
      (25 - temp) * 80 * 2
    );
    memsetw(textmemptr + (25 - temp) * 80, blank, 80);
    csr_y = 25 - 1;
  }
}

static void move_csr(void) {
  unsigned temp;
  temp = csr_y * 80 + csr_x;
  outportb(0x3D4, 14);
  outportb(0x3D5, temp >> 8);
  outportb(0x3D4, 15);
  outportb(0x3D5, temp);
}

void vga_clear(void) {
  unsigned blank;
  int i;
  blank = 0x20 | (attrib << 8);
  for (i = 0; i < 25; i++)
    memsetw(textmemptr + i * 80, blank, 80);
  csr_x = 0;
  csr_y = 0;
  move_csr();
}

int vga_putchar(unsigned char c) {
  unsigned short *where;
  unsigned att = attrib << 8;
  if (c == 0x08) {
    if (csr_x != 0) csr_x--;
  } else if (c == 0x09) {
    csr_x = (csr_x + 8) & ~(8 - 1);
  } else if (c == '\r') {
    csr_x = 0;
  } else if (c == '\n') {
    csr_x = 0;
    csr_y++;
  } else if (c >= ' ') {
    where = textmemptr + (csr_y * 80 + csr_x);
    *where = c | att;
    csr_x++;
  }

  if (csr_x >= 80) {
    csr_x = 0;
    csr_y++;
  }

  scroll();
  move_csr();

  return (int)c;
}

int vga_puts(const char *text) {
  size_t i;
  for (i = 0; i < strlen(text); i++) vga_putchar(text[i]);
  return --i;
}

void vga_init(void) {
  textmemptr = (unsigned short *)VIDEO_MEM_ADDR;
  vga_clear();
}
