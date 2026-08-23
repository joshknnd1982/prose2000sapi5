// Renders Prose 2000 speech to WAV through the same client the SAPI 5 engine uses.
//
// This is the check that the engine works at all, independently of SAPI and of any
// registration. It needs no administrator rights.
//
//   prose_render --out samples\test.wav --voice 1 --rate 200 "Hello there."
//   prose_render --all --out-dir samples

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>

#include "prose_host.hpp"
#include "prose_log.hpp"
#include "prose_paths.hpp"
#include "prose_settings.hpp"
#include "prose_voices.hpp"

namespace {

// Collects an utterance, trimming the firmware's leading pad the same way the SAPI engine
// does so that a rendered WAV sounds like what an application actually hears. --raw turns
// the trimming off, which is what to use when measuring the firmware itself.
class BufferSink : public prose::SynthSink {
public:
    explicit BufferSink(bool trim) : trim_(trim) {}

    bool on_audio(const void* data, DWORD size) override
    {
        if (!trim_) {
            const auto* p = static_cast<const BYTE*>(data);
            pcm_.insert(pcm_.end(), p, p + size);
            return true;
        }
        trimmer_.process(data, size, pcm_);
        return true;
    }

    void finish()
    {
        if (trim_) {
            trimmer_.finish(pcm_);
        }
    }

    [[nodiscard]] const std::vector<BYTE>& pcm() const { return pcm_; }
    [[nodiscard]] std::size_t trimmed_lead_bytes() const
    {
        return trimmer_.trimmed_lead_bytes();
    }

private:
    bool trim_ = true;
    prose::SilenceTrimmer trimmer_;
    std::vector<BYTE> pcm_;
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
    const WORD format_tag = WAVE_FORMAT_PCM;

