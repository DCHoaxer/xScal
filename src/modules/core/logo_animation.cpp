#include "modules/core/logo_animation.hpp"

#include "api/scaleform_movie_interface.hpp"
#include "xscal/movie.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <utility>

namespace sf::modules {
namespace {

// The animation is drawn with Graphics calls and moved by setting properties, so it adds no image data
// to dxgi.dll. Lengths are in units of a 300x300 canvas with its origin at the center.
constexpr double kCanvasSize = 300.0;
constexpr double kCenter = kCanvasSize / 2.0;

// 86.4 screen pixels at 1920x1080, where the parent of Menu_mc scales it by 1.5. Menu_mc is authored at
// 1280x720.
constexpr double kLogoSize = 86.4 * 2.0 / 3.0;
constexpr double kLogoInset = 16.0;
constexpr double kMenuHeight = 720.0;

constexpr std::uint32_t kRingColor = 0xFF7700;
constexpr std::uint32_t kHoleColor = 0x101010;
constexpr std::uint32_t kGearColor = 0x1E6E8E;

constexpr double kRingRadius = 147.0;
constexpr double kGearBodyRadius = 97.0;
constexpr double kGearTipRadius = 121.0;
constexpr double kGearToothHalfWidth = 22.0;
constexpr int kGearTeeth = 8;

// The band of the ring that fades out. Its left edge is hidden under the gear hole.
constexpr double kBandLeft = 75.0;
constexpr double kBandHalfHeight = 84.0;
constexpr double kArcStepDegrees = 3.0;

// The F on the gear, traced from the logo, centered on the gear and inside its body radius.
constexpr std::int8_t kFStem[][2] = {
    {17, -59}, {63, -59}, {58, -31}, {25, -30}, {17, -26}, {13, -18}, {2, 35}, {-3, 44}, {-11, 52},
    {-20, 57}, {-27, 59}, {-63, 59}, {-57, 28}, {-33, 28}, {-27, 23}, {-16, -31}, {-10, -42}, {2, -53}};
constexpr std::int8_t kFUpperBar[][2] = {{28, -17}, {54, -17}, {51, 0}, {24, 0}};
constexpr std::int8_t kFLowerBar[][2] = {{22, 8}, {48, 8}, {45, 25}, {18, 25}};

constexpr double kPi = 3.14159265358979323846;

// The timeline counts positions from 0 to kLastPosition: a pause, the gear leaving and the text
// arriving, a pause with the status dots, and the same motion in reverse.
constexpr double kStartDelayMs = 1500.0;
constexpr double kFirstPauseMs = 670.0;
constexpr double kPositionMs = 30.0;
constexpr double kLastPosition = 78.0;
constexpr double kMotionMs = (kLastPosition - 1.0) * kPositionMs;
constexpr double kHoldStartMs = kFirstPauseMs + kMotionMs;
constexpr double kHoldMs = 2400.0;
constexpr double kEndMs = kHoldStartMs + kHoldMs + kMotionMs;
constexpr double kDotsStepMs = 260.0;

// Canvas x at evenly spaced timeline positions.
constexpr std::int16_t kGearX[] = {150, 144, 130, 104, 68, 24, -20, -66, -100, -130};
constexpr double kGearXFirstPosition = 16.0;
constexpr double kGearXStep = 4.0;
constexpr std::int16_t kTextX[] = {328, 274, 226, 187, 154, 127, 106, 89, 78, 69, 64, 61, 60, 59};
constexpr double kTextXFirstPosition = 52.0;
constexpr double kTextXStep = 2.0;

// The gear rolls without slipping on a circle of this radius.
constexpr double kGearRollRadius = 121.5;
constexpr double kGearGrowFirstPosition = 4.0;
constexpr double kGearGrowLastPosition = 12.0;
constexpr double kGearGrownScale = 1.06;
constexpr double kBandFadeFirstPosition = 20.0;
constexpr double kBandFadeLength = 34.0;

constexpr double kTitleY = 82.0;
constexpr double kTitleSize = 80.0;
constexpr double kStatusY = 160.0;
constexpr double kStatusSize = 40.0;
constexpr double kTextGutter = 2.0;
constexpr const char* kStatusTexts[] = {"Booting...", "Booting", "Booting.", "Booting.."};

constexpr std::uint32_t kCreateAttempts = 600;

[[nodiscard]] double positionAt(double ms) noexcept {
    if (ms < kFirstPauseMs) {
        return 0.0;
    }
    if (ms < kHoldStartMs) {
        return 1.0 + (ms - kFirstPauseMs) / kPositionMs;
    }
    if (ms < kHoldStartMs + kHoldMs) {
        return kLastPosition;
    }
    return kLastPosition - (ms - kHoldStartMs - kHoldMs) / kPositionMs;
}

template <std::size_t Count>
[[nodiscard]] double readTable(
    const std::int16_t (&table)[Count],
    double firstPosition,
    double step,
    double position) noexcept {
    const double index = (std::clamp)(
        (position - firstPosition) / step, 0.0, static_cast<double>(Count - 1));
    const auto low = (std::min)(static_cast<std::size_t>(index), Count - 2);
    const double blend = index - static_cast<double>(low);
    return table[low] + (table[low + 1] - table[low]) * blend;
}

[[nodiscard]] bool addSprite(const xscal::Value& parent, const xscal::Value& sprite) noexcept {
    return sprite.type() == XSCAL_MOVIE_TYPE_DISPLAY_OBJECT && parent.call("addChild", sprite).valid();
}

void lineTo(const xscal::Value& graphics, double x, double y) noexcept {
    graphics.call("lineTo", x, y);
}

void fillPolygon(
    const xscal::Value& graphics,
    std::uint32_t color,
    const std::int8_t (*points)[2],
    std::size_t count) noexcept {
    graphics.call("beginFill", color, 1.0);
    graphics.call("moveTo", static_cast<double>(points[0][0]), static_cast<double>(points[0][1]));
    for (std::size_t index = 1; index < count; ++index) {
        lineTo(graphics, points[index][0], points[index][1]);
    }
    graphics.call("endFill");
}

void drawArc(const xscal::Value& graphics, double firstDegrees, double lastDegrees) noexcept {
    for (double degrees = firstDegrees;; degrees += kArcStepDegrees) {
        const double radians = (std::min)(degrees, lastDegrees) * kPi / 180.0;
        lineTo(graphics, kRingRadius * std::cos(radians), kRingRadius * std::sin(radians));
        if (degrees >= lastDegrees) {
            return;
        }
    }
}

void drawGear(const xscal::Value& sprite, std::uint32_t color) noexcept {
    const xscal::Value graphics = sprite.get("graphics");
    graphics.call("beginFill", color, 1.0);
    const double toothAngle = std::asin(kGearToothHalfWidth / kGearBodyRadius);
    graphics.call(
        "moveTo",
        kGearBodyRadius * std::cos(-toothAngle),
        kGearBodyRadius * std::sin(-toothAngle));
    for (int tooth = 0; tooth < kGearTeeth; ++tooth) {
        const double angle = tooth * 2.0 * kPi / kGearTeeth;
        const double cosine = std::cos(angle);
        const double sine = std::sin(angle);
        lineTo(
            graphics,
            kGearBodyRadius * std::cos(angle - toothAngle),
            kGearBodyRadius * std::sin(angle - toothAngle));
        lineTo(
            graphics,
            kGearTipRadius * cosine + kGearToothHalfWidth * sine,
            kGearTipRadius * sine - kGearToothHalfWidth * cosine);
        lineTo(
            graphics,
            kGearTipRadius * cosine - kGearToothHalfWidth * sine,
            kGearTipRadius * sine + kGearToothHalfWidth * cosine);
        lineTo(
            graphics,
            kGearBodyRadius * std::cos(angle + toothAngle),
            kGearBodyRadius * std::sin(angle + toothAngle));
    }
    graphics.call("endFill");
}

// The game's fonts draw boxes unless setTextFormat follows every change of text. It gets a newly built
// format, as in CoreModule::AddVersionText; the value read back from field.defaultTextFormat is not used.
[[nodiscard]] bool setFieldText(
    const xscal::Movie& movie,
    const xscal::Value& field,
    const char* text,
    double size) noexcept {
    const xscal::Value format = movie.get("root1.Menu_mc.VersionText_tf").get("defaultTextFormat");
    return format.isObject() &&
        format.set("font", "$MAIN_Font") &&
        format.set("bold", false) &&
        format.set("size", size) &&
        format.set("align", "left") &&
        field.set("defaultTextFormat", format) &&
        field.set("text", text) &&
        field.set("embedFonts", false) &&
        field.setTextFormat(format) &&
        field.set("textColor", kRingColor);
}

[[nodiscard]] bool addText(
    const xscal::Movie& movie,
    const xscal::Value& parent,
    const xscal::Value& field,
    const char* text,
    double y,
    double size) noexcept {
    const xscal::Value styleText = movie.get("root1.Menu_mc.SplashScreenHolder_mc.SplashScreenText_tf");
    return styleText.isObject() &&
        field.type() == XSCAL_MOVIE_TYPE_DISPLAY_OBJECT &&
        field.set("y", y - kCenter) &&
        field.set("width", kCanvasSize) &&
        field.set("height", size * 1.5) &&
        field.set("selectable", false) &&
        field.set("mouseEnabled", false) &&
        setFieldText(movie, field, text, size) &&
        field.set("filters", styleText.get("filters")) &&
        field.set("antiAliasType", styleText.get("antiAliasType")) &&
        parent.call("addChild", field).valid();
}

class LogoAnimationPatch final : public xscal::MoviePatch {
public:
    LogoAnimationPatch() : MoviePatch("mainmenu.swf", wantFrames) {}

