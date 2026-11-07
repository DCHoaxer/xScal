#pragma once

#include <string>
#include <string_view>

namespace sf::modules {

// sLanguage from [General] of the first of these files that has it: Fallout76Custom.ini and
// Fallout76.ini in Documents\My Games\Fallout 76, then Fallout76.ini next to the game executable.
// Lower case; empty when none has it.
[[nodiscard]] std::string readGameLanguage();

// The text column of PluginSettings.swf (Texts.as) for a game sLanguage value: en, pl, de, fr,
// it, es, ru, zhhans -> zh, zhhant -> zh2. Other values (ja, ko, ptbr, ...) give "en".
[[nodiscard]] const char* languageCodeForGame(std::string_view gameValue) noexcept;

}
