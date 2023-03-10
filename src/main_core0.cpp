#include <Arduino.h>
#include "piho.h"
#include "config.h"
#include "io.h"
#include "global.h"
#include "uart.h"
#include "storage.h"


void initGPIO() {
    pinMode(ADDR_BIT_0, INPUT_PULLUP);
    pinMode(ADDR_BIT_1, INPUT_PULLUP);
    pinMode(ADDR_BIT_2, INPUT_PULLUP);
    pinMode(ADDR_BIT_3, INPUT_PULLUP);
    pinMode(ADDR_BIT_4, INPUT_PULLUP);
    initPins();

    pinMode(LED_HC, OUTPUT);   // LED
    pinMode(LED_ACT, OUTPUT);  // LED
    pinMode(LED_ERR, OUTPUT);  // LED
    digitalWrite(LED_HC, LOW);
    digitalWrite(LED_ERR, LOW);
    digitalWrite(LED_ACT, HIGH);

    #ifdef IS_INPUT_DEVICE
        initInputs();
    #else
        initOutput();
    #endif
}


void onGPIOSet(uint32_t gpio) {
    #ifndef IS_INPUT_DEVICE
    setGPIOValue(gpio);
    #endif
}

void setup() {
    Serial.begin(115200);
    delay(5000);

    #ifdef IS_INPUT_DEVICE
    Serial.println("( ALL) Device is INPUT");
    #else
    Serial.println("( ALL) Device is OUTPUT");
    #endif

    Serial.printf("( ALL) Device ID: %08x\r\n", getAddr());
    initGPIO();
    piho0.setOnHealthCheck(healthCheckAction);
    piho0.setOnGPIOSet(onGPIOSet);
    piho0.SetID(getAddr());

    #ifndef IS_INPUT_DEVICE
    piho0.setOnPinSet(setPin);
    piho0.setOnByteSet(setGPIO);
    #endif

    piho0.Start();
    Serial.println("( ALL) Initializing FS");
    config.begin();
    Serial.println("( ALL) Started...");

    // config.AddMap(0, 0, 0, Triggers{1,4,5,NO_PIN,NO_PIN,NO_PIN,NO_PIN,NO_PIN});
    // config.AddMap(0, 0, 1, Triggers{4,3,2,NO_PIN,NO_PIN,NO_PIN,NO_PIN,NO_PIN});
    // config.AddMap(1, 0, 2, Triggers{7,8,9,NO_PIN,NO_PIN,NO_PIN,NO_PIN,NO_PIN});
    // config.AddMap(0, 5, 0, Triggers{4,3,2,NO_PIN,NO_PIN,NO_PIN,NO_PIN,NO_PIN});
    // config.Save();
}

void loop() {
    // This core should only deal with IRQ from USB, CAN and UART
    // It should also check if there is a message to send.
    piho0.Loop();
    handleUART();
    checkTimeouts();
}