    void onLoad(xscal::Movie& movie) override {
        movieIdentity_ = movie.identity();
        step_ = Step::Create;
        createAttempts_ = 0;
    }

    void onUnload(xscal::Movie& movie) override {
        if (movie.identity() != movieIdentity_) {
            return;
        }
        dropRefs(movie);
        movieIdentity_ = 0;
    }

    void onFrame(xscal::Movie& movie) override {
        if (movieIdentity_ == 0 || movie.identity() != movieIdentity_) {
            return;
        }
        switch (step_) {
        case Step::Create:
            if (createLogo(movie)) {
                startTime_ = std::chrono::steady_clock::now();
                step_ = Step::Wait;
            } else if (++createAttempts_ == kCreateAttempts) {
                step_ = Step::ShowText;
            }
            break;
        case Step::Wait: {
            const std::chrono::duration<double, std::milli> elapsed =
                std::chrono::steady_clock::now() - startTime_;
            if (elapsed.count() >= kStartDelayMs) {
                (void)movie.load(logoRef_).set("visible", true);
                startTime_ = std::chrono::steady_clock::now();
                step_ = Step::Play;
            }
            break;
        }
        case Step::Play: {
            const std::chrono::duration<double, std::milli> elapsed =
                std::chrono::steady_clock::now() - startTime_;
            if (elapsed.count() >= kEndMs) {
                (void)movie.get("root1.Menu_mc").call("removeChild", movie.load(logoRef_));
                dropRefs(movie);
                step_ = Step::ShowText;
            } else {
                moveLogo(movie, elapsed.count());
            }
            break;
        }
        case Step::ShowText:
            showVersionBlock(movie);
            break;
        case Step::Done:
            break;
        }
    }

private:
    enum class Step : std::uint8_t { Create, Wait, Play, ShowText, Done };

