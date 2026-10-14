#include <Windows.h>

#include <array>
#include <cstddef>

namespace {

INIT_ONCE systemDxgiOnce = INIT_ONCE_STATIC_INIT;
HMODULE systemDxgi = nullptr;

BOOL CALLBACK LoadSystemDxgi(PINIT_ONCE, PVOID, PVOID*) noexcept {
    std::array<wchar_t, MAX_PATH> path{};
    const UINT length = ::GetSystemDirectoryW(path.data(), static_cast<UINT>(path.size()));
    constexpr wchar_t suffix[] = L"\\dxgi.dll";
    constexpr std::size_t suffixLength = (sizeof(suffix) / sizeof(suffix[0])) - 1;
    if (length == 0 || length >= path.size() ||
        suffixLength >= path.size() - length) {
        return FALSE;
    }

    for (std::size_t index = 0; index <= suffixLength; ++index) {
        path[length + index] = suffix[index];
    }
    systemDxgi = ::LoadLibraryW(path.data());
    return systemDxgi != nullptr;
}

}

extern "C" FARPROC __fastcall ResolveSystemDxgiExport(const char* name) noexcept {
    if (name == nullptr ||
        ::InitOnceExecuteOnce(&systemDxgiOnce, &LoadSystemDxgi, nullptr, nullptr) == FALSE ||
        systemDxgi == nullptr) {
        return nullptr;
    }
    return ::GetProcAddress(systemDxgi, name);
}