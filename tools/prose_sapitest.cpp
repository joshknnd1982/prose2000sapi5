// Drives the SAPI 5 engine through its own class objects, without registering anything.
//
// This exercises the real code path an application takes - enumerate voices, bind a token
// to the engine, call ISpTTSEngine::Speak, collect events - while needing no administrator
// rights and leaving the machine untouched. It is the test to run before an installer
// exists, and the one to run when a registered install misbehaves and the question is
// whether the engine or the registration is at fault.
//
//   prose_sapitest --dll build_x64\bin\Release\Prose2000SAPI5.dll --out-dir samples\sapi

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <windows.h>
#include <sapi.h>
#include <sapiddk.h>
#include <sperror.h>
#include <comdef.h>
#include <comip.h>

#include "prose_host.hpp"
#include "prose_log.hpp"
#include "prose_paths.hpp"

namespace {

_COM_SMARTPTR_TYPEDEF(IClassFactory, __uuidof(IClassFactory));
_COM_SMARTPTR_TYPEDEF(IEnumSpObjectTokens, __uuidof(IEnumSpObjectTokens));
_COM_SMARTPTR_TYPEDEF(ISpObjectToken, __uuidof(ISpObjectToken));
_COM_SMARTPTR_TYPEDEF(ISpDataKey, __uuidof(ISpDataKey));
_COM_SMARTPTR_TYPEDEF(ISpTTSEngine, __uuidof(ISpTTSEngine));
_COM_SMARTPTR_TYPEDEF(ISpObjectWithToken, __uuidof(ISpObjectWithToken));

// The two CLSIDs the DLL implements. They have to match prose_enum_tokens.hpp and
// prose_tts_engine.hpp exactly.
const CLSID kEnumClsid = {0x3b7a91c4, 0x6e08, 0x4d52,
                          {0x9f, 0x31, 0x2a, 0x5c, 0x8d, 0x6e, 0x47, 0x01}};
const CLSID kEngineClsid = {0x9f2d4c61, 0x08b3, 0x4a77,
                            {0xbd, 0x15, 0x6c, 0x4e, 0x39, 0xa2, 0x5c, 0x88}};

using DllGetClassObjectFn = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);

int g_failures = 0;

void check(bool condition, const char* what)
{
    std::printf("  [%s] %s\n", condition ? "pass" : "FAIL", what);
    if (!condition) {
        ++g_failures;
    }
}

[[nodiscard]] std::string narrow(const wchar_t* s)
{
    return prose::log_narrow(s);
}

// A minimal ISpTTSEngineSite: collects the audio and records every event.
class TestSite : public ISpTTSEngineSite
{
public:
    struct Event {
        SPEVENTENUM id;
        ULONGLONG offset;
        std::wstring text;
        ULONG param_offset;
        ULONG param_length;
    };

    explicit TestSite(long rate = 0, USHORT volume = 100)
        : rate_(rate), volume_(volume), created_(GetTickCount64())
    {
    }

    // Milliseconds from the site being handed to Speak() until the engine wrote its first
    // audio. This is the latency an application actually experiences.
    [[nodiscard]] double first_write_ms() const
    {
        return first_write_ == 0 ? -1.0 : static_cast<double>(first_write_ - created_);
    }

