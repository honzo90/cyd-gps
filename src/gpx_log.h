// GPX track on the SD card (CYD slot on VSPI: SS 5, SCK 18, MISO 19, MOSI 23).
// One file per power-up and per trip reset: /gpx/YYYYMMDD-HHMMSS.gpx (UTC of the first point).
// The file is a valid GPX after every flush (closing tags rewritten each time): a power cut
// loses at most the last few seconds.
#pragma once
#include <stddef.h>
#include "gps.h"

void gpx_log_begin();                    // mount the card (no card = logging off, retried on new track)
void gpx_log_feed(const gps_data_t *g);  // call for every new fix
void gpx_log_poll();                     // call from loop(): time-based flush
void gpx_log_new_track();                // close the current file; the next good fix opens a new one
void gpx_log_status(char *buf, size_t n);  // one line for the UI
// Name ("20261123-114602.gpx") of the file being written, "" if none; returns the point count.
uint32_t gpx_log_current(char *name, size_t n);

// Track files in /gpx (names like "20261123-114602.gpx"; anything else is never touched).
bool gpx_log_valid_name(const char *name);
void gpx_log_print_files();  // list with sizes on Serial
// Deletes /gpx/<name>. Refuses invalid names and the file being written. Call from the loop task.
// Returns "ok" or a short reason.
const char *gpx_log_delete(const char *name);