    // CoreModule may add the block after the animation ended, so this repeats until the block exists.
    void showVersionBlock(const xscal::Movie& movie) {
        const xscal::Value block =
            movie.get("root1.Menu_mc").call("getChildByName", kVersionBlockName);
        if (block.isObject() && block.set("visible", true)) {
            step_ = Step::Done;
        }
    }

    // The logo joins the menu after all of its parts exist, so a retry does not leave a partial logo.
    [[nodiscard]] bool createLogo(const xscal::Movie& movie) {
        const xscal::Value menu = movie.get("root1.Menu_mc");
        xscal::Value logo = movie.create("flash.display.Sprite");
        if (!menu.isObject() || logo.type() != XSCAL_MOVIE_TYPE_DISPLAY_OBJECT) {
            return false;
        }
        const xscal::Value content = movie.create("flash.display.Sprite");
        const xscal::Value canvasMask = movie.create("flash.display.Sprite");
        const xscal::Value ring = movie.create("flash.display.Sprite");
        xscal::Value band = movie.create("flash.display.Sprite");
        const xscal::Value hole = movie.create("flash.display.Sprite");
        xscal::Value gear = movie.create("flash.display.Sprite");
        if (!addSprite(logo, content) || !addSprite(logo, canvasMask) || !addSprite(content, ring) ||
            !addSprite(content, band) || !addSprite(content, hole) || !addSprite(content, gear)) {
            return false;
        }

        // The gear and the text move past the edges of the canvas.
        const xscal::Value maskGraphics = canvasMask.get("graphics");
        maskGraphics.call("beginFill", kRingColor, 1.0);
        maskGraphics.call("drawRect", -kCenter, -kCenter, kCanvasSize, kCanvasSize);
        maskGraphics.call("endFill");
        if (!content.set("mask", canvasMask)) {
            return false;
        }

        const double bandDegrees =
            std::asin(kBandHalfHeight / kRingRadius) * 180.0 / kPi;
        const xscal::Value ringGraphics = ring.get("graphics");
        ringGraphics.call("beginFill", kRingColor, 1.0);
        ringGraphics.call("moveTo", kBandLeft, kBandHalfHeight);
        drawArc(ringGraphics, bandDegrees, 360.0 - bandDegrees);
        lineTo(ringGraphics, kBandLeft, -kBandHalfHeight);
        ringGraphics.call("endFill");

        const xscal::Value bandGraphics = band.get("graphics");
        bandGraphics.call("beginFill", kRingColor, 1.0);
        bandGraphics.call("moveTo", kBandLeft, -kBandHalfHeight);
        drawArc(bandGraphics, -bandDegrees, bandDegrees);
        lineTo(bandGraphics, kBandLeft, kBandHalfHeight);
        bandGraphics.call("endFill");

        drawGear(hole, kHoleColor);
        drawGear(gear, kGearColor);
        const xscal::Value gearGraphics = gear.get("graphics");
        fillPolygon(gearGraphics, kRingColor, kFStem, std::size(kFStem));
        fillPolygon(gearGraphics, kRingColor, kFUpperBar, std::size(kFUpperBar));
        fillPolygon(gearGraphics, kRingColor, kFLowerBar, std::size(kFLowerBar));

        xscal::Value title = movie.create("flash.text.TextField");
        xscal::Value status = movie.create("flash.text.TextField");
        if (!addText(movie, content, title, "xScal", kTitleY, kTitleSize) ||
            !addText(movie, content, status, kStatusTexts[0], kStatusY, kStatusSize)) {
            return false;
        }

        // kTextX ends at the left edge of the GIF's text. The widths here depend on the game's font, so
        // the title is centered on the canvas and the status under it from the measured widths.
        double titleWidth{};
        double statusWidth{};
        if (!title.get("textWidth").read(titleWidth) || !status.get("textWidth").read(statusWidth)) {
            return false;
        }
        textShift_ = kCenter - titleWidth / 2.0 - kTextX[std::size(kTextX) - 1];
        statusOffset_ = (titleWidth - statusWidth) / 2.0;

        const double logoScale = kLogoSize / kCanvasSize;
        const bool placed = logo.set("x", kLogoInset + kLogoSize / 2.0) &&
            logo.set("y", kMenuHeight - kLogoInset - kLogoSize / 2.0) &&
            logo.set("scaleX", logoScale) &&
            logo.set("scaleY", logoScale) &&
            logo.set("mouseEnabled", false) &&
            logo.set("mouseChildren", false) &&
            logo.set("visible", false);
        if (!placed) {
            return false;
        }

        logoRef_ = movie.retain(std::move(logo));
        bandRef_ = movie.retain(std::move(band));
        gearRef_ = movie.retain(std::move(gear));
        titleRef_ = movie.retain(std::move(title));
        statusRef_ = movie.retain(std::move(status));
        statusIndex_ = 0;
        if (!logoRef_ || !bandRef_ || !gearRef_ || !titleRef_ || !statusRef_) {
            dropRefs(movie);
            return false;
        }
        moveLogo(movie, 0.0);
        if (!menu.call("addChild", movie.load(logoRef_)).valid()) {
            dropRefs(movie);
            return false;
        }
        return true;
    }

