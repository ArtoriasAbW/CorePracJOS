#include <inc/lib.h>

int bol  = 1;
int line = 0;

void
num(int f, const char *s) {
  long n;
  int r;
  char c;

  while ((n = read(f, &c, 1)) > 0) {
    if (bol) {
      printf("%5d ", ++line);
      bol = 0;
    }
    if ((r = write(1, &c, 1)) != 1)
      panic("write error copying %s: %i", s, r);
    if (c == '\n')
      bol = 1;
  }
  if (n < 0)
    panic("error reading %s: %i", s, (int)n);
}

void
umain(int argc, char **argv) {
  int f, i;

  binaryname = "num";
  if (argc == 1)
    num(0, "<stdin>");
  else
    for (i = 1; i < argc; i++) {
      f = open(argv[i], O_RDONLY);
      if (f < 0)
        panic("can't open %s: %i", argv[i], f);
      else {
        num(f, argv[i]);
        close(f);
      }
    }
  exit();
}
