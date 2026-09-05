#include "piho/graph_image.h"

#include <cstring>
#include <limits>

#include "piho/crc32.h"

namespace piho {
namespace {

constexpr uint8_t kMagic[] = {'P', 'H', 'G', 'F'};
constexpr std::size_t kChecksumChunkSize = 64;
constexpr std::size_t kActionHashSlotCount = 1024;

bool readMemory(const void *context, std::size_t offset, uint8_t *output, std::size_t length) {
    if (length != 0 && (context == nullptr || output == nullptr)) {
        return false;
    }
    if (length != 0) {
        std::memcpy(output, static_cast<const uint8_t *>(context) + offset, length);
    }
    return true;
}

uint16_t decodeUint16(const uint8_t *input) {
    return static_cast<uint16_t>(input[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(input[1]) << 8);
}

uint32_t decodeUint32(const uint8_t *input) {
    uint32_t value = 0;
    for (uint8_t byte = 0; byte < 4; ++byte) {
        value |= static_cast<uint32_t>(input[byte]) << (byte * 8);
    }
    return value;
}

bool readDeviceRecord(const GraphImageSource &source, std::size_t offset, GraphDeviceRecord &record,
                      uint16_t &reserved) {
    uint8_t encoded[kGraphDeviceRecordSize]{};
    if (!source.readAt(offset, encoded, sizeof(encoded))) {
        return false;
    }
    record.id = encoded[0];
    record.role = static_cast<GraphDeviceRole>(encoded[1]);
    record.inputStart = decodeUint16(&encoded[2]);
    record.inputCount = decodeUint16(&encoded[4]);
    record.routeStart = decodeUint16(&encoded[6]);
    record.routeCount = decodeUint16(&encoded[8]);
    record.actionStart = decodeUint16(&encoded[10]);
    record.actionCount = decodeUint16(&encoded[12]);
    reserved = decodeUint16(&encoded[14]);
    return true;
}

bool readInputRecord(const GraphImageSource &source, std::size_t offset, GraphInputRecord &record,
                     uint16_t &reserved) {
    uint8_t encoded[kGraphInputRecordSize]{};
    if (!source.readAt(offset, encoded, sizeof(encoded))) {
        return false;
    }
    record.id = decodeUint16(encoded);
    record.device = encoded[2];
    record.pin = encoded[3];
    record.debounceMs = decodeUint16(&encoded[4]);
    reserved = decodeUint16(&encoded[6]);
    return true;
}

bool readRouteRecord(const GraphImageSource &source, std::size_t offset, GraphRouteRecord &record,
                     uint8_t &reserved) {
    uint8_t encoded[kGraphRouteRecordSize]{};
    if (!source.readAt(offset, encoded, sizeof(encoded))) {
        return false;
    }
    record.id = decodeUint16(encoded);
    record.flowId = decodeUint16(&encoded[2]);
    record.inputId = decodeUint16(&encoded[4]);
    record.actionReferenceStart = decodeUint16(&encoded[6]);
    record.ownerDevice = encoded[8];
    record.edge = static_cast<GraphEdge>(encoded[9]);
    record.actionCount = encoded[10];
    reserved = encoded[11];
    return true;
}

bool readActionRecord(const GraphImageSource &source, std::size_t offset, GraphActionRecord &record,
                      uint8_t &encodedValue, uint16_t &reserved) {
    uint8_t encoded[kGraphActionRecordSize]{};
    if (!source.readAt(offset, encoded, sizeof(encoded))) {
        return false;
    }
    record.id = decodeUint16(encoded);
    record.targetDevice = encoded[2];
    record.targetPin = encoded[3];
    record.operation = static_cast<GraphOperation>(encoded[4]);
    encodedValue = encoded[5];
    record.value = encodedValue != 0;
    reserved = decodeUint16(&encoded[6]);
    record.delayMs = decodeUint32(&encoded[8]);
    record.durationMs = decodeUint32(&encoded[12]);
    return true;
}

bool readActionReference(const GraphImageSource &source, std::size_t offset, uint16_t &actionId) {
    uint8_t encoded[kGraphActionReferenceSize]{};
    if (!source.readAt(offset, encoded, sizeof(encoded))) {
        return false;
    }
    actionId = decodeUint16(encoded);
    return true;
}

bool isRoleValid(GraphDeviceRole role) {
    return role == GraphDeviceRole::Input || role == GraphDeviceRole::Output;
}

bool isEdgeValid(GraphEdge edge) {
    return edge == GraphEdge::Rising || edge == GraphEdge::Falling || edge == GraphEdge::Changed;
}

bool isOperationValid(GraphOperation operation) {
    return operation == GraphOperation::Set || operation == GraphOperation::CopySource ||
           operation == GraphOperation::Toggle || operation == GraphOperation::Pulse;
}

bool recordIsInSlice(std::size_t recordIndex, uint16_t start, uint16_t count) {
    return recordIndex >= start && recordIndex < static_cast<std::size_t>(start) + count;
}

bool actionDefinitionsEqual(const GraphActionRecord &left, const GraphActionRecord &right) {
    return left.targetDevice == right.targetDevice && left.targetPin == right.targetPin &&
           left.operation == right.operation && left.value == right.value &&
           left.delayMs == right.delayMs && left.durationMs == right.durationMs;
}

uint32_t hashActionDefinition(const GraphActionRecord &action) {
    uint32_t hash = 2166136261u;
    const auto add = [&hash](uint8_t value) {
        hash ^= value;
        hash *= 16777619u;
    };
    add(action.targetDevice);
    add(action.targetPin);
    add(static_cast<uint8_t>(action.operation));
    add(action.value ? 1 : 0);
    for (uint8_t byte = 0; byte < 4; ++byte) {
        add(static_cast<uint8_t>((action.delayMs >> (byte * 8)) & 0xFFu));
    }
    for (uint8_t byte = 0; byte < 4; ++byte) {
        add(static_cast<uint8_t>((action.durationMs >> (byte * 8)) & 0xFFu));
    }
    return hash;
}

bool routeIsOutOfOrder(const GraphRouteRecord &previous, const GraphRouteRecord &current) {
    if (previous.ownerDevice != current.ownerDevice) {
        return previous.ownerDevice > current.ownerDevice;
    }
    if (previous.inputId != current.inputId) {
        return previous.inputId > current.inputId;
    }
    if (previous.edge != current.edge) {
        return static_cast<uint8_t>(previous.edge) > static_cast<uint8_t>(current.edge);
    }
    if (previous.flowId != current.flowId) {
        return previous.flowId > current.flowId;
    }
    return previous.id >= current.id;
}

GraphImageError verifyManifest(const GraphImageSource &source, const GraphManifest &expected,
                               GraphManifest &verified) {
    const GraphImageError error = GraphImageCodec::validate(source, verified);
    if (error != GraphImageError::None) {
        return error;
    }
    if (expected.imageSize != verified.imageSize || expected.generation != verified.generation ||
        expected.checksum != verified.checksum || expected.format != verified.format ||
        expected.executorApi != verified.executorApi) {
        return GraphImageError::InvalidSection;
    }
    return GraphImageError::None;
}

}  // namespace

bool GraphImageSource::readAt(std::size_t offset, uint8_t *output, std::size_t length) const {
    if (read == nullptr || (length != 0 && output == nullptr) || offset > size || length > size - offset) {
        return false;
    }
    return length == 0 || read(context, offset, output, length);
}

GraphImageSource GraphImageSource::fromMemory(const uint8_t *data, std::size_t size) {
    GraphImageSource source{};
    source.context = data;
    source.size = size;
    source.read = readMemory;
    return source;
}

const GraphDeviceRecord *GraphManifest::findDevice(uint8_t id) const {
    const uint16_t count =
        deviceCount < kGraphDeviceCapacity ? deviceCount : static_cast<uint16_t>(kGraphDeviceCapacity);
    for (uint16_t index = 0; index < count; ++index) {
        if (devices[index].id == id) {
            return &devices[index];
        }
    }
    return nullptr;
}

const GraphActionRecord *LocalInputGraph::findAction(uint16_t id) const {
    if (id == 0 || id > kGraphActionCapacity) {
        return nullptr;
    }
    const uint16_t slot = actionSlotById[id];
    if (slot == 0 || slot > referencedActionCount) {
        return nullptr;
    }
    return &referencedActions[slot - 1];
}

const GraphActionRecord *LocalOutputGraph::findAction(uint16_t id) const {
    if (id == 0 || id > kGraphActionCapacity) {
        return nullptr;
    }
    const uint16_t slot = actionSlotById[id];
    if (slot == 0 || slot > actionCount) {
        return nullptr;
    }
    return &actions[slot - 1];
}

GraphImageError GraphImageCodec::checksum(const GraphImageSource &source, uint32_t &checksum) {
    if (source.size < kGraphHeaderSize || source.size > kGraphImageCapacity) {
        return GraphImageError::InvalidLength;
    }

    Crc32 crc;
    uint8_t chunk[kChecksumChunkSize]{};
    for (std::size_t offset = 0; offset < source.size; offset += sizeof(chunk)) {
        const std::size_t remaining = source.size - offset;
        const std::size_t length = remaining < sizeof(chunk) ? remaining : sizeof(chunk);
        if (!source.readAt(offset, chunk, length)) {
            return GraphImageError::ReadFailure;
        }
        for (std::size_t index = 0; index < length; ++index) {
            const std::size_t imageOffset = offset + index;
            if (imageOffset >= kGraphChecksumOffset && imageOffset < kGraphChecksumOffset + 4) {
                chunk[index] = 0;
            }
        }
        crc.add(chunk, length);
    }
    checksum = crc.value();
    return GraphImageError::None;
}

GraphImageError GraphImageCodec::validate(const GraphImageSource &source, GraphManifest &manifest) {
    if (source.size < kGraphHeaderSize || source.size > kGraphImageCapacity) {
        return GraphImageError::InvalidLength;
    }

    uint8_t header[kGraphHeaderSize]{};
    if (!source.readAt(0, header, sizeof(header))) {
        return GraphImageError::ReadFailure;
    }
    for (std::size_t index = 0; index < sizeof(kMagic); ++index) {
        if (header[index] != kMagic[index]) {
            return GraphImageError::InvalidMagic;
        }
    }

    GraphManifest candidate{};
    candidate.format = decodeUint16(&header[4]);
    candidate.executorApi = decodeUint16(&header[6]);
    candidate.generation = decodeUint32(&header[8]);
    candidate.imageSize = decodeUint32(&header[12]);
    candidate.deviceCount = decodeUint16(&header[16]);
    candidate.inputCount = decodeUint16(&header[18]);
    candidate.flowCount = decodeUint16(&header[20]);
    candidate.routeCount = decodeUint16(&header[22]);
    candidate.actionReferenceCount = decodeUint16(&header[24]);
    candidate.actionCount = decodeUint16(&header[26]);
    candidate.deviceOffset = decodeUint32(&header[32]);
    candidate.inputOffset = decodeUint32(&header[36]);
    candidate.routeOffset = decodeUint32(&header[40]);
    candidate.actionReferenceOffset = decodeUint32(&header[44]);
    candidate.actionOffset = decodeUint32(&header[48]);
    candidate.checksum = decodeUint32(&header[kGraphChecksumOffset]);

    if (candidate.format != kGraphFormatVersion) {
        return GraphImageError::UnsupportedFormat;
    }
    if (candidate.executorApi != kGraphExecutorApiVersion) {
        return GraphImageError::UnsupportedExecutorApi;
    }
    if (candidate.generation == 0 || candidate.imageSize != source.size || decodeUint32(&header[28]) != 0) {
        return GraphImageError::InvalidLength;
    }
    if (candidate.deviceCount == 0 || candidate.deviceCount > kGraphDeviceCapacity ||
        candidate.inputCount > kGraphInputCapacity || candidate.flowCount > kGraphFlowCapacity ||
        candidate.routeCount > kGraphRouteCapacity || candidate.actionCount > kGraphActionCapacity ||
        candidate.actionReferenceCount > candidate.routeCount * kGraphActionsPerEvent) {
        return GraphImageError::InvalidCount;
    }

    const std::size_t expectedDeviceOffset = kGraphHeaderSize;
    const std::size_t expectedInputOffset =
        expectedDeviceOffset + static_cast<std::size_t>(candidate.deviceCount) * kGraphDeviceRecordSize;
    const std::size_t expectedRouteOffset =
        expectedInputOffset + static_cast<std::size_t>(candidate.inputCount) * kGraphInputRecordSize;
    const std::size_t expectedReferenceOffset =
        expectedRouteOffset + static_cast<std::size_t>(candidate.routeCount) * kGraphRouteRecordSize;
    const std::size_t expectedActionOffset =
        expectedReferenceOffset +
        static_cast<std::size_t>(candidate.actionReferenceCount) * kGraphActionReferenceSize;
    const std::size_t expectedImageSize =
        expectedActionOffset + static_cast<std::size_t>(candidate.actionCount) * kGraphActionRecordSize;
    if (candidate.deviceOffset != expectedDeviceOffset || candidate.inputOffset != expectedInputOffset ||
        candidate.routeOffset != expectedRouteOffset ||
        candidate.actionReferenceOffset != expectedReferenceOffset ||
        candidate.actionOffset != expectedActionOffset || expectedImageSize != source.size) {
        return GraphImageError::InvalidOffsets;
    }

    uint32_t calculatedChecksum = 0;
    const GraphImageError checksumError = checksum(source, calculatedChecksum);
    if (checksumError != GraphImageError::None) {
        return checksumError;
    }
    if (candidate.checksum != calculatedChecksum) {
        return GraphImageError::InvalidChecksum;
    }

    uint8_t deviceRoles[kGraphDeviceCapacity]{};
    std::size_t inputStart = 0;
    std::size_t routeStart = 0;
    std::size_t actionStart = 0;
    uint8_t previousDeviceId = 0;
    for (uint16_t index = 0; index < candidate.deviceCount; ++index) {
        GraphDeviceRecord device{};
        uint16_t reserved = 0;
        if (!readDeviceRecord(source,
                              candidate.deviceOffset +
                                  static_cast<std::size_t>(index) * kGraphDeviceRecordSize,
                              device, reserved)) {
            return GraphImageError::ReadFailure;
        }
        if (reserved != 0 || device.id >= kGraphDeviceCapacity || !isRoleValid(device.role)) {
            return GraphImageError::InvalidDevice;
        }
        if ((index != 0 && device.id <= previousDeviceId) || deviceRoles[device.id] != 0) {
            return GraphImageError::DuplicateRecord;
        }
        if (device.inputCount > kGraphLocalInputCapacity ||
            device.routeCount > kGraphLocalRouteCapacity ||
            device.actionCount > kGraphLocalActionCapacity) {
            return GraphImageError::LocalCapacity;
        }
        if ((device.inputCount == 0 && device.inputStart != 0) ||
            (device.inputCount != 0 && device.inputStart != inputStart) ||
            (device.routeCount == 0 && device.routeStart != 0) ||
            (device.routeCount != 0 && device.routeStart != routeStart) ||
            (device.actionCount == 0 && device.actionStart != 0) ||
            (device.actionCount != 0 && device.actionStart != actionStart)) {
            return GraphImageError::InvalidSection;
        }
        if ((device.role == GraphDeviceRole::Input && device.actionCount != 0) ||
            (device.role == GraphDeviceRole::Output &&
             (device.inputCount != 0 || device.routeCount != 0))) {
            return GraphImageError::InvalidSection;
        }
        inputStart += device.inputCount;
        routeStart += device.routeCount;
        actionStart += device.actionCount;
        deviceRoles[device.id] = static_cast<uint8_t>(device.role);
        previousDeviceId = device.id;
        candidate.devices[index] = device;
    }
    if (inputStart != candidate.inputCount || routeStart != candidate.routeCount ||
        actionStart != candidate.actionCount) {
        return GraphImageError::InvalidSection;
    }

    bool inputPresent[kGraphInputCapacity + 1]{};
    bool inputUsed[kGraphInputCapacity + 1]{};
    uint8_t inputOwner[kGraphInputCapacity + 1]{};
    uint16_t inputPinsByDevice[kGraphDeviceCapacity]{};
    GraphInputRecord previousInput{};
    bool havePreviousInput = false;
    for (uint16_t index = 0; index < candidate.inputCount; ++index) {
        GraphInputRecord input{};
        uint16_t reserved = 0;
        if (!readInputRecord(source,
                             candidate.inputOffset +
                                 static_cast<std::size_t>(index) * kGraphInputRecordSize,
                             input, reserved)) {
            return GraphImageError::ReadFailure;
        }
        if (reserved != 0 || input.id == 0 || input.id > candidate.inputCount ||
            input.device >= kGraphDeviceCapacity ||
            deviceRoles[input.device] != static_cast<uint8_t>(GraphDeviceRole::Input) ||
            input.pin >= kPinsPerDevice || input.debounceMs > kGraphMaximumDebounceMs) {
            return GraphImageError::InvalidInput;
        }
        if (inputPresent[input.id] || (inputPinsByDevice[input.device] & (1u << input.pin)) != 0) {
            return GraphImageError::DuplicateRecord;
        }
        if (havePreviousInput &&
            (previousInput.device > input.device ||
             (previousInput.device == input.device && previousInput.id >= input.id))) {
            return GraphImageError::InvalidInput;
        }
        const GraphDeviceRecord *device = candidate.findDevice(input.device);
        if (device == nullptr || !recordIsInSlice(index, device->inputStart, device->inputCount)) {
            return GraphImageError::InvalidSection;
        }
        inputPresent[input.id] = true;
        inputOwner[input.id] = input.device;
        inputPinsByDevice[input.device] |= static_cast<uint16_t>(1u << input.pin);
        previousInput = input;
        havePreviousInput = true;
    }
    for (uint16_t id = 1; id <= candidate.inputCount; ++id) {
        if (!inputPresent[id]) {
            return GraphImageError::InvalidInput;
        }
    }

    bool actionPresent[kGraphActionCapacity + 1]{};
    bool actionUsed[kGraphActionCapacity + 1]{};
    GraphOperation actionOperation[kGraphActionCapacity + 1]{};
    uint16_t actionHashSlots[kActionHashSlotCount]{};
    for (std::size_t index = 0; index < kActionHashSlotCount; ++index) {
        actionHashSlots[index] = std::numeric_limits<uint16_t>::max();
    }
    GraphActionRecord previousAction{};
    bool havePreviousAction = false;
    for (uint16_t index = 0; index < candidate.actionCount; ++index) {
        GraphActionRecord action{};
        uint8_t encodedValue = 0;
        uint16_t reserved = 0;
        if (!readActionRecord(source,
                              candidate.actionOffset +
                                  static_cast<std::size_t>(index) * kGraphActionRecordSize,
                              action, encodedValue, reserved)) {
            return GraphImageError::ReadFailure;
        }
        if (reserved != 0 || action.id == 0 || action.id > candidate.actionCount ||
            action.targetDevice >= kGraphDeviceCapacity ||
            deviceRoles[action.targetDevice] != static_cast<uint8_t>(GraphDeviceRole::Output) ||
            action.targetPin >= kPinsPerDevice || !isOperationValid(action.operation) ||
            action.delayMs > kGraphMaximumActionTimeMs) {
            return GraphImageError::InvalidAction;
        }
        const bool invalidParameters =
            (action.operation == GraphOperation::Set && encodedValue > 1) ||
            (action.operation != GraphOperation::Set && encodedValue != 0) ||
            (action.operation == GraphOperation::Pulse &&
             (action.durationMs == 0 || action.durationMs > kGraphMaximumActionTimeMs)) ||
            (action.operation != GraphOperation::Pulse && action.durationMs != 0);
        if (invalidParameters) {
            return GraphImageError::InvalidAction;
        }
        if (actionPresent[action.id]) {
            return GraphImageError::DuplicateRecord;
        }
        if (havePreviousAction &&
            (previousAction.targetDevice > action.targetDevice ||
             (previousAction.targetDevice == action.targetDevice && previousAction.id >= action.id))) {
            return GraphImageError::InvalidAction;
        }
        const GraphDeviceRecord *device = candidate.findDevice(action.targetDevice);
        if (device == nullptr || !recordIsInSlice(index, device->actionStart, device->actionCount)) {
            return GraphImageError::InvalidSection;
        }

        std::size_t slot = hashActionDefinition(action) & (kActionHashSlotCount - 1);
        bool inserted = false;
        for (std::size_t probe = 0; probe < kActionHashSlotCount; ++probe) {
            const uint16_t existingIndex = actionHashSlots[slot];
            if (existingIndex == std::numeric_limits<uint16_t>::max()) {
                actionHashSlots[slot] = index;
                inserted = true;
                break;
            }
            GraphActionRecord existing{};
            uint8_t existingValue = 0;
            uint16_t existingReserved = 0;
            if (!readActionRecord(source,
                                  candidate.actionOffset +
                                      static_cast<std::size_t>(existingIndex) * kGraphActionRecordSize,
                                  existing, existingValue, existingReserved)) {
                return GraphImageError::ReadFailure;
            }
            if (actionDefinitionsEqual(existing, action)) {
                return GraphImageError::DuplicateRecord;
            }
            slot = (slot + 1) & (kActionHashSlotCount - 1);
        }
        if (!inserted) {
            return GraphImageError::InvalidCount;
        }

        actionPresent[action.id] = true;
        actionOperation[action.id] = action.operation;
        previousAction = action;
        havePreviousAction = true;
    }
    for (uint16_t id = 1; id <= candidate.actionCount; ++id) {
        if (!actionPresent[id]) {
            return GraphImageError::InvalidAction;
        }
    }

    bool routePresent[kGraphRouteCapacity + 1]{};
    bool flowPresent[kGraphFlowCapacity + 1]{};
    GraphRouteRecord previousRoute{};
    bool havePreviousRoute = false;
    uint16_t expectedReferenceStart = 0;
    uint16_t eventInput = 0;
    GraphEdge eventEdge = GraphEdge::Rising;
    uint16_t eventFlow = 0;
    uint16_t eventActionIds[kGraphActionsPerEvent]{};
    uint8_t eventActionCount = 0;
    bool haveEvent = false;
    for (uint16_t index = 0; index < candidate.routeCount; ++index) {
        GraphRouteRecord route{};
        uint8_t reserved = 0;
        if (!readRouteRecord(source,
                             candidate.routeOffset +
                                 static_cast<std::size_t>(index) * kGraphRouteRecordSize,
                             route, reserved)) {
            return GraphImageError::ReadFailure;
        }
        if (reserved != 0 || route.id == 0 || route.id > candidate.routeCount || route.flowId == 0 ||
            route.flowId > candidate.flowCount || route.inputId == 0 ||
            route.inputId > candidate.inputCount || !inputPresent[route.inputId] ||
            route.ownerDevice >= kGraphDeviceCapacity ||
            deviceRoles[route.ownerDevice] != static_cast<uint8_t>(GraphDeviceRole::Input) ||
            !isEdgeValid(route.edge) || route.actionCount == 0 ||
            route.actionCount > kGraphActionsPerEvent ||
            route.actionReferenceStart != expectedReferenceStart ||
            static_cast<std::size_t>(route.actionReferenceStart) + route.actionCount >
                candidate.actionReferenceCount) {
            return GraphImageError::InvalidRoute;
        }
        if (routePresent[route.id]) {
            return GraphImageError::DuplicateRecord;
        }
        if (havePreviousRoute && routeIsOutOfOrder(previousRoute, route)) {
            return GraphImageError::InvalidRoute;
        }
        if (inputOwner[route.inputId] != route.ownerDevice) {
            return GraphImageError::InvalidReference;
        }
        const GraphDeviceRecord *device = candidate.findDevice(route.ownerDevice);
        if (device == nullptr || !recordIsInSlice(index, device->routeStart, device->routeCount)) {
            return GraphImageError::InvalidSection;
        }

        const bool sameEvent = haveEvent && eventInput == route.inputId && eventEdge == route.edge;
        if (!sameEvent) {
            eventInput = route.inputId;
            eventEdge = route.edge;
            eventActionCount = 0;
            eventFlow = 0;
            haveEvent = true;
        }
        if (eventFlow == route.flowId) {
            return GraphImageError::DuplicateRecord;
        }
        eventFlow = route.flowId;
        if (eventActionCount + route.actionCount > kGraphActionsPerEvent) {
            return GraphImageError::LocalCapacity;
        }

        uint16_t previousActionId = 0;
        for (uint8_t reference = 0; reference < route.actionCount; ++reference) {
            uint16_t actionId = 0;
            if (!readActionReference(
                    source,
                    candidate.actionReferenceOffset +
                        (static_cast<std::size_t>(route.actionReferenceStart) + reference) *
                            kGraphActionReferenceSize,
                    actionId)) {
                return GraphImageError::ReadFailure;
            }
            if (actionId == 0 || actionId > candidate.actionCount || !actionPresent[actionId] ||
                actionId <= previousActionId) {
                return GraphImageError::InvalidReference;
            }
            for (uint8_t existing = 0; existing < eventActionCount; ++existing) {
                if (eventActionIds[existing] == actionId) {
                    return GraphImageError::DuplicateRecord;
                }
            }
            if (actionOperation[actionId] == GraphOperation::CopySource &&
                route.edge != GraphEdge::Changed) {
                return GraphImageError::InvalidReference;
            }
            eventActionIds[eventActionCount++] = actionId;
            actionUsed[actionId] = true;
            previousActionId = actionId;
        }

        expectedReferenceStart = static_cast<uint16_t>(expectedReferenceStart + route.actionCount);
        routePresent[route.id] = true;
        flowPresent[route.flowId] = true;
        inputUsed[route.inputId] = true;
        previousRoute = route;
        havePreviousRoute = true;
    }
    if (expectedReferenceStart != candidate.actionReferenceCount) {
        return GraphImageError::InvalidReference;
    }
    for (uint16_t id = 1; id <= candidate.routeCount; ++id) {
        if (!routePresent[id]) {
            return GraphImageError::InvalidRoute;
        }
    }
    for (uint16_t id = 1; id <= candidate.flowCount; ++id) {
        if (!flowPresent[id]) {
            return GraphImageError::InvalidRoute;
        }
    }
    for (uint16_t id = 1; id <= candidate.inputCount; ++id) {
        if (!inputUsed[id]) {
            return GraphImageError::InvalidReference;
        }
    }
    for (uint16_t id = 1; id <= candidate.actionCount; ++id) {
        if (!actionUsed[id]) {
            return GraphImageError::InvalidReference;
        }
    }

    manifest = candidate;
    return GraphImageError::None;
}

GraphImageError GraphImageCodec::loadInputSection(const GraphImageSource &source,
                                                  const GraphManifest &manifest, uint8_t deviceId,
                                                  LocalInputGraph &staging) {
    GraphManifest verified{};
    const GraphImageError verifyError = verifyManifest(source, manifest, verified);
    if (verifyError != GraphImageError::None) {
        return verifyError;
    }
    const GraphDeviceRecord *device = verified.findDevice(deviceId);
    if (device == nullptr) {
        return GraphImageError::DeviceNotFound;
    }
    if (device->role != GraphDeviceRole::Input) {
        return GraphImageError::DeviceRoleMismatch;
    }
    if (device->inputCount > kGraphLocalInputCapacity || device->routeCount > kGraphLocalRouteCapacity) {
        return GraphImageError::LocalCapacity;
    }

    staging.identity = GraphIdentity{};
    staging.device = deviceId;
    staging.inputCount = device->inputCount;
    staging.routeCount = device->routeCount;
    staging.actionReferenceCount = 0;
    staging.referencedActionCount = 0;
    std::memset(staging.actionSlotById, 0, sizeof(staging.actionSlotById));
    staging.identity.generation = verified.generation;
    staging.identity.checksum = verified.checksum;
    for (uint16_t index = 0; index < device->inputCount; ++index) {
        uint16_t reserved = 0;
        if (!readInputRecord(source,
                             verified.inputOffset +
                                 (static_cast<std::size_t>(device->inputStart) + index) *
                                     kGraphInputRecordSize,
                             staging.inputs[index], reserved)) {
            return GraphImageError::ReadFailure;
        }
    }

    bool referencedActions[kGraphActionCapacity + 1]{};
    for (uint16_t index = 0; index < device->routeCount; ++index) {
        GraphRouteRecord route{};
        uint8_t reserved = 0;
        if (!readRouteRecord(source,
                             verified.routeOffset +
                                 (static_cast<std::size_t>(device->routeStart) + index) *
                                     kGraphRouteRecordSize,
                             route, reserved)) {
            return GraphImageError::ReadFailure;
        }
        if (static_cast<std::size_t>(staging.actionReferenceCount) + route.actionCount >
            kGraphLocalActionReferenceCapacity) {
            return GraphImageError::LocalCapacity;
        }
        LocalGraphRoute &local = staging.routes[index];
        local.id = route.id;
        local.flowId = route.flowId;
        local.inputId = route.inputId;
        local.actionReferenceStart = staging.actionReferenceCount;
        local.edge = route.edge;
        local.actionCount = route.actionCount;
        for (uint8_t reference = 0; reference < route.actionCount; ++reference) {
            uint16_t actionId = 0;
            if (!readActionReference(
                    source,
                    verified.actionReferenceOffset +
                        (static_cast<std::size_t>(route.actionReferenceStart) + reference) *
                            kGraphActionReferenceSize,
                    actionId)) {
                return GraphImageError::ReadFailure;
            }
            staging.actionReferences[staging.actionReferenceCount++] = actionId;
            referencedActions[actionId] = true;
        }
    }

    for (uint16_t index = 0; index < verified.actionCount; ++index) {
        GraphActionRecord action{};
        uint8_t encodedValue = 0;
        uint16_t reserved = 0;
        if (!readActionRecord(source,
                              verified.actionOffset +
                                  static_cast<std::size_t>(index) * kGraphActionRecordSize,
                              action, encodedValue, reserved)) {
            return GraphImageError::ReadFailure;
        }
        if (!referencedActions[action.id]) {
            continue;
        }
        if (staging.referencedActionCount >= kGraphActionCapacity) {
            return GraphImageError::LocalCapacity;
        }
        const uint16_t slot = staging.referencedActionCount++;
        staging.referencedActions[slot] = action;
        staging.actionSlotById[action.id] = static_cast<uint16_t>(slot + 1);
    }
    return GraphImageError::None;
}

GraphImageError GraphImageCodec::loadOutputSection(const GraphImageSource &source,
                                                   const GraphManifest &manifest, uint8_t deviceId,
                                                   LocalOutputGraph &staging) {
    GraphManifest verified{};
    const GraphImageError verifyError = verifyManifest(source, manifest, verified);
    if (verifyError != GraphImageError::None) {
        return verifyError;
    }
    const GraphDeviceRecord *device = verified.findDevice(deviceId);
    if (device == nullptr) {
        return GraphImageError::DeviceNotFound;
    }
    if (device->role != GraphDeviceRole::Output) {
        return GraphImageError::DeviceRoleMismatch;
    }
    if (device->actionCount > kGraphLocalActionCapacity) {
        return GraphImageError::LocalCapacity;
    }

    staging.identity = GraphIdentity{};
    staging.device = deviceId;
    staging.actionCount = device->actionCount;
    std::memset(staging.actionSlotById, 0, sizeof(staging.actionSlotById));
    staging.identity.generation = verified.generation;
    staging.identity.checksum = verified.checksum;
    for (uint16_t index = 0; index < device->actionCount; ++index) {
        uint8_t encodedValue = 0;
        uint16_t reserved = 0;
        if (!readActionRecord(source,
                              verified.actionOffset +
                                  (static_cast<std::size_t>(device->actionStart) + index) *
                                      kGraphActionRecordSize,
                              staging.actions[index], encodedValue, reserved)) {
            return GraphImageError::ReadFailure;
        }
        staging.actionSlotById[staging.actions[index].id] = static_cast<uint16_t>(index + 1);
    }
    return GraphImageError::None;
}

}  // namespace piho
