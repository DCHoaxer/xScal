#include "modules/core/plugin_settings_menu.hpp"

#include "api/scaleform_movie_interface.hpp"
#include "modules/core/game_language.hpp"
#include "xscal/movie.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace sf::modules {
namespace {

// plugin_settings_swf/build.sh generates this from plugin_settings_swf/src. xScal ships as dxgi.dll
// alone, so the SWF is loaded from memory instead of a BA2 listed in the game's ini.
constexpr unsigned char kSwfBytes[] = {
#include "modules/core/plugin_settings_swf_bytes.inc"
};

// root1 of this movie is sealed, so the SWF publishes itself on root1.SubListAnimation, a timeline
// clip whose class is dynamic (SeventySixMenu_fla.SubListAnimation_79).
constexpr const char* kCodeObjectOwner = "root1.SubListAnimation.PluginSettings";
constexpr std::uint32_t kRetryFrames = 30;
constexpr std::uint32_t kMaxLoadAttempts = 5;
constexpr std::uint32_t kOwnerWaitFrames = 600;
constexpr std::uint32_t kAttachAttempts = 10;

std::atomic<bool> pluginWindowOpen{false};

// mainmenu.swf is a separate movie below seventysixmenu.swf, so the SWF cannot reach its news panel,
// which would show through the option list.
class NewsPanelPatch final : public xscal::MoviePatch {
public:
    NewsPanelPatch() : MoviePatch("mainmenu.swf", wantFrames) {}

    void onUnload(xscal::Movie&) override {
        panelHidden_ = false;
    }

    void onFrame(xscal::Movie& movie) override {
        const bool windowOpen = pluginWindowOpen.load(std::memory_order_relaxed);
        if (windowOpen == panelHidden_) {
            return;
        }
        xscal::Value panel = movie.get("root1.Menu_mc").call("getChildByName", "MOTD_mc");
        if (!panel.isObject()) {
            return;
        }
        if (windowOpen) {
            panelWasVisible_ = false;
            (void)panel.get("visible").read(panelWasVisible_);
            panelHidden_ = panel.set("visible", false);
        } else {
            (void)panel.set("visible", panelWasVisible_);
            panelHidden_ = false;
        }
    }

private:
    bool panelHidden_{};
    bool panelWasVisible_{};
};

NewsPanelPatch newsPanelPatch;

// One seventysixmenu.swf movie at a time. All members are used only from that movie's callbacks.
class MainMenuPatch final : public xscal::MoviePatch {
public:
    MainMenuPatch() : MoviePatch("seventysixmenu.swf", wantFrames) {}

    void onLoad(xscal::Movie& movie) override {
        clearLoad();
        movieIdentity_ = movie.identity();
    }

    void onUnload(xscal::Movie& movie) override {
        if (movie.identity() != movieIdentity_) {
            return;
        }
        movie.drop(loaderRef_);
        clearLoad();
        movieIdentity_ = 0;
        pluginWindowOpen.store(false, std::memory_order_relaxed);
    }

    void onFrame(xscal::Movie& movie) override {
        if (movieIdentity_ == 0 || movie.identity() != movieIdentity_) {
            return;
        }
        if (!loadStarted_) {
            if (framesUntilRetry_ > 0) {
                --framesUntilRetry_;
                return;
            }
            if (attempts_ >= kMaxLoadAttempts) {
                return;
            }
            ++attempts_;
            if (!startLoad(movie)) {
                framesUntilRetry_ = kRetryFrames;
            }
            return;
        }
        xscal::Value content = movie.load(loaderRef_).get("content");
        if (!content.isObject()) {
            return;
        }
        bool windowOpen = false;
        (void)content.get("isWindowOpen").read(windowOpen);
        pluginWindowOpen.store(windowOpen, std::memory_order_relaxed);
        setLanguage(content);
        attachCodeObject(movie);
    }

private:
    void clearLoad() {
        loaderRef_ = {};
        loadStarted_ = false;
        languageSet_ = false;
        codeObjectDone_ = false;
        attempts_ = 0;
        attachAttempts_ = 0;
        framesUntilRetry_ = 0;
        framesWaitingForOwner_ = 0;
    }

    // Every call into the movie costs a frame-time slice, and the SWF is thousands of bytes. Four
    // bytes go in per writeInt call with big-endian order, which is the AS3 default. If the
    // movie's ByteArray rejects a call or the first byte does not read back as written, the array
    // is emptied and filled with one writeByte call per byte.
    static bool writeSwfBytes(xscal::Value& bytes) {
        if (writeSwfBytesAsInts(bytes)) {
            return true;
        }
        if (!bytes.set("length", 0) || !bytes.set("position", 0)) {
            return false;
        }
        for (const unsigned char byte : kSwfBytes) {
            if (!bytes.call("writeByte", static_cast<int>(byte)).valid()) {
                return false;
            }
        }
        return true;
    }

