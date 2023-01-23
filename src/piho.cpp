#include "piho.h"

#include "config.h"

#define PIO_NUM 0

#define CAN_MANDATORY 0x10000
#define CAN_DEV_ID_MASK 0x000FF
#define CAN_MSG_ID_MASK 0x0FF00
#define CAN_ID_ALL 0xFF

namespace {
// Sort of bad workaround. This limits PihoController instances to be 1
std::function<void(void)> irqCallback;
std::function<void(can2040 *, uint32_t, can2040_msg *)> canCallback;

extern "C" void irqWrapper() {
    irqCallback();
}
extern "C" void canWrapper(can2040 *bus, uint32_t n, can2040_msg *msg) {
    canCallback(bus, n, msg);
}
}  // namespace

PihoController piho0(CAN_RXD, CAN_TXD, CAN_BAUD);

PihoController::PihoController(uint32_t gpioRX, uint32_t gpioTX, uint32_t bitrate) : gpioRX(gpioRX), gpioTX(gpioTX), bitrate(bitrate) {
    queue_init(&rxMsgQueue, sizeof(can2040_msg), PIHO_QUEUE_MAX_ITEMS);
    queue_init(&txMsgQueue, sizeof(can2040_msg), PIHO_QUEUE_MAX_ITEMS);
    if (irqCallback != nullptr || canCallback != nullptr) {
        Serial.println("(ERR ) MULTIPLE PIHO INSTANCES");
        return;
    }
    canCallback = std::bind(&PihoController::CANHandler, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3);
    irqCallback = std::bind(&PihoController::IRQHandler, this);
}

void PihoController::IRQHandler() {
    can2040_pio_irq_handler(&this->bus);
}

void PihoController::Start() {
    // Setup canbus
    can2040_setup(&bus, PIO_NUM);
    can2040_callback_config(&bus, &canWrapper);

    // Enable irqs
    irq_set_exclusive_handler(PIO0_IRQ_0_IRQn, &irqWrapper);
    NVIC_SetPriority(PIO0_IRQ_0_IRQn, 1);
    NVIC_EnableIRQ(PIO0_IRQ_0_IRQn);

    // Start canbus
    can2040_start(&bus, F_CPU, bitrate, gpioRX, gpioTX);
}

void PihoController::Loop() {
    checkTX();
    checkRX();
}

void PihoController::checkTX() {
    if (!can2040_check_transmit(&bus)) {
        return;
    }
    if (!queue_try_remove(&txMsgQueue, &txMsg)) {
        return;
    }
    if (can2040_transmit(&bus, &txMsg) != -1) {
        return;
    }

    // Error in TX
    setError();
}

void PihoController::setError() {
    if (onError) {
        onError();
    }
}

void PihoController::checkRX() {
    char msgBuff[9];

    if (!queue_try_remove(&rxMsgQueue, &rxMsg)) {
        return;
    }

    uint8_t DevID = static_cast<uint8_t>(rxMsg.id & CAN_DEV_ID_MASK);
    MessageType MsgID = static_cast<MessageType>((rxMsg.id & CAN_MSG_ID_MASK) >> 8);
    if (DevID == this->myId || DevID == CAN_ID_ALL) {
        switch (MsgID) {
            case TXT:
                memset(msgBuff, 0x00, 9);
                memcpy(msgBuff, rxMsg.data, rxMsg.dlc > 8 ? 8 : rxMsg.dlc);
                Serial.printf("( ALL) %s\r\n", msgBuff);
                break;
            case HEALTH_CHECK:
                if (this->onHealthCheck) {
                    this->onHealthCheck();
                }
                break;
            case RESET:
                Serial.println("( ALL) Received RESET request.");
                watchdog_reboot(0, 0, 0);
                break;
            case IO_IN:
                Serial.printf("(GPIO)%d-0-%d\r\n", DevID, (rxMsg.data32[0] & 0x00FF) >> 0);
                Serial.printf("(GPIO)%d-1-%d\r\n", DevID, (rxMsg.data32[0] & 0xFF00) >> 8);
                break;
            case IO_OUT:
                if (this->onGPIOSet) {
                    this->onGPIOSet(rxMsg.data32[0]);
                }
                break;
            case SET_PIN:
                if (this->onSetPin) {
                    this->onSetPin(static_cast<uint16_t>(rxMsg.data32[0]), static_cast<uint8_t>(rxMsg.data32[1]));
                }
                break;
            case SET_BYTE:
                if (this->onSetByte) {
                    this->onSetByte(static_cast<uint16_t>(rxMsg.data32[0]), static_cast<uint8_t>(rxMsg.data32[1]));
                }
                break;
        }
    } else {
        // Serial.println("forward to uart");
        // Forward mode, packet not for us.
        if (MsgID == IO_IN) {
            // Report to UART
            uint32_t gpio = rxMsg.data32[0];
            Serial.printf("(GPIO)%d-0-%d\r\n", DevID, (gpio & 0x00FF) >> 0);
            Serial.printf("(GPIO)%d-1-%d\r\n", DevID, (gpio & 0xFF00) >> 8);
            return;
        }
        // Anymore packets?
    }
}

void PihoController::CANHandler(can2040 *bus, uint32_t notify, can2040_msg *msg) {
    // Keep it short as possible
    queue_try_add(&this->rxMsgQueue, msg);
}

void PihoController::ReportGPIOChange(uint32_t gpio) {
    txMsg.dlc = 4;
    txMsg.id = CAN2040_ID_EFF | CAN_MANDATORY | (static_cast<uint16_t>(IO_IN) << 8) | myId;
    txMsg.data32[0] = gpio;
    txMsg.data32[1] = 0;

    if (!queue_try_add(&txMsgQueue, &txMsg)) {
        setError();
    }
}

void PihoController::SetPin(uint16_t pin, uint8_t value) {
    txMsg.dlc = 4;
    txMsg.id = CAN2040_ID_EFF | CAN_MANDATORY | (static_cast<uint16_t>(SET_PIN) << 8) | CAN_ID_ALL;
    txMsg.data32[0] = pin;
    txMsg.data32[1] = value;

    if (!queue_try_add(&txMsgQueue, &txMsg)) {
        setError();
    }
}

void PihoController::SetByte(uint16_t byte, uint8_t value) {
    txMsg.dlc = 4;
    txMsg.id = CAN2040_ID_EFF | CAN_MANDATORY | (static_cast<uint16_t>(SET_BYTE) << 8) | CAN_ID_ALL;
    txMsg.data32[0] = byte;
    txMsg.data32[1] = value;

    if (!queue_try_add(&txMsgQueue, &txMsg)) {
        setError();
    }
}

void PihoController::SetGPIO(uint8_t devId, uint32_t gpio) {
    txMsg.dlc = 4;
    txMsg.id = CAN2040_ID_EFF | CAN_MANDATORY | (static_cast<uint16_t>(IO_OUT) << 8) | devId;
    txMsg.data32[0] = gpio;
    txMsg.data32[1] = 0;

    if (!queue_try_add(&txMsgQueue, &txMsg)) {
        setError();
    }
}

void PihoController::BroadcastHealthCheck() {
    txMsg.dlc = 4;
    txMsg.id = CAN2040_ID_EFF | CAN_MANDATORY | (static_cast<uint16_t>(HEALTH_CHECK) << 8) | CAN_ID_ALL;

    if (!queue_try_add(&txMsgQueue, &txMsg)) {
        setError();
    }
}