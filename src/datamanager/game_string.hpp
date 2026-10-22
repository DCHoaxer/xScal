#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace sf::datamanager {

// String buffer filled by game text functions. The game allocates data; the
// caller frees it with the game deallocator.
struct GameString final {
    char* data{};
    std::uint16_t length{};
    std::uint16_t capacity{};
};
static_assert(sizeof(GameString) == 0x10);

using GameDeallocate = void(__fastcall*)(void* memory);

// Copies a game string. A length of 0xFFFF means the game did not store the
// length and the text is NUL-terminated. A null buffer copies as empty text;
// text longer than maximumLength fails.
[[nodiscard]] bool CopyGameString(
    const GameString& text,
    std::size_t maximumLength,
    std::string& destination) noexcept;

// Frees a GameString's buffer with the game deallocator when it goes out of
// scope.
class GameStringOwner final {
public:
    GameStringOwner(GameDeallocate deallocate, GameString& text) noexcept
        : deallocate_(deallocate), text_(text) {}

    GameStringOwner(const GameStringOwner&) = delete;
    GameStringOwner& operator=(const GameStringOwner&) = delete;

    ~GameStringOwner() {
        if (text_.data != nullptr && deallocate_ != nullptr) {
            deallocate_(text_.data);
        }
        text_ = {};
    }

private:
    GameDeallocate deallocate_{};
    GameString& text_;
};

}
