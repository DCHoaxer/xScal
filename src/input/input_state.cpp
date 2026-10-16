#include "input/input_state.hpp"

#include "input/xinput_hook.hpp"

#include <Windows.h>
#include <CommCtrl.h>
#include <Xinput.h>

#include <algorithm>
#include <atomic>
#include <new>

namespace sf {
namespace {

constexpr LPARAM kSubclassDispatchMarker = 1;

// A pending install stops waiting for the window thread after this time.
constexpr ULONGLONG kSubclassInstallTimeoutMs = 2'000;

// Posted to the window thread to install the subclass there. The request stays
// in pendingSubclassRequest until InputState takes its result. The
// caller never waits for the window thread, because that thread can be waiting
// on the thread that runs the Scaleform callback.
struct SubclassRequest final {
    InputState* input{};
    HWND window{};
    HHOOK hook{};
    ULONGLONG queuedAt{};
    std::atomic<bool> completed{};
    std::atomic<bool> succeeded{};
};

std::mutex subclassDispatchMutex;
std::atomic<SubclassRequest*> pendingSubclassRequest{};

LRESULT CALLBACK KeyboardWindowProcedure(
    HWND window,
    UINT message,
    WPARAM wParam,
    LPARAM lParam,
    UINT_PTR,
    DWORD_PTR referenceData) {
    if (message == WM_NULL && lParam == kSubclassDispatchMarker) {
        return DefSubclassProc(window, message, wParam, lParam);
    }
    auto* const input = reinterpret_cast<InputState*>(referenceData);
    if (input != nullptr && input->FilterKeyboardMessage(
            message,
            static_cast<std::uintptr_t>(wParam),
            static_cast<std::intptr_t>(lParam))) {
        if (message == WM_INPUT) {
            return DefWindowProcW(window, message, wParam, lParam);
        }
        return message == WM_UNICHAR && wParam == UNICODE_NOCHAR ? TRUE : 0;
    }
    return DefSubclassProc(window, message, wParam, lParam);
}

[[nodiscard]] bool SetKeyboardWindowSubclass(
    HWND window,
    InputState* input) noexcept {
    return ::SetWindowSubclass(
        window,
        &KeyboardWindowProcedure,
        reinterpret_cast<UINT_PTR>(input),
        reinterpret_cast<DWORD_PTR>(input)) != FALSE;
}

// Runs on the window thread when it retrieves the posted WM_NULL.
LRESULT CALLBACK SubclassInstallHook(
    int code, WPARAM wParam, LPARAM lParam) noexcept {
    if (code >= 0 && lParam != 0) {
        const auto* const message = reinterpret_cast<const MSG*>(lParam);
        std::scoped_lock dispatchLock{subclassDispatchMutex};
        SubclassRequest* const request =
            pendingSubclassRequest.load(std::memory_order_relaxed);
        if (request != nullptr && message->hwnd == request->window &&
            message->message == WM_NULL &&
            message->wParam == reinterpret_cast<WPARAM>(request) &&
            message->lParam == kSubclassDispatchMarker &&
            !request->completed.load(std::memory_order_acquire)) {
            request->succeeded.store(
                SetKeyboardWindowSubclass(request->window, request->input),
                std::memory_order_relaxed);
            request->completed.store(true, std::memory_order_release);
        }
    }
    return ::CallNextHookEx(nullptr, code, wParam, lParam);
}

// Queues the subclass install on ownerThread. Returns false when another
// install is pending or the request cannot be posted.
[[nodiscard]] bool QueueSubclassInstall(
    InputState& input,
    HWND window,
    DWORD ownerThread) noexcept {
    std::scoped_lock dispatchLock{subclassDispatchMutex};
    if (pendingSubclassRequest.load(std::memory_order_relaxed) !=
        nullptr) {
        return false;
    }
    auto* const request = new (std::nothrow) SubclassRequest{};
    if (request == nullptr) {
        return false;
    }
    request->input = &input;
    request->window = window;
    request->queuedAt = ::GetTickCount64();
    request->hook = ::SetWindowsHookExW(
        WH_GETMESSAGE,
        &SubclassInstallHook,
        nullptr,
        ownerThread);
    if (request->hook == nullptr) {
        delete request;
        return false;
    }
    pendingSubclassRequest.store(request, std::memory_order_relaxed);
    if (::PostMessageW(
            window,
            WM_NULL,
            reinterpret_cast<WPARAM>(request),
            kSubclassDispatchMarker) == FALSE) {
        pendingSubclassRequest.store(
            nullptr, std::memory_order_relaxed);
        (void)::UnhookWindowsHookEx(request->hook);
        delete request;
        return false;
    }
    return true;
}

[[nodiscard]] bool QueryAsyncKeyState(std::uint8_t key) noexcept {
    return (static_cast<unsigned short>(
                ::GetAsyncKeyState(static_cast<int>(key))) &
            0x8000U) != 0;
}

[[nodiscard]] bool IsCurrentProcessForeground() noexcept {
    const HWND foregroundWindow = ::GetForegroundWindow();
    if (foregroundWindow == nullptr) {
        return false;
    }

    DWORD processId{};
    (void)::GetWindowThreadProcessId(foregroundWindow, &processId);
    return processId == ::GetCurrentProcessId();
}

[[nodiscard]] void* QueryTextInputWindow() noexcept {
    const HWND window = ::GetForegroundWindow();
    if (window == nullptr) {
        return nullptr;
    }

    DWORD processId{};
    return ::GetWindowThreadProcessId(window, &processId) != 0 &&
            processId == ::GetCurrentProcessId()
        ? window
        : nullptr;
}

[[nodiscard]] bool IsKeyboardRawInput(std::intptr_t lParam) noexcept {
    RAWINPUTHEADER header{};
    UINT size = sizeof(header);
    return lParam != 0 &&
        ::GetRawInputData(
            reinterpret_cast<HRAWINPUT>(lParam),
            RID_HEADER,
            &header,
            &size,
            sizeof(header)) == sizeof(header) &&
        header.dwType == RIM_TYPEKEYBOARD;
}

[[nodiscard]] constexpr bool IsHighSurrogate(wchar_t value) noexcept {
    return value >= 0xD800 && value <= 0xDBFF;
}

[[nodiscard]] constexpr bool IsLowSurrogate(wchar_t value) noexcept {
    return value >= 0xDC00 && value <= 0xDFFF;
}

[[nodiscard]] std::size_t PreviousCharacter(
    const std::wstring& text,
    std::size_t cursor) noexcept {
    if (cursor >= 2 && IsLowSurrogate(text[cursor - 1]) &&
        IsHighSurrogate(text[cursor - 2])) {
        return cursor - 2;
    }
    return cursor - 1;
}

[[nodiscard]] std::size_t NextCharacter(
    const std::wstring& text,
    std::size_t cursor) noexcept {
    return cursor + (cursor + 1 < text.size() &&
            IsHighSurrogate(text[cursor]) &&
            IsLowSurrogate(text[cursor + 1])
        ? 2
        : 1);
}

[[nodiscard]] bool QueryGamepadState(
    std::uint32_t userIndex,
    InputState::GamepadSnapshot& snapshot) noexcept {
    // The hook replaces the game's import slot, so this call reads the
    // physical state.
    snapshot = {};
    XINPUT_STATE state{};
    if (::XInputGetState(userIndex, &state) != ERROR_SUCCESS) {
        return false;
    }
    snapshot.buttons = state.Gamepad.wButtons;
    snapshot.leftTrigger = state.Gamepad.bLeftTrigger;
    snapshot.rightTrigger = state.Gamepad.bRightTrigger;
    snapshot.leftX = state.Gamepad.sThumbLX;
    snapshot.leftY = state.Gamepad.sThumbLY;
    snapshot.rightX = state.Gamepad.sThumbRX;
    snapshot.rightY = state.Gamepad.sThumbRY;
    snapshot.connected = true;
    return true;
}

}

InputState::InputState() noexcept
    : InputState(
          &QueryAsyncKeyState,
          &IsCurrentProcessForeground,
          &QueryGamepadState,
          &SuppressGamepadButtonsForGame,
          &UnsuppressGamepadButtonsForGame,
          &ClearGamepadSuppressionsForGame,
          &QueryTextInputWindow,
          &SuppressGamepadAnalogForGame,
          &UnsuppressGamepadAnalogForGame) {}

InputState::~InputState() {
    std::scoped_lock lock{mutex_};
    TakeSubclassInstallResult(true);
}

InputState::InputState(
    KeyStateQuery keyStateQuery,
    ForegroundProcessQuery foregroundProcessQuery,
    GamepadStateQuery gamepadStateQuery,
    GamepadButtonsCommand suppressGamepadButtons,
    GamepadButtonsCommand unsuppressGamepadButtons,
    ClearGamepadSuppressionsCommand clearGamepadSuppressions,
    TextInputWindowQuery textInputWindowQuery,
    GamepadAnalogCommand suppressGamepadAnalog,
    GamepadAnalogCommand unsuppressGamepadAnalog) noexcept
    : keyStateQuery_{keyStateQuery},
      foregroundProcessQuery_{foregroundProcessQuery},
      gamepadStateQuery_{gamepadStateQuery},
      suppressGamepadButtons_{suppressGamepadButtons},
      unsuppressGamepadButtons_{unsuppressGamepadButtons},
      clearGamepadSuppressions_{clearGamepadSuppressions},
      textInputWindowQuery_{textInputWindowQuery},
      suppressGamepadAnalog_{suppressGamepadAnalog},
      unsuppressGamepadAnalog_{unsuppressGamepadAnalog} {}

bool InputState::RegisterKey(std::uint8_t key) noexcept {
    std::scoped_lock lock{mutex_};
    registeredKeys_.set(key);
    return true;
}

bool InputState::UnregisterKey(std::uint8_t key) noexcept {
    std::scoped_lock lock{mutex_};
    if (!registeredKeys_.test(key)) {
        return false;
    }
    registeredKeys_.reset(key);
    return true;
}

void InputState::ClearKeys() noexcept {
    std::scoped_lock lock{mutex_};
    registeredKeys_.reset();
}

bool InputState::IsKeyPressed(std::uint8_t key) noexcept {
    {
        std::scoped_lock lock{mutex_};
        if (!registeredKeys_.test(key)) {
            return false;
        }
    }

    return foregroundProcessQuery_ != nullptr && keyStateQuery_ != nullptr &&
        foregroundProcessQuery_() && keyStateQuery_(key);
}

bool InputState::RegisterGamepadButtons(
    std::uint16_t buttons,
    std::uint32_t userIndex) noexcept {
    if (buttons == 0 || userIndex >= kGamepadCount) {
        return false;
    }
    std::scoped_lock lock{mutex_};
    registeredGamepadButtonMasks_[userIndex].set(buttons);
    return true;
}

bool InputState::UnregisterGamepadButtons(
    std::uint16_t buttons,
    std::uint32_t userIndex) noexcept {
    if (buttons == 0 || userIndex >= kGamepadCount) {
        return false;
    }
    std::scoped_lock lock{mutex_};
    auto& registeredMasks = registeredGamepadButtonMasks_[userIndex];
    if (!registeredMasks.test(buttons)) {
        return false;
    }
    registeredMasks.reset(buttons);
    return true;
}

void InputState::ClearGamepadButtons() noexcept {
    std::scoped_lock lock{mutex_};
    for (auto& registeredMasks : registeredGamepadButtonMasks_) {
        registeredMasks.reset();
    }
}

bool InputState::AreGamepadButtonsPressed(
    std::uint16_t buttons,
    std::uint32_t userIndex) noexcept {
    if (buttons == 0 || userIndex >= kGamepadCount) {
        return false;
    }
    {
        std::scoped_lock lock{mutex_};
        if (!registeredGamepadButtonMasks_[userIndex].test(buttons)) {
            return false;
        }
    }

    GamepadSnapshot snapshot;
    return foregroundProcessQuery_ != nullptr && gamepadStateQuery_ != nullptr &&
        foregroundProcessQuery_() &&
        gamepadStateQuery_(userIndex, snapshot) &&
        (snapshot.buttons & buttons) == buttons;
}

InputState::GamepadSnapshot InputState::PollGamepad(
    std::uint32_t userIndex) noexcept {
    GamepadSnapshot snapshot;
    if (userIndex >= kGamepadCount || foregroundProcessQuery_ == nullptr ||
        gamepadStateQuery_ == nullptr || !foregroundProcessQuery_() ||
        !gamepadStateQuery_(userIndex, snapshot)) {
        return {};
    }
    return snapshot;
}

bool InputState::SuppressGamepadAnalog(std::uint32_t userIndex) noexcept {
    return suppressGamepadAnalog_ != nullptr &&
        suppressGamepadAnalog_(userIndex);
}

bool InputState::UnsuppressGamepadAnalog(std::uint32_t userIndex) noexcept {
    return unsuppressGamepadAnalog_ != nullptr &&
        unsuppressGamepadAnalog_(userIndex);
}

bool InputState::SuppressGamepadButtons(
    std::uint16_t buttons,
    std::uint32_t userIndex) noexcept {
    return suppressGamepadButtons_ != nullptr &&
        suppressGamepadButtons_(buttons, userIndex);
}

bool InputState::UnsuppressGamepadButtons(
    std::uint16_t buttons,
    std::uint32_t userIndex) noexcept {
    return unsuppressGamepadButtons_ != nullptr &&
        unsuppressGamepadButtons_(buttons, userIndex);
}

bool InputState::ClearGamepadSuppressions() noexcept {
    return clearGamepadSuppressions_ != nullptr &&
        clearGamepadSuppressions_();
}

bool InputState::SuppressKey(std::uint8_t key) noexcept {
    std::scoped_lock lock{mutex_};
    if (key == 0) {
        return false;
    }
    // The key stays recorded when the install fails, so the next SuppressKey
    // call retries the install and the key takes effect once it succeeds.
    suppressedKeys_.set(key);
    return InstallSubclassIfMissing();
}

// Called with mutex_ held.
bool InputState::InstallSubclassIfMissing() noexcept {
    TakeSubclassInstallResult(false);
    if (subclassWindow_ != nullptr || subclassInstallPending_) {
        return true;
    }
    if (textInputWindowQuery_ == nullptr) {
        return false;
    }
    void* const window = textInputWindowQuery_();
    return window != nullptr && StartSubclassInstall(window);
}

bool InputState::BeginMouseCapture() noexcept {
    std::scoped_lock lock{mutex_};
    // Like SuppressKey, the capture stays set when the install fails, and the
    // next call retries the install.
    mouse_ = {};
    mouseCapturing_ = true;
    if (keyStateQuery_ != nullptr) {
        mouse_.left = keyStateQuery_(VK_LBUTTON);
        mouse_.right = keyStateQuery_(VK_RBUTTON);
    }
    return InstallSubclassIfMissing();
}

InputState::MouseSnapshot InputState::PollMouse() noexcept {
    std::scoped_lock lock{mutex_};
    if (!mouseCapturing_) {
        return {};
    }
    MouseSnapshot snapshot = mouse_;
    snapshot.capturing = true;
    mouse_.dx = 0;
    mouse_.dy = 0;
    mouse_.wheel = 0;
    mouse_.leftPressed = false;
    mouse_.rightPressed = false;
    return snapshot;
}

bool InputState::EndMouseCapture() noexcept {
    std::scoped_lock lock{mutex_};
    mouseCapturing_ = false;
    mouse_ = {};
    return true;
}

bool InputState::UnsuppressKey(std::uint8_t key) noexcept {
    std::scoped_lock lock{mutex_};
    if (!suppressedKeys_.test(key)) {
        return false;
    }
    suppressedKeys_.reset(key);
    return true;
}

bool InputState::ClearKeySuppressions() noexcept {
    std::scoped_lock lock{mutex_};
    suppressedKeys_.reset();
    return true;
}

// Starts the subclass install for window. On the owner thread it runs now.
// From another thread it is queued, and the session starts before the window
// thread installs the subclass; keys typed before then are not captured.
bool InputState::StartSubclassInstall(void* window) noexcept {
    const auto handle = static_cast<HWND>(window);
    DWORD ownerProcess{};
    const DWORD ownerThread =
        ::GetWindowThreadProcessId(handle, &ownerProcess);
    if (ownerThread == 0 || ownerProcess != ::GetCurrentProcessId()) {
        return false;
    }
    if (ownerThread == ::GetCurrentThreadId()) {
        if (!SetKeyboardWindowSubclass(handle, this)) {
            return false;
        }
        subclassWindow_ = window;
        return true;
    }
    if (subclassInstallPending_) {
        return pendingSubclassWindow_ == window;
    }
    if (!QueueSubclassInstall(*this, handle, ownerThread)) {
        return false;
    }
    subclassInstallPending_ = true;
    pendingSubclassWindow_ = window;
    return true;
}

// Called with mutex_ held. Takes the result of a finished install. A failed
// install cancels the active session. A request the window thread has not run
// within kSubclassInstallTimeoutMs, or any request when abandon is true, is
// discarded.
void InputState::TakeSubclassInstallResult(bool abandon) noexcept {
    if (!subclassInstallPending_) {
        return;
    }
    std::scoped_lock dispatchLock{subclassDispatchMutex};
    SubclassRequest* const request =
        pendingSubclassRequest.load(std::memory_order_relaxed);
    if (request == nullptr || request->input != this) {
        subclassInstallPending_ = false;
        pendingSubclassWindow_ = nullptr;
        return;
    }
    const bool completed = request->completed.load(std::memory_order_acquire);
    const bool expired =
        ::GetTickCount64() - request->queuedAt >= kSubclassInstallTimeoutMs;
    if (!completed && !abandon && !expired) {
        return;
    }
    if (completed && request->succeeded.load(std::memory_order_relaxed)) {
        subclassWindow_ = request->window;
    } else if (textInputSession_ != 0) {
        textInputStatus_ = TextInputStatus::Cancelled;
        ++textInputRevision_;
    }
    pendingSubclassRequest.store(nullptr, std::memory_order_relaxed);
    (void)::UnhookWindowsHookEx(request->hook);
    delete request;
    subclassInstallPending_ = false;
    pendingSubclassWindow_ = nullptr;
}

std::uint32_t InputState::BeginTextInput() noexcept {
    std::scoped_lock lock{mutex_};
    if (textInputSession_ != 0 || textInputWindowQuery_ == nullptr) {
        return 0;
    }

    const auto window = static_cast<HWND>(textInputWindowQuery_());
    if (window == nullptr) {
        return 0;
    }
    try {
        textInputText_.reserve(512);
    } catch (...) {
        return 0;
    }
    TakeSubclassInstallResult(false);
    if (subclassWindow_ == nullptr) {
        if (!StartSubclassInstall(window)) {
            return 0;
        }
    } else if (subclassWindow_ != window) {
        return 0;
    }

    textInputSession_ = nextTextInputSession_++;
    if (textInputSession_ == 0) {
        textInputSession_ = nextTextInputSession_++;
    }
    swallowNextChar_ = false;
    textInputRevision_ = 0;
    textInputStatus_ = TextInputStatus::Active;
    textInputText_.clear();
    textInputCursor_ = 0;
    pendingHighSurrogate_ = 0;
    return textInputSession_;
}

bool InputState::PollTextInput(
    std::uint32_t sessionId, TextInputSnapshot& snapshot) noexcept {
    std::scoped_lock lock{mutex_};
    TakeSubclassInstallResult(false);
    if (sessionId == 0 || sessionId != textInputSession_) {
        return false;
    }
    const int size = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        textInputText_.data(),
        static_cast<int>(textInputText_.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (size == 0 && !textInputText_.empty()) {
        return false;
    }
    try {
        snapshot.text.resize(static_cast<std::size_t>(size));
    } catch (...) {
        return false;
    }
    if (size != 0) {
        (void)WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            textInputText_.data(),
            static_cast<int>(textInputText_.size()),
            snapshot.text.data(),
            size,
            nullptr,
            nullptr);
    }
    snapshot.sessionId = sessionId;
    snapshot.revision = textInputRevision_;
    snapshot.status = textInputStatus_;
    return true;
}

bool InputState::EndTextInput(std::uint32_t sessionId) noexcept {
    std::scoped_lock lock{mutex_};
    TakeSubclassInstallResult(false);
    if (sessionId == 0 || sessionId != textInputSession_) {
        return false;
    }
    textInputReleaseKeys_ |= textInputKeysDown_;
    textInputKeysDown_.reset();
    textInputSession_ = 0;
    textInputText_.clear();
    textInputCursor_ = 0;
    pendingHighSurrogate_ = 0;
    return true;
}

bool InputState::FilterKeyboardMessage(
    unsigned int message,
    std::uintptr_t wParam,
    std::intptr_t lParam) noexcept {
    std::scoped_lock lock{mutex_};
    const auto key = static_cast<std::uint8_t>(wParam & 0xFFU);
    if (message == WM_KEYUP || message == WM_SYSKEYUP) {
        swallowNextChar_ = false;
        textInputKeysDown_.reset(key);
        const bool consumed = textInputSession_ != 0 ||
            textInputReleaseKeys_.test(key) ||
            consumedDownKeys_.test(key);
        textInputReleaseKeys_.reset(key);
        consumedDownKeys_.reset(key);
        return consumed;
    }
    if (message == WM_INPUT && mouseCapturing_ && FilterMouseRawInput(lParam)) {
        return true;
    }
    if (textInputSession_ == 0) {
        return FilterSuppressedKey(message, wParam, lParam);
    }
    if (message == WM_INPUT) {
        return IsKeyboardRawInput(lParam);
    }
    if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) {
        textInputKeysDown_.set(key);
        if (textInputStatus_ != TextInputStatus::Active) {
            return true;
        }
        bool changed = false;
        if (key == VK_RETURN) {
            textInputStatus_ = TextInputStatus::Submitted;
            pendingHighSurrogate_ = 0;
            changed = true;
        } else if (key == VK_ESCAPE) {
            textInputStatus_ = TextInputStatus::Cancelled;
            pendingHighSurrogate_ = 0;
            changed = true;
        } else if (key == VK_BACK && textInputCursor_ != 0) {
            const auto start = PreviousCharacter(textInputText_, textInputCursor_);
            textInputText_.erase(start, textInputCursor_ - start);
            textInputCursor_ = start;
            changed = true;
        } else if (key == VK_DELETE && textInputCursor_ < textInputText_.size()) {
            const auto end = NextCharacter(textInputText_, textInputCursor_);
            textInputText_.erase(textInputCursor_, end - textInputCursor_);
            changed = true;
        } else if (key == VK_LEFT && textInputCursor_ != 0) {
            textInputCursor_ = PreviousCharacter(textInputText_, textInputCursor_);
        } else if (key == VK_RIGHT && textInputCursor_ < textInputText_.size()) {
            textInputCursor_ = NextCharacter(textInputText_, textInputCursor_);
        } else if (key == VK_HOME) {
            textInputCursor_ = 0;
        } else if (key == VK_END) {
            textInputCursor_ = textInputText_.size();
        }
        if (changed) {
            ++textInputRevision_;
        }
        return true;
    }
    if (message == WM_CHAR || message == WM_SYSCHAR) {
        if (textInputStatus_ != TextInputStatus::Active) {
            return true;
        }
        const wchar_t character = static_cast<wchar_t>(wParam);
        if (IsHighSurrogate(character)) {
            pendingHighSurrogate_ = character;
        } else if (IsLowSurrogate(character)) {
            if (pendingHighSurrogate_ != 0 && textInputText_.size() <= 510) {
                const wchar_t pair[]{pendingHighSurrogate_, character};
                textInputText_.insert(textInputCursor_, pair, 2);
                textInputCursor_ += 2;
                ++textInputRevision_;
            }
            pendingHighSurrogate_ = 0;
        } else {
            pendingHighSurrogate_ = 0;
            if (character >= L' ' && character != 0x7F &&
                textInputText_.size() < 512) {
                textInputText_.insert(textInputCursor_++, 1, character);
                ++textInputRevision_;
            }
        }
        return true;
    }
    if (message == WM_UNICHAR) {
        if (wParam == UNICODE_NOCHAR) {
            return true;
        }
        pendingHighSurrogate_ = 0;
        if (textInputStatus_ != TextInputStatus::Active ||
            wParam < L' ' || wParam == 0x7F || wParam > 0x10FFFF ||
            (wParam >= 0xD800 && wParam <= 0xDFFF)) {
            return true;
        }
        if (wParam <= 0xFFFF && textInputText_.size() < 512) {
            textInputText_.insert(
                textInputCursor_++, 1, static_cast<wchar_t>(wParam));
            ++textInputRevision_;
        } else if (wParam > 0xFFFF && textInputText_.size() <= 510) {
            const auto codePoint = static_cast<std::uint32_t>(wParam - 0x10000);
            const wchar_t pair[]{
                static_cast<wchar_t>(0xD800 + (codePoint >> 10)),
                static_cast<wchar_t>(0xDC00 + (codePoint & 0x3FF))};
            textInputText_.insert(textInputCursor_, pair, 2);
            textInputCursor_ += 2;
            ++textInputRevision_;
        }
        return true;
    }
    return false;
}

