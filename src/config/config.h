#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace sf::config {

struct XScalVersion final {
    std::uint8_t major;
    std::uint8_t minor;
    std::uint8_t patch;
};

inline constexpr XScalVersion kXScalVersion{0, 4, 27};

inline constexpr std::size_t kMaximumScaleformComponentPaths = 16;
inline constexpr std::size_t kMaximumScaleformComponentPathLength = 255;

struct ScaleformConfig final {
    std::array<
        std::array<char, kMaximumScaleformComponentPathLength + 1>,
        kMaximumScaleformComponentPaths> componentPaths{};
    std::size_t componentPathCount{};

    [[nodiscard]] std::string_view ComponentPath(
        std::size_t index) const noexcept {
        return index < componentPathCount
            ? std::string_view{componentPaths[index].data()}
            : std::string_view{};
    }
};

//Default ini chatconfig values, can be overridden by the user in the xscal.ini file
struct ChatConfig final {
    bool enabled{true};
    char relayEndpoint[256]{"wss://falloutchatmod.com/relay"};

    [[nodiscard]] std::string_view Endpoint() const noexcept {
        return relayEndpoint;
    }
};

// Default ini address library values, can be overridden by the user in the xscal.ini file
struct AddressLibraryConfig final {
    bool autoUpdate{true};
};


enum class ProcessPriorityClass {
    NoChange,
    AboveNormal,
    High,
};

struct XScalPriorityConfig final {
    bool enabled{true};
    ProcessPriorityClass priority{ProcessPriorityClass::AboveNormal};
    bool autoAffinity{true};
    bool performanceCoresOnly{true};
    bool oneLogicalProcessorPerCore{false};
    std::uint32_t minimumLogicalProcessors{};

    [[nodiscard]] constexpr bool operator==(
        const XScalPriorityConfig&) const noexcept = default;
};

// Default xScalPriority settings. Other code refers to this constant, so keep
// it in sync with the member defaults above by using them directly.
inline constexpr XScalPriorityConfig kXScalPriority{};

struct RuntimeConfig final {
    ChatConfig chat{};
    AddressLibraryConfig addressLibrary{};
    XScalPriorityConfig xscalPriority{};
    ScaleformConfig scaleform{};
    bool loadedFromIni{};
};

[[nodiscard]] bool ApplyXScalIni(
    std::string_view contents,
    RuntimeConfig& config) noexcept;
[[nodiscard]] const RuntimeConfig& GetRuntimeConfig() noexcept;

}
