#include "platform/diagnostics.hpp"
#include "plugins/plugins.hpp"
#include "runtime/runtime.hpp"

#include <Windows.h>

#include <atomic>
#include <new>

namespace {

// Main instance. GFx function objects can remain callable after the MovieRoot hook is
// restored. InitializeBridge pins the module and leaves this runtime allocated
// so NativeFunctionHandler's exact 0x10-byte ABI object can route those calls.
std::atomic<sf::Runtime*> runtimeInstance{};

bool InitializeBridge() noexcept {
    HMODULE pinnedModule{};
    if (::GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&runtimeInstance),
            &pinnedModule) == FALSE) {
        sf::DiagnosticLog("xScal initialization failed: could not pin module");
        return false;
    }

    auto* candidate = new (std::nothrow) sf::Runtime();
    if (candidate == nullptr) {
        return false;
    }

    sf::Runtime* expected = nullptr;
    if (!runtimeInstance.compare_exchange_strong(
            expected, candidate, std::memory_order_acq_rel, std::memory_order_acquire)) {
        delete candidate;
        return false;
    }

    const auto hookStatus = sf::InitializeCurrentProcess(*candidate);
    if (hookStatus == sf::VtableHookStatus::Installed) {
        sf::DiagnosticLog("Hook installed");
    } else {
        sf::DiagnosticLogFormat(
            "Hook installation failed: %s",
            sf::VtableHookStatusName(hookStatus));
    }
    return hookStatus == sf::VtableHookStatus::Installed;
}

DWORD WINAPI InitializePlugins(LPVOID) noexcept {
    sf::plugins::Load();
    return 0;
}

}

// export stuff
extern "C" BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) noexcept {
    if (reason == DLL_PROCESS_ATTACH) {
        sf::SetDiagnosticModule(module);
        sf::plugins::InstallCreationHooks(::GetModuleHandleW(nullptr), &InitializeBridge);
        (void)::DisableThreadLibraryCalls(module);
        HANDLE thread = ::CreateThread(nullptr, 0, &InitializePlugins, nullptr, 0, nullptr);
        if (thread != nullptr) {
            (void)::CloseHandle(thread);
        }
    }
    return TRUE;
}