// Called with mutex_ held and no text session active.
bool InputState::FilterSuppressedKey(
    unsigned int message,
    std::uintptr_t wParam,
    std::intptr_t lParam) noexcept {
    const auto key = static_cast<std::uint8_t>(wParam & 0xFFU);
    if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) {
        swallowNextChar_ = false;
        // Bit 30 is set for auto-repeat. A repeat of a consumed key-down is
        // consumed even if the key was unsuppressed while it was held.
        const bool isRepeat =
            ((static_cast<std::uintptr_t>(lParam) >> 30) & 1U) != 0;
        if (!suppressedKeys_.test(key) &&
            !(isRepeat && consumedDownKeys_.test(key))) {
            return false;
        }
        consumedDownKeys_.set(key);
        swallowNextChar_ = true;
        return true;
    }
    if (message == WM_CHAR || message == WM_SYSCHAR ||
        message == WM_DEADCHAR || message == WM_SYSDEADCHAR) {
        // TranslateMessage posts the character right after the key-down, so
        // the next character message belongs to the consumed key-down.
        const bool consumed = swallowNextChar_;
        swallowNextChar_ = false;
        return consumed;
    }
    if (message == WM_INPUT) {
        swallowNextChar_ = false;
        return FilterSuppressedRawInput(lParam);
    }
    return false;
}