    static bool writeSwfBytesAsInts(xscal::Value& bytes) {
        if (!bytes.set("endian", "bigEndian")) {
            return false;
        }
        constexpr std::size_t kSize = sizeof(kSwfBytes);
        std::size_t offset = 0;
        for (; offset + 4 <= kSize; offset += 4) {
            const std::uint32_t word = (static_cast<std::uint32_t>(kSwfBytes[offset]) << 24) |
                (static_cast<std::uint32_t>(kSwfBytes[offset + 1]) << 16) |
                (static_cast<std::uint32_t>(kSwfBytes[offset + 2]) << 8) |
                static_cast<std::uint32_t>(kSwfBytes[offset + 3]);
            if (!bytes.call("writeInt", static_cast<std::int32_t>(word)).valid()) {
                return false;
            }
        }
        for (; offset < kSize; ++offset) {
            if (!bytes.call("writeByte", static_cast<int>(kSwfBytes[offset])).valid()) {
                return false;
            }
        }
        std::uint32_t length = 0;
        std::uint32_t firstByte = 0;
        if (!bytes.set("position", 0) ||
            !bytes.call("readUnsignedByte").read(firstByte) ||
            firstByte != kSwfBytes[0] ||
            !bytes.get("length").read(length) || length != kSize) {
            return false;
        }
        return bytes.set("position", static_cast<std::uint32_t>(kSize));
    }

    bool startLoad(xscal::Movie& movie) {
        xscal::Value root = movie.get("root1");
        xscal::Value loader = movie.create("flash.display.Loader");
        xscal::Value bytes = movie.create("flash.utils.ByteArray");
        xscal::Value context = movie.create("flash.system.LoaderContext");
        xscal::Value domain = movie.get("root1.loaderInfo.applicationDomain");
        if (!root.isObject() || !loader.isObject() || !bytes.isObject() ||
            !context.isObject() || !domain.isObject()) {
            return false;
        }
        if (!writeSwfBytes(bytes)) {
            return false;
        }
        // Flash refuses loadBytes of a SWF with code unless allowCodeImport is set; Scaleform may not
        // have the property, so a failed set does not stop the load.
        (void)context.set("allowCodeImport", true);
        // The window extends the game's MenuComponent and uses the game's row classes, so it needs
        // the game's domain.
        if (!context.set("applicationDomain", domain)) {
            return false;
        }
        // loadBytes runs last: a loader that started loading is never left behind when a later step
        // fails, because onFrame would then create another one on the next attempt.
        loaderRef_ = movie.retain(std::move(loader));
        if (!loaderRef_) {
            return false;
        }
        xscal::Value retained = movie.load(loaderRef_);
        if (!root.call("addChild", retained).valid()) {
            movie.drop(loaderRef_);
            loaderRef_ = {};
            return false;
        }
        if (!retained.call("loadBytes", bytes, context).valid()) {
            (void)root.call("removeChild", retained);
            movie.drop(loaderRef_);
            loaderRef_ = {};
            return false;
        }
        loadStarted_ = true;
        return true;
    }

    // The SWF holds its texts in every language; only native code can read sLanguage. The ini files
    // are read once per game session.
    void setLanguage(xscal::Value& content) {
        if (languageSet_) {
            return;
        }
        static const std::string gameLanguage = readGameLanguage();
        languageSet_ = content.set("language", languageCodeForGame(gameLanguage));
    }

    // xScal attaches call() by itself only for root1.__SFCodeObj and configured paths, and this
    // movie has neither. attachCodeObject also fails while another thread holds the attach lock, so
    // it is tried on a few frames.
    void attachCodeObject(xscal::Movie& movie) {
        if (codeObjectDone_) {
            return;
        }
        if (!movie.get(kCodeObjectOwner).isObject()) {
            codeObjectDone_ = ++framesWaitingForOwner_ == kOwnerWaitFrames;
            return;
        }
        codeObjectDone_ = movie.attachCodeObject(kCodeObjectOwner) ||
            ++attachAttempts_ == kAttachAttempts;
    }

    // movie.identity() of the open menu movie, 0 when none. XScalMovie pointers are not kept: the
    // host passes a different one to each callback.
    std::uintptr_t movieIdentity_{};
    xscal::Ref loaderRef_;
    bool loadStarted_{};
    bool languageSet_{};
    bool codeObjectDone_{};
    std::uint32_t attempts_{};
    std::uint32_t attachAttempts_{};
    std::uint32_t framesUntilRetry_{};
    std::uint32_t framesWaitingForOwner_{};
};

MainMenuPatch mainMenuPatch;

}  // namespace

void InstallPluginSettingsMenu() noexcept {
    (void)mainMenuPatch.install(movieapi::Interface());
    (void)newsPanelPatch.install(movieapi::Interface());
}

}  // namespace sf::modules
