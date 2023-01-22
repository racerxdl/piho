#include "storage.h"

#include <vector>

Storage config;

#define NULL_TRIGGER -1

struct TriggerConfig {
    uint8_t outputId;
    int16_t inputId;
    uint8_t pin;

    Triggers trigger;

    TriggerConfig() : inputId(NULL_TRIGGER) {}
};

Storage::Storage() {
}

void Storage::begin() {
    LittleFS.begin();
    fs::FSInfo fsinfo;
    LittleFS.info(fsinfo);
    Serial.printf("( ALL) FS Total Bytes: %d\r\n", fsinfo.totalBytes);
    Serial.printf("( ALL) FS Used Bytes: %d\r\n", fsinfo.usedBytes);
    Serial.printf("( ALL) FS Block Size: %d\r\n", fsinfo.blockSize);
    Serial.printf("( ALL) FS Page Size: %d\r\n", fsinfo.pageSize);

    Load();
}

void Storage::AddMap(uint8_t inputDevice, uint8_t outputDevice, uint8_t pin, Triggers triggers) {
    if (deviceMaps.count(inputDevice) == 0) {
        deviceMaps[inputDevice] = DeviceMap();
    }
    auto &map = deviceMaps[outputDevice];
    if (map.ioMaps.count(outputDevice) == 0) {
        map.ioMaps[outputDevice] = IOMap();
    }
    auto &io = map.ioMaps[outputDevice];
    if (io.triggers.count(pin) == 0) {
        io.triggers[pin] = Triggers();
    }
    auto &trigger = io.triggers[pin];
    for (int i = 0; i < MAX_TRIGGERS; i++) {
        trigger.v[i] = triggers.v[i];
    }
    Serial.printf("( ALL) Added trigger from IDEV=%d to ODEV=%d => ", inputDevice, outputDevice);
    Serial.printf("Pin %d -> ", pin);
    for (int i = 0; i < MAX_TRIGGERS; i++) {
        if (triggers.v[i] != NO_PIN) {
            Serial.printf("%d ", triggers.v[i]);
        }
    }
    Serial.printf("\r\n");
}

void Storage::Load() {
    printf("( ALL) Loading configuration from triggers.bin");
    File f = LittleFS.open("/triggers.bin", "r");
    if (!f) {
        Serial.println("( ALL) No config file to load.");
        Serial.println("( ALL) Creating default config.");
        Save();
        return;
    }

    TriggerConfig cfg;
    int readBytes = f.read((uint8_t *)&cfg, sizeof(TriggerConfig));
    int cfgRead = 0;
    while (cfg.inputId != NULL_TRIGGER && readBytes > 0) {
        AddMap((uint8_t)cfg.inputId, cfg.outputId, cfg.pin, cfg.trigger);
        readBytes = f.read((uint8_t *)&cfg, sizeof(TriggerConfig));
        cfgRead++;
    }
    Serial.printf("( ALL) Loaded %d pin config.\r\n", cfgRead);

    f.close();
}

void Storage::Save() {
    File f = LittleFS.open("/triggers.bin", "w");
    if (!f) {
        Serial.println("( ERR) Cannot write file cfg.bin");
        return;
    }

    std::vector<TriggerConfig> triggers;

    for (auto const &map : deviceMaps) {
        auto outputDeviceId = map.first;
        for (auto const &io : map.second.ioMaps) {
            auto inputDeviceId = io.first;
            for (auto const &trig : io.second.triggers) {
                auto t = TriggerConfig{};

                t.inputId = inputDeviceId;
                t.outputId = outputDeviceId;
                for (int i = 0; i < MAX_TRIGGERS; i++) {
                    t.trigger.v[i] = trig.second.v[i];
                }
                t.pin = trig.first;
                triggers.push_back(t);
            }
        }
    }

    triggers.push_back(TriggerConfig());

    for (auto const &t: triggers) {
        f.write((uint8_t *)&t, sizeof(TriggerConfig));
    }

    Serial.printf("( ALL) Saved %d triggers\r\n", triggers.size()-1);

    f.close();
    Serial.println("( ALL) Configuration saved!");
}

void Storage::GetTriggers(uint8_t inputDevice, uint8_t outputDevice, uint8_t pin, Triggers &triggers) {
    for (int i = 0; i < MAX_TRIGGERS; i++) {
        triggers.v[i] = NO_PIN;
    }

    if (deviceMaps.count(inputDevice) == 0) {
        return;
    }
    auto p = deviceMaps[inputDevice];

    if (p.ioMaps.count(outputDevice) == 0) {
        return;
    }

    auto m = p.ioMaps[outputDevice];
    if (m.triggers.count(pin) == 0) {
        return;
    }
    auto t = m.triggers[pin];
    for (int i = 0; i < MAX_TRIGGERS; i++) {
        triggers.v[i] = t.v[i];
    }
}