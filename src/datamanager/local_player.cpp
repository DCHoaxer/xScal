#include "datamanager/local_player.hpp"

#include "platform/native_access.hpp"

namespace sf::datamanager {

bool ReadLocalPlayer(
    std::uintptr_t localPlayerGlobal,
    std::uintptr_t localPlayerPrimaryVtable,
    std::uintptr_t& player) noexcept {
    player = 0;
    if (localPlayerGlobal == 0 || localPlayerPrimaryVtable == 0) {
        return false;
    }

    std::uintptr_t candidate{};
    std::uintptr_t primaryVtable{};
    if (!platform::TryCopyBytes(
            reinterpret_cast<const void*>(localPlayerGlobal),
            &candidate,
            sizeof(candidate)) ||
        candidate == 0 ||
        candidate % alignof(std::uintptr_t) != 0 ||
        !platform::TryCopyBytes(
            reinterpret_cast<const void*>(candidate),
            &primaryVtable,
            sizeof(primaryVtable)) ||
        primaryVtable != localPlayerPrimaryVtable) {
        return false;
    }

    player = candidate;
    return true;
}

}