// Called with mutex_ held. Reads the record only while a key is suppressed or
// a consumed make code is waiting for its break code.
bool InputState::FilterSuppressedRawInput(std::intptr_t lParam) noexcept {
    if (lParam == 0 || (suppressedKeys_.none() && consumedRawDownKeys_.none())) {
        return false;
    }
    RAWINPUT record{};
    UINT size = sizeof(record);
    if (::GetRawInputData(
            reinterpret_cast<HRAWINPUT>(lParam),
            RID_INPUT,
            &record,
            &size,
            sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1) ||
        record.header.dwType != RIM_TYPEKEYBOARD) {
        return false;
    }
    // VKey 0xFF marks fake keys such as the shift that Windows inserts around
    // some numpad keys, and VKey 0 carries no key.
    const USHORT virtualKey = record.data.keyboard.VKey;
    if (virtualKey == 0 || virtualKey >= 0xFF) {
        return false;
    }
    const auto key = static_cast<std::uint8_t>(virtualKey);
    if ((record.data.keyboard.Flags & RI_KEY_BREAK) != 0) {
        const bool consumed = consumedRawDownKeys_.test(key);
        consumedRawDownKeys_.reset(key);
        return consumed;
    }
    if (!suppressedKeys_.test(key) && !consumedRawDownKeys_.test(key)) {
        return false;
    }
    consumedRawDownKeys_.set(key);
    return true;
}

