#include "io.h"

#include <Arduino.h>

#include "config.h"
#include "piho/debouncer.h"

#ifdef IS_INPUT_DEVICE
namespace {

piho::InputDebouncer debouncer(DEBOUNCE_TIME_MS);
bool initialStateReported = false;
uint16_t currentState = 0;

uint16_t readLogicalInputs() {
    uint16_t sample = static_cast<uint16_t>((gpio_get_all() & DATA_MASK) >> DATA_SHIFT);
    if (INPUT_ACTIVE_LOW) {
        sample = static_cast<uint16_t>(~sample);
    }
    return sample;
}

}  // namespace

void initializeInputs() {
    for (uint8_t pin = 0; pin < piho::kPinsPerDevice; ++pin) {
        pinMode(static_cast<uint8_t>(DATA_SHIFT + pin), INPUT_PULLUP);
    }
    debouncer.reset();
    currentState = 0;
    initialStateReported = false;
}

bool sampleInputs(uint32_t nowMilliseconds, uint16_t &state) {
    const piho::DebounceUpdate update = debouncer.update(readLogicalInputs(), nowMilliseconds);
    currentState = update.state;
    state = currentState;
    if (!initialStateReported) {
        initialStateReported = true;
        return true;
    }
    return update.changed != 0;
}

uint16_t inputState() {
    return currentState;
}

#endif