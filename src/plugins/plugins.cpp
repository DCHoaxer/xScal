#include "plugins/plugins.hpp"

#include "xscal/plugin_api.h"
#include "address/address_library_v3.hpp"
#include "api/json_interface.hpp"
#include "common/json.hpp"
#include "config/target_profile.hpp"
#include "platform/diagnostics.hpp"
#include "platform/win32_file.hpp"
#include "plugins/plugin_signature.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <deque>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sf::plugins {
namespace {

// Oldest plugin ABI this host loads. ABI 5 is the first SDK release; raising
// this drops every plugin built with an older SDK.
constexpr uint32_t kOldestPluginAbiVersion = 5u;

// Size of the XScalPlugin a plugin built for abiVersion fills in. Later ABI
// versions only append fields, so an older descriptor is a prefix of the
// current one. Returns 0 for versions this host does not know.
[[nodiscard]] constexpr uint32_t DescriptorSize(uint32_t abiVersion) noexcept {
    switch (abiVersion) {
    case 5u:
        return static_cast<uint32_t>(offsetof(XScalPlugin, scaleformCallbacks) +
            sizeof(XScalPlugin::scaleformCallbacks));
    default:
        return 0;
    }
}
static_assert(DescriptorSize(XSCAL_PLUGIN_ABI_VERSION) == sizeof(XScalPlugin),
    "Add the current ABI version and its descriptor size to DescriptorSize");

constexpr const char* kUnsupportedAbiRejection =
    "unsupported ABI version or descriptor size";
constexpr const char* kOutsideIdPrefixRejection =
    "plugin ID outside certificate prefix";

struct PluginModule {
    HMODULE library;
    XScalPlugin descriptor;
    bool active;
    bool loading;
    XScalHost host;
    const XScalPluginOptionsProviderV1* options;
    const XScalPluginOptionsProviderV2* optionsV2;
};

INIT_ONCE loadOnce = INIT_ONCE_STATIC_INIT;
platform::ExecutableRelativePath searchPath;
std::deque<PluginModule> modules;
PFN_D3D11_CREATE_DEVICE originalCreateDevice{};
PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN originalCreateDeviceAndSwapChain{};
PFN_D3D11_CREATE_DEVICE pluginCreateDevice{};
PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN pluginCreateDeviceAndSwapChain{};
const char* graphicsPluginName{};
bool startupChecked{};
bool hasCandidates{};
unsigned int hookedImports{};
DWORD startupError{};
thread_local bool inPluginCall{};
uint32_t targetKind = XSCAL_TARGET_UNKNOWN;
const XScalAddressLibraryV1* addressLibrary{};
const XScalScaleformMovieV2* scaleformMovieInterface{};
const XScalScaleformMovieV1* scaleformMovieInterfaceV1{};
bool (*initializeHost)() noexcept{};
volatile LONG crashDialogShown{};

int32_t __cdecl SetOptionsProvider(
    void* hostContext,
    const XScalPluginOptionsProviderV1* provider) noexcept {
    if (provider == nullptr ||
        provider->size < sizeof(XScalPluginOptionsProviderV1) ||
        provider->version < 1u ||
        provider->count == nullptr || provider->getOption == nullptr ||
        provider->setOption == nullptr) {
        return 0;
    }
    for (auto& module : modules) {
        if (&module == hostContext) {
            module.options = provider;
            module.optionsV2 = nullptr;
            return 1;
        }
    }
    return 0;
}

int32_t __cdecl SetOptionsProviderV2(
    void* hostContext,
    const XScalPluginOptionsProviderV2* provider) noexcept {
    if (provider == nullptr ||
        provider->size < sizeof(XScalPluginOptionsProviderV2) ||
        provider->version < 2u ||
        provider->count == nullptr || provider->getOption == nullptr ||
        provider->setOption == nullptr) {
        return 0;
    }
    for (auto& module : modules) {
        if (&module == hostContext) {
            module.options = nullptr;
            module.optionsV2 = provider;
            return 1;
        }
    }
    return 0;
}

constexpr XScalPluginOptionsV2 kPluginOptions{
    sizeof(XScalPluginOptionsV2),
    XSCAL_PLUGIN_OPTIONS_API_VERSION,
    &SetOptionsProvider,
    &SetOptionsProviderV2};

const void* __cdecl QueryHostInterface(
    void*,
    const char* name,
    uint32_t maximumVersion) noexcept {
    if (name == nullptr) return nullptr;
    // Each interface structure only grows at the end and keeps the layout of
    // every earlier version as its prefix, so the current structure is
    // returned to callers built for any version from 1 on. Comparing against
    // the current *_API_VERSION here would drop plugins built before an update.
    if (maximumVersion >= 1u &&
        std::strcmp(name, XSCAL_ADDRESS_LIBRARY_INTERFACE) == 0) {
        return addressLibrary;
    }
    if (maximumVersion >= 1u &&
        std::strcmp(name, XSCAL_SCALEFORM_MOVIE_INTERFACE) == 0) {
        // Plugins built with SDK 5 may require version == 1, so they get the
        // version 1 structure.
        return maximumVersion == 1u
            ? static_cast<const void*>(scaleformMovieInterfaceV1)
            : static_cast<const void*>(scaleformMovieInterface);
    }
    // Version 1 callers read only the XScalPluginOptionsV1 prefix.
    if (maximumVersion >= 1u &&
        std::strcmp(name, XSCAL_PLUGIN_OPTIONS_INTERFACE) == 0) {
        return &kPluginOptions;
    }
    if (maximumVersion >= 1u &&
        std::strcmp(name, XSCAL_JSON_INTERFACE) == 0) {
        return jsonapi::Interface();
    }
    return nullptr;
}

bool IsPluginFile(const WIN32_FIND_DATAW& file) noexcept {
    if (file.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return false;
    // The Windows wildcard search can also return names ending in .dll_disabled.
    const auto* extension = std::wcsrchr(file.cFileName, L'.');
    return extension && _wcsicmp(extension, L".dll") == 0;
}

[[nodiscard]] std::string_view CallbackName(
    const XScalScaleformCallback& callback) noexcept {
    const auto* const end = static_cast<const char*>(
        std::memchr(callback.name, '\0', sizeof(callback.name)));
    return end != nullptr
        ? std::string_view{callback.name,
              static_cast<std::size_t>(end - callback.name)}
        : std::string_view{};
}

[[nodiscard]] bool ActivePluginHasCallback(std::string_view name) noexcept {
    for (const auto& module : modules) {
        if (!module.active) continue;
        for (uint32_t index = 0;
             index < module.descriptor.scaleformCallbackCount;
             ++index) {
            if (CallbackName(module.descriptor.scaleformCallbacks[index]) == name)
                return true;
        }
    }
    return false;
}

[[nodiscard]] bool ScaleformCallbacksValid(
    const XScalPlugin& descriptor) noexcept {
    if (descriptor.scaleformCallbackCount > XSCAL_SCALEFORM_MAX_CALLBACKS ||
        (descriptor.scaleformCallbackCount != 0 &&
         descriptor.scaleformCallbacks == nullptr)) {
        return false;
    }
    for (uint32_t index = 0;
         index < descriptor.scaleformCallbackCount;
         ++index) {
        const auto& callback = descriptor.scaleformCallbacks[index];
        const std::string_view name = CallbackName(callback);
        if (name.empty() || callback.callback == nullptr ||
            ActivePluginHasCallback(name)) {
            return false;
        }
        for (uint32_t previous = 0; previous < index; ++previous) {
            if (CallbackName(descriptor.scaleformCallbacks[previous]) == name)
                return false;
        }
    }
    return true;
}

[[nodiscard]] bool ScaleformResultValid(
    const XScalScaleformValue& result) noexcept {
    switch (result.type) {
        case XSCAL_SCALEFORM_VALUE_BOOLEAN:
        case XSCAL_SCALEFORM_VALUE_INT32:
        case XSCAL_SCALEFORM_VALUE_UINT32:
        case XSCAL_SCALEFORM_VALUE_NUMBER:
            return true;
        case XSCAL_SCALEFORM_VALUE_STRING:
            return result.value.stringValue != nullptr &&
                result.stringLength <= XSCAL_SCALEFORM_MAX_RESULT_STRING_BYTES;
        default:
            return false;
    }
}

void __cdecl LogMessage(void* context, const char* message) {
    const auto* const module = static_cast<const PluginModule*>(context);
    if (module != nullptr && message != nullptr) {
        DiagnosticLogFormatImmediate(
            "%s: %s", module->descriptor.name, message);
    }
}

[[nodiscard]] bool IsPluginCrashException(DWORD code) noexcept {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        case EXCEPTION_DATATYPE_MISALIGNMENT:
        case EXCEPTION_FLT_DENORMAL_OPERAND:
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        case EXCEPTION_FLT_INEXACT_RESULT:
        case EXCEPTION_FLT_INVALID_OPERATION:
        case EXCEPTION_FLT_OVERFLOW:
        case EXCEPTION_FLT_STACK_CHECK:
        case EXCEPTION_FLT_UNDERFLOW:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_IN_PAGE_ERROR:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_INT_OVERFLOW:
        case EXCEPTION_PRIV_INSTRUCTION:
        case EXCEPTION_STACK_OVERFLOW:
            return true;
        default:
            return false;
    }
}

void ShowPluginCrash(DWORD code, const wchar_t* pluginName) noexcept {
    if (::InterlockedCompareExchange(&crashDialogShown, 1, 0) != 0) {
        return;
    }
    wchar_t message[512]{};
    std::swprintf(
        message,
        std::size(message),
        L"Plugin %ls crashed.\n\nException code: 0x%08lX\n\n"
        L"Fallout 76 cannot safely continue.",
        pluginName != nullptr && pluginName[0] != L'\0'
            ? pluginName
            : L"unknown",
        static_cast<unsigned long>(code));
    (void)::MessageBoxW(
        nullptr,
        message,
        L"xScal plugin crash",
        MB_OK | MB_ICONERROR | MB_SYSTEMMODAL);
}

int PluginCrashFilter(DWORD code, const char* pluginName) noexcept {
    if (!IsPluginCrashException(code)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    wchar_t wideName[128]{};
    if (pluginName == nullptr || pluginName[0] == '\0' ||
        ::MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            pluginName,
            -1,
            wideName,
            static_cast<int>(std::size(wideName))) == 0) {
        (void)wcscpy_s(wideName, L"unknown");
    }
    ShowPluginCrash(code, wideName);
    return EXCEPTION_CONTINUE_SEARCH;
}

int PluginCrashFilter(DWORD code, const wchar_t* pluginName) noexcept {
    if (IsPluginCrashException(code)) {
        ShowPluginCrash(code, pluginName);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

int32_t CallPluginQuery(
    XScalPluginQueryFn query,
    uint32_t hostAbiVersion,
    uint32_t descriptorSize,
    XScalPlugin* descriptor,
    const wchar_t* pluginName) noexcept {
    __try {
        return query(hostAbiVersion, descriptorSize, descriptor);
    } __except (PluginCrashFilter(GetExceptionCode(), pluginName)) {
        return 0;
    }
}

int32_t CallPluginLoad(
    int32_t (__cdecl *load)(const XScalHost*),
    const XScalHost* host,
    const char* pluginName) noexcept {
    __try {
        return load(host);
    } __except (PluginCrashFilter(GetExceptionCode(), pluginName)) {
        return 0;
    }
}

int32_t CallPluginScaleform(
    XScalScaleformCallbackFn callback,
    const XScalScaleformValue* arguments,
    uint32_t argumentCount,
    XScalScaleformValue* result,
    const char* pluginName) noexcept {
    __try {
        return callback(arguments, argumentCount, result);
    } __except (PluginCrashFilter(GetExceptionCode(), pluginName)) {
        return 0;
    }
}

uint32_t CallPluginOptionCount(
    const XScalPluginOptionsProviderV1& provider,
    const char* pluginName) noexcept {
    __try {
        return provider.count(provider.userData);
    } __except (PluginCrashFilter(GetExceptionCode(), pluginName)) {
        return 0;
    }
}

int32_t CallPluginGetOption(
    const XScalPluginOptionsProviderV1& provider,
    uint32_t index,
    XScalPluginOption* option,
    const char* pluginName) noexcept {
    __try {
        return provider.getOption(provider.userData, index, option);
    } __except (PluginCrashFilter(GetExceptionCode(), pluginName)) {
        return 0;
    }
}

int32_t CallPluginSetOption(
    const XScalPluginOptionsProviderV1& provider,
    const char* id,
    int32_t value,
    const char* pluginName) noexcept {
    __try {
        return provider.setOption(provider.userData, id, value);
    } __except (PluginCrashFilter(GetExceptionCode(), pluginName)) {
        return 0;
    }
}

uint32_t CallPluginOptionCountV2(
    const XScalPluginOptionsProviderV2& provider,
    const char* pluginName) noexcept {
    __try {
        return provider.count(provider.userData);
    } __except (PluginCrashFilter(GetExceptionCode(), pluginName)) {
        return 0;
    }
}

int32_t CallPluginGetOptionV2(
    const XScalPluginOptionsProviderV2& provider,
    uint32_t index,
    XScalPluginOptionV2* option,
    const char* pluginName) noexcept {
    __try {
        return provider.getOption(provider.userData, index, option);
    } __except (PluginCrashFilter(GetExceptionCode(), pluginName)) {
        return 0;
    }
}

int32_t CallPluginSetOptionV2(
    const XScalPluginOptionsProviderV2& provider,
    const char* id,
    double value,
    const char* pluginName) noexcept {
    __try {
        return provider.setOption(provider.userData, id, value);
    } __except (PluginCrashFilter(GetExceptionCode(), pluginName)) {
        return 0;
    }
}

HRESULT CallPluginCreateDevice(
    PFN_D3D11_CREATE_DEVICE create,
    IDXGIAdapter* adapter,
    D3D_DRIVER_TYPE driverType,
    HMODULE software,
    UINT flags,
    const D3D_FEATURE_LEVEL* featureLevels,
    UINT featureLevelCount,
    UINT sdkVersion,
    ID3D11Device** device,
    D3D_FEATURE_LEVEL* selectedFeatureLevel,
    ID3D11DeviceContext** immediateContext) noexcept {
    __try {
        return create(
            adapter,
            driverType,
            software,
            flags,
            featureLevels,
            featureLevelCount,
            sdkVersion,
            device,
            selectedFeatureLevel,
            immediateContext);
    } __except (PluginCrashFilter(GetExceptionCode(), graphicsPluginName)) {
        return E_FAIL;
    }
}

HRESULT CallPluginCreateDeviceAndSwapChain(
    PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN create,
    IDXGIAdapter* adapter,
    D3D_DRIVER_TYPE driverType,
    HMODULE software,
    UINT flags,
    const D3D_FEATURE_LEVEL* featureLevels,
    UINT featureLevelCount,
    UINT sdkVersion,
    const DXGI_SWAP_CHAIN_DESC* swapChainDesc,
    IDXGISwapChain** swapChain,
    ID3D11Device** device,
    D3D_FEATURE_LEVEL* selectedFeatureLevel,
    ID3D11DeviceContext** immediateContext) noexcept {
    __try {
        return create(
            adapter,
            driverType,
            software,
            flags,
            featureLevels,
            featureLevelCount,
            sdkVersion,
            swapChainDesc,
            swapChain,
            device,
            selectedFeatureLevel,
            immediateContext);
    } __except (PluginCrashFilter(GetExceptionCode(), graphicsPluginName)) {
        return E_FAIL;
    }
}

// The name of a result code that D3D11 device creation returns, or null for
// any other code.
[[nodiscard]] const char* DeviceCreationResultName(HRESULT result) noexcept {
    switch (result) {
        case S_OK:
            return "S_OK";
        case S_FALSE:
            return "S_FALSE";
        case E_INVALIDARG:
            return "E_INVALIDARG";
        case E_OUTOFMEMORY:
            return "E_OUTOFMEMORY";
        case E_FAIL:
            return "E_FAIL";
        case E_NOINTERFACE:
            return "E_NOINTERFACE";
        case DXGI_ERROR_UNSUPPORTED:
            return "DXGI_ERROR_UNSUPPORTED";
        case DXGI_ERROR_INVALID_CALL:
            return "DXGI_ERROR_INVALID_CALL";
        case DXGI_ERROR_SDK_COMPONENT_MISSING:
            return "DXGI_ERROR_SDK_COMPONENT_MISSING";
        case DXGI_ERROR_DEVICE_REMOVED:
            return "DXGI_ERROR_DEVICE_REMOVED";
        default:
            return nullptr;
    }
}

void LogDeviceCreationResult(const char* hookName, HRESULT result) noexcept {
    const char* const name = DeviceCreationResultName(result);
    const auto code = static_cast<unsigned long>(result);
    if (SUCCEEDED(result)) {
        if (name != nullptr) {
            DiagnosticLogFormatImmediate(
                "xScal %s hook: device created (%s)", hookName, name);
        } else {
            DiagnosticLogFormatImmediate(
                "xScal %s hook: device created (0x%08lX)", hookName, code);
        }
        return;
    }
    if (name != nullptr) {
        DiagnosticLogFormatImmediate(
            "xScal %s hook: device creation failed: %s (0x%08lX)",
            hookName, name, code);
    } else {
        DiagnosticLogFormatImmediate(
            "xScal %s hook: device creation failed (0x%08lX)", hookName, code);
    }
}

HRESULT WINAPI CreateDevice(
    IDXGIAdapter* adapter, D3D_DRIVER_TYPE driverType, HMODULE software,
    UINT flags, const D3D_FEATURE_LEVEL* featureLevels, UINT featureLevelCount,
    UINT sdkVersion, ID3D11Device** device, D3D_FEATURE_LEVEL* selectedFeatureLevel,
    ID3D11DeviceContext** immediateContext) {
    if (inPluginCall) {
        DiagnosticLogImmediate(
            "xScal D3D11CreateDevice hook: reentrant call dispatched to system D3D11");
        return originalCreateDevice(adapter, driverType, software, flags,
            featureLevels, featureLevelCount, sdkVersion, device,
            selectedFeatureLevel, immediateContext);
    }
    DiagnosticLogImmediate("xScal D3D11CreateDevice hook: entered");
    Load();
    const auto create = pluginCreateDevice ? pluginCreateDevice : originalCreateDevice;
    DiagnosticLogImmediate(pluginCreateDevice
        ? "xScal D3D11CreateDevice hook: dispatching to graphics provider"
        : "xScal D3D11CreateDevice hook: dispatching to system D3D11");
    inPluginCall = true;
    const HRESULT result = pluginCreateDevice
        ? CallPluginCreateDevice(
            create,
            adapter,
            driverType,
            software,
            flags,
            featureLevels,
            featureLevelCount,
            sdkVersion,
            device,
            selectedFeatureLevel,
            immediateContext)
        : create(
            adapter,
            driverType,
            software,
            flags,
            featureLevels,
            featureLevelCount,
            sdkVersion,
            device,
            selectedFeatureLevel,
            immediateContext);
    inPluginCall = false;
    LogDeviceCreationResult("D3D11CreateDevice", result);
    return result;
}

HRESULT WINAPI CreateDeviceAndSwapChain(
    IDXGIAdapter* adapter, D3D_DRIVER_TYPE driverType, HMODULE software,
    UINT flags, const D3D_FEATURE_LEVEL* featureLevels, UINT featureLevelCount,
    UINT sdkVersion, const DXGI_SWAP_CHAIN_DESC* swapChainDesc,
    IDXGISwapChain** swapChain, ID3D11Device** device,
    D3D_FEATURE_LEVEL* selectedFeatureLevel, ID3D11DeviceContext** immediateContext) {
    if (inPluginCall) {
        DiagnosticLogImmediate(
            "xScal D3D11CreateDeviceAndSwapChain hook: reentrant call dispatched to system D3D11");
        return originalCreateDeviceAndSwapChain(adapter, driverType, software,
            flags, featureLevels, featureLevelCount, sdkVersion, swapChainDesc,
            swapChain, device, selectedFeatureLevel, immediateContext);
    }
    DiagnosticLogImmediate(
        "xScal D3D11CreateDeviceAndSwapChain hook: entered");
    Load();
    const auto create = pluginCreateDeviceAndSwapChain
        ? pluginCreateDeviceAndSwapChain : originalCreateDeviceAndSwapChain;
    DiagnosticLogImmediate(pluginCreateDeviceAndSwapChain
        ? "xScal D3D11CreateDeviceAndSwapChain hook: dispatching to graphics provider"
        : "xScal D3D11CreateDeviceAndSwapChain hook: dispatching to system D3D11");
    inPluginCall = true;
    const HRESULT result = pluginCreateDeviceAndSwapChain
        ? CallPluginCreateDeviceAndSwapChain(
            create,
            adapter,
            driverType,
            software,
            flags,
            featureLevels,
            featureLevelCount,
            sdkVersion,
            swapChainDesc,
            swapChain,
            device,
            selectedFeatureLevel,
            immediateContext)
        : create(
            adapter,
            driverType,
            software,
            flags,
            featureLevels,
            featureLevelCount,
            sdkVersion,
            swapChainDesc,
            swapChain,
            device,
            selectedFeatureLevel,
            immediateContext);
    inPluginCall = false;
    LogDeviceCreationResult("D3D11CreateDeviceAndSwapChain", result);
    return result;
}

bool Contains(std::size_t imageSize, std::size_t offset, std::size_t size) noexcept {
    return offset <= imageSize && size <= imageSize - offset;
}

const char* ImageString(BYTE* image, std::size_t imageSize, DWORD offset) noexcept {
    if (offset >= imageSize) return nullptr;
    const auto* text = reinterpret_cast<const char*>(image + offset);
    return std::memchr(text, '\0', imageSize - offset) ? text : nullptr;
}

template<class Function>
void PatchImport(IMAGE_THUNK_DATA64& thunk, Function replacement,
                 Function& original) noexcept {
    if (original != nullptr || thunk.u1.Function == 0) return;
    DWORD previousProtection{};
    if (!::VirtualProtect(&thunk.u1.Function, sizeof(thunk.u1.Function),
                          PAGE_READWRITE, &previousProtection)) {
        startupError = ::GetLastError();
        return;
    }
    original = reinterpret_cast<Function>(thunk.u1.Function);
    ::InterlockedExchangePointer(reinterpret_cast<void**>(&thunk.u1.Function),
                                 reinterpret_cast<void*>(replacement));
    DWORD ignored{};
    if (!::VirtualProtect(&thunk.u1.Function, sizeof(thunk.u1.Function),
                          previousProtection, &ignored)) {
        startupError = ::GetLastError();
    }
    ++hookedImports;
}

void PatchCreationImports(HMODULE executable) noexcept {
    auto* image = reinterpret_cast<BYTE*>(executable);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_IMPORT) return;
    const std::size_t imageSize = nt->OptionalHeader.SizeOfImage;
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress || !Contains(imageSize, directory.VirtualAddress, directory.Size)) return;
    const auto* imports = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(image + directory.VirtualAddress);
    for (std::size_t index = 0; index < directory.Size / sizeof(*imports); ++index) {
        const auto& entry = imports[index];
        if (!entry.Name) break;
        const char* name = ImageString(image, imageSize, entry.Name);
        if (!name || ::_stricmp(name, "d3d11.dll") != 0 ||
            !entry.OriginalFirstThunk || !entry.FirstThunk) continue;
        std::size_t nameOffset = entry.OriginalFirstThunk;
        std::size_t slotOffset = entry.FirstThunk;
        while (Contains(imageSize, nameOffset, sizeof(IMAGE_THUNK_DATA64)) &&
               Contains(imageSize, slotOffset, sizeof(IMAGE_THUNK_DATA64))) {
            const auto* named = reinterpret_cast<const IMAGE_THUNK_DATA64*>(image + nameOffset);
            auto* slot = reinterpret_cast<IMAGE_THUNK_DATA64*>(image + slotOffset);
            if (!named->u1.AddressOfData) break;
            if (!IMAGE_SNAP_BY_ORDINAL64(named->u1.Ordinal) &&
                named->u1.AddressOfData < imageSize &&
                Contains(imageSize, named->u1.AddressOfData, sizeof(IMAGE_IMPORT_BY_NAME))) {
                const char* function = ImageString(image, imageSize,
                    static_cast<DWORD>(named->u1.AddressOfData + offsetof(IMAGE_IMPORT_BY_NAME, Name)));
                if (function && std::strcmp(function, "D3D11CreateDevice") == 0)
                    PatchImport(*slot, &CreateDevice, originalCreateDevice);
                else if (function && std::strcmp(function, "D3D11CreateDeviceAndSwapChain") == 0)
                    PatchImport(*slot, &CreateDeviceAndSwapChain, originalCreateDeviceAndSwapChain);
            }
            nameOffset += sizeof(IMAGE_THUNK_DATA64);
            slotOffset += sizeof(IMAGE_THUNK_DATA64);
        }
    }
}

// Read-only file handle that closes itself. The plugin DLL stays open without
// write sharing from hashing until LoadLibraryEx returns, so the file cannot be
// replaced between the signature check and the load.
class ReadOnlyFile final {
public:
    explicit ReadOnlyFile(const wchar_t* path) noexcept
        : handle_(::CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)) {}
    ~ReadOnlyFile() { Close(); }
    ReadOnlyFile(const ReadOnlyFile&) = delete;
    ReadOnlyFile& operator=(const ReadOnlyFile&) = delete;

