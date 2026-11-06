// c++ -std=c++17 -I src test/check.cpp -o test/check && test/check
#include <assert.h>
#include <stdlib.h>
#include "logic.h"

int main() {
  char b[32];
  asciify("Kőbánya tér ► Újváros", b, sizeof b);
  assert(!strcmp(b, "Kobanya ter"));
  asciify("Hűvös Ödön út M", b, sizeof b);
  assert(!strcmp(b, "Huvos Odon ut M"));
  asciify("Őrült űr", b, 6);
  assert(!strcmp(b, "Orult"));
  char h[64];
  stripHtml("<p>A <strong>42</strong>-es&nbsp;busz\n &amp; <em>X</em></p><p>Y</p>", h, sizeof h);
  assert(!strcmp(h, "A 42-es busz & X Y"));
  setenv("TZ", "UTC", 1);
  tzset();
  minutesLabel(1030, 1000, b, sizeof b); assert(!strcmp(b, "most"));
  minutesLabel(1000 + 60 * 7 + 59, 1000, b, sizeof b); assert(!strcmp(b, "7'"));
  minutesLabel(3600 * 13 + 60 * 5, 3600 * 12, b, sizeof b); assert(!strcmp(b, "13:05"));
  puts("ok");
}
