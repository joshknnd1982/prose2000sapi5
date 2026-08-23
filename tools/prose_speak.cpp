// Speaks out loud through the registered SAPI 5 stack.
//
// This is the check that registration worked: unlike prose_sapitest, it goes through
// CLSID_SpVoice and the system voice catalogue exactly as any other application would, so
// if this speaks then so will Narrator, NVDA and anything else on the machine.
//
//   prose_speak                       list the installed Prose 2000 voices and speak one
//   prose_speak --voice "Prose 2000 Deep" "Hello there."
//   prose_speak --all                 speak a sample through every Prose 2000 voice

#include <cstdio>
#include <string>
#include <vector>

#include <windows.h>
#include <sapi.h>
#include <comdef.h>
#include <comip.h>

#include "prose_log.hpp"
#include "prose_sapi_helpers.hpp"
#include "prose_voices.hpp"

namespace {

_COM_SMARTPTR_TYPEDEF(ISpVoice, __uuidof(ISpVoice));
_COM_SMARTPTR_TYPEDEF(ISpObjectToken, __uuidof(ISpObjectToken));
_COM_SMARTPTR_TYPEDEF(IEnumSpObjectTokens, __uuidof(IEnumSpObjectTokens));

[[nodiscard]] std::string narrow(const wchar_t* s)
{
    return prose::log_narrow(s);
}

// Every installed voice whose name matches one in our catalogue.
std::vector<std::pair<std::wstring, ISpObjectTokenPtr>> find_our_voices()
{
    std::vector<std::pair<std::wstring, ISpObjectTokenPtr>> found;

    IEnumSpObjectTokensPtr tokens;
    if (FAILED(prose::sapi::enum_voice_tokens(&tokens)) || !tokens) {
        return found;
    }

    ULONG count = 0;
    tokens->GetCount(&count);
    for (ULONG i = 0; i < count; ++i) {
        ISpObjectTokenPtr token;
        if (FAILED(tokens->Item(i, &token)) || !token) {
            continue;
        }
        std::wstring name;
        if (FAILED(prose::sapi::token_description(token, name)) || name.empty()) {
            continue;
        }
        if (prose::find_voice(name)) {
            found.push_back({name, token});
        }
    }
    return found;
}

int speak_with(ISpVoicePtr& voice, ISpObjectTokenPtr token, const std::wstring& name,
               const std::wstring& text)
{
    if (FAILED(voice->SetVoice(token))) {
        std::printf("could not select '%s'\n", narrow(name.c_str()).c_str());
        return 1;
    }
    std::printf("speaking through '%s'...\n", narrow(name.c_str()).c_str());
    const HRESULT hr = voice->Speak(text.c_str(), SPF_IS_NOT_XML, nullptr);
    if (FAILED(hr)) {
        std::printf("  Speak failed 0x%08lX\n", static_cast<unsigned long>(hr));
        return 1;
    }
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv)
{
    prose::log_init(L"speak");

    std::wstring wanted;
    std::wstring text;
    bool all = false;
    bool list_only = false;

    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--voice" && i + 1 < argc) {
            wanted = argv[++i];
        } else if (arg == L"--all") {
            all = true;
        } else if (arg == L"--list") {
            list_only = true;
        } else if (arg == L"--help" || arg == L"/?") {
            std::printf("prose_speak [--voice NAME | --all | --list] [text]\n");
            return 0;
        } else {
            if (!text.empty()) {
                text += L" ";
            }
            text += arg;
        }
    }

    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(co)) {
        std::printf("CoInitializeEx failed 0x%08lX\n", static_cast<unsigned long>(co));
        return 1;
    }

    int result = 0;
    {
        const auto voices = find_our_voices();
        if (voices.empty()) {
            std::printf(
                "No Prose 2000 voices are registered with SAPI 5.\n"
                "Install the package, or register the DLL by hand with regsvr32.\n");
            CoUninitialize();
            return 1;
        }

        std::printf("Registered Prose 2000 voices:\n");
        for (const auto& [name, token] : voices) {
            std::printf("  %s\n", narrow(name.c_str()).c_str());
        }
        std::printf("\n");

        if (list_only) {
            CoUninitialize();
            return 0;
        }

        ISpVoicePtr voice;
        const HRESULT hr = voice.CreateInstance(CLSID_SpVoice);
        if (FAILED(hr) || !voice) {
            std::printf("could not create an SpVoice: 0x%08lX\n",
                        static_cast<unsigned long>(hr));
            CoUninitialize();
            return 1;
        }

        if (all) {
            for (const auto& [name, token] : voices) {
                const std::wstring sample =
                    text.empty() ? L"This is " + name +
                                       L". The quick brown fox jumps over the lazy dog."
                                 : text;
                result |= speak_with(voice, token, name, sample);
            }
        } else {
            auto chosen = voices.begin();
            if (!wanted.empty()) {
                chosen = voices.end();
                for (auto it = voices.begin(); it != voices.end(); ++it) {
                    if (_wcsicmp(it->first.c_str(), wanted.c_str()) == 0) {
                        chosen = it;
                        break;
                    }
                }
                if (chosen == voices.end()) {
                    std::printf("no installed voice is called '%s'\n",
                                narrow(wanted.c_str()).c_str());
                    CoUninitialize();
                    return 1;
                }
            }
            const std::wstring sample =
                text.empty()
                    ? L"This is the Prose 2000, speaking through the Microsoft Speech "
                      L"A P I version five. Installation was successful."
                    : text;
            result = speak_with(voice, chosen->second, chosen->first, sample);
        }
    }

    CoUninitialize();
    return result;
}