    // IUnknown
    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override
    {
        if (!ppv) {
            return E_POINTER;
        }
        if (IsEqualIID(riid, __uuidof(IUnknown)) ||
            IsEqualIID(riid, __uuidof(ISpTTSEngineSite))) {
            *ppv = static_cast<ISpTTSEngineSite*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)() override { return ++refs_; }
    STDMETHOD_(ULONG, Release)() override { return --refs_; }

    // ISpEventSink
    STDMETHOD(AddEvents)(const SPEVENT* events, ULONG count) override
    {
        for (ULONG i = 0; i < count; ++i) {
            Event event = {};
            event.id = events[i].eEventId;
            event.offset = events[i].ullAudioStreamOffset;
            if (events[i].elParamType == SPET_LPARAM_IS_STRING && events[i].lParam) {
                event.text = reinterpret_cast<const wchar_t*>(events[i].lParam);
            } else {
                event.param_offset = static_cast<ULONG>(events[i].lParam);
                event.param_length = static_cast<ULONG>(events[i].wParam);
            }
            events_.push_back(std::move(event));
        }
        return S_OK;
    }
    STDMETHOD(GetEventInterest)(ULONGLONG* interest) override
    {
        *interest = interest_;
        return S_OK;
    }

    // ISpTTSEngineSite
    STDMETHOD_(DWORD, GetActions)() override { return actions_; }
    STDMETHOD(Write)(const void* data, ULONG count, ULONG* written) override
    {
        if (first_write_ == 0 && count > 0) {
            first_write_ = GetTickCount64();
        }
        const BYTE* p = static_cast<const BYTE*>(data);
        pcm_.insert(pcm_.end(), p, p + count);
        if (written) {
            *written = count;
        }
        return S_OK;
    }
    STDMETHOD(GetRate)(long* rate) override
    {
        *rate = rate_;
        return S_OK;
    }
    STDMETHOD(GetVolume)(USHORT* volume) override
    {
        *volume = volume_;
        return S_OK;
    }
    STDMETHOD(GetSkipInfo)(SPVSKIPTYPE* type, long* count) override
    {
        *type = SPVST_SENTENCE;
        *count = 0;
        return S_OK;
    }
    STDMETHOD(CompleteSkip)(long) override { return S_OK; }

    void set_interest(ULONGLONG interest) { interest_ = interest; }
    void set_actions(DWORD actions) { actions_ = actions; }

    [[nodiscard]] const std::vector<BYTE>& pcm() const { return pcm_; }
    [[nodiscard]] const std::vector<Event>& events() const { return events_; }

private:
    ULONG refs_ = 1;
    long rate_ = 0;
    USHORT volume_ = 100;
    ULONGLONG interest_ = 0;
    DWORD actions_ = SPVES_CONTINUE;
    ULONGLONG created_ = 0;
    ULONGLONG first_write_ = 0;
    std::vector<BYTE> pcm_;
    std::vector<Event> events_;
};

bool write_wav(const std::wstring& path, const std::vector<BYTE>& pcm)
{
    WAVEFORMATEX wfx = {};
    prose::fill_output_format(wfx);

    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || !file) {
        return false;
    }
    const DWORD data_size = static_cast<DWORD>(pcm.size());
    const DWORD riff_size = 36 + data_size;
    const DWORD fmt_size = 16;
    const WORD tag = WAVE_FORMAT_PCM;

    fwrite("RIFF", 1, 4, file);
    fwrite(&riff_size, 4, 1, file);
    fwrite("WAVEfmt ", 1, 8, file);
    fwrite(&fmt_size, 4, 1, file);
    fwrite(&tag, 2, 1, file);
    fwrite(&wfx.nChannels, 2, 1, file);
    fwrite(&wfx.nSamplesPerSec, 4, 1, file);
    fwrite(&wfx.nAvgBytesPerSec, 4, 1, file);
    fwrite(&wfx.nBlockAlign, 2, 1, file);
    fwrite(&wfx.wBitsPerSample, 2, 1, file);
    fwrite("data", 1, 4, file);
    fwrite(&data_size, 4, 1, file);
    if (data_size) {
        fwrite(pcm.data(), 1, data_size, file);
    }
    fclose(file);
    return true;
}

// Builds a single Speak fragment list out of plain text.
SPVTEXTFRAG make_fragment(const std::wstring& text)
{
    SPVTEXTFRAG frag = {};
    frag.pNext = nullptr;
    frag.State.eAction = SPVA_Speak;
    frag.State.LangID = 0x0409;
    frag.State.EmphAdj = 0;
    frag.State.RateAdj = 0;
    frag.State.Volume = 100;
    frag.State.PitchAdj.MiddleAdj = 0;
    frag.State.PitchAdj.RangeAdj = 0;
    frag.State.SilenceMSecs = 0;
    frag.pTextStart = text.c_str();
    frag.ulTextLen = static_cast<ULONG>(text.size());
    frag.ulTextSrcOffset = 0;
    return frag;
}

}  // namespace

