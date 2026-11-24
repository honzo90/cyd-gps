// Firmware version: the single source of truth. Bump it with tools/release.sh; the GitHub
// release workflow refuses a tag (v1.2.3) that does not match this value.
#pragma once
#ifndef FW_VERSION  // test builds may override it (env:cyd2usb_rollbacktest)
#define FW_VERSION "1.0.17"
#endif