    void moveLogo(const xscal::Movie& movie, double ms) {
        const double position = positionAt(ms);

        const double gearX = readTable(kGearX, kGearXFirstPosition, kGearXStep, position);
        const double growth = (std::clamp)(
            (position - kGearGrowFirstPosition) / (kGearGrowLastPosition - kGearGrowFirstPosition),
            0.0,
            1.0);
        const double gearScale = 1.0 + (kGearGrownScale - 1.0) * growth;
        const xscal::Value gear = movie.load(gearRef_);
        (void)gear.set("x", gearX - kCenter);
        (void)gear.set("rotation", (gearX - kCenter) / kGearRollRadius * 180.0 / kPi);
        (void)gear.set("scaleX", gearScale);
        (void)gear.set("scaleY", gearScale);

        const double bandAlpha =
            1.0 - (std::clamp)((position - kBandFadeFirstPosition) / kBandFadeLength, 0.0, 1.0);
        (void)movie.load(bandRef_).set("alpha", bandAlpha);

        const double textX =
            readTable(kTextX, kTextXFirstPosition, kTextXStep, position) + textShift_ - kCenter;
        (void)movie.load(titleRef_).set("x", textX - kTextGutter);
        const xscal::Value status = movie.load(statusRef_);
        (void)status.set("x", textX + statusOffset_ - kTextGutter);

        // The dots restart from an empty row, one step after the hold begins.
        std::size_t statusIndex = 0;
        if (ms >= kHoldStartMs && ms < kHoldStartMs + kHoldMs) {
            const auto steps = static_cast<std::size_t>((ms - kHoldStartMs) / kDotsStepMs);
            statusIndex = steps % std::size(kStatusTexts);
        }
        if (statusIndex != statusIndex_ &&
            setFieldText(movie, status, kStatusTexts[statusIndex], kStatusSize)) {
            statusIndex_ = statusIndex;
        }
    }

    void dropRefs(const xscal::Movie& movie) {
        movie.drop(logoRef_);
        movie.drop(bandRef_);
        movie.drop(gearRef_);
        movie.drop(titleRef_);
        movie.drop(statusRef_);
        logoRef_ = {};
        bandRef_ = {};
        gearRef_ = {};
        titleRef_ = {};
        statusRef_ = {};
    }

    // movie.identity() of the open menu movie, 0 when none.
    std::uintptr_t movieIdentity_{};
    Step step_{Step::Create};
    std::uint32_t createAttempts_{};
    std::chrono::steady_clock::time_point startTime_{};
    std::size_t statusIndex_{};
    double textShift_{};
    double statusOffset_{};
    xscal::Ref logoRef_;
    xscal::Ref bandRef_;
    xscal::Ref gearRef_;
    xscal::Ref titleRef_;
    xscal::Ref statusRef_;
};

LogoAnimationPatch logoAnimationPatch;

}  // namespace

void InstallLogoAnimation() noexcept {
    (void)logoAnimationPatch.install(movieapi::Interface());
}

}  // namespace sf::modules
