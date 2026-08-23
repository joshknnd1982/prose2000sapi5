// DLL entry points for the Prose 2000 SAPI 5 engine.
//
// Registration is deliberately minimal: two CLSIDs and one TokenEnums key per speech
// catalogue. The three voices are never written to the registry - SAPI asks our enumerator
// for them, and the enumerator reads the catalogue compiled into this DLL.

#include <new>

#include <windows.h>
#include <sapi.h>

#include "prose_com.hpp"
#include "prose_enum_tokens.hpp"
#include "prose_log.hpp"
#include "prose_paths.hpp"
#include "prose_registry.hpp"
#include "prose_tts_engine.hpp"
#include "prose_voices.hpp"

namespace {

HINSTANCE g_dll_handle = nullptr;
prose::com::class_object_factory g_class_factory;

// SAPI looks for third-party voice enumerators here. The classic key is what every SAPI 5
// application reads; the OneCore key is what the newer Windows speech stack reads, and
// registering in both costs one extra value.
//
// Both live under HKLM. An enumerator registered under HKCU is accepted without complaint
// and then never called, which looks exactly like a working install that has no voices.
constexpr const wchar_t* kTokenEnumPaths[] = {
    L"Software\\Microsoft\\Speech\\Voices\\TokenEnums",
    L"Software\\Microsoft\\Speech_OneCore\\Voices\\TokenEnums",
};

constexpr const wchar_t* kEnumeratorName = L"Prose2000";

[[nodiscard]] std::wstring clsid_to_string(const GUID& clsid)
{
    wchar_t buffer[64] = {0};
    StringFromGUID2(clsid, buffer, 64);
    return std::wstring(buffer);
}

void register_token_enumerator()
{
    using namespace prose::sapi;
    using namespace prose::registry;

    const std::wstring clsid = clsid_to_string(__uuidof(IEnumSpObjectTokensImpl));

    for (const wchar_t* path : kTokenEnumPaths) {
        try {
            key enums(HKEY_LOCAL_MACHINE, path, KEY_CREATE_SUB_KEY | KEY_SET_VALUE, true);
            key entry(enums, kEnumeratorName, KEY_SET_VALUE, true);
            entry.set(L"Prose 2000 Voices");
            entry.set(L"CLSID", clsid);
            PROSE_LOG_I("registered the voice enumerator under %s",
                        prose::log_narrow(path).c_str());
        }
        catch (const std::exception&) {
            // Speech_OneCore does not exist on every Windows edition. Missing it is not a
            // failure; missing the classic key is, and that one is reported by the caller.
            PROSE_LOG_W("could not register the enumerator under %s",
                        prose::log_narrow(path).c_str());
        }
    }
}

void unregister_token_enumerator() noexcept
{
    using namespace prose::registry;

    for (const wchar_t* path : kTokenEnumPaths) {
        try {
            key enums(HKEY_LOCAL_MACHINE, path, KEY_ALL_ACCESS);
            enums.delete_subkey(kEnumeratorName);
        }
        catch (...) {
        }
    }
}

}  // namespace

BOOL APIENTRY DllMain(HINSTANCE instance, DWORD reason, LPVOID /*reserved*/)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_dll_handle = instance;
        DisableThreadLibraryCalls(instance);

#ifdef _WIN64
        prose::log_init(L"sapi5-x64");
#else
        prose::log_init(L"sapi5-x86");
#endif
        PROSE_LOG_I("DLL attached to process");

        try {
            g_class_factory.register_class<prose::sapi::IEnumSpObjectTokensImpl>();
            g_class_factory.register_class<prose::sapi::ISpTTSEngineImpl>();
        }
        catch (...) {
            PROSE_LOG_E("failed to build the class factory");
            return FALSE;
        }
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv)
{
    return g_class_factory.create(rclsid, riid, ppv);
}

STDAPI DllCanUnloadNow()
{
    return prose::com::object_counter::is_zero() ? S_OK : S_FALSE;
}

STDAPI DllRegisterServer()
{
    try {
        PROSE_LOG_I("DllRegisterServer");
        prose::com::class_registrar registrar(g_dll_handle);
        registrar.register_class<prose::sapi::IEnumSpObjectTokensImpl>();
        registrar.register_class<prose::sapi::ISpTTSEngineImpl>();
        register_token_enumerator();

        const std::size_t voices = prose::voice_catalogue().size();
        std::wstring missing;
        const bool files_ok = prose::engine_files_present(&missing);

        PROSE_LOG_I("registration complete; %zu voices, engine files %s", voices,
                    files_ok ? "present" : "MISSING");
        if (!files_ok) {
            PROSE_LOG_E("registered, but %s is missing - the voices will not speak",
                        prose::log_narrow(missing.c_str()).c_str());
        }
        return S_OK;
    }
    catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    }
    catch (...) {
        PROSE_LOG_E("DllRegisterServer failed");
        return E_UNEXPECTED;
    }
}

STDAPI DllUnregisterServer()
{
    try {
        PROSE_LOG_I("DllUnregisterServer");
        unregister_token_enumerator();
        prose::com::class_registrar registrar(g_dll_handle);
        registrar.unregister_class<prose::sapi::IEnumSpObjectTokensImpl>();
        registrar.unregister_class<prose::sapi::ISpTTSEngineImpl>();
        return S_OK;
    }
    catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    }
    catch (...) {
        // Unregistering something that was never registered is not worth failing over.
        return S_OK;
    }
}
