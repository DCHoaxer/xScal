#include "modules/core/core_module.hpp"

#include "config/config.h"
#include "plugins/plugins.hpp"
#include "api/scaleform_movie_interface.hpp"
#include "scaleform/value.hpp"
#include "xscal/movie.hpp"
#include "xscal_build_timestamp.hpp"

#include <algorithm>
#include <array>
#include <cstdio>

namespace sf::modules {
namespace {

constexpr char kTextName[] = "xScalInfoText_tf";

}  // namespace

void CoreModule::Update(const MovieRootContext& context) noexcept {
    if (!versionTextAdded_) {
        // Menu children may appear after the first batch of variable lookups.
        constexpr std::uint8_t initialAttempts = 32;
        constexpr std::uint8_t retryInterval = 64;
        const auto attempt = ++versionTextAttempts_;
        if (attempt <= initialAttempts ||
            attempt == initialAttempts + retryInterval) {
            if (attempt > initialAttempts) {
                versionTextAttempts_ = initialAttempts;
            }
            versionTextAdded_ = AddVersionText(context);
        }
    }
}

// Uses the plugin movie interface. Calling methods on the text field needs the
// XSCAL_ADDRESS_SCALEFORM_INVOKE Address Library record.
[[nodiscard]] bool CoreModule::AddVersionText(
    const MovieRootContext& context) noexcept {
    XScalMovie handle{&context};
    const xscal::Movie movie{movieapi::Interface(), &handle};

    const xscal::Value menu = movie.get("root1.Menu_mc");
    const xscal::Value versionText =
        movie.get("root1.Menu_mc.VersionText_tf");
    const xscal::Value styleText = movie.get(
        "root1.Menu_mc.SplashScreenHolder_mc.SplashScreenText_tf");
    if (!menu.isObject() || !versionText.isObject() ||
        !styleText.isObject()) {
        return false;
    }
    if (menu.call("getChildByName", kTextName).isObject()) {
        return true;
    }

    double width{};
    double height{};
    if (!versionText.get("width").read(width) ||
        !versionText.get("height").read(height)) {
        return false;
    }

    // Menu_mc is authored at 1280x720; its parent applies the screen scale.
    constexpr double edgeInset = 16.0;
    constexpr double menuHeight = 720.0;
    constexpr double verticalOffset = 15.0;
    const double textX = edgeInset;
    const double textY =
        menuHeight - height - edgeInset + verticalOffset;

    const xscal::Value format = versionText.get("defaultTextFormat");
    if (!format.isObject() ||
        !format.set("font", "$MAIN_Font") ||
        !format.set("bold", true) ||
        !format.set("size", 12.0) ||
        !format.set("align", "left")) {
        return false;
    }

    const xscal::Value textField = movie.create("flash.text.TextField");
    if (textField.type() != XSCAL_MOVIE_TYPE_DISPLAY_OBJECT) {
        return false;
    }

    std::array<char, 512> pluginNames{};
    const std::size_t pluginNamesLength =
        plugins::CopyActiveModuleNames(
            pluginNames.data(), pluginNames.size());
    std::array<char, 640> text{};
    const int textLength = pluginNamesLength == 0
        ? std::snprintf(
            text.data(),
            text.size(),
            "xScal %u.%u.%u (Built %s %s) with AddressLibrary",
            static_cast<unsigned int>(config::kXScalVersion.major),
            static_cast<unsigned int>(config::kXScalVersion.minor),
            static_cast<unsigned int>(config::kXScalVersion.patch),
            build_info::kDate,
            build_info::kTime)
        : std::snprintf(
            text.data(),
            text.size(),
            "xScal %u.%u.%u (Built %s %s, Plugins: %.*s) with AddressLibrary",
            static_cast<unsigned int>(config::kXScalVersion.major),
            static_cast<unsigned int>(config::kXScalVersion.minor),
            static_cast<unsigned int>(config::kXScalVersion.patch),
            build_info::kDate,
            build_info::kTime,
            static_cast<int>(pluginNamesLength),
            pluginNames.data());
    if (textLength <= 0 ||
        static_cast<std::size_t>(textLength) >= text.size()) {
        return false;
    }

    const double textWidth = (std::min)(
        1000.0,
        (std::max)(width * 1.5, static_cast<double>(textLength) * 7.0));
    // setTextFormat applies the font to the text; defaultTextFormat alone is
    // not used when a runtime-created field is drawn.
    return textField.set("defaultTextFormat", format) &&
        textField.set("name", kTextName) &&
        textField.set("text", text.data()) &&
        textField.set("x", textX) &&
        textField.set("y", textY) &&
        textField.set("width", textWidth) &&
        textField.set("height", height) &&
        textField.set("alpha", 1.0) &&
        textField.set("visible", true) &&
        textField.set("embedFonts", false) &&
        textField.set("selectable", false) &&
        textField.set("mouseEnabled", false) &&
        textField.setTextFormat(format) &&
        textField.set("textColor", styleText.get("textColor")) &&
        textField.set("filters", styleText.get("filters")) &&
        textField.set("antiAliasType", styleText.get("antiAliasType")) &&
        menu.call("addChild", textField).valid();
}

}  // namespace sf::modules
