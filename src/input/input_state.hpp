#pragma once

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

namespace sf {

class InputState final {
public:
    enum class TextInputStatus { Active, Submitted, Cancelled };
    struct TextInputSnapshot final {
        std::uint32_t sessionId{};
        std::uint32_t revision{};
        std::string text;
        TextInputStatus status{TextInputStatus::Active};
    };
    struct MouseSnapshot final {
        std::int64_t dx{};
        std::int64_t dy{};
        std::int64_t wheel{};
        bool left{};
        bool right{};
        bool leftPressed{};
        bool rightPressed{};
        bool capturing{};
    };
    // Physical XInput state, read before any suppression is applied.
    struct GamepadSnapshot final {
        std::uint16_t buttons{};
        std::uint8_t leftTrigger{};
        std::uint8_t rightTrigger{};
        std::int16_t leftX{};
        std::int16_t leftY{};
        std::int16_t rightX{};
        std::int16_t rightY{};
        bool connected{};
    };
    using KeyStateQuery = bool(*)(std::uint8_t key) noexcept;
    using GamepadStateQuery = bool(*)(
        std::uint32_t userIndex,
        GamepadSnapshot& snapshot) noexcept;
    using GamepadAnalogCommand = bool(*)(std::uint32_t userIndex) noexcept;
    using GamepadButtonsCommand = bool(*)(
        std::uint16_t buttons,
        std::uint32_t userIndex) noexcept;
    using ClearGamepadSuppressionsCommand = bool(*)() noexcept;
    using ForegroundProcessQuery = bool(*)() noexcept;
    using TextInputWindowQuery = void*(*)() noexcept;

    InputState() noexcept;
    ~InputState();
    InputState(
        KeyStateQuery keyStateQuery,
        ForegroundProcessQuery foregroundProcessQuery,
        GamepadStateQuery gamepadStateQuery = nullptr,
        GamepadButtonsCommand suppressGamepadButtons = nullptr,
        GamepadButtonsCommand unsuppressGamepadButtons = nullptr,
        ClearGamepadSuppressionsCommand clearGamepadSuppressions = nullptr,
        TextInputWindowQuery textInputWindowQuery = nullptr,
        GamepadAnalogCommand suppressGamepadAnalog = nullptr,
        GamepadAnalogCommand unsuppressGamepadAnalog = nullptr)
        noexcept;

    [[nodiscard]] bool RegisterKey(std::uint8_t key) noexcept;
    [[nodiscard]] bool UnregisterKey(std::uint8_t key) noexcept;
    void ClearKeys() noexcept;
    [[nodiscard]] bool IsKeyPressed(std::uint8_t key) noexcept;

    [[nodiscard]] bool RegisterGamepadButtons(
        std::uint16_t buttons,
        std::uint32_t userIndex) noexcept;
    [[nodiscard]] bool UnregisterGamepadButtons(
        std::uint16_t buttons,
        std::uint32_t userIndex) noexcept;
    void ClearGamepadButtons() noexcept;
    [[nodiscard]] bool AreGamepadButtonsPressed(
        std::uint16_t buttons,
        std::uint32_t userIndex) noexcept;

    [[nodiscard]] bool SuppressGamepadButtons(
        std::uint16_t buttons,
        std::uint32_t userIndex) noexcept;
    [[nodiscard]] bool UnsuppressGamepadButtons(
        std::uint16_t buttons,
        std::uint32_t userIndex) noexcept;
    [[nodiscard]] bool ClearGamepadSuppressions() noexcept;

    // Zeroes the triggers and sticks the game reads for one controller.
    // Buttons are suppressed separately.
    [[nodiscard]] bool SuppressGamepadAnalog(std::uint32_t userIndex) noexcept;
    [[nodiscard]] bool UnsuppressGamepadAnalog(
        std::uint32_t userIndex) noexcept;
    // Returns an unconnected snapshot while Fallout is not the foreground
    // process, like AreGamepadButtonsPressed.
    [[nodiscard]] GamepadSnapshot PollGamepad(std::uint32_t userIndex) noexcept;

