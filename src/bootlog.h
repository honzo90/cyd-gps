// Why did the CYD restart? Each boot stores the reset reason (NVS "sys"), so a restart in the
// field (power bank, no laptop) can be read later: USB command B, or the web page.
#pragma once
#include <stddef.h>
#include <stdint.h>

#define BOOTLOG_N 8

void bootlog_begin();                        // once, early in setup()
void bootlog_print();                        // Serial: boot count and the last BOOTLOG_N reasons
void bootlog_text(char *buf, size_t n);      // short text for the web page
