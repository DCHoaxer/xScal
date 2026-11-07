#include "modules/core/game_language.hpp"

#include <Windows.h>
#include <KnownFolders.h>
#include <ShlObj.h>

#include <vector>

namespace sf::modules {
namespace {

struct GameLanguage final {
    std::string_view gameValue;
    const char* code;
};

constexpr GameLanguage kGameLanguages[]{
    {"en", "en"}, {"pl", "pl"}, {"de", "de"}, {"fr", "fr"}, {"it", "it"},
    {"es", "es"}, {"ru", "ru"}, {"zhhans", "zh"}, {"zhhant", "zh2"},
};

std::wstring gameDirectory()
{
    wchar_t path[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return {};
    std::wstring result{path, length};
    result.resize(result.find_last_of(L'\\') + 1);
    return result;
}

}

std::string readGameLanguage()
{
    std::vector<std::wstring> files;
    PWSTR documents = nullptr;
    if (SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &documents) == S_OK) {
        const std::wstring folder = std::wstring{documents} + L"\\My Games\\Fallout 76\\";
        files.push_back(folder + L"Fallout76Custom.ini");
        files.push_back(folder + L"Fallout76.ini");
    }
    CoTaskMemFree(documents);
    if (const std::wstring directory = gameDirectory(); !directory.empty())
        files.push_back(directory + L"Fallout76.ini");

    for (const std::wstring& file : files) {
        wchar_t value[64];
        GetPrivateProfileStringW(L"General", L"sLanguage", L"", value, 64, file.c_str());
        if (value[0] == L'\0')
            continue;
        std::string result;
        for (const wchar_t* c = value; *c != L'\0'; ++c)
            result += *c >= L'A' && *c <= L'Z' ? static_cast<char>(*c - L'A' + 'a')
                : *c < 128                     ? static_cast<char>(*c)
                                               : '?';
        return result;
    }
    return {};
}

const char* languageCodeForGame(std::string_view gameValue) noexcept
{
    for (const GameLanguage& language : kGameLanguages) {
        if (language.gameValue == gameValue)
            return language.code;
    }
    return "en";
}

}
