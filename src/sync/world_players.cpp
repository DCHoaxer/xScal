#include "sync/world_players.hpp"

#include "datamanager/game_read_lock.hpp"
#include "platform/native_access.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace sf::sync {
namespace {

constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

// FindPlayerCharacterByPlayerId 0x1403BCE90, GetLocalPlayerId 0x1403BAAE0.
constexpr std::size_t kPlayerMapOffset = 0x68;
constexpr std::size_t kLocalPlayerIdOffset = 0x78;
constexpr std::size_t kPlayerMapLockOffset = 0x7C;
// FindPlayerRecordByPlayerId 0x1420739E0.
constexpr std::size_t kRecordOwnerOffset = 0x388;
constexpr std::size_t kRecordManagerOffset = 0x98;
constexpr std::size_t kRecordMapOffset = 0x88;
constexpr std::size_t kRecordMapLockOffset = 0x60;
constexpr std::size_t kNodeLeftOffset = 0x0;
constexpr std::size_t kNodeParentOffset = 0x8;
constexpr std::size_t kNodeRightOffset = 0x10;
constexpr std::size_t kNodeIsNilOffset = 0x19;
constexpr std::size_t kNodeKeyOffset = 0x20;
constexpr std::size_t kNodeValueOffset = 0x28;
constexpr std::size_t kRecordNameOffset = 0x00;
constexpr std::size_t kRecordAccountIdOffset = 0x60;
constexpr std::size_t kMaximumNameLength = 64;
constexpr std::size_t kMaximumMapSteps = 1024;
constexpr std::size_t kMaximumPlayers = 256;

template <class T>
[[nodiscard]] bool ReadValue(std::uintptr_t address, T& value) noexcept {
    return address != 0 &&
        platform::TryCopyBytes(reinterpret_cast<const void*>(address), &value, sizeof(value));
}

[[nodiscard]] std::uint64_t HashBytes(
    std::string_view salt, const std::uint8_t* data, std::size_t size, bool lowercase) noexcept {
    std::uint64_t hash = kFnvOffset;
    for (const char character : salt) {
        hash ^= static_cast<std::uint8_t>(character);
        hash *= kFnvPrime;
    }
    for (std::size_t index = 0; index < size; ++index) {
        std::uint8_t value = data[index];
        if (lowercase && value >= 'A' && value <= 'Z') {
            value = static_cast<std::uint8_t>(value - 'A' + 'a');
        }
        hash ^= value;
        hash *= kFnvPrime;
    }
    return hash == 0 ? 1 : hash;
}

[[nodiscard]] bool IsNilNode(std::uintptr_t node, bool& isNil) noexcept {
    std::uint8_t value{};
    if (!ReadValue(node + kNodeIsNilOffset, value)) {
        return false;
    }
    isNil = value != 0;
    return true;
}

template <class Visit>
void VisitMapNodes(std::uintptr_t head, Visit&& visit) noexcept {
    std::uintptr_t node{};
    if (!ReadValue(head + kNodeLeftOffset, node)) {
        return;
    }
    std::size_t steps = 0;
    while (node != head && steps++ < kMaximumMapSteps) {
        bool isNil{};
        if (!IsNilNode(node, isNil) || isNil) {
            return;
        }
        visit(node);

        std::uintptr_t right{};
        bool rightIsNil{};
        if (!ReadValue(node + kNodeRightOffset, right) || !IsNilNode(right, rightIsNil)) {
            return;
        }
        if (!rightIsNil) {
            node = right;
            for (;;) {
                std::uintptr_t left{};
                bool leftIsNil{};
                if (steps++ >= kMaximumMapSteps ||
                    !ReadValue(node + kNodeLeftOffset, left) || !IsNilNode(left, leftIsNil)) {
                    return;
                }
                if (leftIsNil) {
                    break;
                }
                node = left;
            }
            continue;
        }

        std::uintptr_t parent{};
        if (!ReadValue(node + kNodeParentOffset, parent)) {
            return;
        }
        for (;;) {
            bool parentIsNil{};
            std::uintptr_t parentRight{};
            if (steps++ >= kMaximumMapSteps || !IsNilNode(parent, parentIsNil)) {
                return;
            }
            if (parentIsNil || !ReadValue(parent + kNodeRightOffset, parentRight) ||
                node != parentRight) {
                break;
            }
            node = parent;
            if (!ReadValue(node + kNodeParentOffset, parent)) {
                return;
            }
        }
        node = parent;
    }
}

[[nodiscard]] std::uint64_t ReadNameKey(std::uintptr_t address) noexcept {
    struct MsvcString final {
        std::array<char, 16> buffer;
        std::size_t size;
        std::size_t capacity;
    } value{};
    if (!ReadValue(address, value) || value.size == 0 || value.size > kMaximumNameLength ||
        value.capacity < value.size) {
        return 0;
    }
    std::array<char, kMaximumNameLength> text{};
    if (value.capacity <= 15) {
        std::memcpy(text.data(), value.buffer.data(), value.size);
    } else {
        std::uintptr_t data{};
        std::memcpy(&data, value.buffer.data(), sizeof(data));
        if (data == 0 ||
            !platform::TryCopyBytes(reinterpret_cast<const void*>(data), text.data(), value.size)) {
            return 0;
        }
    }
    return NameKey(std::string_view(text.data(), value.size));
}

// Try-lock, so a game writer never stalls the caller.
class MapReadLock final {
public:
    MapReadLock(const WorldAddresses& addresses, std::uintptr_t lock) noexcept
        : release_(addresses.releaseReadLock), lock_(lock) {
        bool acquired{};
        held_ = datamanager::TryAcquireGameReadLock(addresses.tryAcquireReadLock, lock, acquired) &&
            acquired;
    }
    MapReadLock(const MapReadLock&) = delete;
    MapReadLock& operator=(const MapReadLock&) = delete;
    ~MapReadLock() {
        if (held_) {
            reinterpret_cast<void(__fastcall*)(void*)>(release_)(reinterpret_cast<void*>(lock_));
        }
    }
    [[nodiscard]] bool IsHeld() const noexcept { return held_; }

private:
    std::uintptr_t release_{};
    std::uintptr_t lock_{};
    bool held_{};
};

}  // namespace

