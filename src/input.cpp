#include "io.h"

#include <Arduino.h>

#include "config.h"
#include "piho/debouncer.h"

#ifdef IS_INPUT_DEVICE
namespace {

piho::InputDebouncer debouncer;
uint16_t currentState = 0;

uint16_t readLogicalInputs() {
    uint16_t sample =
        static_cast<uint16_t>((gpio_get_all() & DATA_MASK) >> DATA_SHIFT);
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
}

uint16_t readInputPins() {
    return readLogicalInputs();
}

void configureInputDebounce(const piho::LocalInputGraph &graph,
                            uint16_t baseline,
                            uint32_t nowMilliseconds) {
    for (uint8_t pin = 0; pin < piho::kPinsPerDevice; ++pin) {
        debouncer.setDebounceMilliseconds(pin, 0);
    }
    for (uint16_t index = 0; index < graph.inputCount; ++index) {
        debouncer.setDebounceMilliseconds(graph.inputs[index].pin,
                                          graph.inputs[index].debounceMs);
    }
    debouncer.reset(baseline, nowMilliseconds);
    currentState = baseline;
}

bool sampleInputs(uint32_t nowMilliseconds, piho::DebounceUpdate &update) {
    update = debouncer.update(readLogicalInputs(), nowMilliseconds);
    currentState = update.state;
    return update.initialized || update.changed != 0;
}

uint16_t inputState() {
    return currentState;
}

#endif