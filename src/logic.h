#pragma once
#include <stdio.h>
#include <string.h>
#include <time.h>

// UTF-8 Hungarian -> ASCII (built-in TFT fonts are ASCII only). Cuts at "►".
inline void asciify(const char* in, char* out, size_t n) {
  static const char* from[] = {"á", "é", "í", "ó", "ö", "ő", "ú", "ü", "ű",
                               "Á", "É", "Í", "Ó", "Ö", "Ő", "Ú", "Ü", "Ű"};
  static const char to[] = "aeiooouuuAEIOOOUUU";
  size_t o = 0;
  while (*in && o + 1 < n) {
    if ((unsigned char)*in < 0x80) { out[o++] = *in++; continue; }
    if (!strncmp(in, "►", 3)) break;
    char c = '?';
    for (int i = 0; i < 18; i++)
      if (!strncmp(in, from[i], 2)) { c = to[i]; break; }
    out[o++] = c;
    in++;
    while ((*in & 0xC0) == 0x80) in++;  // skip continuation bytes
  }
  while (o && out[o - 1] == ' ') o--;
  out[o] = 0;
}

// Remove HTML tags, decode the nbsp and amp entities, merge whitespace.
inline void stripHtml(const char* in, char* out, size_t n) {
  size_t o = 0;
  bool tag = false;
  while (*in && o + 1 < n) {
    char c = *in++;
    if (c == '<') {  // paragraph/line breaks become a space, inline tags vanish
      tag = true;
      bool brk = !strncmp(in, "p", 1) || !strncmp(in, "/p", 2) || !strncmp(in, "br", 2) || !strncmp(in, "li", 2);
      if (!brk) continue;
      c = ' ';
    } else if (tag) { if (c == '>') tag = false; continue; }
    else if (c == '&' && !strncmp(in, "nbsp;", 5)) { in += 5; c = ' '; }
    else if (c == '&' && !strncmp(in, "amp;", 4)) { in += 4; }
    if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    if (c == ' ' && (o == 0 || out[o - 1] == ' ')) continue;
    out[o++] = c;
  }
  while (o && out[o - 1] == ' ') o--;
  out[o] = 0;
}

// "most" (now) under 1 min, "N'" under 30 min, else local HH:MM.
inline void minutesLabel(time_t dep, time_t now, char* out, size_t n) {
  long s = (long)(dep - now);
  if (s < 60) { snprintf(out, n, "most"); return; }
  if (s < 30 * 60) { snprintf(out, n, "%ld'", s / 60); return; }
  struct tm lt;
  localtime_r(&dep, &lt);
  strftime(out, n, "%H:%M", &lt);
}