    [[nodiscard]] bool IsOpen() const noexcept { return handle_ != INVALID_HANDLE_VALUE; }
    [[nodiscard]] HANDLE Get() const noexcept { return handle_; }
    void Close() noexcept {
        if (handle_ != INVALID_HANDLE_VALUE) (void)::CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
    }

private:
    HANDLE handle_;
};

// Reads size bytes at offset. The handle is synchronous, so this moves the
// file pointer; HashFile seeks to the start before it reads.
[[nodiscard]] bool ReadFileAt(
    HANDLE file, std::uint64_t offset, void* buffer, DWORD size) noexcept {
    OVERLAPPED position{};
    position.Offset = static_cast<DWORD>(offset);
    position.OffsetHigh = static_cast<DWORD>(offset >> 32);
    DWORD read{};
    return ::ReadFile(file, buffer, size, &read, &position) != FALSE &&
        read == size;
}

enum class PluginQueryExport : std::uint8_t {
    Present,
    Absent,
    // The headers could not be read. The file is treated as a plugin, so a
    // damaged plugin still gets its usual rejection message.
    Unknown,
};

// Looks for the XScalPlugin_Query export in the DLL file without loading it.
// Other DLLs can sit in the plugin folder, such as a runtime that a plugin
// loads itself. The export names are searched by binary search, the same way
// GetProcAddress finds them, so they must be sorted as the linker writes them.
[[nodiscard]] PluginQueryExport FindPluginQueryExport(HANDLE file) noexcept {
    constexpr char kQueryName[] = "XScalPlugin_Query";
    constexpr DWORD kMaximumHeaderOffset = 0x1000000;
    constexpr WORD kMaximumSections = 96;
    constexpr DWORD kMaximumNames = 0x10000;

    IMAGE_DOS_HEADER dosHeader{};
    if (!ReadFileAt(file, 0, &dosHeader, sizeof(dosHeader)) ||
        dosHeader.e_magic != IMAGE_DOS_SIGNATURE || dosHeader.e_lfanew <= 0 ||
        static_cast<DWORD>(dosHeader.e_lfanew) > kMaximumHeaderOffset) {
        return PluginQueryExport::Unknown;
    }
    const std::uint64_t ntOffset = static_cast<DWORD>(dosHeader.e_lfanew);
    IMAGE_NT_HEADERS64 ntHeaders{};
    if (!ReadFileAt(file, ntOffset, &ntHeaders, sizeof(ntHeaders)) ||
        ntHeaders.Signature != IMAGE_NT_SIGNATURE ||
        ntHeaders.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        ntHeaders.FileHeader.SizeOfOptionalHeader <
            offsetof(IMAGE_OPTIONAL_HEADER64, DataDirectory) +
                (IMAGE_DIRECTORY_ENTRY_EXPORT + 1) * sizeof(IMAGE_DATA_DIRECTORY) ||
        ntHeaders.FileHeader.NumberOfSections == 0 ||
        ntHeaders.FileHeader.NumberOfSections > kMaximumSections) {
        return PluginQueryExport::Unknown;
    }
    if (ntHeaders.OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT) {
        return PluginQueryExport::Absent;
    }
    const auto& exportDirectoryEntry =
        ntHeaders.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (exportDirectoryEntry.VirtualAddress == 0 || exportDirectoryEntry.Size == 0) {
        return PluginQueryExport::Absent;
    }

    std::array<IMAGE_SECTION_HEADER, kMaximumSections> sections{};
    const WORD sectionCount = ntHeaders.FileHeader.NumberOfSections;
    const std::uint64_t sectionsOffset = ntOffset + sizeof(DWORD) +
        sizeof(IMAGE_FILE_HEADER) + ntHeaders.FileHeader.SizeOfOptionalHeader;
    if (!ReadFileAt(file, sectionsOffset, sections.data(),
            static_cast<DWORD>(sectionCount * sizeof(IMAGE_SECTION_HEADER)))) {
        return PluginQueryExport::Unknown;
    }
    // Returns the file offset of rva, or false when no section holds that
    // many bytes of file data at rva.
    const auto fileOffsetOf = [&](DWORD rva, DWORD length, std::uint64_t& offset) noexcept {
        for (WORD index = 0; index < sectionCount; ++index) {
            const auto& section = sections[index];
            if (rva < section.VirtualAddress) {
                continue;
            }
            const DWORD delta = rva - section.VirtualAddress;
            if (delta < section.SizeOfRawData &&
                length <= section.SizeOfRawData - delta) {
                offset = std::uint64_t{section.PointerToRawData} + delta;
                return true;
            }
        }
        return false;
    };

    IMAGE_EXPORT_DIRECTORY exportDirectory{};
    std::uint64_t exportDirectoryOffset{};
    if (!fileOffsetOf(exportDirectoryEntry.VirtualAddress, sizeof(exportDirectory),
            exportDirectoryOffset) ||
        !ReadFileAt(file, exportDirectoryOffset, &exportDirectory, sizeof(exportDirectory))) {
        return PluginQueryExport::Unknown;
    }
    if (exportDirectory.NumberOfNames == 0) {
        return PluginQueryExport::Absent;
    }
    std::uint64_t namesOffset{};
    if (exportDirectory.NumberOfNames > kMaximumNames ||
        !fileOffsetOf(exportDirectory.AddressOfNames,
            exportDirectory.NumberOfNames * sizeof(DWORD), namesOffset)) {
        return PluginQueryExport::Unknown;
    }

    DWORD low = 0;
    DWORD high = exportDirectory.NumberOfNames;
    while (low < high) {
        const DWORD middle = low + (high - low) / 2;
        DWORD nameRva{};
        std::uint64_t nameOffset{};
        // One byte more than the name, so a longer name that starts with it
        // compares as greater.
        char name[sizeof(kQueryName)]{};
        if (!ReadFileAt(file, namesOffset + std::uint64_t{middle} * sizeof(DWORD),
                &nameRva, sizeof(nameRva)) ||
            !fileOffsetOf(nameRva, 1, nameOffset)) {
            return PluginQueryExport::Unknown;
        }
        DWORD read{};
        OVERLAPPED position{};
        position.Offset = static_cast<DWORD>(nameOffset);
        position.OffsetHigh = static_cast<DWORD>(nameOffset >> 32);
        if (::ReadFile(file, name, sizeof(name), &read, &position) == FALSE || read == 0) {
            return PluginQueryExport::Unknown;
        }
        const int order = std::strncmp(name, kQueryName, sizeof(name));
        if (order == 0) {
            return PluginQueryExport::Present;
        }
        if (order < 0) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return PluginQueryExport::Absent;
}

struct MessageDialog final {
    std::wstring text;
    UINT icon;
};

// Shown from a new thread so the game's device creation does not wait for
// the player to close the box.
void ShowRevocationMessage(const address::v3::Message& message, const std::wstring& path) noexcept {
    FILETIME now{};
    ::GetSystemTimeAsFileTime(&now);
    const std::uint64_t nowFileTime =
        (static_cast<std::uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
    if (!address::v3::ClaimMessage(message, nowFileTime)) return;
    std::unique_ptr<MessageDialog> dialog;
    try {
        dialog = std::make_unique<MessageDialog>();
        const std::size_t slash = path.find_last_of(L"\\/");
        const std::wstring lead = L"The plugin " +
            (slash == std::wstring::npos ? path : path.substr(slash + 1)) + L" was not loaded.";
        if (!address::v3::BuildMessageText(message, lead, dialog->text, dialog->icon))
            return;
    } catch (...) {
        return;
    }
    const HANDLE thread = ::CreateThread(
        nullptr, 0,
        [](LPVOID parameter) -> DWORD {
            std::unique_ptr<MessageDialog> dialog{static_cast<MessageDialog*>(parameter)};
            (void)::MessageBoxW(
                nullptr, dialog->text.c_str(), L"xScal", MB_OK | dialog->icon);
            return 0;
        },
        dialog.get(), 0, nullptr);
    if (thread == nullptr) return;
    (void)dialog.release();
    (void)::CloseHandle(thread);
}

void LogSignatureRejection(
    const std::wstring& path,
    const signing::SignatureResult& result) noexcept {
    const char* reason = signing::DescribeCheck(result.check);
    address::v3::OnlineRejection online{};
    if (address::v3::FindOnlineRejection(result, online)) {
        DiagnosticLogFormatImmediate(
            "xScal plugin '%ls': %s (developer %u, online revocation: %s)",
            path.c_str(), reason, result.developerId, online.reason);
        if (online.message != nullptr) ShowRevocationMessage(*online.message, path);
        return;
    }
    switch (result.check) {
    case signing::SignatureCheck::DeveloperRevoked:
    case signing::SignatureCheck::BadCertificate:
        DiagnosticLogFormatImmediate(
            "xScal plugin '%ls': %s (developer %u)",
            path.c_str(), reason, result.developerId);
        break;
    case signing::SignatureCheck::BuildRevoked: {
        char hash[signing::kHashSize * 2 + 1]{};
        for (std::size_t index = 0; index < signing::kHashSize; ++index)
            std::snprintf(hash + index * 2, 3, "%02x", result.fileHash[index]);
        DiagnosticLogFormatImmediate(
            "xScal plugin '%ls': %s (SHA-256 %s)", path.c_str(), reason, hash);
        break;
    }
    case signing::SignatureCheck::ReadError:
        DiagnosticLogFormatImmediate(
            "xScal plugin '%ls': %s (%lu)",
            path.c_str(), reason, static_cast<unsigned long>(result.error));
        break;
    case signing::SignatureCheck::CryptoError:
        DiagnosticLogFormatImmediate(
            "xScal plugin '%ls': %s (0x%08lX)",
            path.c_str(), reason, static_cast<unsigned long>(result.error));
        break;
    default:
        DiagnosticLogFormatImmediate("xScal plugin '%ls': %s", path.c_str(), reason);
        break;
    }
}

void LoadModule(const std::wstring& path) {
    ReadOnlyFile dllFile{path.c_str()};
    if (!dllFile.IsOpen()) {
        DiagnosticLogFormatImmediate(
            "xScal plugin '%ls': could not open file (%lu)",
            path.c_str(), ::GetLastError());
        return;
    }
    // A DLL without XScalPlugin_Query is not an xScal plugin and is left
    // alone without a log line.
    if (FindPluginQueryExport(dllFile.Get()) == PluginQueryExport::Absent) {
        return;
    }
    const std::wstring signaturePath = path + L".sig";
    ReadOnlyFile signatureFile{signaturePath.c_str()};
    if (!signatureFile.IsOpen() && ::GetLastError() != ERROR_FILE_NOT_FOUND) {
        DiagnosticLogFormatImmediate(
            "xScal plugin '%ls': could not open signature file (%lu)",
            path.c_str(), ::GetLastError());
        return;
    }
    const signing::SignatureResult signature =
        signing::CheckPluginFile(dllFile.Get(), signatureFile.Get());
    signatureFile.Close();
    if (signature.check != signing::SignatureCheck::Valid) {
        LogSignatureRejection(path, signature);
        return;
    }

    const HMODULE library = ::LoadLibraryExW(path.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    dllFile.Close();
    if (!library) {
        DiagnosticLogFormatImmediate(
            "xScal plugin '%ls': LoadLibraryEx failed (%lu)",
            path.c_str(),
            ::GetLastError());
        return;
    }
    const auto query = reinterpret_cast<XScalPluginQueryFn>(::GetProcAddress(library, "XScalPlugin_Query"));
    const wchar_t* pluginFileName = std::wcsrchr(path.c_str(), L'\\');
    pluginFileName = pluginFileName != nullptr
        ? pluginFileName + 1
        : path.c_str();
    // A plugin built for an older ABI fills only its shorter descriptor, so the
    // fields added later stay zero from this initialization.
    XScalPlugin descriptor{};
    const char* rejection = nullptr;
    if (!query) rejection = "missing XScalPlugin_Query export";
    else if (!CallPluginQuery(
                 query,
                 XSCAL_PLUGIN_ABI_VERSION,
                 sizeof(descriptor),
                 &descriptor,
                 pluginFileName)) rejection = "query declined";
    else if (descriptor.abiVersion < kOldestPluginAbiVersion ||
             descriptor.abiVersion > XSCAL_PLUGIN_ABI_VERSION ||
             descriptor.size != DescriptorSize(descriptor.abiVersion))
        rejection = kUnsupportedAbiRejection;
    else if (!descriptor.id[0] || !std::memchr(descriptor.id, '\0', sizeof(descriptor.id)) ||
             !descriptor.name[0] || !std::memchr(descriptor.name, '\0', sizeof(descriptor.name)) || !descriptor.load)
        rejection = "missing load callback or invalid module ID/name";
    else if (!signing::IdMatchesPrefix(descriptor.id, signature.idPrefix))
        rejection = kOutsideIdPrefixRejection;
    else if (!ScaleformCallbacksValid(descriptor))
        rejection = "invalid or duplicate Scaleform callbacks";
    else if (std::any_of(modules.begin(), modules.end(), [&](const auto& module) {
                 return std::strcmp(module.descriptor.id, descriptor.id) == 0;
             })) rejection = "duplicate module ID";
    else if ((descriptor.createDevice || descriptor.createDeviceAndSwapChain) &&
             (pluginCreateDevice || pluginCreateDeviceAndSwapChain))
        rejection = "a graphics provider is already loaded";
    if (rejection == kUnsupportedAbiRejection) {
        DiagnosticLogFormatImmediate(
            "xScal plugin '%ls': %s (plugin ABI %u, descriptor size %u; this xScal loads ABI %u to %u)",
            path.c_str(),
            rejection,
            descriptor.abiVersion,
            descriptor.size,
            kOldestPluginAbiVersion,
            XSCAL_PLUGIN_ABI_VERSION);
    } else if (rejection == kOutsideIdPrefixRejection) {
        DiagnosticLogFormatImmediate(
            "xScal plugin '%ls': plugin ID '%s' outside certificate prefix '%s' (developer %u)",
            path.c_str(),
            descriptor.id,
            signature.idPrefix,
            signature.developerId);
    } else if (rejection) {
        DiagnosticLogFormatImmediate(
            "xScal plugin '%ls': %s", path.c_str(), rejection);
    }
    if (rejection) {
        (void)::FreeLibrary(library);
        return;
    }

    // Reserve before calling plugin code that may publish callbacks to the game.
    try {
        modules.push_back({library, descriptor, false, false, {}, nullptr, nullptr});
    } catch (...) {
        (void)::FreeLibrary(library);
        throw;
    }
    HMODULE pinned{};
    if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                              reinterpret_cast<LPCWSTR>(library), &pinned)) {
        DiagnosticLogFormatImmediate(
            "xScal plugin '%s': could not pin DLL (%lu)",
            descriptor.id,
            ::GetLastError());
        modules.pop_back();
        (void)::FreeLibrary(library);
        return;
    }
    auto& module = modules.back();
    module.host = {
        sizeof(XScalHost),
        XSCAL_PLUGIN_ABI_VERSION,
        &module,
        &LogMessage,
        targetKind,
        &QueryHostInterface};
    module.loading = true;
    module.active = CallPluginLoad(
        descriptor.load, &module.host, descriptor.name) != 0;
    module.loading = false;
    if (module.active) {
        if (descriptor.createDevice || descriptor.createDeviceAndSwapChain) {
            pluginCreateDevice = descriptor.createDevice;
            pluginCreateDeviceAndSwapChain = descriptor.createDeviceAndSwapChain;
            graphicsPluginName = module.descriptor.name;
        }
        DiagnosticLogFormatImmediate(
            "xScal module '%s' (%s): loaded (developer %u)",
            descriptor.id, descriptor.name, signature.developerId);
    } else {
        DiagnosticLogFormatImmediate(
            "xScal module '%s': load declined", descriptor.id);
    }
}

[[nodiscard]] std::string_view FixedString(
    const char* text,
    std::size_t capacity) noexcept {
    const auto* const end =
        static_cast<const char*>(std::memchr(text, '\0', capacity));
    return end != nullptr
        ? std::string_view{text, static_cast<std::size_t>(end - text)}
        : std::string_view{};
}

// snprintf comes from the shared CRT; std::to_chars for double would add its
// conversion tables to dxgi.dll.
void AppendNumber(double value, std::string& output) {
    char buffer[32]{};
    const int length = std::isfinite(value)
        ? std::snprintf(buffer, sizeof(buffer), "%.9g", value) : -1;
    if (length > 0 && length < static_cast<int>(sizeof(buffer)))
        output.append(buffer, static_cast<std::size_t>(length));
    else
        output += '0';
}

// Each option listed by xsGetPluginOptions takes more than 40 bytes of the
// result string, so one result never holds more options than this. Options
// at higher indexes are not read: they are read on the game thread, and an
// unbounded count from a plugin could stall it for billions of calls.
constexpr uint32_t kMaximumPluginOptions =
    XSCAL_SCALEFORM_MAX_RESULT_STRING_BYTES / 40u;

// Reads every option of a module as a version 2 option. Version 1 options
// become toggles.
template<class Visit>
void ForEachPluginOption(const PluginModule& module, Visit&& visit) {
    if (module.optionsV2 != nullptr) {
        const auto& provider = *module.optionsV2;
        const uint32_t count = (std::min)(
            CallPluginOptionCountV2(provider, module.descriptor.name),
            kMaximumPluginOptions);
        for (uint32_t index = 0; index < count; ++index) {
            XScalPluginOptionV2 option{};
            if (CallPluginGetOptionV2(provider, index, &option, module.descriptor.name) &&
                !visit(option)) {
                return;
            }
        }
        return;
    }
    if (module.options == nullptr) return;
    const auto& provider = *module.options;
    const uint32_t count = (std::min)(
        CallPluginOptionCount(provider, module.descriptor.name),
        kMaximumPluginOptions);
    for (uint32_t index = 0; index < count; ++index) {
        XScalPluginOption legacy{};
        if (!CallPluginGetOption(provider, index, &legacy, module.descriptor.name)) continue;
        XScalPluginOptionV2 option{};
        std::memcpy(option.id, legacy.id, sizeof(option.id));
        std::memcpy(option.group, legacy.group, sizeof(option.group));
        std::memcpy(option.label, legacy.label, sizeof(option.label));
        option.type = XSCAL_PLUGIN_OPTION_TOGGLE;
        option.value = legacy.value != 0 ? 1.0 : 0.0;
        if (!visit(option)) return;
    }
}

// Appends one JSON object per option that fits below the result size limit.
// Options with invalid UTF-8 are skipped. Without `typed`, only toggles are
// listed, as {"id","group","label","value":bool}, for menus that predate
// choice and number options. With `typed`, every option carries "type" and
// toggles keep a boolean value; choices add "choices", numbers add "min",
// "max", "step" and "decimals".
void AppendPluginOptions(const PluginModule& module, bool typed, std::string& output) {
    bool first = true;
    ForEachPluginOption(module, [&](const XScalPluginOptionV2& option) {
        if (!typed && option.type != XSCAL_PLUGIN_OPTION_TOGGLE) return true;
        if (option.type > XSCAL_PLUGIN_OPTION_NUMBER) return true;
        const auto id = FixedString(option.id, sizeof(option.id));
        const auto group = FixedString(option.group, sizeof(option.group));
        const auto label = FixedString(option.label, sizeof(option.label));
        if (id.empty() || !json::IsValidUtf8(id) ||
            !json::IsValidUtf8(group) || !json::IsValidUtf8(label)) {
            return true;
        }
        const std::size_t start = output.size();
        output += first ? "{\"id\":" : ",{\"id\":";
        json::AppendString(id, output);
        output += ",\"group\":";
        json::AppendString(group, output);
        output += ",\"label\":";
        json::AppendString(label, output);
        if (typed) {
            constexpr const char* typeNames[] = {"toggle", "choice", "number"};
            output += ",\"type\":\"";
            output += typeNames[option.type];
            output += '"';
        }
        if (option.type == XSCAL_PLUGIN_OPTION_TOGGLE) {
            output += option.value != 0.0 ? ",\"value\":true" : ",\"value\":false";
        } else {
            output += ",\"value\":";
            AppendNumber(option.value, output);
        }
        if (option.type == XSCAL_PLUGIN_OPTION_CHOICE) {
            output += ",\"choices\":[";
            const uint32_t choiceCount =
                (std::min)(option.choiceCount, XSCAL_PLUGIN_OPTION_CHOICE_CAPACITY);
            for (uint32_t choice = 0; choice < choiceCount; ++choice) {
                const auto text = FixedString(option.choices[choice], sizeof(option.choices[choice]));
                if (choice != 0) output += ',';
                json::AppendString(json::IsValidUtf8(text) ? text : std::string_view{}, output);
            }
            output += ']';
        } else if (option.type == XSCAL_PLUGIN_OPTION_NUMBER) {
            output += ",\"min\":";
            AppendNumber(option.minimum, output);
            output += ",\"max\":";
            AppendNumber(option.maximum, output);
            output += ",\"step\":";
            AppendNumber(option.step, output);
            output += ",\"decimals\":";
            AppendNumber((std::min)(option.decimals, 6u), output);
        }
        output += '}';
        // Leaves room for the closing "]}]}".
        if (output.size() + 8 > XSCAL_SCALEFORM_MAX_RESULT_STRING_BYTES) {
            output.resize(start);
            return false;
        }
        first = false;
        return true;
    });
}

// xsGetPluginOptions([version]): {"plugins":[{"id":...,"name":...,
// "options":[...]}]} for every active plugin. Plugins without a provider list
// no options. Version 2 or higher lists choice and number options as well.
bool WritePluginOptions(
    const XScalScaleformValue* arguments,
    uint32_t argumentCount,
    XScalScaleformValue& result) noexcept {
    bool typed = false;
    if (argumentCount >= 1) {
        const auto& version = arguments[0];
        typed = (version.type == XSCAL_SCALEFORM_VALUE_NUMBER && version.value.numberValue >= 2.0) ||
            (version.type == XSCAL_SCALEFORM_VALUE_INT32 && version.value.int32Value >= 2) ||
            (version.type == XSCAL_SCALEFORM_VALUE_UINT32 && version.value.uint32Value >= 2u);
    }
    thread_local std::string output;
    try {
        output = "{\"plugins\":[";
        bool first = true;
        for (const auto& module : modules) {
            if (!module.active) continue;
            const std::size_t start = output.size();
            output += first ? "{\"id\":" : ",{\"id\":";
            json::AppendString(module.descriptor.id, output);
            output += ",\"name\":";
            json::AppendString(module.descriptor.name, output);
            output += ",\"options\":[";
            inPluginCall = true;
            AppendPluginOptions(module, typed, output);
            inPluginCall = false;
            output += "]}";
            if (output.size() + 2 > XSCAL_SCALEFORM_MAX_RESULT_STRING_BYTES) {
                output.resize(start);
                break;
            }
            first = false;
        }
        output += "]}";
    } catch (...) {
        inPluginCall = false;
        return false;
    }
    result.type = XSCAL_SCALEFORM_VALUE_STRING;
    result.stringLength = static_cast<uint32_t>(output.size());
    result.value.stringValue = output.c_str();
    return true;
}

// xsSetPluginOption(pluginId, optionId, value): true when the plugin applied
// the value. value is a Boolean for toggles, or a number: the choice index or
// the number. Version 1 providers take only toggles; a number is nonzero-on.
bool SetPluginOption(
    const XScalScaleformValue* arguments,
    uint32_t argumentCount,
    XScalScaleformValue& result) noexcept {
    if (argumentCount != 3 ||
        arguments[0].type != XSCAL_SCALEFORM_VALUE_STRING ||
        arguments[1].type != XSCAL_SCALEFORM_VALUE_STRING ||
        (arguments[2].type != XSCAL_SCALEFORM_VALUE_BOOLEAN &&
         arguments[2].type != XSCAL_SCALEFORM_VALUE_NUMBER &&
         arguments[2].type != XSCAL_SCALEFORM_VALUE_INT32 &&
         arguments[2].type != XSCAL_SCALEFORM_VALUE_UINT32)) {
        return false;
    }
    double value = 0.0;
    switch (arguments[2].type) {
    case XSCAL_SCALEFORM_VALUE_BOOLEAN:
        value = arguments[2].value.booleanValue != 0 ? 1.0 : 0.0;
        break;
    case XSCAL_SCALEFORM_VALUE_NUMBER: value = arguments[2].value.numberValue; break;
    case XSCAL_SCALEFORM_VALUE_INT32: value = arguments[2].value.int32Value; break;
    default: value = arguments[2].value.uint32Value; break;
    }
    if (!std::isfinite(value)) return false;
    const std::string_view pluginId{
        arguments[0].value.stringValue, arguments[0].stringLength};
    char optionId[XSCAL_PLUGIN_OPTION_ID_CAPACITY]{};
    if (arguments[1].stringLength == 0 ||
        arguments[1].stringLength >= sizeof(optionId)) {
        return false;
    }
    std::memcpy(optionId, arguments[1].value.stringValue,
        arguments[1].stringLength);

    bool applied = false;
    for (const auto& module : modules) {
        if (!module.active || pluginId != module.descriptor.id) {
            continue;
        }
        inPluginCall = true;
        if (module.optionsV2 != nullptr) {
            applied = CallPluginSetOptionV2(*module.optionsV2, optionId,
                value, module.descriptor.name) != 0;
        } else if (module.options != nullptr) {
            applied = CallPluginSetOption(*module.options, optionId,
                value != 0.0 ? 1 : 0, module.descriptor.name) != 0;
        }
        inPluginCall = false;
        break;
    }
    result.type = XSCAL_SCALEFORM_VALUE_BOOLEAN;
    result.value.booleanValue = applied ? 1 : 0;
    return true;
}

constexpr std::size_t kMaximumLoadOrderSize = 64 * 1024;

// DLL file names from Data\XSCAL\Plugins\loadorder.txt in file order. Blank
// lines and lines starting with '#' are skipped. A missing file returns an
// empty list, which keeps the alphabetical order.
std::vector<std::wstring> ReadLoadOrder() {
    std::vector<std::wstring> names;
    platform::ExecutableRelativePath path;
    if (!platform::ResolveExecutableRelativePath(
            L"Data\\XSCAL\\Plugins\\loadorder.txt", path)) return names;
    std::string contents;
    if (!platform::ReadBoundedFile(path, kMaximumLoadOrderSize, contents)) {
        // The presence query only runs after a failed read: a missing file is
        // normal, an unreadable one is logged.
        if (platform::QueryFilePresence(path) == platform::FilePresence::Missing) return names;
        DiagnosticLogImmediate("xScal plugins: could not read loadorder.txt");
        return names;
    }
    std::string_view text = contents;
    // Notepad's "Unicode" encodings save UTF-16 with a byte order mark; read
    // as UTF-8, every line would fail to convert.
    if (text.starts_with("\xFF\xFE") || text.starts_with("\xFE\xFF")) {
        DiagnosticLogImmediate(
            "xScal plugins: loadorder.txt is saved as UTF-16 and is ignored; save it as UTF-8");
        return names;
    }
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
    while (!text.empty()) {
        const auto end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
            line.remove_prefix(1);
        while (!line.empty() &&
               (line.back() == ' ' || line.back() == '\t' || line.back() == '\r'))
            line.remove_suffix(1);
        if (line.empty() || line.front() == '#') continue;
        const int lineLength = static_cast<int>(line.size());
        const int length = ::MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, line.data(), lineLength, nullptr, 0);
        if (length <= 0) {
            DiagnosticLogFormatImmediate(
                "xScal plugins: loadorder.txt line '%.*s' is not valid UTF-8",
                lineLength, line.data());
            continue;
        }
        std::wstring name(static_cast<std::size_t>(length), L'\0');
        (void)::MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, line.data(), lineLength, name.data(), length);
        names.push_back(std::move(name));
    }
    return names;
}

