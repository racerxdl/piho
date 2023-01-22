#pragma once

#include <map>

#include "LittleFS.h"

#define MAX_TRIGGERS 8
#define NO_PIN -1

struct Triggers {
    int v[MAX_TRIGGERS];

    Triggers() {
        for (int i = 0; i < MAX_TRIGGERS; i++) {
            v[i] = NO_PIN;
        }
    }
    Triggers(int a[MAX_TRIGGERS]) {
        for (int i = 0; i < MAX_TRIGGERS; i++) {
            v[i] = a[i];
        }
    }
    Triggers(int a0, int a1, int a2, int a3, int a4, int a5, int a6, int a7) : v{a0, a1, a2, a3, a4, a5, a6, a7} {}
};

struct IOMap {
    // Input Number, Trigger Outputs
    std::map<uint8_t, Triggers> triggers;
};

struct DeviceMap {
    // inputDeviceId, Maps
    std::map<uint8_t, IOMap> ioMaps;
};

class Storage {
   private:
    // outputDeviceId, ioMap
    std::map<uint8_t, DeviceMap> deviceMaps;

   public:
    Storage();
    void begin();
    void Load();
    void Save();
    void AddMap(uint8_t inputDevice, uint8_t outputDevice, uint8_t pin, Triggers triggers);

    void GetTriggers(uint8_t inputDevice, uint8_t outputDevice, uint8_t pin, Triggers &triggers);
};

extern Storage config;