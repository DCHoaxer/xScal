#include "sync/proto_sync.hpp"

#include "common/json.hpp"
#include "platform/diagnostics.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <utility>

namespace sf::sync {
namespace {

constexpr char kIdentityKey[] = "id";
constexpr char kChannelKeyPrefix[] = "c.";
constexpr std::uint64_t kWorldReadInterval = 1000;
constexpr std::uint64_t kLobbyStartInterval = 5000;
constexpr std::size_t kMaximumRemovals = 64;

[[nodiscard]] bool ParseHexKey(std::string_view text, std::uint64_t& value) noexcept {
    if (text.size() != 16) {
        return false;
    }
    value = 0;
    for (const char character : text) {
        std::uint64_t digit{};
        if (character >= '0' && character <= '9') {
            digit = static_cast<std::uint64_t>(character - '0');
        } else if (character >= 'a' && character <= 'f') {
            digit = static_cast<std::uint64_t>(character - 'a' + 10);
        } else {
            return false;
        }
        value = (value << 4) | digit;
    }
    return true;
}

[[nodiscard]] const std::string* FindValue(
    const SteamLobby::Member& member, std::string_view key) noexcept {
    for (const SteamLobby::Value& value : member.values) {
        if (value.key == key) {
            return &value.text;
        }
    }
    return nullptr;
}

void AppendSteamId(std::uint64_t steamId, std::string& json) {
    std::array<char, 24> text{};
    const int length = std::snprintf(
        text.data(), text.size(), "\"%llu\"", static_cast<unsigned long long>(steamId));
    json.append(text.data(), static_cast<std::size_t>(length));
}

}  // namespace

const char* ProtoSyncStateName(ProtoSyncState state) noexcept {
    switch (state) {
    case ProtoSyncState::off:
        return "off";
    case ProtoSyncState::unavailable:
        return "unavailable";
    case ProtoSyncState::idle:
        return "idle";
    case ProtoSyncState::connecting:
        return "connecting";
    case ProtoSyncState::ready:
        return "ready";
    }
    return "unavailable";
}

bool IsValidChannelName(std::string_view name) noexcept {
    if (name.empty() || name.size() > kMaximumChannelNameLength) {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [](char character) {
        return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') ||
            character == '.' || character == '_' || character == '-';
    });
}

void ProtoSync::Configure(
    bool enabled,
    bool isSteam,
    const config::ProtoSyncProfile& profile,
    std::uintptr_t moduleBase) noexcept {
    const std::scoped_lock lock(mutex_);
    configured_ = true;
    enabled_ = enabled;
    hasAddresses_ = isSteam && ReadWorldAddresses(profile, moduleBase, addresses_);
    if (!enabled_) {
        DiagnosticLog("xScal protoSync: off in xscal.ini");
    } else if (!isSteam) {
        DiagnosticLog("xScal protoSync: unavailable; it needs the Steam version of the game");
    } else if (!hasAddresses_) {
        DiagnosticLog("xScal protoSync: unavailable; the Address Library lacks its records");
    }
}

bool ProtoSync::Open(const void* owner, std::string_view channel) noexcept {
    if (owner == nullptr || !IsValidChannelName(channel)) {
        return false;
    }
    const std::scoped_lock lock(mutex_);
    if (!enabled_ || !hasAddresses_ || lobbyFailed_) {
        return false;
    }
    Channel* found = FindChannel(channel);
    try {
        if (found == nullptr) {
            if (channels_.size() >= kMaximumChannels) {
                return false;
            }
            Channel& added = channels_.emplace_back();
            added.name.assign(channel);
            added.key.assign(kChannelKeyPrefix);
            added.key.append(channel);
            added.revision = nextRevision_++;
            found = &added;
            WriteReadKeys();
        }
        if (std::find(found->owners.begin(), found->owners.end(), owner) == found->owners.end()) {
            found->owners.push_back(owner);
        }
    } catch (...) {
        return false;
    }
    Update();
    return true;
}

void ProtoSync::Close(const void* owner, std::string_view channel) noexcept {
    const std::scoped_lock lock(mutex_);
    for (std::size_t index = 0; index < channels_.size(); ++index) {
        Channel& entry = channels_[index];
        if (entry.name != channel) {
            continue;
        }
        std::erase(entry.owners, owner);
        if (entry.owners.empty()) {
            CloseChannel(index);
        }
        break;
    }
    Update();
}

void ProtoSync::ForgetOwner(const void* owner) noexcept {
    const std::scoped_lock lock(mutex_);
    for (std::size_t index = channels_.size(); index-- > 0;) {
        Channel& entry = channels_[index];
        if (std::erase(entry.owners, owner) != 0 && entry.owners.empty()) {
            CloseChannel(index);
        }
    }
    Update();
}

void ProtoSync::CloseChannel(std::size_t index) noexcept {
    (void)lobby_.SetValue(channels_[index].key, {});
    channels_.erase(channels_.begin() + static_cast<std::ptrdiff_t>(index));
    WriteReadKeys();
}

bool ProtoSync::Publish(std::string_view channel, std::string_view data) noexcept {
    if (data.size() > kMaximumDataSize || data.find('\0') != std::string_view::npos ||
        !json::IsValidUtf8(data)) {
        return false;
    }
    const std::scoped_lock lock(mutex_);
    const Channel* const found = FindChannel(channel);
    if (found == nullptr || !lobby_.SetValue(found->key, data)) {
        return false;
    }
    Update();
    return true;
}

ProtoSyncState ProtoSync::State() noexcept {
    const std::scoped_lock lock(mutex_);
    Update();
    return StateLocked();
}

ProtoSyncState ProtoSync::StateLocked() const noexcept {
    if (configured_ && !enabled_) {
        return ProtoSyncState::off;
    }
    if (!hasAddresses_ || lobbyFailed_) {
        return ProtoSyncState::unavailable;
    }
    if (channels_.empty()) {
        return ProtoSyncState::idle;
    }
    return inWorld_ && lobby_.IsInLobby() ? ProtoSyncState::ready : ProtoSyncState::connecting;
}

std::uint64_t ProtoSync::Revision(std::string_view channel) noexcept {
    const std::scoped_lock lock(mutex_);
    Update();
    const Channel* const found = FindChannel(channel);
    return found != nullptr ? found->revision : 0;
}

bool ProtoSync::MembersJson(
    std::string_view channel, std::uint64_t since, std::string& json) noexcept {
    const std::scoped_lock lock(mutex_);
    Update();
    try {
        json.assign("{\"state\":\"");
        json.append(ProtoSyncStateName(StateLocked()));
        json.append("\"");
        const Channel* const found = FindChannel(channel);
        if (found == nullptr) {
            json.append(",\"open\":false}");
            return true;
        }
        const bool reset = since < found->removalFloor;
        json.append(",\"open\":true,\"revision\":");
        json.append(std::to_string(found->revision));
        json.append(reset ? ",\"reset\":true" : ",\"reset\":false");
        json.append(",\"members\":[");
        bool first = true;
        for (const Member& member : found->members) {
            if (!reset && member.revision <= since) {
                continue;
            }
            json.append(first ? "{\"steamId\":" : ",{\"steamId\":");
            first = false;
            AppendSteamId(member.steamId, json);
            json.append(",\"playerId\":");
            json.append(std::to_string(member.playerId));
            json.append(",\"data\":");
            json::AppendString(member.data, json);
            json.append("}");
        }
        json.append("],\"removed\":[");
        first = true;
        for (const Removal& removal : found->removals) {
            if (reset || removal.revision <= since) {
                continue;
            }
            if (!first) {
                json.append(",");
            }
            first = false;
            AppendSteamId(removal.steamId, json);
        }
        json.append("]}");
    } catch (...) {
        json.clear();
        return false;
    }
    return true;
}

ProtoSync::Channel* ProtoSync::FindChannel(std::string_view name) noexcept {
    for (Channel& channel : channels_) {
        if (channel.name == name) {
            return &channel;
        }
    }
    return nullptr;
}

void ProtoSync::Update() noexcept {
    if (!enabled_ || !hasAddresses_) {
        return;
    }
    if (channels_.empty()) {
        lobby_.Update({});
        return;
    }
    const std::uint64_t now = ::GetTickCount64();
    StartLobbyIfLoaded(now);
    if (!lobby_.IsStarted()) {
        return;
    }
    UpdateWorld(now);
    lobby_.Update(inWorld_ ? std::string_view{lobbyName_} : std::string_view{});
    ApplyMembers();
}

void ProtoSync::StartLobbyIfLoaded(std::uint64_t now) noexcept {
    if (lobby_.IsStarted() || lobbyFailed_ || now < nextLobbyStartTime_) {
        return;
    }
    if (::GetModuleHandleW(L"steam_api64.dll") == nullptr) {
        nextLobbyStartTime_ = now + kLobbyStartInterval;
        return;
    }
    if (!lobby_.Start()) {
        lobbyFailed_ = true;
        channels_.clear();
        return;
    }
    WriteReadKeys();
    WriteIdentity();
}

void ProtoSync::UpdateWorld(std::uint64_t now) noexcept {
    if (now < nextWorldReadTime_) {
        return;
    }
    nextWorldReadTime_ = now + kWorldReadInterval;

    std::vector<WorldPlayer> players;
    if (ReadWorldPlayers(addresses_, players)) {
        const bool same = players.size() == players_.size() &&
            std::equal(players.begin(), players.end(), players_.begin(),
                [](const WorldPlayer& left, const WorldPlayer& right) {
                    return left.playerId == right.playerId &&
                        left.accountKey == right.accountKey &&
                        left.nameKey == right.nameKey && left.isLocal == right.isLocal;
                });
        if (!same) {
            players_ = std::move(players);
            ++playersRevision_;
        }
    }
    const auto local = std::find_if(players_.begin(), players_.end(),
        [](const WorldPlayer& player) { return player.isLocal; });
    const std::uint64_t worldId = ReadJoinedWorldId(addresses_);
    // The world ID outlives the world, so the local actor is required too.
    const bool inWorld = worldId != 0 && local != players_.end();
    if (inWorld != inWorld_ || worldId != worldId_) {
        DiagnosticLog(inWorld ? "xScal protoSync: in a world" : "xScal protoSync: not in a world");
    }
    inWorld_ = inWorld;
    worldId_ = worldId;

    std::array<char, 40> text{};
    (void)std::snprintf(text.data(), text.size(), "w%016llx",
        static_cast<unsigned long long>(WorldKey(worldId)));
    if (lobbyName_ != text.data()) {
        try {
            lobbyName_.assign(text.data());
        } catch (...) {
            lobbyName_.clear();
        }
    }
    if (local != players_.end()) {
        (void)std::snprintf(text.data(), text.size(), "%016llx%016llx",
            static_cast<unsigned long long>(local->accountKey),
            static_cast<unsigned long long>(local->nameKey));
        if (identity_ != text.data()) {
            try {
                identity_.assign(text.data());
            } catch (...) {
                identity_.clear();
            }
            WriteIdentity();
        }
    }
}

void ProtoSync::WriteIdentity() noexcept {
    (void)lobby_.SetValue(kIdentityKey, identity_);
}

void ProtoSync::WriteReadKeys() noexcept {
    try {
        std::vector<std::string> keys;
        keys.reserve(channels_.size() + 1);
        keys.emplace_back(kIdentityKey);
        for (const Channel& channel : channels_) {
            keys.push_back(channel.key);
        }
        (void)lobby_.SetReadKeys(keys);
    } catch (...) {
    }
}

// Remote Steam players have no SteamID in their record, so the account key is
// matched first and the name key second.
std::uint32_t ProtoSync::FindPlayerId(std::string_view identity) const noexcept {
    std::uint64_t accountKey{};
    std::uint64_t nameKey{};
    if (identity.size() != 32 || !ParseHexKey(identity.substr(0, 16), accountKey) ||
        !ParseHexKey(identity.substr(16), nameKey)) {
        return 0;
    }
    for (const WorldPlayer& player : players_) {
        if (!player.isLocal && accountKey != 0 && player.accountKey == accountKey) {
            return player.playerId;
        }
    }
    for (const WorldPlayer& player : players_) {
        if (!player.isLocal && nameKey != 0 && player.nameKey == nameKey) {
            return player.playerId;
        }
    }
    return 0;
}

void ProtoSync::RemoveMember(Channel& channel, std::size_t index) noexcept {
    const std::uint64_t revision = nextRevision_++;
    const std::uint64_t steamId = channel.members[index].steamId;
    channel.members.erase(channel.members.begin() + static_cast<std::ptrdiff_t>(index));
    channel.revision = revision;
    try {
        if (channel.removals.size() >= kMaximumRemovals) {
            channel.removalFloor = channel.removals.front().revision;
            channel.removals.erase(channel.removals.begin());
        }
        channel.removals.push_back({steamId, revision});
    } catch (...) {
        channel.removalFloor = revision;
    }
}

void ProtoSync::ApplyMembers() noexcept {
    if (lobby_.MembersRevision() == appliedMembersRevision_ &&
        playersRevision_ == appliedPlayersRevision_) {
        return;
    }
    appliedMembersRevision_ = lobby_.MembersRevision();
    appliedPlayersRevision_ = playersRevision_;
    const std::vector<SteamLobby::Member>& lobbyMembers = lobby_.Members();

    for (Channel& channel : channels_) {
        for (std::size_t index = channel.members.size(); index-- > 0;) {
            const std::uint64_t steamId = channel.members[index].steamId;
            const auto source = std::find_if(lobbyMembers.begin(), lobbyMembers.end(),
                [&](const SteamLobby::Member& member) { return member.steamId == steamId; });
            const std::string* const data =
                source != lobbyMembers.end() ? FindValue(*source, channel.key) : nullptr;
            if (data == nullptr || !json::IsValidUtf8(*data)) {
                RemoveMember(channel, index);
            }
        }
        for (const SteamLobby::Member& source : lobbyMembers) {
            const std::string* const data = FindValue(source, channel.key);
            if (data == nullptr || !json::IsValidUtf8(*data)) {
                continue;
            }
            const std::string* const identity = FindValue(source, kIdentityKey);
            const std::uint32_t playerId =
                identity != nullptr ? FindPlayerId(*identity) : 0;
            auto member = std::find_if(channel.members.begin(), channel.members.end(),
                [&](const Member& entry) { return entry.steamId == source.steamId; });
            if (member != channel.members.end() && member->data == *data &&
                member->playerId == playerId) {
                continue;
            }
            const std::uint64_t revision = nextRevision_++;
            try {
                if (member == channel.members.end()) {
                    member = channel.members.insert(channel.members.end(), Member{});
                    member->steamId = source.steamId;
                }
                member->data = *data;
            } catch (...) {
                continue;
            }
            member->playerId = playerId;
            member->revision = revision;
            channel.revision = revision;
            std::erase_if(channel.removals,
                [&](const Removal& removal) { return removal.steamId == source.steamId; });
        }
    }
}

ProtoSync& GlobalProtoSync() noexcept {
    static ProtoSync protoSync;
    return protoSync;
}

}  // namespace sf::sync