BOOL CALLBACK LoadModules(PINIT_ONCE, PVOID, PVOID*) noexcept {
    ClearDiagnosticLog();
    address::v3::ApplyCachedRevocations();
    inPluginCall = true;
    if (initializeHost && !initializeHost()) {
        DiagnosticLogImmediate(
            "xScal plugins: host initialization declined; using original D3D11 calls");
        inPluginCall = false;
        return TRUE;
    }
    if (!hasCandidates) {
        inPluginCall = false;
        return TRUE;
    }
    HANDLE search = INVALID_HANDLE_VALUE;
    try {
        if (startupError)
            DiagnosticLogFormatImmediate(
                "xScal plugin startup: Windows error %lu", startupError);
        DiagnosticLogFormatImmediate(
            "xScal plugins: intercepted %u D3D11 creation imports", hookedImports);
        const std::wstring directory(searchPath.c_str(), searchPath.length - 5);
        std::vector<std::wstring> paths;
        WIN32_FIND_DATAW file{};
        search = ::FindFirstFileW(searchPath.c_str(), &file);
        if (search != INVALID_HANDLE_VALUE) {
            do {
                if (IsPluginFile(file))
                    paths.push_back(directory + file.cFileName);
            } while (::FindNextFileW(search, &file));
            const DWORD error = ::GetLastError();
            (void)::FindClose(search);
            search = INVALID_HANDLE_VALUE;
            if (error != ERROR_NO_MORE_FILES)
                DiagnosticLogFormatImmediate(
                    "xScal plugin scan failed (%lu)", error);
        }
        const std::vector<std::wstring> loadOrder = ReadLoadOrder();
        if (!loadOrder.empty())
            DiagnosticLogFormatImmediate(
                "xScal plugins: loadorder.txt lists %zu plugins", loadOrder.size());
        const auto isNamed = [&](const std::wstring& pluginPath, const std::wstring& name) {
            return _wcsicmp(pluginPath.c_str() + directory.size(), name.c_str()) == 0;
        };
        // The rank is the line position in loadorder.txt. Unlisted plugins rank
        // after every listed one and keep the alphabetical order among themselves.
        const auto rank = [&](const std::wstring& pluginPath) {
            for (std::size_t index = 0; index < loadOrder.size(); ++index)
                if (isNamed(pluginPath, loadOrder[index])) return index;
            return loadOrder.size();
        };
        // Each rank is computed once; computing it inside the comparison scans
        // loadorder.txt for every comparison.
        std::vector<std::pair<std::size_t, std::wstring>> ranked;
        ranked.reserve(paths.size());
        for (auto& pluginPath : paths) {
            const std::size_t pluginRank = rank(pluginPath);
            ranked.emplace_back(pluginRank, std::move(pluginPath));
        }
        std::sort(ranked.begin(), ranked.end());
        for (std::size_t index = 0; index < ranked.size(); ++index)
            paths[index] = std::move(ranked[index].second);
        for (const auto& name : loadOrder) {
            if (std::none_of(paths.begin(), paths.end(),
                    [&](const auto& pluginPath) { return isNamed(pluginPath, name); }))
                DiagnosticLogFormatImmediate(
                    "xScal plugins: loadorder.txt entry '%ls' is not installed", name.c_str());
        }
        for (const auto& path : paths) LoadModule(path);
    } catch (...) {
        if (search != INVALID_HANDLE_VALUE) (void)::FindClose(search);
        DiagnosticLogImmediate(
            "xScal plugin loading stopped after an exception");
    }
    inPluginCall = false;
    return TRUE;
}

}

