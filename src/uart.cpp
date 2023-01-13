
#include <Arduino.h>

#include "pb.h"
#include "pb_common.h"
#include "pb_decode.h"
#include "shift.pb.h"
#include "piho.h"
#include "global.h"
#include "io.h"
#include "config.h"

CmdMsg message = CmdMsg_init_zero;
uint16_t numBytes = 0;
uint8_t buffer[MAX_UART_BYTES];
long timeSinceLastByte = millis();

#ifdef IS_INPUT_DEVICE
bool INPUT_DEVICE = true;
#else
bool INPUT_DEVICE = false;
#endif

void processPayload() {
    pb_istream_t stream = pb_istream_from_buffer(buffer, numBytes);
    if (!pb_decode(&stream, CmdMsg_fields, &message)) {
        Serial.print("( SER) Error parsing message");
        Serial.println(stream.errmsg);
        piho0.setError();
        return;
    }

    uint8_t pin, val, computedId, byteNum;

    switch (message.cmd) {
        case CmdMsg_Command_SetByte:
            byteNum = message.data.bytes[0];
            val = message.data.bytes[1];
            if (INPUT_DEVICE || byteNum >= byteNumEnd || byteNum < byteNumStart) {
                // Not on this device, forward to CAN
                piho0.SetByte(byteNum, val);
                return;
            }
            #ifndef IS_INPUT_DEVICE
            setGPIO(byteNum, val);
            Serial.printf("( OK) Byte %d state set to %08b\n", byteNum, val);
            #endif
            break;
        case CmdMsg_Command_SetPin:
            pin = message.data.bytes[0];
            val = message.data.bytes[1] ? LOW : HIGH;
            if (pin >= pinNumEnd || pin < pinNumStart) {
                // Not on this device, forward to CAN
                piho0.SetPin(pin, val);
                return;
            }
            #ifndef IS_INPUT_DEVICE
            setPin(pin, val);
            Serial.printf("( OK) Pin %d state set to %d\n", pin, val);
            #endif
        case CmdMsg_Command_HealthCheck:
            piho0.BroadcastHealthCheck();
            healthCheckAction();
            break;
        case CmdMsg_Command_Reset:
            Serial.println("( ALL) Reset OK");
            piho0.setError();
            // Broken, we cannot reset it since it re-initializes USB
            // ctrlMsg.dlc = 0;
            // ctrlMsg.id = CAN2040_ID_EFF | CAN_MANDATORY | (static_cast<uint16_t>(myId) << 8) | CAN_RESET_ID;
            // sendMessage(&ctrlMsg);
            // delay(500);
            // watchdog_reboot(0, 0, 0);
            break;
    }
}

void handleUART() {
    int n = Serial.available();
    if (n >= 2) {
        buffer[0] = Serial.read();
        buffer[1] = Serial.read();
        numBytes = *((uint16_t*)buffer);

        if (numBytes >= MAX_UART_BYTES) {
            Serial.print("( ERR) Wanted to receive ");
            Serial.print(numBytes);
            Serial.print("bytes. But max is MAX_UART_BYTES");
            piho0.setError();
            return;
        }
        timeSinceLastByte = millis();
        while (Serial.available() < numBytes) {
            if (millis() - timeSinceLastByte > 1000) {
                Serial.println("( ERR) TIMEOUT");
                piho0.setError();
                return;
            }
        }
        memset(buffer, 0, MAX_UART_BYTES);
        for (int i = 0; i < numBytes; i++) {
            buffer[i] = Serial.read();
        }

        processPayload();
    }
}
