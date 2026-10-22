#include "datamanager/game_string.hpp"

#include "platform/native_access.hpp"

namespace sf::datamanager {

bool CopyGameString(
    const GameString& text,
    std::size_t maximumLength,
    std::string& destination) noexcept {
    destination.clear();
    if (text.data == nullptr) {
        return true;
    }
    if (text.length == 0xFFFF) {
        return platform::CopyBoundedCString(
            text.data, maximumLength, true, destination);
    }
    if (text.length > maximumLength) {
        return false;
    }
    try {
        destination.resize(text.length);
    } catch (...) {
        return false;
    }
    return text.length == 0 ||
        platform::TryCopyBytes(text.data, destination.data(), text.length);
}

}
