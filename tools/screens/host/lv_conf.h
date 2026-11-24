// Host build: the device's lv_conf.h with a larger heap (64-bit pointers make every LVGL object
// bigger, so 40 KB is not enough here). Memory use must still be checked on the device.
#include "../../../include/lv_conf.h"
#undef LV_MEM_SIZE
#define LV_MEM_SIZE (256U * 1024U)
