#include "modules/core/credits_music.hpp"

#include "platform/diagnostics.hpp"
#include "plugins/plugins.hpp"
#include "xscal/address_ids.h"
#include "xscal/plugin_api.h"

#include <cstdint>
#include <mutex>

// The main menu starts its music by resource ID from the path in sMainMenuMusic whenever its
// BSSoundHandle is not playing, and reads that path again each time. While the credits are open
// the setting points at kCreditsTrack and the handle is stopped, so the menu itself starts that
// track; on close the old path goes back and the handle is stopped again. The track is one of the
// game's own menu themes in SeventySix - Sounds02.ba2, so the resource index already has it.

namespace sf::modules {
namespace {

constexpr char kCreditsTrack[] = "Data\\Music\\Special\\MUS_76_Special_MainTheme_Nuka.xwm";

struct GameAddresses final {
    // char* value of the sMainMenuMusic:General setting.
    const char** mainMenuMusicPath;
    void* mainMenuMusicHandle;
    // BSSoundHandle::Stop(handle*): queues a stop command and keeps the sound ID.
    bool(__fastcall* stopSound)(void* handle);
};

std::mutex stateMutex;
GameAddresses addresses{};
bool addressesResolved = false;
bool addressesMissing = false;
bool musicSwapped = false;
const char* replacedPath = nullptr;

template <typename T>
bool resolve(const XScalAddressLibraryV1& library, std::uint64_t id, T& value) noexcept {
    std::uintptr_t address{};
    if (library.resolveAddress == nullptr || !library.resolveAddress(library.context, id, &address) ||
        address == 0) {
        return false;
    }
    value = reinterpret_cast<T>(address);
    return true;
}

bool resolveAddresses() noexcept {
    const XScalAddressLibraryV1* library = plugins::AddressLibrary();
    if (library == nullptr) {
        return false;
    }
    return resolve(*library, XSCAL_ADDRESS_ALL_THEMES_MAIN_MENU_MUSIC_VALUE, addresses.mainMenuMusicPath) &&
        resolve(*library, XSCAL_ADDRESS_MAIN_MENU_MUSIC_HANDLE, addresses.mainMenuMusicHandle) &&
        resolve(*library, XSCAL_ADDRESS_SOUND_HANDLE_STOP, addresses.stopSound);
}

}  // namespace

void SetCreditsMusicPlaying(bool playing) noexcept {
    const std::lock_guard lock{stateMutex};
    if (playing == musicSwapped) {
        return;
    }
    if (!playing) {
        // The menu's own path goes back unless something else replaced the setting meanwhile.
        if (*addresses.mainMenuMusicPath == kCreditsTrack) {
            *addresses.mainMenuMusicPath = replacedPath;
            addresses.stopSound(addresses.mainMenuMusicHandle);
        }
        replacedPath = nullptr;
        musicSwapped = false;
        return;
    }
    if (!addressesResolved) {
        if (addressesMissing) {
            return;
        }
        addressesResolved = resolveAddresses();
        if (!addressesResolved) {
            addressesMissing = true;
            DiagnosticLog("xScal credits music: the Address Library has no records for this game build");
            return;
        }
    }
    replacedPath = *addresses.mainMenuMusicPath;
    *addresses.mainMenuMusicPath = kCreditsTrack;
    // Stop keeps the handle's sound ID. Once the audio thread has removed the sound, the menu's
    // per-frame update sees the handle idle and starts kCreditsTrack on it.
    addresses.stopSound(addresses.mainMenuMusicHandle);
    musicSwapped = true;
}

}  // namespace sf::modules
