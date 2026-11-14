#pragma once

#include "config/target_profile.hpp"
#include "sync/steam_lobby.hpp"
#include "sync/world_players.hpp"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace sf::sync {

// k_cubChatMetadataMax is 8192.
inline constexpr std::size_t kMaximumDataSize = 8000;
inline constexpr std::size_t kMaximumChannelNameLength = 32;
inline constexpr std::size_t kMaximumChannels = 16;

enum class ProtoSyncState {
    off,
    unavailable,
    idle,
    connecting,
    ready,
};

[[nodiscard]] const char* ProtoSyncStateName(ProtoSyncState state) noexcept;
[[nodiscard]] bool IsValidChannelName(std::string_view name) noexcept;

// Thread-safe. Core has no game-thread tick, so the Steam work runs inside the
// calls, at most every 250 ms.
class ProtoSync final {
public:
    struct Member final {
        std::uint64_t steamId{};
        std::uint32_t playerId{};
        std::string data;
        std::uint64_t revision{};
    };

    void Configure(
        bool enabled,
        bool isSteam,
        const config::ProtoSyncProfile& profile,
        std::uintptr_t moduleBase) noexcept;

    [[nodiscard]] bool Open(const void* owner, std::string_view channel) noexcept;
    void Close(const void* owner, std::string_view channel) noexcept;
    void ForgetOwner(const void* owner) noexcept;

    [[nodiscard]] bool Publish(std::string_view channel, std::string_view data) noexcept;

    [[nodiscard]] ProtoSyncState State() noexcept;
    [[nodiscard]] std::uint64_t Revision(std::string_view channel) noexcept;

    template <class Visit>
    std::size_t VisitMembers(std::string_view channel, Visit&& visit) noexcept {
        const std::scoped_lock lock(mutex_);
        Update();
        const Channel* const found = FindChannel(channel);
        if (found == nullptr) {
            return 0;
        }
        for (const Member& member : found->members) {
            visit(member);
        }
        return found->members.size();
    }

    [[nodiscard]] bool MembersJson(
        std::string_view channel, std::uint64_t since, std::string& json) noexcept;

private:
    struct Removal final {
        std::uint64_t steamId{};
        std::uint64_t revision{};
    };
    struct Channel final {
        std::string name;
        std::string key;
        std::vector<const void*> owners;
        std::vector<Member> members;
        std::vector<Removal> removals;
        std::uint64_t removalFloor{};
        std::uint64_t revision{};
    };

    void Update() noexcept;
    void UpdateWorld(std::uint64_t now) noexcept;
    void StartLobbyIfLoaded(std::uint64_t now) noexcept;
    void ApplyMembers() noexcept;
    void RemoveMember(Channel& channel, std::size_t index) noexcept;
    [[nodiscard]] std::uint32_t FindPlayerId(std::string_view identity) const noexcept;
    void WriteReadKeys() noexcept;
    void WriteIdentity() noexcept;
    [[nodiscard]] Channel* FindChannel(std::string_view name) noexcept;
    void CloseChannel(std::size_t index) noexcept;
    [[nodiscard]] ProtoSyncState StateLocked() const noexcept;

    std::mutex mutex_;
    bool enabled_{};
    bool configured_{};
    bool hasAddresses_{};
    WorldAddresses addresses_{};
    SteamLobby lobby_;
    bool lobbyFailed_{};
    std::uint64_t nextLobbyStartTime_{};
    std::vector<Channel> channels_;
    std::vector<WorldPlayer> players_;
    std::uint64_t playersRevision_{};
    std::uint64_t appliedPlayersRevision_{};
    std::uint64_t appliedMembersRevision_{};
    std::uint64_t nextWorldReadTime_{};
    std::uint64_t worldId_{};
    bool inWorld_{};
    std::string lobbyName_;
    std::string identity_;
    std::uint64_t nextRevision_{1};
};

[[nodiscard]] ProtoSync& GlobalProtoSync() noexcept;

}  // namespace sf::sync