// Called with mutex_ held while the mouse is captured. Mouse records are
// added to the totals only while Fallout's process has the foreground; in the
// background they reach the game's window procedure unchanged so alt-tab
// behaves normally.
bool InputState::FilterMouseRawInput(std::intptr_t lParam) noexcept {
    if (lParam == 0) {
        return false;
    }
    RAWINPUT record{};
    UINT size = sizeof(record);
    if (::GetRawInputData(
            reinterpret_cast<HRAWINPUT>(lParam),
            RID_INPUT,
            &record,
            &size,
            sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1) ||
        record.header.dwType != RIM_TYPEMOUSE) {
        return false;
    }
    if (foregroundProcessQuery_ == nullptr || !foregroundProcessQuery_()) {
        // Button releases during alt-tab would be missed, so drop the state.
        mouse_.left = false;
        mouse_.right = false;
        return false;
    }
    const RAWMOUSE& raw = record.data.mouse;
    if ((raw.usFlags & MOUSE_MOVE_ABSOLUTE) == 0) {
        mouse_.dx += raw.lLastX;
        mouse_.dy += raw.lLastY;
    }
    const USHORT buttons = raw.usButtonFlags;
    if ((buttons & RI_MOUSE_LEFT_BUTTON_DOWN) != 0) {
        mouse_.left = true;
        mouse_.leftPressed = true;
    }
    if ((buttons & RI_MOUSE_LEFT_BUTTON_UP) != 0) {
        mouse_.left = false;
    }
    if ((buttons & RI_MOUSE_RIGHT_BUTTON_DOWN) != 0) {
        mouse_.right = true;
        mouse_.rightPressed = true;
    }
    if ((buttons & RI_MOUSE_RIGHT_BUTTON_UP) != 0) {
        mouse_.right = false;
    }
    if ((buttons & RI_MOUSE_WHEEL) != 0) {
        mouse_.wheel += static_cast<SHORT>(raw.usButtonData);
    }
    return true;
}

}
