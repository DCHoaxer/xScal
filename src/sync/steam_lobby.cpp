#include "sync/steam_lobby.hpp"

#include "platform/diagnostics.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <utility>

namespace sf::sync {
namespace {

// isteammatchmaking.h values.
constexpr int kLobbyTypeInvisible = 3;
constexpr int kLobbyDistanceWorldwide = 3;
constexpr int kLobbyComparisonEqual = 0;
constexpr int kLobbyEnterCallback = 504;
constexpr int kLobbyDataUpdateCallback = 505;
constexpr int kLobbyChatUpdateCallback = 506;
constexpr int kLobbyMatchListCallback = 510;
constexpr int kLobbyCreatedCallback = 513;
constexpr int kResultOk = 1;
constexpr std::uint32_t kChatRoomEnterSuccess = 1;
constexpr int kMaximumMembers = 250;

constexpr char kLobbyKey[] = "xscal_proto_sync";

constexpr std::uint64_t kPollInterval = 250;
constexpr std::uint64_t kCallTimeout = 30000;
constexpr std::uint64_t kAbandonedCallTimeout = 120000;
constexpr std::uint64_t kRetryDelay = 60000;
constexpr std::uint64_t kMergeSearchInterval = 300000;
constexpr std::uint64_t kMemberReadInterval = 2000;
constexpr std::uint64_t kFullMemberReadInterval = 60000;
constexpr std::uint64_t kWriteInterval = 2000;

struct LobbyEnter final {
    std::uint64_t lobbyId;
    std::uint32_t chatPermissions;
    bool locked;
    std::uint32_t response;
};
struct LobbyMatchList final {
    std::uint32_t lobbyCount;
};
struct LobbyCreated final {
    std::int32_t result;
    std::uint64_t lobbyId;
};
struct LobbyDataUpdate final {
    std::uint64_t lobbyId;
    std::uint64_t memberId;
    std::uint8_t success;
};
struct LobbyChatUpdate final {
    std::uint64_t lobbyId;
    std::uint64_t changedId;
    std::uint64_t makingChangeId;
    std::uint32_t stateChange;
};
static_assert(sizeof(LobbyEnter) == 24);
static_assert(sizeof(LobbyMatchList) == 4);
static_assert(sizeof(LobbyCreated) == 16);
static_assert(sizeof(LobbyDataUpdate) == 24);
static_assert(sizeof(LobbyChatUpdate) == 32);

template <class Function>
[[nodiscard]] bool LoadExport(HMODULE module, const char* name, Function& function) noexcept {
    function = reinterpret_cast<Function>(::GetProcAddress(module, name));
    return function != nullptr;
}

}  // namespace

bool SteamLobby::Start() noexcept {
    if (!LoadApi()) {
        matchmaking_ = nullptr;
        return false;
    }
    RegisterCallbacks();
    return true;
}

// steam_api64.dll calls vtable offset 8 with the data. MSVC puts the two Run
// overloads in reverse order, so offset 0 is Run(data, ioFailure, call).
void SteamLobby::RegisterCallbacks() noexcept {
    static const std::array<const void*, 3> kVtable{
        reinterpret_cast<const void*>(&SteamLobby::RunCallResult),
        reinterpret_cast<const void*>(&SteamLobby::RunCallback),
        reinterpret_cast<const void*>(&SteamLobby::CallbackSize),
    };
    if (registerCallback_ == nullptr) {
        DiagnosticLog(
            "xScal protoSync: steam_api64.dll has no SteamAPI_RegisterCallback; "
            "members are read every 2 seconds");
        return;
    }
    // Flags 1 is k_ECallbackFlagsRegistered.
    dataUpdateCallback_ = {kVtable.data(), 1, kLobbyDataUpdateCallback, this};
    chatUpdateCallback_ = {kVtable.data(), 1, kLobbyChatUpdateCallback, this};
    registerCallback_(&dataUpdateCallback_, kLobbyDataUpdateCallback);
    registerCallback_(&chatUpdateCallback_, kLobbyChatUpdateCallback);
    callbacksRegistered_ = true;
}

void __cdecl SteamLobby::RunCallback(CallbackObject* self, void* data) noexcept {
    if (self == nullptr || self->owner == nullptr || data == nullptr) {
        return;
    }
    if (self->callbackId == kLobbyDataUpdateCallback) {
        const auto* const update = static_cast<const LobbyDataUpdate*>(data);
        self->owner->OnLobbyDataUpdate(update->lobbyId, update->memberId);
    } else if (self->callbackId == kLobbyChatUpdateCallback) {
        self->owner->OnLobbyChatUpdate(static_cast<const LobbyChatUpdate*>(data)->lobbyId);
    }
}

void __cdecl SteamLobby::RunCallResult(
    CallbackObject* self, void* data, bool /*ioFailure*/, std::uint64_t /*call*/) noexcept {
    RunCallback(self, data);
}

int __cdecl SteamLobby::CallbackSize(CallbackObject* self) noexcept {
    if (self != nullptr && self->callbackId == kLobbyChatUpdateCallback) {
        return static_cast<int>(sizeof(LobbyChatUpdate));
    }
    return static_cast<int>(sizeof(LobbyDataUpdate));
}

// Runs inside the game's SteamAPI_RunCallbacks.
void SteamLobby::OnLobbyDataUpdate(std::uint64_t lobby, std::uint64_t member) noexcept {
    if (lobby == 0 || lobby != watchedLobby_.load() || member == lobby) {
        return;
    }
    const std::scoped_lock lock(changesMutex_);
    if (std::find(changedMembers_.begin(), changedMembers_.end(), member) ==
        changedMembers_.end()) {
        try {
            changedMembers_.push_back(member);
        } catch (...) {
            membershipChanged_ = true;
        }
    }
}

void SteamLobby::OnLobbyChatUpdate(std::uint64_t lobby) noexcept {
    if (lobby == 0 || lobby != watchedLobby_.load()) {
        return;
    }
    const std::scoped_lock lock(changesMutex_);
    membershipChanged_ = true;
}

bool SteamLobby::LoadApi() noexcept {
    const HMODULE module = ::GetModuleHandleW(L"steam_api64.dll");
    if (module == nullptr) {
        DiagnosticLog("xScal protoSync: steam_api64.dll is not loaded");
        return false;
    }
    int (*getUser)(){};
    int (*getPipe)(){};
    void* (*getClient)(){};
    void* (*getMatchmaking)(void*, int, int, const char*){};
    void* (*getUtils)(void*, int, const char*){};
    void* (*getUserInterface)(void*, int, int, const char*){};
    std::uint64_t (*getSteamId)(void*){};
    if (!LoadExport(module, "SteamAPI_GetHSteamUser", getUser) ||
        !LoadExport(module, "SteamAPI_GetHSteamPipe", getPipe) ||
        !LoadExport(module, "SteamClient", getClient) ||
        !LoadExport(module, "SteamAPI_ISteamClient_GetISteamMatchmaking", getMatchmaking) ||
        !LoadExport(module, "SteamAPI_ISteamClient_GetISteamUtils", getUtils) ||
        !LoadExport(module, "SteamAPI_ISteamClient_GetISteamUser", getUserInterface) ||
        !LoadExport(module, "SteamAPI_ISteamUser_GetSteamID", getSteamId) ||
        !LoadExport(module, "SteamAPI_ISteamMatchmaking_AddRequestLobbyListStringFilter",
            addStringFilter_) ||
        !LoadExport(module, "SteamAPI_ISteamMatchmaking_AddRequestLobbyListDistanceFilter",
            addDistanceFilter_) ||
        !LoadExport(module,
            "SteamAPI_ISteamMatchmaking_AddRequestLobbyListFilterSlotsAvailable",
            addSlotsFilter_) ||
        !LoadExport(module, "SteamAPI_ISteamMatchmaking_RequestLobbyList", requestLobbyList_) ||
        !LoadExport(module, "SteamAPI_ISteamMatchmaking_GetLobbyByIndex", getLobbyByIndex_) ||
        !LoadExport(module, "SteamAPI_ISteamMatchmaking_CreateLobby", createLobby_) ||
        !LoadExport(module, "SteamAPI_ISteamMatchmaking_JoinLobby", joinLobby_) ||
        !LoadExport(module, "SteamAPI_ISteamMatchmaking_LeaveLobby", leaveLobby_) ||
        !LoadExport(module, "SteamAPI_ISteamMatchmaking_SetLobbyData", setLobbyData_) ||
        !LoadExport(module, "SteamAPI_ISteamMatchmaking_SetLobbyMemberData",
            setLobbyMemberData_) ||
        !LoadExport(module, "SteamAPI_ISteamMatchmaking_GetLobbyMemberData",
            getLobbyMemberData_) ||
        !LoadExport(module, "SteamAPI_ISteamMatchmaking_GetNumLobbyMembers",
            getNumLobbyMembers_) ||
        !LoadExport(module, "SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex",
            getLobbyMemberByIndex_) ||
        !LoadExport(module, "SteamAPI_ISteamUtils_IsAPICallCompleted", isCallCompleted_) ||
        !LoadExport(module, "SteamAPI_ISteamUtils_GetAPICallResult", getCallResult_)) {
        DiagnosticLog("xScal protoSync: steam_api64.dll lacks a lobby export");
        return false;
    }
    (void)LoadExport(module, "SteamAPI_RegisterCallback", registerCallback_);

    void* const client = getClient();
    const int user = getUser();
    const int pipe = getPipe();
    if (client == nullptr || user == 0 || pipe == 0) {
        DiagnosticLog("xScal protoSync: the Steam client is not running");
        return false;
    }
    void* const matchmaking = getMatchmaking(client, user, pipe, "SteamMatchMaking009");
    utils_ = getUtils(client, pipe, "SteamUtils009");
    void* const steamUser = getUserInterface(client, user, pipe, "SteamUser020");
    if (matchmaking == nullptr || utils_ == nullptr || steamUser == nullptr) {
        DiagnosticLog(
            "xScal protoSync: Steam has no SteamMatchMaking009, SteamUtils009 or "
            "SteamUser020 interface");
        return false;
    }
    steamId_ = getSteamId(steamUser);
    if (steamId_ == 0) {
        DiagnosticLog("xScal protoSync: the SteamID is 0");
        return false;
    }
    matchmaking_ = matchmaking;
    return true;
}

void SteamLobby::Update(std::string_view name) noexcept {
    if (matchmaking_ == nullptr) {
        return;
    }
    const std::uint64_t now = ::GetTickCount64();
    if (name != name_) {
        if (step_ != Step::idle) {
            Leave();
            step_ = Step::idle;
        }
        try {
            name_.assign(name);
        } catch (...) {
            name_.clear();
            return;
        }
        nextPollTime_ = 0;
    }
    if (now < nextPollTime_) {
        return;
    }
    nextPollTime_ = now + kPollInterval;
    LeaveAbandonedLobby(now);
    if (name_.empty()) {
        return;
    }

    switch (step_) {
    case Step::idle:
        StartSearch();
        return;
    case Step::waitingToRetry:
        if (now >= stepTime_ + kRetryDelay) {
            StartSearch();
        }
        return;
    case Step::searching:
        FinishSearch();
        break;
    case Step::joining:
        FinishJoin();
        break;
    case Step::creating:
        FinishCreate();
        break;
    case Step::inLobby:
        if (pendingCall_ != 0) {
            FinishSearch();
            if (pendingCall_ != 0 && now >= stepTime_ + kCallTimeout) {
                pendingCall_ = 0;
            }
        } else if (now >= nextMergeSearchTime_) {
            nextMergeSearchTime_ = now + kMergeSearchInterval;
            StartSearch();
        }
        if (step_ == Step::inLobby) {
            WriteValues();
            if (now >= nextMemberReadTime_) {
                nextMemberReadTime_ =
                    now + (callbacksRegistered_ ? kFullMemberReadInterval : kMemberReadInterval);
                {
                    const std::scoped_lock lock(changesMutex_);
                    changedMembers_.clear();
                    membershipChanged_ = false;
                }
                ReadMembers();
            } else if (callbacksRegistered_) {
                ReadChangedMembers();
            }
        }
        return;
    }

    if (pendingCall_ != 0 && now >= stepTime_ + kCallTimeout) {
        RetryLater("xScal protoSync: a Steam lobby call did not complete in 30 seconds");
    }
}

void SteamLobby::LeaveAbandonedLobby(std::uint64_t now) noexcept {
    if (abandonedCall_ == 0) {
        return;
    }
    bool failed = false;
    if (!isCallCompleted_(utils_, abandonedCall_, &failed)) {
        if (now >= abandonedTime_ + kAbandonedCallTimeout) {
            abandonedCall_ = 0;
        }
        return;
    }
    std::uint64_t lobby = 0;
    if (abandonedCallbackId_ == kLobbyEnterCallback) {
        LobbyEnter entered{};
        if (getCallResult_(utils_, abandonedCall_, &entered, sizeof(entered),
                kLobbyEnterCallback, &failed) && !failed &&
            entered.response == kChatRoomEnterSuccess) {
            lobby = entered.lobbyId;
        }
    } else {
        LobbyCreated created{};
        if (getCallResult_(utils_, abandonedCall_, &created, sizeof(created),
                kLobbyCreatedCallback, &failed) && !failed && created.result == kResultOk) {
            lobby = created.lobbyId;
        }
    }
    abandonedCall_ = 0;
    if (lobby != 0 && lobby != lobby_) {
        leaveLobby_(matchmaking_, lobby);
    }
}

bool SteamLobby::SetValue(std::string_view key, std::string_view text) noexcept {
    const auto entry = std::find_if(values_.begin(), values_.end(),
        [&](const WrittenValue& value) { return value.key == key; });
    try {
        if (entry != values_.end()) {
            entry->text.assign(text);
        } else {
            WrittenValue& value = values_.emplace_back();
            value.key.assign(key);
            value.text.assign(text);
        }
    } catch (...) {
        return false;
    }
    return true;
}

bool SteamLobby::SetReadKeys(const std::vector<std::string>& keys) noexcept {
    if (keys == readKeys_) {
        return true;
    }
    try {
        readKeys_ = keys;
    } catch (...) {
        return false;
    }
    nextMemberReadTime_ = 0;
    return true;
}

bool SteamLobby::ReadCallResult(
    void* result, int size, int callbackId, bool& failed) noexcept {
    bool callFailed = false;
    if (!isCallCompleted_(utils_, pendingCall_, &callFailed)) {
        return false;
    }
    failed = !getCallResult_(utils_, pendingCall_, result, size, callbackId, &callFailed) ||
        callFailed;
    pendingCall_ = 0;
    return true;
}

void SteamLobby::StartSearch() noexcept {
    addStringFilter_(matchmaking_, kLobbyKey, name_.c_str(), kLobbyComparisonEqual);
    addDistanceFilter_(matchmaking_, kLobbyDistanceWorldwide);
    addSlotsFilter_(matchmaking_, 1);
    pendingCall_ = requestLobbyList_(matchmaking_);
    stepTime_ = ::GetTickCount64();
    if (pendingCall_ == 0) {
        RetryLater("xScal protoSync: RequestLobbyList returned no call");
        return;
    }
    if (step_ != Step::inLobby) {
        step_ = Step::searching;
    }
}

void SteamLobby::FinishSearch() noexcept {
    LobbyMatchList list{};
    bool failed{};
    if (!ReadCallResult(&list, sizeof(list), kLobbyMatchListCallback, failed)) {
        return;
    }
    const bool inLobby = step_ == Step::inLobby;
    if (failed) {
        if (!inLobby) {
            RetryLater("xScal protoSync: the lobby search failed");
        }
        return;
    }
    std::uint64_t lowest = 0;
    for (std::uint32_t index = 0; index < list.lobbyCount; ++index) {
        const std::uint64_t lobby = getLobbyByIndex_(matchmaking_, static_cast<int>(index));
        if (lobby != 0 && (lowest == 0 || lobby < lowest)) {
            lowest = lobby;
        }
    }

    if (inLobby) {
        // Lobbies created at the same moment merge into the lowest ID.
        if (lowest == 0 || lowest >= lobby_) {
            return;
        }
        DiagnosticLog("xScal protoSync: moving to a lobby with a lower ID");
        Leave();
    }
    stepTime_ = ::GetTickCount64();
    if (lowest != 0) {
        pendingCall_ = joinLobby_(matchmaking_, lowest);
        step_ = Step::joining;
    } else {
        pendingCall_ = createLobby_(matchmaking_, kLobbyTypeInvisible, kMaximumMembers);
        step_ = Step::creating;
    }
    if (pendingCall_ == 0) {
        RetryLater("xScal protoSync: JoinLobby or CreateLobby returned no call");
    }
}

void SteamLobby::FinishJoin() noexcept {
    LobbyEnter entered{};
    bool failed{};
    if (!ReadCallResult(&entered, sizeof(entered), kLobbyEnterCallback, failed)) {
        return;
    }
    if (failed || entered.response != kChatRoomEnterSuccess || entered.lobbyId == 0) {
        DiagnosticLogFormat(
            "xScal protoSync: joining the lobby failed (response %u)",
            failed ? 0U : entered.response);
        RetryLater(nullptr);
        return;
    }
    DiagnosticLog("xScal protoSync: joined the lobby");
    EnterLobby(entered.lobbyId);
}

void SteamLobby::FinishCreate() noexcept {
    LobbyCreated created{};
    bool failed{};
    if (!ReadCallResult(&created, sizeof(created), kLobbyCreatedCallback, failed)) {
        return;
    }
    if (failed || created.result != kResultOk || created.lobbyId == 0) {
        DiagnosticLogFormat(
            "xScal protoSync: creating the lobby failed (result %d)",
            failed ? 0 : created.result);
        RetryLater(nullptr);
        return;
    }
    if (!setLobbyData_(matchmaking_, created.lobbyId, kLobbyKey, name_.c_str())) {
        DiagnosticLog("xScal protoSync: SetLobbyData failed on the new lobby; others cannot find it");
    }
    DiagnosticLog("xScal protoSync: created the lobby");
    EnterLobby(created.lobbyId);
}

void SteamLobby::EnterLobby(std::uint64_t lobby) noexcept {
    lobby_ = lobby;
    watchedLobby_.store(lobby);
    step_ = Step::inLobby;
    for (WrittenValue& value : values_) {
        value.written = false;
        value.nextWriteTime = 0;
    }
    nextMemberReadTime_ = 0;
    nextMergeSearchTime_ = ::GetTickCount64() + kMergeSearchInterval;
}

void SteamLobby::Leave() noexcept {
    if (lobby_ != 0) {
        leaveLobby_(matchmaking_, lobby_);
        DiagnosticLog("xScal protoSync: left the lobby");
    }
    // A join or create still running is read later, so the lobby it enters is left.
    if (pendingCall_ != 0 && (step_ == Step::joining || step_ == Step::creating)) {
        abandonedCall_ = pendingCall_;
        abandonedCallbackId_ = step_ == Step::joining ? kLobbyEnterCallback : kLobbyCreatedCallback;
        abandonedTime_ = ::GetTickCount64();
    }
    lobby_ = 0;
    watchedLobby_.store(0);
    pendingCall_ = 0;
    if (!members_.empty()) {
        members_.clear();
        ++membersRevision_;
    }
    for (WrittenValue& value : values_) {
        value.written = false;
    }
}

void SteamLobby::RetryLater(const char* reason) noexcept {
    if (reason != nullptr) {
        DiagnosticLog(reason);
    }
    Leave();
    step_ = Step::waitingToRetry;
    stepTime_ = ::GetTickCount64();
}

void SteamLobby::WriteValues() noexcept {
    const std::uint64_t now = ::GetTickCount64();
    for (WrittenValue& value : values_) {
        if ((value.written && value.text == value.writtenText) || now < value.nextWriteTime) {
            continue;
        }
        if (!value.written && value.text.empty() && value.writtenText.empty()) {
            value.written = true;
            continue;
        }
        setLobbyMemberData_(matchmaking_, lobby_, value.key.c_str(), value.text.c_str());
        try {
            value.writtenText = value.text;
        } catch (...) {
            continue;
        }
        value.written = true;
        value.nextWriteTime = now + kWriteInterval;
    }
}

bool SteamLobby::ReadMemberValues(std::uint64_t member, Member& entry) noexcept {
    std::size_t count = 0;
    for (const std::string& key : readKeys_) {
        const char* const text = getLobbyMemberData_(matchmaking_, lobby_, member, key.c_str());
        if (text == nullptr || text[0] == '\0') {
            continue;
        }
        try {
            if (count == entry.values.size()) {
                entry.values.emplace_back();
            }
            entry.values[count].key.assign(key);
            entry.values[count].text.assign(text);
        } catch (...) {
            break;
        }
        ++count;
    }
    entry.values.resize((std::min)(count, entry.values.size()));
    return count != 0;
}

void SteamLobby::ReadMembers() noexcept {
    const int count = getNumLobbyMembers_(matchmaking_, lobby_);
    if (count <= 0) {
        // The count includes this client, so 0 means Steam dropped it.
        DiagnosticLog("xScal protoSync: no longer in the lobby; joining again");
        Leave();
        step_ = Step::idle;
        return;
    }
    std::vector<Member> members;
    try {
        members.reserve(static_cast<std::size_t>(count));
    } catch (...) {
        return;
    }
    for (int index = 0; index < count; ++index) {
        const std::uint64_t member = getLobbyMemberByIndex_(matchmaking_, lobby_, index);
        if (member == 0 || member == steamId_) {
            continue;
        }
        Member entry;
        entry.steamId = member;
        const auto previous = std::find_if(members_.begin(), members_.end(),
            [&](const Member& old) { return old.steamId == member; });
        if (previous != members_.end()) {
            entry.values = std::move(previous->values);
        }
        if (ReadMemberValues(member, entry)) {
            members.push_back(std::move(entry));
        }
    }
    members_ = std::move(members);
    ++membersRevision_;
}

void SteamLobby::ReadChangedMembers() noexcept {
    bool membershipChanged = false;
    changesInUse_.clear();
    {
        const std::scoped_lock lock(changesMutex_);
        changesInUse_.swap(changedMembers_);
        membershipChanged = membershipChanged_;
        membershipChanged_ = false;
    }
    if (membershipChanged) {
        ReadMembers();
        return;
    }
    for (const std::uint64_t member : changesInUse_) {
        ReadMember(member);
    }
}

void SteamLobby::ReadMember(std::uint64_t member) noexcept {
    if (member == 0 || member == steamId_) {
        return;
    }
    const auto entry = std::find_if(members_.begin(), members_.end(),
        [&](const Member& old) { return old.steamId == member; });
    Member updated;
    updated.steamId = member;
    if (entry != members_.end()) {
        updated.values = entry->values;
    }
    const bool hasValues = ReadMemberValues(member, updated);
    if (!hasValues) {
        if (entry != members_.end()) {
            members_.erase(entry);
            ++membersRevision_;
        }
        return;
    }
    if (entry != members_.end()) {
        const bool same = entry->values.size() == updated.values.size() &&
            std::equal(entry->values.begin(), entry->values.end(), updated.values.begin(),
                [](const Value& left, const Value& right) {
                    return left.key == right.key && left.text == right.text;
                });
        if (same) {
            return;
        }
        *entry = std::move(updated);
    } else {
        try {
            members_.push_back(std::move(updated));
        } catch (...) {
            return;
        }
    }
    ++membersRevision_;
}

}  // namespace sf::sync