bool ReadWorldAddresses(
    const config::ProtoSyncProfile& profile,
    std::uintptr_t moduleBase,
    WorldAddresses& addresses) noexcept {
    addresses = {};
    if (moduleBase == 0 || profile.playerManagerGlobal == 0 || profile.recordOwnerGlobal == 0 ||
        profile.worldDataModelGlobal == 0 || profile.worldDataModelVtable == 0 ||
        profile.tryAcquireReadLock == 0 || profile.releaseReadLock == 0 ||
        profile.joinedWorldOffset == 0) {
        return false;
    }
    addresses.playerManagerGlobal = moduleBase + profile.playerManagerGlobal;
    addresses.recordOwnerGlobal = moduleBase + profile.recordOwnerGlobal;
    addresses.worldDataModelGlobal = moduleBase + profile.worldDataModelGlobal;
    addresses.worldDataModelVtable = moduleBase + profile.worldDataModelVtable;
    addresses.tryAcquireReadLock = moduleBase + profile.tryAcquireReadLock;
    addresses.releaseReadLock = moduleBase + profile.releaseReadLock;
    addresses.joinedWorldOffset = profile.joinedWorldOffset;
    return true;
}

std::uint64_t AccountIdKey(std::uint64_t accountId) noexcept {
    if (accountId == 0) {
        return 0;
    }
    std::array<std::uint8_t, 8> bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<std::uint8_t>(accountId >> (index * 8));
    }
    return HashBytes("xscal-proto-sync-account:", bytes.data(), bytes.size(), false);
}

