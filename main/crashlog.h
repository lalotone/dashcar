#pragma once

#include <stddef.h>

// Crash records that survive the restart (and, once saved at boot, a power-off): the faulting address,
// the call chain and the task, captured by a wrapper around ESP-IDF's panic handler. The serial port is
// often not connected in the car, so this is how a crash gets diagnosed: http://dashcar.local/ shows the
// last one; decode with xtensa-esp32s3-elf-addr2line -pfiaC -e build/dashcar.elf <addresses>.

void crashlog_init(void);                    // at boot, after settings_init()
void crashlog_text(char *buf, size_t len);   // "none" or "uptime 123 s, task lvgl, PCs 0x... 0x..."