void InstallCreationHooks(HMODULE executable, bool (*initialize)() noexcept) noexcept {
    if (startupChecked || !executable) return;
    startupChecked = true;
    initializeHost = initialize;
    if (!platform::ResolveExecutableRelativePath(L"Data\\XSCAL\\Plugins\\*.dll", searchPath)) return;
    WIN32_FIND_DATAW file{};
    const HANDLE search = ::FindFirstFileW(searchPath.c_str(), &file);
    if (search == INVALID_HANDLE_VALUE) return;
    do {
        if (IsPluginFile(file)) {
            hasCandidates = true;
            break;
        }
    } while (::FindNextFileW(search, &file));
    (void)::FindClose(search);
    if (!hasCandidates) return;
    HMODULE pinned{};
    if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                              reinterpret_cast<LPCWSTR>(&InstallCreationHooks), &pinned)) {
        startupError = ::GetLastError();
        return;
    }
    PatchCreationImports(executable);
}

void SetTargetKind(TargetKind kind) noexcept {
    switch (kind) {
        case TargetKind::Steam: targetKind = XSCAL_TARGET_STEAM; break;
        case TargetKind::GamePass: targetKind = XSCAL_TARGET_GAMEPASS; break;
        default: targetKind = XSCAL_TARGET_UNKNOWN; break;
    }
}

