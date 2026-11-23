// Odometer on top of lib/odo_core: feeds it GPS fixes and keeps the values in NVS ("odo").
#pragma once
#include "gps.h"
#include "odo_core.h"

void odometer_begin();                     // load saved trip/total
void odometer_feed(const gps_data_t *g);   // call for every new fix (g->fix_seq changed)
void odometer_reset_trip();                // saves immediately
void odometer_reset_all();                 // trip and total; saves immediately
void odometer_save();                      // e.g. before a restart
const odo_state_t *odometer_get();
