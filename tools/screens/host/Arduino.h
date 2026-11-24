// Host stand-in for Arduino.h: just what src/main.cpp uses (tools/screens renders the UI on a Mac).
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <string>
#define LOW 0
#define HIGH 1
#define INPUT_PULLUP 2
uint32_t millis();
inline void delay(uint32_t) {}
inline void pinMode(int, int) {}
inline int digitalRead(int) { return HIGH; }
class String {
    std::string s_;
public:
    String(const char *c = "") : s_(c) {}
    void trim() {}
    bool isEmpty() const { return s_.empty(); }
    const char *c_str() const { return s_.c_str(); }
};
struct HostSerial {
    void begin(int) {}
    int available() { return 0; }
    int read() { return -1; }
    String readStringUntil(char) { return String(); }
    void print(const char *) {}
    void println(const char * = "") {}
    int printf(const char *, ...) { return 0; }
};
extern HostSerial Serial;
struct HostEsp {
    uint32_t getFreeHeap() { return 100000; }
    uint32_t getMinFreeHeap() { return 90000; }
    uint32_t getMaxAllocHeap() { return 60000; }
};
extern HostEsp ESP;
typedef void (*TaskFunction_t)(void *);
inline int xTaskCreatePinnedToCore(TaskFunction_t, const char *, int, void *, int, void *, int) { return 0; }
inline void vTaskDelete(void *) {}
