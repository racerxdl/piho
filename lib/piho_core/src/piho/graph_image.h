#pragma once

#include <cstddef>
#include <cstdint>

#include "piho/addressing.h"

namespace piho {

constexpr uint16_t kGraphFormatVersion = 1;
constexpr uint16_t kGraphExecutorApiVersion = 1;
constexpr std::size_t kGraphImageCapacity = 16 * 1024;
constexpr std::size_t kGraphHeaderSize = 56;
constexpr std::size_t kGraphDeviceRecordSize = 16;
constexpr std::size_t kGraphInputRecordSize = 8;
constexpr std::size_t kGraphRouteRecordSize = 12;
constexpr std::size_t kGraphActionReferenceSize = 2;
constexpr std::size_t kGraphActionRecordSize = 16;
constexpr std::size_t kGraphChecksumOffset = 52;
constexpr std::size_t kGraphDeviceCapacity = kDeviceCount;
constexpr std::size_t kGraphInputCapacity = 512;
constexpr std::size_t kGraphFlowCapacity = 256;
constexpr std::size_t kGraphRouteCapacity = 512;
constexpr std::size_t kGraphActionCapacity = 512;
constexpr std::size_t kGraphLocalInputCapacity = kPinsPerDevice;
constexpr std::size_t kGraphLocalRouteCapacity = 128;
constexpr std::size_t kGraphLocalActionCapacity = 128;
constexpr std::size_t kGraphActionsPerEvent = 16;
constexpr std::size_t kGraphLocalActionReferenceCapacity =
    kGraphLocalRouteCapacity * kGraphActionsPerEvent;
constexpr uint16_t kGraphMaximumDebounceMs = 5000;
constexpr uint32_t kGraphMaximumActionTimeMs = 86400000;

using GraphImageRead = bool (*)(const void *context, std::size_t offset, uint8_t *output,
                                std::size_t length);

struct GraphImageSource {
    const void *context = nullptr;
    std::size_t size = 0;
    GraphImageRead read = nullptr;

    bool readAt(std::size_t offset, uint8_t *output, std::size_t length) const;
    static GraphImageSource fromMemory(const uint8_t *data, std::size_t size);
};

enum class GraphImageError : uint8_t {
    None,
    ReadFailure,
    InvalidLength,
    InvalidMagic,
    UnsupportedFormat,
    UnsupportedExecutorApi,
    InvalidChecksum,
    InvalidOffsets,
    InvalidCount,
    InvalidDevice,
    InvalidInput,
    InvalidRoute,
    InvalidAction,
    InvalidReference,
    InvalidSection,
    DuplicateRecord,
    DeviceNotFound,
    DeviceRoleMismatch,
    LocalCapacity,
};

enum class GraphDeviceRole : uint8_t {
    Input = 1,
    Output = 2,
};

enum class GraphEdge : uint8_t {
    Rising = 1,
    Falling = 2,
    Changed = 3,
};

enum class GraphOperation : uint8_t {
    Set = 1,
    CopySource = 2,
    Toggle = 3,
    Pulse = 4,
};

struct GraphDeviceRecord {
    uint8_t id = 0;
    GraphDeviceRole role = GraphDeviceRole::Input;
    uint16_t inputStart = 0;
    uint16_t inputCount = 0;
    uint16_t routeStart = 0;
    uint16_t routeCount = 0;
    uint16_t actionStart = 0;
    uint16_t actionCount = 0;
};

struct GraphInputRecord {
    uint16_t id = 0;
    uint8_t device = 0;
    uint8_t pin = 0;
    uint16_t debounceMs = 0;
};

struct GraphRouteRecord {
    uint16_t id = 0;
    uint16_t flowId = 0;
    uint16_t inputId = 0;
    uint16_t actionReferenceStart = 0;
    uint8_t ownerDevice = 0;
    GraphEdge edge = GraphEdge::Rising;
    uint8_t actionCount = 0;
};

struct GraphActionRecord {
    uint16_t id = 0;
    uint8_t targetDevice = 0;
    uint8_t targetPin = 0;
    GraphOperation operation = GraphOperation::Set;
    bool value = false;
    uint32_t delayMs = 0;
    uint32_t durationMs = 0;
};

struct GraphManifest {
    uint16_t format = 0;
    uint16_t executorApi = 0;
    uint32_t generation = 0;
    uint32_t imageSize = 0;
    uint32_t checksum = 0;
    uint16_t deviceCount = 0;
    uint16_t inputCount = 0;
    uint16_t flowCount = 0;
    uint16_t routeCount = 0;
    uint16_t actionReferenceCount = 0;
    uint16_t actionCount = 0;
    uint32_t deviceOffset = 0;
    uint32_t inputOffset = 0;
    uint32_t routeOffset = 0;
    uint32_t actionReferenceOffset = 0;
    uint32_t actionOffset = 0;
    GraphDeviceRecord devices[kGraphDeviceCapacity]{};

    const GraphDeviceRecord *findDevice(uint8_t id) const;
};

struct GraphIdentity {
    uint16_t format = 0;
    uint16_t executorApi = 0;
    uint32_t generation = 0;
    uint32_t checksum = 0;
};

struct LocalGraphRoute {
    uint16_t id = 0;
    uint16_t flowId = 0;
    uint16_t inputId = 0;
    uint16_t actionReferenceStart = 0;
    GraphEdge edge = GraphEdge::Rising;
    uint8_t actionCount = 0;
};

struct LocalInputGraph {
    GraphIdentity identity{};
    uint8_t device = 0;
    GraphDeviceRole role = GraphDeviceRole::Input;
    uint16_t inputCount = 0;
    uint16_t routeCount = 0;
    uint16_t actionReferenceCount = 0;
    uint16_t referencedActionCount = 0;
    GraphInputRecord inputs[kGraphLocalInputCapacity]{};
    LocalGraphRoute routes[kGraphLocalRouteCapacity]{};
    uint16_t actionReferences[kGraphLocalActionReferenceCapacity]{};
    GraphActionRecord referencedActions[kGraphActionCapacity]{};
    uint16_t actionSlotById[kGraphActionCapacity + 1]{};

    const GraphActionRecord *findAction(uint16_t id) const;
};

struct LocalOutputGraph {
    GraphIdentity identity{};
    uint8_t device = 0;
    uint16_t actionCount = 0;
    GraphDeviceRole role = GraphDeviceRole::Output;
    GraphActionRecord actions[kGraphLocalActionCapacity]{};
    uint16_t actionSlotById[kGraphActionCapacity + 1]{};

    const GraphActionRecord *findAction(uint16_t id) const;
};

class GraphImageCodec {
   public:
    static GraphImageError checksum(const GraphImageSource &source, uint32_t &checksum);
    // validate commits manifest only after the complete image passes every check.
    static GraphImageError validate(const GraphImageSource &source, GraphManifest &manifest);
    // Loaders write a caller-owned staging section. Replace an active section only after None.
    static GraphImageError loadInputSection(const GraphImageSource &source,
                                            const GraphManifest &manifest, uint8_t device,
                                            LocalInputGraph &staging);
    static GraphImageError loadOutputSection(const GraphImageSource &source,
                                             const GraphManifest &manifest, uint8_t device,
                                             LocalOutputGraph &staging);
};

}  // namespace piho