    fwrite("RIFF", 1, 4, file);
    fwrite(&riff_size, 4, 1, file);
    fwrite("WAVEfmt ", 1, 8, file);
    fwrite(&fmt_size, 4, 1, file);
    fwrite(&format_tag, 2, 1, file);
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

[[nodiscard]] std::string narrow(const std::wstring& s)
{
    return prose::log_narrow(s.c_str());
}

int usage()
{
    std::printf(
        "Renders Prose 2000 speech to a WAV file.\n\n"
        "  prose_render [options] \"text to speak\"\n"
        "  prose_render --all [--out-dir DIR]\n\n"
        "Options:\n"
        "  --out FILE        where to write the WAV (default prose.wav)\n"
        "  --out-dir DIR     directory for --all (default samples)\n"
        "  --voice N         firmware voice 0..2 (default 0)\n"
        "  --rate N          %d..%d words per minute (default %d)\n"
        "  --pitch N         %d..%d (default %d)\n"
        "  --atten N         %d..%d, 0 is loudest (default 0)\n"
        "  --set ID=VALUE    an extra firmware parameter, repeatable\n"
        "  --all             render one sample per voice plus a parameter sweep\n"
        "  --list            list the voices and parameters, then exit\n"
        "  --data-dir DIR    where ProseHost.exe and roms\\ live\n",
        prose::kRateMin, prose::kRateMax, prose::kRateDefault, prose::kPitchMin,
        prose::kPitchMax, prose::kPitchDefault, prose::kAttenMin, prose::kAttenMax);
    return 2;
}

void list_capabilities()
{
    std::printf("Voices:\n");
    for (const prose::VoiceDesc& voice : prose::voice_catalogue()) {
        std::printf("  %d  %-20s  ~%d Hz  %s\n", voice.firmware_voice,
                    narrow(voice.display_name).c_str(), voice.nominal_f0,
                    narrow(voice.description).c_str());
    }
    std::printf("\nExtra firmware parameters:\n");
    for (const prose::ParamDesc& param : prose::extra_parameters()) {
        std::printf("  %-12s ESC[N%lc  %d..%d (default %d)  %s\n", narrow(param.id).c_str(),
                    param.command, param.min_value, param.max_value, param.default_value,
                    narrow(param.label).c_str());
    }
}

bool g_trim = true;

bool render_one(prose::ProseHostClient& host, const prose::SpeakRequest& request,
                const std::wstring& path)
{
    BufferSink sink(g_trim);
    const HRESULT hr = host.speak(request, sink);
    sink.finish();
    if (FAILED(hr) || sink.pcm().empty()) {
        std::printf("FAIL %-34s hr=0x%08lX, %zu bytes\n", narrow(path).c_str(),
                    static_cast<unsigned long>(hr), sink.pcm().size());
        return false;
    }
    if (!write_wav(path, sink.pcm())) {
        std::printf("FAIL %-34s could not write the file\n", narrow(path).c_str());
        return false;
    }
    std::printf("OK   %-34s %6.2fs  %7zu bytes  (trimmed %.0f ms of pad)\n",
                narrow(path).c_str(), sink.pcm().size() / double(prose::kSampleRate * 2),
                sink.pcm().size(),
                1000.0 * sink.trimmed_lead_bytes() / (prose::kSampleRate * 2.0));
    return true;
}

int render_all(prose::ProseHostClient& host, const std::wstring& out_dir)
{
    CreateDirectoryW(out_dir.c_str(), nullptr);
    int failures = 0;

    for (const prose::VoiceDesc& voice : prose::voice_catalogue()) {
        prose::SpeakRequest request;
        request.voice = voice.firmware_voice;
        request.text = L"This is " + voice.display_name +
                       L". The quick brown fox jumps over the lazy dog.";
        const std::wstring path =
            out_dir + L"\\voice" + std::to_wstring(voice.firmware_voice) + L"_" +
            voice.token_name + L".wav";
        if (!render_one(host, request, path)) {
            ++failures;
        }
    }

    struct Sweep {
        const wchar_t* name;
        int rate;
        int pitch;
        int atten;
    };
    const Sweep sweeps[] = {
        {L"rate_slow", prose::kRateMin, prose::kPitchDefault, 0},
        {L"rate_fast", prose::kRateMax, prose::kPitchDefault, 0},
        {L"pitch_low", prose::kRateDefault, prose::kPitchMin, 0},
        {L"pitch_high", prose::kRateDefault, prose::kPitchMax, 0},
        {L"volume_quiet", prose::kRateDefault, prose::kPitchDefault, prose::kAttenMax},
    };
    for (const Sweep& sweep : sweeps) {
        prose::SpeakRequest request;
        request.rate = sweep.rate;
        request.pitch = sweep.pitch;
        request.attenuation = sweep.atten;
        request.text = L"The quick brown fox jumps over the lazy dog.";
        if (!render_one(host, request, out_dir + L"\\" + sweep.name + L".wav")) {
            ++failures;
        }
    }

    for (const prose::ParamDesc& param : prose::extra_parameters()) {
        prose::SpeakRequest request;
        request.text = L"The quick brown fox jumps over the lazy dog.";
        request.extras.push_back({param.command, param.max_value});
        if (!render_one(host, request,
                        out_dir + L"\\param_" + param.id + L"_max.wav")) {
            ++failures;
        }
    }

    return failures;
}

}  // namespace

int wmain(int argc, wchar_t** argv)
{
    prose::log_init(L"render");

    std::wstring out = L"prose.wav";
    std::wstring out_dir = L"samples";
    std::wstring text;
    bool all = false;
    prose::SpeakRequest request;

    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        const auto next = [&](const wchar_t* name) -> std::wstring {
            if (i + 1 >= argc) {
                std::printf("%s needs a value\n", prose::log_narrow(name).c_str());
                std::exit(2);
            }
            return argv[++i];
        };

        if (arg == L"--help" || arg == L"-h" || arg == L"/?") {
            return usage();
        } else if (arg == L"--list") {
            list_capabilities();
            return 0;
        } else if (arg == L"--all") {
            all = true;
        } else if (arg == L"--raw") {
            g_trim = false;
        } else if (arg == L"--out") {
            out = next(L"--out");
        } else if (arg == L"--out-dir") {
            out_dir = next(L"--out-dir");
        } else if (arg == L"--data-dir") {
            prose::set_data_root(next(L"--data-dir"));
        } else if (arg == L"--voice") {
            request.voice = _wtoi(next(L"--voice").c_str());
        } else if (arg == L"--rate") {
            request.rate = _wtoi(next(L"--rate").c_str());
        } else if (arg == L"--pitch") {
            request.pitch = _wtoi(next(L"--pitch").c_str());
        } else if (arg == L"--atten") {
            request.attenuation = _wtoi(next(L"--atten").c_str());
        } else if (arg == L"--set") {
            const std::wstring pair = next(L"--set");
            const std::size_t eq = pair.find(L'=');
            if (eq == std::wstring::npos) {
                std::printf("--set wants ID=VALUE\n");
                return 2;
            }
            const prose::ParamDesc* param = prose::find_parameter(pair.substr(0, eq));
            if (!param) {
                std::printf("unknown parameter '%s'\n", narrow(pair.substr(0, eq)).c_str());
                return 2;
            }
            request.extras.push_back({param->command, _wtoi(pair.c_str() + eq + 1)});
        } else if (!arg.empty() && arg[0] == L'-') {
            std::printf("unknown option '%s'\n", narrow(arg).c_str());
            return usage();
        } else {
            if (!text.empty()) {
                text += L" ";
            }
            text += arg;
        }
    }

    if (!all && text.empty()) {
        return usage();
    }

    std::wstring missing;
    if (!prose::engine_files_present(&missing)) {
        std::printf("the engine files are not where they were expected: %s\n",
                    narrow(missing).c_str());
        std::printf("data root was resolved to '%s'\n", narrow(prose::data_root()).c_str());
        return 1;
    }

    prose::ProseHostClient host;
    if (FAILED(host.ensure_ready())) {
        std::printf("the emulator would not start; see %s\n",
                    narrow(prose::log_file_path()).c_str());
        return 1;
    }

    int failures = 0;
    if (all) {
        failures = render_all(host, out_dir);
    } else {
        request.text = text;
        failures = render_one(host, request, out) ? 0 : 1;
    }

    host.stop();
    if (failures) {
        std::printf("\n%d render(s) failed; see %s\n", failures,
                    narrow(prose::log_file_path()).c_str());
    }
    return failures ? 1 : 0;
}
