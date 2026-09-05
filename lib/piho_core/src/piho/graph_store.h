#pragma once

#include <cstddef>
#include <cstdint>

#include "piho/crc32.h"
#include "piho/graph_image.h"

namespace piho {

constexpr std::size_t kGraphStoreSlotCount = 3;
constexpr std::size_t kGraphStoreMetadataCopyCount = 2;
constexpr std::size_t kGraphStoreMetadataSize = 32;
constexpr std::size_t kGraphStoreChunkCapacity = 256;
constexpr uint8_t kGraphStoreNoSlot = 0xFF;

enum class GraphStoreState : uint8_t {
    Empty = 0,
    Receiving = 1,
    Staged = 2,
    Active = 3,
    Invalid = 4,
    Rollback = 5,
};

enum class GraphStoreError : uint8_t {
    None = 0,
    Mount = 1,
    MetadataRead = 2,
    MetadataCorrupt = 3,
    MetadataWrite = 4,
    InvalidArgument = 5,
    Busy = 6,
    NoSlot = 7,
    SlotOpen = 8,
    SlotWrite = 9,
    SlotClose = 10,
    SlotRead = 11,
    InvalidLength = 12,
    InvalidChecksum = 13,
    InvalidImage = 14,
    Interrupted = 15,
    NoStagedGraph = 16,
    NoRollbackGraph = 17,
    NoActiveGraph = 18,
};

struct GraphReceiveDescriptor {
    uint32_t imageSize = 0;
    uint32_t generation = 0;
    uint32_t checksum = 0;
};

struct GraphStoreStatus {
    GraphStoreState state = GraphStoreState::Empty;
    GraphStoreError lastError = GraphStoreError::None;
    uint32_t receivedBytes = 0;
    uint32_t expectedBytes = 0;
    GraphIdentity active{};
    GraphIdentity staged{};
    GraphIdentity rollback{};
    uint32_t activeDevices = 0;
    uint32_t stagedDevices = 0;
    uint32_t rollbackDevices = 0;
};

class GraphStoreBackend {
   public:
    virtual ~GraphStoreBackend() = default;

    virtual bool begin() = 0;
    // A missing metadata copy is a successful read with size zero.
    virtual bool readMetadata(uint8_t copy, uint8_t *output, std::size_t capacity,
                              std::size_t &size) = 0;
    // Replaces one metadata copy atomically with respect to power loss.
    virtual bool writeMetadata(uint8_t copy, const uint8_t *data, std::size_t size) = 0;

    virtual bool beginSlotWrite(uint8_t slot) = 0;
    virtual bool appendSlot(const uint8_t *data, std::size_t size) = 0;
    virtual bool finishSlotWrite() = 0;
    virtual void abortSlotWrite() = 0;
    virtual bool eraseSlot(uint8_t slot) = 0;
    virtual bool slotSize(uint8_t slot, std::size_t &size) = 0;
    virtual bool readSlot(uint8_t slot, std::size_t offset, uint8_t *output,
                          std::size_t size) const = 0;
};

class GraphStore {
   public:
    explicit GraphStore(GraphStoreBackend &backend) : backend_(backend) {}

    GraphStoreError begin();
    GraphStoreError beginReceive(const GraphReceiveDescriptor &descriptor);
    GraphStoreError writeChunk(const uint8_t *data, std::size_t size);
    GraphStoreError finishReceive();
    GraphStoreError cancelReceive();
    GraphStoreError discardStaged();
    GraphStoreError activate();
    GraphStoreError rollback();
    bool stagedDeviceRoleMatches(uint8_t device, GraphDeviceRole role) const;

    GraphStoreError loadActiveInput(uint8_t device, LocalInputGraph &staging);
    GraphStoreError loadActiveOutput(uint8_t device, LocalOutputGraph &staging);

    const GraphStoreStatus &status() const { return status_; }
    bool hasActiveGraph() const { return activeSlot_ != kGraphStoreNoSlot; }
    bool hasStagedGraph() const { return stagedSlot_ != kGraphStoreNoSlot; }
    bool hasRollbackGraph() const { return rollbackSlot_ != kGraphStoreNoSlot; }

   private:
    struct Metadata {
        uint32_t sequence = 0;
        GraphStoreState state = GraphStoreState::Empty;
        GraphStoreError lastError = GraphStoreError::None;
        uint8_t activeSlot = kGraphStoreNoSlot;
        uint8_t stagedSlot = kGraphStoreNoSlot;
        uint8_t rollbackSlot = kGraphStoreNoSlot;
        uint8_t receivingSlot = kGraphStoreNoSlot;
        GraphReceiveDescriptor receive{};
    };

    struct SlotSourceContext {
        const GraphStoreBackend *backend = nullptr;
        uint8_t slot = kGraphStoreNoSlot;
    };

    static bool readSlotSource(const void *context, std::size_t offset, uint8_t *output,
                               std::size_t size);
    static bool generationIsNewer(uint32_t candidate, uint32_t reference);
    static bool decodeMetadata(const uint8_t *data, std::size_t size, Metadata &metadata);
    static void encodeMetadata(const Metadata &metadata, uint8_t *output);

    Metadata currentMetadata() const;
    void applyMetadata(const Metadata &metadata, uint8_t copy);
    GraphStoreError commit(Metadata metadata);
    GraphStoreError validateSlot(uint8_t slot, GraphManifest &manifest) const;
    GraphStoreError failReceive(GraphStoreError error);
    uint8_t availableSlot() const;
    void refreshStatus();
    void clearManifest(GraphManifest &manifest);

    GraphStoreBackend &backend_;
    GraphStoreStatus status_{};
    GraphManifest activeManifest_{};
    GraphManifest stagedManifest_{};
    GraphManifest rollbackManifest_{};
    uint32_t metadataSequence_ = 0;
    uint8_t metadataCopy_ = kGraphStoreNoSlot;
    uint8_t activeSlot_ = kGraphStoreNoSlot;
    uint8_t stagedSlot_ = kGraphStoreNoSlot;
    uint8_t rollbackSlot_ = kGraphStoreNoSlot;
    uint8_t receivingSlot_ = kGraphStoreNoSlot;
    GraphReceiveDescriptor receive_{};
    uint32_t receivedBytes_ = 0;
    Crc32 receiveChecksum_{};
    bool receivingOpen_ = false;
    bool mounted_ = false;
};

}  // namespace piho
