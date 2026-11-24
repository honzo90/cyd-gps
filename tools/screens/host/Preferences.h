// Host stand-in: nothing is stored.
#pragma once
#include <stdint.h>
class Preferences {
public:
    bool begin(const char *, bool = false) { return false; }
    void end() {}
    uint8_t getUChar(const char *, uint8_t d = 0) { return d; }
    void putUChar(const char *, uint8_t) {}
    bool getBool(const char *, bool d = false) { return d; }
    void putBool(const char *, bool) {}
};