    // Hides a key from Fallout's window messages and raw input. IsKeyPressed
    // still reports it because GetAsyncKeyState does not depend on messages.
    [[nodiscard]] bool SuppressKey(std::uint8_t key) noexcept;
    [[nodiscard]] bool UnsuppressKey(std::uint8_t key) noexcept;
    [[nodiscard]] bool ClearKeySuppressions() noexcept;

    // While captured, WM_INPUT mouse records are added to the totals returned
    // by PollMouse and are not passed to Fallout. Relative motion only:
    // records with MOUSE_MOVE_ABSOLUTE (tablets, remote desktop) add no
    // motion. Button state is the physical button, as in GetAsyncKeyState.
    [[nodiscard]] bool BeginMouseCapture() noexcept;
    // Returns the totals since the previous poll and resets them. The
    // pressed flags report a button-down seen since the previous poll.
    [[nodiscard]] MouseSnapshot PollMouse() noexcept;
    [[nodiscard]] bool EndMouseCapture() noexcept;

    [[nodiscard]] std::uint32_t BeginTextInput() noexcept;
    [[nodiscard]] bool PollTextInput(
        std::uint32_t sessionId, TextInputSnapshot& snapshot) noexcept;
    [[nodiscard]] bool EndTextInput(std::uint32_t sessionId) noexcept;
    [[nodiscard]] bool FilterKeyboardMessage(
        unsigned int message,
        std::uintptr_t wParam,
        std::intptr_t lParam) noexcept;

private:
    [[nodiscard]] bool StartSubclassInstall(void* window) noexcept;
    void TakeSubclassInstallResult(bool abandon) noexcept;
    [[nodiscard]] bool FilterSuppressedKey(
        unsigned int message,
        std::uintptr_t wParam,
        std::intptr_t lParam) noexcept;
    [[nodiscard]] bool FilterSuppressedRawInput(std::intptr_t lParam) noexcept;
    [[nodiscard]] bool FilterMouseRawInput(std::intptr_t lParam) noexcept;
    [[nodiscard]] bool InstallSubclassIfMissing() noexcept;

    static constexpr std::size_t kGamepadCount = 4;
    static constexpr std::size_t kGamepadButtonMaskCount = 65536;

    KeyStateQuery keyStateQuery_{};
    ForegroundProcessQuery foregroundProcessQuery_{};
    GamepadStateQuery gamepadStateQuery_{};
    GamepadButtonsCommand suppressGamepadButtons_{};
    GamepadButtonsCommand unsuppressGamepadButtons_{};
    ClearGamepadSuppressionsCommand clearGamepadSuppressions_{};
    TextInputWindowQuery textInputWindowQuery_{};
    GamepadAnalogCommand suppressGamepadAnalog_{};
    GamepadAnalogCommand unsuppressGamepadAnalog_{};
    std::mutex mutex_;
    std::bitset<256> registeredKeys_;
    std::array<std::bitset<kGamepadButtonMaskCount>, kGamepadCount>
        registeredGamepadButtonMasks_;
    void* subclassWindow_{};
    void* pendingSubclassWindow_{};
    bool subclassInstallPending_{};
    std::uint32_t textInputSession_{};
    std::uint32_t nextTextInputSession_{1};
    std::uint32_t textInputRevision_{};
    TextInputStatus textInputStatus_{TextInputStatus::Active};
    std::wstring textInputText_;
    std::size_t textInputCursor_{};
    wchar_t pendingHighSurrogate_{};
    std::bitset<256> textInputKeysDown_;
    std::bitset<256> textInputReleaseKeys_;
    std::bitset<256> suppressedKeys_;
    // Keys whose WM_KEYDOWN / raw make code was consumed. Their key-up is
    // consumed even after the key is unsuppressed.
    std::bitset<256> consumedDownKeys_;
    std::bitset<256> consumedRawDownKeys_;
    // Set when a suppressed key-down was consumed. The WM_CHAR that
    // TranslateMessage posted for it is consumed next.
    bool swallowNextChar_{};
    bool mouseCapturing_{};
    MouseSnapshot mouse_;
};

}