std::uint64_t NameKey(std::string_view name) noexcept {
    if (name.empty()) {
        return 0;
    }
    return HashBytes("xscal-proto-sync-name:",
        reinterpret_cast<const std::uint8_t*>(name.data()), name.size(), true);
}

std::uint64_t WorldKey(std::uint64_t worldId) noexcept {
    if (worldId == 0) {
        return 0;
    }
    std::array<std::uint8_t, 8> bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<std::uint8_t>(worldId >> (index * 8));
    }
    return HashBytes("xscal-proto-sync-world:", bytes.data(), bytes.size(), false);
}

// Set by the join completion at 0x14154BCEF; RequestJoinWorld (0x141534C10)
// compares against it.
std::uint64_t ReadJoinedWorldId(const WorldAddresses& addresses) noexcept {
    std::uintptr_t model{};
    std::uintptr_t vtable{};
    std::uint64_t worldId{};
    if (!ReadValue(addresses.worldDataModelGlobal, model) || model == 0 ||
        !ReadValue(model, vtable) || vtable != addresses.worldDataModelVtable ||
        !ReadValue(model + addresses.joinedWorldOffset, worldId)) {
        return 0;
    }
    return worldId;
}

bool ReadWorldPlayers(
    const WorldAddresses& addresses,
    std::vector<WorldPlayer>& players) noexcept {
    players.clear();
    std::uintptr_t playerManager{};
    std::uintptr_t playerHead{};
    std::uint32_t localId{};
    std::uintptr_t owner{};
    std::uintptr_t ownerChild{};
    std::uintptr_t recordManager{};
    std::uintptr_t recordHead{};
    if (!ReadValue(addresses.playerManagerGlobal, playerManager) || playerManager == 0 ||
        !ReadValue(playerManager + kPlayerMapOffset, playerHead) || playerHead == 0 ||
        !ReadValue(playerManager + kLocalPlayerIdOffset, localId) ||
        !ReadValue(addresses.recordOwnerGlobal, owner) || owner == 0 ||
        !ReadValue(owner + kRecordOwnerOffset, ownerChild) || ownerChild == 0 ||
        !ReadValue(ownerChild + kRecordManagerOffset, recordManager) || recordManager == 0 ||
        !ReadValue(recordManager + kRecordMapOffset, recordHead) || recordHead == 0) {
        return false;
    }
    try {
        players.reserve(kMaximumPlayers);
    } catch (...) {
        return false;
    }

    {
        MapReadLock lock{addresses, playerManager + kPlayerMapLockOffset};
        if (!lock.IsHeld()) {
            return false;
        }
        VisitMapNodes(playerHead, [&](std::uintptr_t node) noexcept {
            WorldPlayer player;
            std::uintptr_t actor{};
            if (players.size() >= kMaximumPlayers ||
                !ReadValue(node + kNodeKeyOffset, player.playerId) ||
                !ReadValue(node + kNodeValueOffset, actor) || actor == 0) {
                return;
            }
            player.isLocal = player.playerId == localId;
            players.push_back(player);
        });
    }
    if (players.empty()) {
        return true;
    }

    MapReadLock lock{addresses, recordManager + kRecordMapLockOffset};
    if (!lock.IsHeld()) {
        players.clear();
        return false;
    }
    VisitMapNodes(recordHead, [&](std::uintptr_t node) noexcept {
        std::uint32_t playerId{};
        std::uintptr_t record{};
        if (!ReadValue(node + kNodeKeyOffset, playerId) ||
            !ReadValue(node + kNodeValueOffset, record) || record == 0) {
            return;
        }
        const auto player = std::find_if(players.begin(), players.end(),
            [&](const WorldPlayer& entry) { return entry.playerId == playerId; });
        if (player == players.end()) {
            return;
        }
        std::uint64_t accountId{};
        if (ReadValue(record + kRecordAccountIdOffset, accountId)) {
            player->accountKey = AccountIdKey(accountId);
        }
        player->nameKey = ReadNameKey(record + kRecordNameOffset);
    });
    return true;
}

}  // namespace sf::sync
