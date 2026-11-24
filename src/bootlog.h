// Why did the CYD restart? Each boot stores the reset reason (NVS "sys"), so a restart in the
// field (power bank, no laptop) can be read later: USB command B, or the web page.
// Also kept: how long the previous run lasted (RTC memory survives every reset except a power
// loss), and for a task-watchdog reset which tasks were running on the two cores at that moment.
#pragma once
#include <stddef.h>
#include <stdint.h>

#define BOOTLOG_N 8

void bootlog_begin();                        // once, early in setup()
void bootlog_tick();                         // from loop(): remembers the uptime for the next boot
void bootlog_print();                        // Serial: boot count and the last BOOTLOG_N reasons
void bootlog_text(char *buf, size_t n);      // short text for the web page
