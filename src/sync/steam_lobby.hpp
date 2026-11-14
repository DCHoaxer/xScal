#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace sf::sync {

// Not thread-safe; ProtoSync calls it under its mutex. The Steam callbacks only
// queue changes. Steam keeps the callback objects until the process ends, so a
// started SteamLobby must not be destroyed.
class SteamLobby final {
public:
    struct Value final {
        std::string key;
        std::string text;
    };
    struct Member final {
        std::uint64_t steamId{};
        std::vector<Value> values;
    };

    [[nodiscard]] bool Start() noexcept;
    [[nodiscard]] bool IsStarted() const noexcept { return matchmaking_ != nullptr; }
    [[nodiscard]] std::uint64_t LocalSteamId() const noexcept { return steamId_; }

    void Update(std::string_view name) noexcept;
    [[nodiscard]] bool IsInLobby() const noexcept { return step_ == Step::inLobby; }

    [[nodiscard]] bool SetValue(std::string_view key, std::string_view text) noexcept;
    [[nodiscard]] bool SetReadKeys(const std::vector<std::string>& keys) noexcept;

    [[nodiscard]] const std::vector<Member>& Members() const noexcept { return members_; }
    [[nodiscard]] std::uint64_t MembersRevision() const noexcept { return membersRevision_; }

private:
    enum class Step {
        idle,
        searching,
        joining,
        creating,
        inLobby,
        waitingToRetry,
    };

    // CCallbackBase layout: vtable, flags at +8, callback ID at +12.
    struct CallbackObject final {
        const void* const* vtable{};
        std::uint8_t flags{};
        std::int32_t callbackId{};
        SteamLobby* owner{};
    };

    struct WrittenValue final {
        std::string key;
        std::string text;
        std::string writtenText;
        bool written{};
        std::uint64_t nextWriteTime{};
    };

    [[nodiscard]] bool LoadApi() noexcept;
    [[nodiscard]] bool ReadCallResult(
        void* result, int size, int callbackId, bool& failed) noexcept;
    void StartSearch() noexcept;
    void FinishSearch() noexcept;
    void FinishJoin() noexcept;
    void FinishCreate() noexcept;
    void EnterLobby(std::uint64_t lobby) noexcept;
    void Leave() noexcept;
    void LeaveAbandonedLobby(std::uint64_t now) noexcept;
    void RetryLater(const char* reason) noexcept;
    void WriteValues() noexcept;
    void ReadMembers() noexcept;
    void ReadMember(std::uint64_t member) noexcept;
    void ReadChangedMembers() noexcept;
    [[nodiscard]] bool ReadMemberValues(std::uint64_t member, Member& entry) noexcept;
    void RegisterCallbacks() noexcept;

    static void __cdecl RunCallback(CallbackObject* self, void* data) noexcept;
    static void __cdecl RunCallResult(
        CallbackObject* self, void* data, bool ioFailure, std::uint64_t call) noexcept;
    static int __cdecl CallbackSize(CallbackObject* self) noexcept;
    void OnLobbyDataUpdate(std::uint64_t lobby, std::uint64_t member) noexcept;
    void OnLobbyChatUpdate(std::uint64_t lobby) noexcept;

    void* matchmaking_{};
    void* utils_{};
    void (*addStringFilter_)(void*, const char*, const char*, int){};
    void (*addDistanceFilter_)(void*, int){};
    void (*addSlotsFilter_)(void*, int){};
    std::uint64_t (*requestLobbyList_)(void*){};
    std::uint64_t (*getLobbyByIndex_)(void*, int){};
    std::uint64_t (*createLobby_)(void*, int, int){};
    std::uint64_t (*joinLobby_)(void*, std::uint64_t){};
    void (*leaveLobby_)(void*, std::uint64_t){};
    bool (*setLobbyData_)(void*, std::uint64_t, const char*, const char*){};
    void (*setLobbyMemberData_)(void*, std::uint64_t, const char*, const char*){};
    const char* (*getLobbyMemberData_)(void*, std::uint64_t, std::uint64_t, const char*){};
    int (*getNumLobbyMembers_)(void*, std::uint64_t){};
    std::uint64_t (*getLobbyMemberByIndex_)(void*, std::uint64_t, int){};
    bool (*isCallCompleted_)(void*, std::uint64_t, bool*){};
    bool (*getCallResult_)(void*, std::uint64_t, void*, int, int, bool*){};
    void (*registerCallback_)(CallbackObject*, int){};

    std::uint64_t steamId_{};
    std::string name_;
    Step step_{Step::idle};
    std::uint64_t stepTime_{};
    std::uint64_t nextPollTime_{};
    std::uint64_t pendingCall_{};
    std::uint64_t lobby_{};
    std::uint64_t abandonedCall_{};
    int abandonedCallbackId_{};
    std::uint64_t abandonedTime_{};
    std::uint64_t nextMergeSearchTime_{};
    std::uint64_t nextMemberReadTime_{};
    std::vector<WrittenValue> values_;
    std::vector<std::string> readKeys_;
    std::vector<Member> members_;
    std::uint64_t membersRevision_{};
    CallbackObject dataUpdateCallback_{};
    CallbackObject chatUpdateCallback_{};
    bool callbacksRegistered_{};
    std::atomic<std::uint64_t> watchedLobby_{};
    std::mutex changesMutex_;
    std::vector<std::uint64_t> changedMembers_;
    bool membershipChanged_{};
    std::vector<std::uint64_t> changesInUse_;
};

}  // namespace sf::sync
