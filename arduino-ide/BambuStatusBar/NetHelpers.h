#pragma once

#include <Arduino.h>

// Read an HTTP-style header value (case-insensitive key, first match only at
// the start of a line so substrings inside other names are ignored).
inline String httpHeaderValue(const String& text, const char* key) {
  if (!key || !key[0]) return "";
  String lower = text;
  lower.toLowerCase();
  String needle(key);
  needle.toLowerCase();
  needle += ":";
  int at = 0;
  while (at >= 0 && at < lower.length()) {
    int found = lower.indexOf(needle, at);
    if (found < 0) return "";
    if (found == 0 || lower[found - 1] == '\n') {
      int colon = found + needle.length() - 1;
      int end = text.indexOf('\n', colon);
      if (end < 0) end = text.length();
      String value = text.substring(colon + 1, end);
      value.trim();
      value.replace("\r", "");
      return value;
    }
    at = found + 1;
  }
  return "";
}