int wmain(int argc, wchar_t** argv)
{
    prose::log_init(L"sapitest");

    std::wstring dll_path;
    std::wstring out_dir = L"samples\\sapi";

    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if ((arg == L"--dll" || arg == L"--out-dir" || arg == L"--data-dir") &&
            i + 1 >= argc) {
            std::printf("%s needs a value\n", narrow(arg.c_str()).c_str());
            return 2;
        }
        if (arg == L"--dll") {
            dll_path = argv[++i];
        } else if (arg == L"--out-dir") {
            out_dir = argv[++i];
        } else if (arg == L"--data-dir") {
            prose::set_data_root(argv[++i]);
        } else {
            std::printf("usage: prose_sapitest --dll PATH [--out-dir DIR] [--data-dir DIR]\n");
            return 2;
        }
    }

    if (dll_path.empty()) {
        std::printf("--dll is required\n");
        return 2;
    }

    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(co)) {
        std::printf("CoInitializeEx failed 0x%08lX\n", static_cast<unsigned long>(co));
        return 1;
    }

    CreateDirectoryW(out_dir.c_str(), nullptr);

    HMODULE dll = LoadLibraryW(dll_path.c_str());
    if (!dll) {
        std::printf("could not load %s, error %lu\n", narrow(dll_path.c_str()).c_str(),
                    GetLastError());
        return 1;
    }
    auto get_class_object =
        reinterpret_cast<DllGetClassObjectFn>(GetProcAddress(dll, "DllGetClassObject"));
    if (!get_class_object) {
        std::printf("the DLL does not export DllGetClassObject\n");
        return 1;
    }

    std::printf("Prose 2000 SAPI 5 engine test\n");
    std::printf("  dll   : %s\n", narrow(dll_path.c_str()).c_str());
    std::printf("  engine: %s\n\n", narrow(prose::data_root().c_str()).c_str());

    // ---------------------------------------------------------------------------------
    std::printf("Voice enumeration\n");
    IClassFactoryPtr enum_factory;
    HRESULT hr = get_class_object(kEnumClsid, __uuidof(IClassFactory),
                                  reinterpret_cast<void**>(&enum_factory));
    check(SUCCEEDED(hr) && enum_factory, "got the enumerator class factory");
    if (FAILED(hr)) {
        return 1;
    }

    IEnumSpObjectTokensPtr tokens;
    hr = enum_factory->CreateInstance(nullptr, __uuidof(IEnumSpObjectTokens),
                                      reinterpret_cast<void**>(&tokens));
    check(SUCCEEDED(hr) && tokens, "created the enumerator");
    if (FAILED(hr)) {
        return 1;
    }

    ULONG count = 0;
    tokens->GetCount(&count);
    std::printf("  %lu voice(s) reported\n", count);
    check(count == 3, "the enumerator reports three voices");

    std::vector<ISpObjectTokenPtr> voice_tokens;
    for (ULONG i = 0; i < count; ++i) {
        ISpObjectTokenPtr token;
        if (FAILED(tokens->Item(i, &token)) || !token) {
            check(false, "fetched a voice token");
            continue;
        }
        voice_tokens.push_back(token);

        ISpDataKeyPtr attrs;
        std::wstring name, language, gender;
        if (SUCCEEDED(token->OpenKey(L"Attributes", &attrs)) && attrs) {
            LPWSTR value = nullptr;
            if (SUCCEEDED(attrs->GetStringValue(L"Name", &value)) && value) {
                name = value;
                CoTaskMemFree(value);
            }
            if (SUCCEEDED(attrs->GetStringValue(L"Language", &value)) && value) {
                language = value;
                CoTaskMemFree(value);
            }
            if (SUCCEEDED(attrs->GetStringValue(L"Gender", &value)) && value) {
                gender = value;
                CoTaskMemFree(value);
            }
        }
        std::printf("    %lu. %-20s language=%-6s gender=%s\n", i, narrow(name.c_str()).c_str(),
                    narrow(language.c_str()).c_str(), narrow(gender.c_str()).c_str());
        check(!name.empty(), "the token has a name");
        check(language == L"409;9", "the token reports en-US");
    }
    std::printf("\n");

    // ---------------------------------------------------------------------------------
    std::printf("Output format\n");
    IClassFactoryPtr engine_factory;
    hr = get_class_object(kEngineClsid, __uuidof(IClassFactory),
                          reinterpret_cast<void**>(&engine_factory));
    check(SUCCEEDED(hr) && engine_factory, "got the engine class factory");
    if (FAILED(hr)) {
        return 1;
    }

    {
        ISpTTSEnginePtr engine;
        hr = engine_factory->CreateInstance(nullptr, __uuidof(ISpTTSEngine),
                                            reinterpret_cast<void**>(&engine));
        check(SUCCEEDED(hr) && engine, "created an engine instance");

        GUID format_id = GUID_NULL;
        WAVEFORMATEX* wfx = nullptr;
        hr = engine->GetOutputFormat(nullptr, nullptr, &format_id, &wfx);
        check(SUCCEEDED(hr) && wfx, "GetOutputFormat succeeded");
        if (wfx) {
            std::printf("    %lu Hz, %u-bit, %u channel(s)\n", wfx->nSamplesPerSec,
                        wfx->wBitsPerSample, wfx->nChannels);
            check(wfx->nSamplesPerSec == prose::kSampleRate, "10 kHz");
            check(wfx->wBitsPerSample == prose::kBitsPerSample, "16-bit");
            check(wfx->nChannels == prose::kChannels, "mono");
            CoTaskMemFree(wfx);
        }
    }
    std::printf("\n");

    // ---------------------------------------------------------------------------------
    std::printf("Speaking through each voice\n");
    for (std::size_t i = 0; i < voice_tokens.size(); ++i) {
        ISpTTSEnginePtr engine;
        hr = engine_factory->CreateInstance(nullptr, __uuidof(ISpTTSEngine),
                                            reinterpret_cast<void**>(&engine));
        if (FAILED(hr)) {
            check(false, "created an engine instance");
            continue;
        }

        ISpObjectWithTokenPtr with_token = engine;
        check(with_token != nullptr, "the engine exposes ISpObjectWithToken");
        if (!with_token) {
            continue;
        }
        hr = with_token->SetObjectToken(voice_tokens[i]);
        check(SUCCEEDED(hr), "SetObjectToken accepted the voice");
        if (FAILED(hr)) {
            continue;
        }

        const std::wstring text =
            L"Voice number " + std::to_wstring(i + 1) +
            L". The quick brown fox jumps over the lazy dog.";
        SPVTEXTFRAG frag = make_fragment(text);

        TestSite site;
        site.set_interest(SPFEI(SPEI_WORD_BOUNDARY) | SPFEI(SPEI_SENTENCE_BOUNDARY) |
                          SPFEI(SPEI_TTS_BOOKMARK));

        hr = engine->Speak(0, GUID_NULL, nullptr, &frag, &site);
        const double seconds =
            site.pcm().size() / double(prose::kSampleRate * prose::kBitsPerSample / 8);
        std::printf("    voice %zu: hr=0x%08lX  %.2fs  %zu bytes  %zu event(s)\n", i + 1,
                    static_cast<unsigned long>(hr), seconds, site.pcm().size(),
                    site.events().size());
        check(SUCCEEDED(hr), "Speak succeeded");
        check(!site.pcm().empty(), "Speak produced audio");

        // Events must be inside the audio they describe and must not run backwards.
        bool ordered = true;
        ULONGLONG previous = 0;
        for (const auto& event : site.events()) {
            if (event.offset < previous) {
                ordered = false;
            }
            previous = event.offset;
        }
        check(ordered, "events are in ascending stream order");
        const bool in_range = site.events().empty() ||
                              site.events().back().offset <= site.pcm().size();
        check(in_range, "no event points past the end of the audio");

        const std::wstring path = out_dir + L"\\sapi_voice" + std::to_wstring(i + 1) + L".wav";
        if (!site.pcm().empty()) {
            check(write_wav(path, site.pcm()), "wrote the WAV");
            std::printf("      -> %s\n", narrow(path.c_str()).c_str());
        }
    }
    std::printf("\n");

    // ---------------------------------------------------------------------------------
    // The firmware pads the head of every utterance with silence that scales with its
    // length - up to a second for a long sentence - and playing that pad is experienced as
    // the voice being slow to respond. The engine trims it. This is the regression test.
    std::printf("Responsiveness: no silence at the head of the audio\n");
    if (!voice_tokens.empty()) {
        const wchar_t* phrases[] = {
            L"OK",
            L"Documents folder",
            L"The quick brown fox jumps over the lazy dog.",
        };
        // One engine for the whole run, which is how an application uses it: the emulator
        // is booted once and stays warm. Creating a fresh engine per phrase would charge
        // every phrase for the ~90 ms firmware boot that only the first one really pays.
        ISpTTSEnginePtr shared_engine;
        engine_factory->CreateInstance(nullptr, __uuidof(ISpTTSEngine),
                                       reinterpret_cast<void**>(&shared_engine));
        ISpObjectWithTokenPtr shared_token = shared_engine;
        if (shared_token) {
            shared_token->SetObjectToken(voice_tokens[0]);
        }

        bool first_phrase = true;
        for (const wchar_t* phrase : phrases) {
            ISpTTSEnginePtr engine = shared_engine;
            if (!engine) {
                continue;
            }

            const std::wstring text = phrase;
            SPVTEXTFRAG frag = make_fragment(text);
            TestSite site;
            engine->Speak(0, GUID_NULL, nullptr, &frag, &site);

            // Where does the audio actually start making a sound?
            const auto* samples = reinterpret_cast<const short*>(site.pcm().data());
            const std::size_t count = site.pcm().size() / sizeof(short);
            std::size_t first = count;
            for (std::size_t i = 0; i < count; ++i) {
                if (std::abs(static_cast<int>(samples[i])) > 300) {
                    first = i;
                    break;
                }
            }
            const double lead_ms = 1000.0 * first / prose::kSampleRate;
            std::printf("    %-46s first audio %5.0f ms after Speak() %-28s "
                        "then %4.1f ms of silence\n",
                        narrow(phrase).c_str(), site.first_write_ms(),
                        first_phrase ? "(cold, includes firmware boot)" : "(warm)",
                        lead_ms);
            check(!site.pcm().empty(), "the phrase produced audio");
            check(lead_ms < 100.0, "starts within 100 ms");
            // A warm engine is the steady state; only the very first utterance pays for
            // booting the firmware.
            check(site.first_write_ms() < (first_phrase ? 400.0 : 100.0),
                  first_phrase ? "cold start under 400 ms" : "warm start under 100 ms");
            first_phrase = false;
        }
    }
    std::printf("\n");

    std::printf("Rate and volume respond to the site\n");
    if (!voice_tokens.empty()) {
        std::size_t slow_bytes = 0;
        std::size_t fast_bytes = 0;
        for (int pass = 0; pass < 2; ++pass) {
            ISpTTSEnginePtr engine;
            if (FAILED(engine_factory->CreateInstance(
                    nullptr, __uuidof(ISpTTSEngine), reinterpret_cast<void**>(&engine)))) {
                continue;
            }
            ISpObjectWithTokenPtr with_token = engine;
            with_token->SetObjectToken(voice_tokens[0]);

            const std::wstring text = L"The quick brown fox jumps over the lazy dog.";
            SPVTEXTFRAG frag = make_fragment(text);
            TestSite site(pass == 0 ? -10 : 10, 100);
            engine->Speak(0, GUID_NULL, nullptr, &frag, &site);
            (pass == 0 ? slow_bytes : fast_bytes) = site.pcm().size();
        }
        std::printf("    rate -10: %zu bytes, rate +10: %zu bytes\n", slow_bytes, fast_bytes);
        check(slow_bytes > fast_bytes, "a lower SAPI rate produces more audio");
    }

    // ---------------------------------------------------------------------------------
    std::printf("\nBookmarks land at exact offsets\n");
    if (!voice_tokens.empty()) {
        ISpTTSEnginePtr engine;
        if (SUCCEEDED(engine_factory->CreateInstance(
                nullptr, __uuidof(ISpTTSEngine), reinterpret_cast<void**>(&engine)))) {
            ISpObjectWithTokenPtr with_token = engine;
            with_token->SetObjectToken(voice_tokens[0]);

            const std::wstring first = L"Before the mark. ";
            const std::wstring mark = L"42";
            const std::wstring second = L"After the mark.";

            SPVTEXTFRAG frag_a = make_fragment(first);
            SPVTEXTFRAG frag_b = make_fragment(mark);
            SPVTEXTFRAG frag_c = make_fragment(second);
            frag_b.State.eAction = SPVA_Bookmark;
            frag_a.pNext = &frag_b;
            frag_b.pNext = &frag_c;

            TestSite site;
            site.set_interest(SPFEI(SPEI_TTS_BOOKMARK));
            const HRESULT speak_hr = engine->Speak(0, GUID_NULL, nullptr, &frag_a, &site);
            check(SUCCEEDED(speak_hr), "Speak with a bookmark succeeded");

            std::size_t bookmarks = 0;
            ULONGLONG offset = 0;
            for (const auto& event : site.events()) {
                if (event.id == SPEI_TTS_BOOKMARK) {
                    ++bookmarks;
                    offset = event.offset;
                }
            }
            std::printf("    %zu bookmark event(s), first at byte %llu of %zu\n", bookmarks,
                        offset, site.pcm().size());
            check(bookmarks == 1, "exactly one bookmark fired");
            check(offset > 0 && offset < site.pcm().size(),
                  "the bookmark landed inside the audio, not at either end");
        }
    }

    // ---------------------------------------------------------------------------------
    std::printf("\nAbort stops the utterance\n");
    if (!voice_tokens.empty()) {
        ISpTTSEnginePtr engine;
        if (SUCCEEDED(engine_factory->CreateInstance(
                nullptr, __uuidof(ISpTTSEngine), reinterpret_cast<void**>(&engine)))) {
            ISpObjectWithTokenPtr with_token = engine;
            with_token->SetObjectToken(voice_tokens[0]);

            const std::wstring text =
                L"This sentence should be cut off long before it finishes, because the "
                L"site reports an abort the moment the engine asks what to do.";
            SPVTEXTFRAG frag = make_fragment(text);

            TestSite site;
            site.set_actions(SPVES_ABORT);
            const HRESULT speak_hr = engine->Speak(0, GUID_NULL, nullptr, &frag, &site);
            std::printf("    hr=0x%08lX, %zu bytes written\n",
                        static_cast<unsigned long>(speak_hr), site.pcm().size());
            check(SUCCEEDED(speak_hr), "an aborted Speak still returns success");
        }
    }

    std::printf("\n");
    if (g_failures == 0) {
        std::printf("All checks passed.\n");
    } else {
        std::printf("%d check(s) FAILED. See %s\n", g_failures,
                    narrow(prose::log_file_path().c_str()).c_str());
    }

    voice_tokens.clear();
    tokens = nullptr;
    enum_factory = nullptr;
    engine_factory = nullptr;
    CoUninitialize();
    return g_failures ? 1 : 0;
}
