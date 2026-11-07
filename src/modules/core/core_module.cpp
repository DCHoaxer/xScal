#include "modules/core/core_module.hpp"

#include "config/config.h"
#include "modules/core/logo_animation.hpp"
#include "plugins/plugins.hpp"
#include "api/scaleform_movie_interface.hpp"
#include "scaleform/value.hpp"
#include "xscal/movie.hpp"
#include "xscal_build_timestamp.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace sf::modules {
namespace {

struct TextStyle final {
    double size;
    bool bold;
    std::uint32_t color;
};

constexpr TextStyle kTitleStyle{14.0, true, 0xE0E0E0};
constexpr TextStyle kBuildDetailsStyle{14.0, false, 0xBABABA};
constexpr TextStyle kLabelStyle{12.0, false, 0xE0E0E0};
constexpr TextStyle kValueStyle{12.0, false, 0xBABABA};

// Row offsets from the top of the block.
constexpr double kPluginsLabelY = 18.0;
constexpr double kPluginRowsY = 32.0;
constexpr double kPluginRowHeight = 14.0;
constexpr std::size_t kPluginRowsPerColumn = 2;

// Minimum plugin column width, and the minimum offset of the build details after
// the title.
constexpr double kColumnWidth = 88.0;
constexpr double kColumnGap = 12.0;
constexpr double kBuildDetailsGap = 6.0;
constexpr double kFieldWidth = 400.0;
constexpr double kFieldHeight = 22.0;

// The font sizes are screen pixels from the layout mockup. Menu_mc is scaled by
// 1.5 at 1920x1080, so this draws the text at the mockup's size there.
constexpr double kBlockScale = 2.0 / 3.0;

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
    if (menu.call("getChildByName", kVersionBlockName).isObject()) {
        return true;
    }

    // The block joins the menu only after all of its fields exist, so a retry
    // after a failed field does not leave a partial block on screen.
    const xscal::Value block = movie.create("flash.display.Sprite");
    if (block.type() != XSCAL_MOVIE_TYPE_DISPLAY_OBJECT) {
        return false;
    }

    const auto addText = [&](
        const char* text,
        double x,
        double y,
        const TextStyle& style,
        double* textWidth = nullptr) noexcept {
        const xscal::Value format = versionText.get("defaultTextFormat");
        if (!format.isObject() ||
            !format.set("font", "$MAIN_Font") ||
            !format.set("bold", style.bold) ||
            !format.set("size", style.size) ||
            !format.set("align", "left")) {
            return false;
        }

        const xscal::Value field = movie.create("flash.text.TextField");
        if (field.type() != XSCAL_MOVIE_TYPE_DISPLAY_OBJECT) {
            return false;
        }
        // setTextFormat applies the font to the text; defaultTextFormat alone
        // is not used when a runtime-created field is drawn.
        const bool fieldReady = field.set("defaultTextFormat", format) &&
            field.set("text", text) &&
            field.set("x", x) &&
            field.set("y", y) &&
            field.set("width", kFieldWidth) &&
            field.set("height", kFieldHeight) &&
            field.set("alpha", 1.0) &&
            field.set("visible", true) &&
            field.set("embedFonts", false) &&
            field.set("selectable", false) &&
            field.set("mouseEnabled", false) &&
            field.setTextFormat(format) &&
            field.set("textColor", style.color) &&
            field.set("filters", styleText.get("filters")) &&
            field.set("antiAliasType", styleText.get("antiAliasType")) &&
            block.call("addChild", field).valid();
        if (fieldReady && textWidth != nullptr) {
            field.get("textWidth").read(*textWidth);
        }
        return fieldReady;
    };

    std::array<char, 32> title{};
    const int titleLength = std::snprintf(
        title.data(),
        title.size(),
        "xScal %u.%u.%u",
        static_cast<unsigned int>(config::kXScalVersion.major),
        static_cast<unsigned int>(config::kXScalVersion.minor),
        static_cast<unsigned int>(config::kXScalVersion.patch));
    if (titleLength <= 0 ||
        static_cast<std::size_t>(titleLength) >= title.size()) {
        return false;
    }

    std::array<char, 64> buildDetails{};
    const int buildDetailsLength = std::snprintf(
        buildDetails.data(),
        buildDetails.size(),
        "(Built %s %s) %s",
        build_info::kDate,
        build_info::kTime,
        plugins::PlatformName());
    if (buildDetailsLength <= 0 ||
        static_cast<std::size_t>(buildDetailsLength) >= buildDetails.size()) {
        return false;
    }

    double titleWidth{};
    if (!addText(title.data(), 0.0, 0.0, kTitleStyle, &titleWidth)) {
        return false;
    }
    const double buildDetailsX =
        (std::max)(kColumnWidth, titleWidth + kBuildDetailsGap);
    if (!addText(buildDetails.data(), buildDetailsX, 0.0, kBuildDetailsStyle)) {
        return false;
    }

    const bool hasPlugins = plugins::ActiveModuleName(0) != nullptr;
    if (hasPlugins && !addText("Plugins", 0.0, kPluginsLabelY, kLabelStyle)) {
        return false;
    }

    // Plugins fill two rows, then continue in a new column to the right, so
    // the block height does not depend on the plugin count.
    double columnX = 0.0;
    double columnWidth = kColumnWidth;
    for (std::size_t index = 0;; ++index) {
        const char* const name = plugins::ActiveModuleName(index);
        if (name == nullptr) {
            break;
        }
        const std::size_t row = index % kPluginRowsPerColumn;
        if (index != 0 && row == 0) {
            columnX += columnWidth;
            columnWidth = kColumnWidth;
        }
        double textWidth{};
        if (!addText(
                name,
                columnX,
                kPluginRowsY + static_cast<double>(row) * kPluginRowHeight,
                kValueStyle,
                &textWidth)) {
            return false;
        }
        columnWidth = (std::max)(columnWidth, textWidth + kColumnGap);
    }

    // Menu_mc is authored at 1280x720; its parent applies the screen scale.
    constexpr double leftInset = 16.0;
    constexpr double bottomInset = 1.0;
    constexpr double menuHeight = 720.0;
    const double lastRowY = hasPlugins
        ? kPluginRowsY +
            static_cast<double>(kPluginRowsPerColumn - 1) * kPluginRowHeight
        : 0.0;
    const double blockY =
        menuHeight - bottomInset - (lastRowY + kFieldHeight) * kBlockScale;

    // The logo animation shows the block when it ends.
    return block.set("name", kVersionBlockName) &&
        block.set("visible", false) &&
        block.set("x", leftInset) &&
        block.set("y", blockY) &&
        block.set("scaleX", kBlockScale) &&
        block.set("scaleY", kBlockScale) &&
        block.set("mouseEnabled", false) &&
        block.set("mouseChildren", false) &&
        menu.call("addChild", block).valid();
}

}  // namespace sf::modules
