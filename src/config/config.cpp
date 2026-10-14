#include "config/config.h"

#include "common/ascii_case.hpp"
#include "platform/diagnostics.hpp"
#include "platform/win32_file.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <string>

namespace sf::config {
namespace {

constexpr std::size_t kMaximumIniSize = 64 * 1024;

enum class IniSection {
    None,
    AddressLibrary,
    Chat,
    Scaleform,
    XScalPriority,
};

struct IniSectionName final {
    IniSection section;
    std::string_view name;
};

constexpr std::array kIniSectionNames{
    IniSectionName{IniSection::AddressLibrary, "AddressLibrary"},
    IniSectionName{IniSection::Chat, "Chat"},
    IniSectionName{IniSection::Scaleform, "Scaleform"},
    IniSectionName{IniSection::XScalPriority, "xScalPriority"},
};

[[nodiscard]] constexpr bool IsSpace(char character) noexcept {
    return character == ' ' || character == '\t' || character == '\r';
}

[[nodiscard]] std::string_view Trim(std::string_view value) noexcept {
    while (!value.empty() && IsSpace(value.front())) {
        value.remove_prefix(1);
    }
    while (!value.empty() && IsSpace(value.back())) {
        value.remove_suffix(1);
    }
    return value;
}

[[nodiscard]] bool EqualsIgnoreCase(
    std::string_view left,
    std::string_view right) noexcept {
    return EqualsAsciiIgnoreCase(left, right);
}

[[nodiscard]] bool ReadBoolean(
    std::string_view value,
    bool& result) noexcept {
    if (EqualsIgnoreCase(value, "true")) {
        result = true;
        return true;
    }
    if (EqualsIgnoreCase(value, "false")) {
        result = false;
        return true;
    }
    return false;
}

[[nodiscard]] bool ApplyAddressLibrarySetting(
    std::string_view name,
    std::string_view value,
    AddressLibraryConfig& config) noexcept {
    return EqualsIgnoreCase(name, "autoUpdate") &&
        ReadBoolean(value, config.autoUpdate);
}

[[nodiscard]] bool ReadPriority(
    std::string_view value,
    ProcessPriorityClass& result) noexcept {
    if (value == "-1" || EqualsIgnoreCase(value, "NoChange")) {
        result = ProcessPriorityClass::NoChange;
        return true;
    }
    if (value == "3" || EqualsIgnoreCase(value, "AboveNormal")) {
        result = ProcessPriorityClass::AboveNormal;
        return true;
    }
    if (value == "4" || EqualsIgnoreCase(value, "High")) {
        result = ProcessPriorityClass::High;
        return true;
    }
    return false;
}

[[nodiscard]] bool ReadMinimumLogicalProcessors(
    std::string_view value,
    std::uint32_t& result) noexcept {
    std::uint32_t parsed{};
    const auto [end, error] = std::from_chars(
        value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || end != value.data() + value.size()) {
        return false;
    }
    result = parsed;
    return true;
}

void LogInvalidSetting(
    std::string_view section,
    std::string_view name) noexcept {
    DiagnosticLogFormat(
        "xScal config: invalid %.*s.%.*s",
        static_cast<int>(section.size()),
        section.data(),
        static_cast<int>(name.size()),
        name.data());
}

[[nodiscard]] bool ApplyChatSetting(
    std::string_view name,
    std::string_view value,
    ChatConfig& config) noexcept {
    if (EqualsIgnoreCase(name, "enabled")) {
        return ReadBoolean(value, config.enabled);
    }
    if (EqualsIgnoreCase(name, "relayEndpoint")) {
        if (value.empty() || value.find('\0') != std::string_view::npos ||
            value.size() >= sizeof(config.relayEndpoint)) {
            return false;
        }
        std::fill_n(
            config.relayEndpoint, sizeof(config.relayEndpoint), '\0');
        std::copy(
            value.begin(), value.end(), config.relayEndpoint);
        return true;
    }
    return false;
}

[[nodiscard]] bool ApplyXScalPrioritySetting(
    std::string_view name,
    std::string_view value,
    XScalPriorityConfig& config) noexcept {
    if (EqualsIgnoreCase(name, "enabled")) {
        return ReadBoolean(value, config.enabled);
    }
    if (EqualsIgnoreCase(name, "priority")) {
        return ReadPriority(value, config.priority);
    }
    if (EqualsIgnoreCase(name, "autoAffinity")) {
        return ReadBoolean(value, config.autoAffinity);
    }
    if (EqualsIgnoreCase(name, "performanceCoresOnly")) {
        return ReadBoolean(value, config.performanceCoresOnly);
    }
    if (EqualsIgnoreCase(name, "oneLogicalProcessorPerCore")) {
        return ReadBoolean(value, config.oneLogicalProcessorPerCore);
    }
    if (EqualsIgnoreCase(name, "minimumLogicalProcessors")) {
        return ReadMinimumLogicalProcessors(
            value, config.minimumLogicalProcessors);
    }

    return false;
}

[[nodiscard]] bool IsScaleformComponentPath(
    std::string_view value) noexcept {
    constexpr std::array<std::string_view, 3> roots{
        "root1.",
        "root.",
        "_root.",
    };
    const auto root = std::find_if(
        roots.begin(),
        roots.end(),
        [value](std::string_view candidate) {
            return value.starts_with(candidate);
        });
    if (root == roots.end()) {
        return false;
    }

    std::string_view components = value.substr(root->size());
    while (!components.empty()) {
        const std::size_t separator = components.find('.');
        const std::string_view component = components.substr(0, separator);
        if (component.empty() || component == "__SFCodeObj") {
            return false;
        }
        if (separator == std::string_view::npos) {
            return true;
        }
        components.remove_prefix(separator + 1);
    }
    return false;
}

[[nodiscard]] bool ApplyScaleformSetting(
    std::string_view name,
    std::string_view value,
    ScaleformConfig& config) noexcept {
    if (!EqualsIgnoreCase(name, "componentPath") ||
        value.empty() || value.find('\0') != std::string_view::npos ||
        value.size() > kMaximumScaleformComponentPathLength ||
        !IsScaleformComponentPath(value)) {
        return false;
    }
    for (std::size_t index = 0;
         index < config.componentPathCount;
         ++index) {
        if (config.ComponentPath(index) == value) {
            return true;
        }
    }
    if (config.componentPathCount == config.componentPaths.size()) {
        return false;
    }

    auto& destination =
        config.componentPaths[config.componentPathCount++];
    std::copy(value.begin(), value.end(), destination.begin());
    destination[value.size()] = '\0';
    return true;
}

}

bool ApplyXScalIni(
    std::string_view contents,
    RuntimeConfig& config) noexcept {
    IniSection section{IniSection::None};
    bool valid{true};

    if (contents.starts_with("\xEF\xBB\xBF")) {
        contents.remove_prefix(3);
    }
    while (!contents.empty()) {
        const std::size_t lineEnd = contents.find('\n');
        std::string_view line = contents.substr(0, lineEnd);
        contents = lineEnd == std::string_view::npos
            ? std::string_view{}
            : contents.substr(lineEnd + 1);

        // ';' and '#' start a comment at the beginning of a line or after
        // whitespace, so values such as URLs can contain them.
        for (std::size_t index = 1; index < line.size(); ++index) {
            if ((line[index] == ';' || line[index] == '#') &&
                (line[index - 1] == ' ' || line[index - 1] == '\t')) {
                line = line.substr(0, index);
                break;
            }
        }
        line = Trim(line);
        if (line.empty() || line.front() == ';' || line.front() == '#') {
            continue;
        }
        if (line.front() == '[') {
            if (line.back() != ']') {
                DiagnosticLog("xScal config: invalid section line");
                section = IniSection::None;
                valid = false;
                continue;
            }
            const std::string_view name =
                Trim(line.substr(1, line.size() - 2));
            section = IniSection::None;
            for (const auto& candidate : kIniSectionNames) {
                if (EqualsIgnoreCase(name, candidate.name)) {
                    section = candidate.section;
                }
            }
            continue;
        }
        if (section == IniSection::None) {
            continue;
        }

        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos) {
            DiagnosticLog("xScal config: invalid setting line");
            valid = false;
            continue;
        }
        const std::string_view name = Trim(line.substr(0, equals));
        const std::string_view value = Trim(line.substr(equals + 1));
        bool applied{};
        if (!name.empty()) {
            if (section == IniSection::AddressLibrary) {
                applied = ApplyAddressLibrarySetting(
                    name, value, config.addressLibrary);
            } else if (section == IniSection::Chat) {
                applied = ApplyChatSetting(name, value, config.chat);
            } else if (section == IniSection::Scaleform) {
                applied = ApplyScaleformSetting(
                    name, value, config.scaleform);
            } else {
                applied = ApplyXScalPrioritySetting(
                    name, value, config.xscalPriority);
            }
        }
        if (!applied) {
            if (!name.empty()) {
                for (const auto& candidate : kIniSectionNames) {
                    if (candidate.section == section) {
                        LogInvalidSetting(candidate.name, name);
                    }
                }
            }
            valid = false;
        }
    }
    return valid;
}

RuntimeConfig LoadRuntimeConfig() noexcept {
    RuntimeConfig config{};
    platform::ExecutableRelativePath path;
    if (!platform::ResolveExecutableRelativePath(L"xscal.ini", path)) {
        DiagnosticLog("xScal config: could not resolve xscal.ini path");
        return config;
    }

    const platform::FilePresence presence = platform::QueryFilePresence(path);
    if (presence == platform::FilePresence::Missing) {
        return config;
    }
    if (presence == platform::FilePresence::Error) {
        DiagnosticLog("xScal config: could not query xscal.ini");
        return config;
    }

    std::string contents;
    if (!platform::ReadBoundedFile(path, kMaximumIniSize, contents)) {
        DiagnosticLog("xScal config: could not read xscal.ini");
        return config;
    }
    config.loadedFromIni = true;
    (void)ApplyXScalIni(contents, config);
    return config;
}

const RuntimeConfig& GetRuntimeConfig() noexcept {
    static const RuntimeConfig config = LoadRuntimeConfig();
    return config;
}

}  // namespace sf::config