void SetAddressLibrary(
    const XScalAddressLibraryV1* library) noexcept {
    addressLibrary = library;
}

void SetScaleformMovieInterface(
    const XScalScaleformMovieV2* movieInterface,
    const XScalScaleformMovieV1* movieInterfaceV1) noexcept {
    scaleformMovieInterface = movieInterface;
    scaleformMovieInterfaceV1 = movieInterfaceV1;
}

void Load() noexcept {
    // Query/load may create a device through the game's imports on this thread.
    // Such calls use the original entry points instead of reentering InitOnce.
    if (!inPluginCall) (void)::InitOnceExecuteOnce(&loadOnce, &LoadModules, nullptr, nullptr);
}

bool DispatchScaleform(
    std::string_view name,
    const XScalScaleformValue* arguments,
    std::uint32_t argumentCount,
    XScalScaleformValue& result) noexcept {
    if (inPluginCall) return false;
    if (name.empty() || argumentCount > XSCAL_SCALEFORM_MAX_ARGUMENTS ||
        (argumentCount != 0 && arguments == nullptr)) return false;
    Load();
    if (name == "xsGetPluginOptions")
        return WritePluginOptions(arguments, argumentCount, result);
    if (name == "xsSetPluginOption")
        return SetPluginOption(arguments, argumentCount, result);
    for (const auto& module : modules) {
        if (!module.active) continue;
        for (uint32_t index = 0;
             index < module.descriptor.scaleformCallbackCount;
             ++index) {
            const auto& callback = module.descriptor.scaleformCallbacks[index];
            if (CallbackName(callback) != name) continue;
            result = {};
            inPluginCall = true;
            const bool called = CallPluginScaleform(
                callback.callback,
                arguments,
                argumentCount,
                &result,
                module.descriptor.name) != 0;
            inPluginCall = false;
            return called && ScaleformResultValid(result);
        }
    }
    return false;
}

std::size_t ModuleCount() noexcept {
    Load();
    return static_cast<std::size_t>(std::count_if(modules.begin(), modules.end(),
        [](const auto& module) { return module.active; }));
}

const char* ActiveModuleName(std::size_t index) noexcept {
    Load();

    for (const auto& module : modules) {
        if (!module.active) {
            continue;
        }
        if (index == 0) {
            return module.descriptor.name;
        }
        --index;
    }
    return nullptr;
}

const char* PlatformName() noexcept {
    switch (targetKind) {
        case XSCAL_TARGET_STEAM: return "Steam";
        case XSCAL_TARGET_GAMEPASS: return "Game Pass";
        default: return "Unknown";
    }
}

}
