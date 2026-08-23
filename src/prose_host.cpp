#include "prose_host.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>

#include "prose_log.hpp"
#include "prose_paths.hpp"
#include "prose_utils.hpp"
#include "prose_voices.hpp"

namespace prose {
namespace {

constexpr DWORD kMagic = 0x4B325250;  // 'PR2K'

constexpr DWORD kSpeak = 1;
constexpr DWORD kCancel = 2;
constexpr DWORD kQuit = 3;

constexpr DWORD kReady = 101;
constexpr DWORD kAudio = 102;
constexpr DWORD kDone = 103;
constexpr DWORD kError = 104;
constexpr DWORD kCancelled = 105;

constexpr DWORD kHeaderSize = 16;
constexpr DWORD kMaxPayload = 1u << 20;

// How long to wait for the emulator to report ready. Booting the firmware takes well
// under a second on any machine that can run Windows 11, but a cold disk can be slow.
constexpr DWORD kStartupTimeoutMs = 10000;

// How long a single utterance may go without any message before the host is presumed
// wedged. Synthesis runs at 3-11x real time, so this is generous.
constexpr DWORD kStallTimeoutMs = 30000;

// After asking the firmware to stop, how long to keep draining before giving up on a clean
// cancel and restarting the process instead.
//
// Measured: a cancel issued while the firmware is genuinely speaking is acknowledged in
// 62-78 ms. A cancel for an utterance that has already finished is never acknowledged at
// all - correctly, since there is nothing left to stop - so this timeout is the only way
// out of that case, and it is on the path between a keypress and the next thing spoken.
// 400 ms leaves five times the measured headroom while capping the worst case at a quarter
// of what it was.
constexpr DWORD kCancelTimeoutMs = 400;

void put_u32(BYTE* p, DWORD value)
{
    p[0] = static_cast<BYTE>(value & 0xFF);
    p[1] = static_cast<BYTE>((value >> 8) & 0xFF);
    p[2] = static_cast<BYTE>((value >> 16) & 0xFF);
    p[3] = static_cast<BYTE>((value >> 24) & 0xFF);
}

[[nodiscard]] DWORD get_u32(const BYTE* p)
{
    return static_cast<DWORD>(p[0]) | (static_cast<DWORD>(p[1]) << 8) |
           (static_cast<DWORD>(p[2]) << 16) | (static_cast<DWORD>(p[3]) << 24);
}

// The firmware only understands 7-bit ASCII. Anything else is folded to a space rather
// than dropped, so word spacing survives and the rhythm of the sentence is preserved.
[[nodiscard]] std::string to_firmware_ascii(const std::wstring& text)
{
    std::string out;
    out.reserve(text.size());
    for (const wchar_t wc : text) {
        if (wc >= 0x20 && wc < 0x7F) {
            out.push_back(static_cast<char>(wc));
        } else if (wc == L'\t' || wc == L'\n' || wc == L'\r') {
            out.push_back(' ');
        } else if (wc >= 0x80) {
            // A few characters are worth spelling rather than blanking, because they turn
            // up constantly in ordinary text and the firmware predates all of them.
            switch (wc) {
                case 0x2018: case 0x2019: out.push_back('\''); break;   // curly quotes
                case 0x201C: case 0x201D: out.push_back('"'); break;
                case 0x2013: case 0x2014: out.push_back('-'); break;    // dashes
                case 0x2026: out.append("..."); break;                  // ellipsis
                case 0x00A0: out.push_back(' '); break;                 // nbsp
                default: out.push_back(' '); break;
            }
        }
    }
    return out;
}

}  // namespace

void fill_output_format(WAVEFORMATEX& wfx)
{
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = kChannels;
    wfx.nSamplesPerSec = kSampleRate;
    wfx.wBitsPerSample = kBitsPerSample;
    wfx.nBlockAlign = static_cast<WORD>(wfx.nChannels * wfx.wBitsPerSample / 8);
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
    wfx.cbSize = 0;
}

void apply_volume(void* samples, std::size_t bytes, int percent)
{
    if (percent >= 100 || bytes < sizeof(short)) {
        return;
    }
    percent = (std::max)(0, percent);
    auto* p = static_cast<short*>(samples);
    const std::size_t count = bytes / sizeof(short);
    for (std::size_t i = 0; i < count; ++i) {
        const int scaled = (static_cast<int>(p[i]) * percent) / 100;
        p[i] = static_cast<short>((std::max)(-32768, (std::min)(32767, scaled)));
    }
}

namespace {

// A genuine pause inside an utterance is worth keeping, so silence is only treated as a
// trailing pad once it has run longer than this without anything following it.
constexpr std::size_t kMaxHeldSilenceSamples = kSampleRate / 2;  // 500 ms

// Left on the end so the final phoneme's decay is not clipped.
constexpr std::size_t kKeepTailSamples = kSampleRate / 50;  // 20 ms

// If no speech has been found after this much audio the utterance is presumably very quiet
// or is silence on purpose, so the search gives up rather than swallowing it.
constexpr std::size_t kMaxHeadBytes = 3 * kSampleRate * sizeof(short);  // 3 s

void append_silence(std::vector<BYTE>& out, std::size_t samples)
{
    out.insert(out.end(), samples * sizeof(short), 0);
}

}  // namespace

void SilenceTrimmer::set_preserved_lead_ms(unsigned ms)
{
    preserved_lead_ms_ = ms;
}

bool SilenceTrimmer::find_onset(std::size_t& offset) const
{
    const auto* samples = reinterpret_cast<const short*>(head_.data());
    const std::size_t count = head_.size() / sizeof(short);
    const std::size_t windows = count / kGateWindowSamples;
    if (windows == 0) {
        return false;
    }

    const auto window_rms = [&](std::size_t w) {
        double sum = 0.0;
        const short* p = samples + w * kGateWindowSamples;
        for (std::size_t i = 0; i < kGateWindowSamples; ++i) {
            sum += static_cast<double>(p[i]) * p[i];
        }
        return std::sqrt(sum / kGateWindowSamples);
    };

    for (std::size_t w = 0; w < windows; ++w) {
        if (window_rms(w) < kSpeechRms) {
            continue;
        }
        // Found speech. Walk back over the quieter windows leading into it so the attack
        // of the first phoneme survives.
        std::size_t start = w;
        while (start > 0 && window_rms(start - 1) >= kOnsetRms) {
            --start;
        }
        offset = start * kGateWindowSamples * sizeof(short);
        return true;
    }
    return false;
}

void SilenceTrimmer::process(const void* data, std::size_t bytes, std::vector<BYTE>& out)
{
    const auto* samples = static_cast<const short*>(data);
    const std::size_t count = bytes / sizeof(short);
    if (count == 0) {
        return;
    }

    if (!lead_done_) {
        // Hold the audio until the speech onset is known. Nothing is delayed by this: the
        // firmware delivers far faster than real time, and what is being held back is the
        // silence that would otherwise have to be played before anything was heard.
        const auto* p = static_cast<const BYTE*>(data);
        head_.insert(head_.end(), p, p + bytes);

        std::size_t onset = 0;
        if (find_onset(onset)) {
            lead_done_ = true;
            trimmed_lead_bytes_ += onset;
            if (preserved_lead_ms_ > 0) {
                append_silence(out, preserved_lead_ms_ * kSampleRate / 1000);
                preserved_lead_ms_ = 0;
            }
            out.insert(out.end(), head_.begin() + onset, head_.end());
            head_.clear();
            head_.shrink_to_fit();
        } else if (head_.size() > kMaxHeadBytes) {
            // Nothing loud enough to be speech in a long time. Rather than keep swallowing
            // audio, fall back to dropping only the leading digital silence.
            std::size_t first = 0;
            const auto* h = reinterpret_cast<const short*>(head_.data());
            const std::size_t n = head_.size() / sizeof(short);
            while (first < n && std::abs(static_cast<int>(h[first])) < kSilenceThreshold) {
                ++first;
            }
            lead_done_ = true;
            trimmed_lead_bytes_ += first * sizeof(short);
            if (preserved_lead_ms_ > 0) {
                append_silence(out, preserved_lead_ms_ * kSampleRate / 1000);
                preserved_lead_ms_ = 0;
            }
            out.insert(out.end(), head_.begin() + first * sizeof(short), head_.end());
            head_.clear();
            head_.shrink_to_fit();
        }
        return;
    }

    const std::size_t start = 0;

    // Find where the block's own trailing run of silence begins.
    std::size_t last_loud = count;
    for (std::size_t i = count; i > start; --i) {
        if (std::abs(static_cast<int>(samples[i - 1])) >= kSilenceThreshold) {
            last_loud = i;
            break;
        }
    }

    if (last_loud == count) {
        // No loud sample in this block at all: it is all potential trailing silence.
        const auto* p = reinterpret_cast<const BYTE*>(samples + start);
        pending_.insert(pending_.end(), p, p + (count - start) * sizeof(short));
    } else {
        // Something loud arrived, so the silence held back was internal after all.
        out.insert(out.end(), pending_.begin(), pending_.end());
        pending_.clear();
        const auto* p = reinterpret_cast<const BYTE*>(samples + start);
        out.insert(out.end(), p, p + (last_loud - start) * sizeof(short));
        const auto* tail = reinterpret_cast<const BYTE*>(samples + last_loud);
        pending_.insert(pending_.end(), tail, tail + (count - last_loud) * sizeof(short));
    }

    // A pause this long is deliberate, not padding; let the excess through so it is heard.
    const std::size_t held = pending_.size() / sizeof(short);
    if (held > kMaxHeldSilenceSamples) {
        const std::size_t release = (held - kMaxHeldSilenceSamples) * sizeof(short);
        out.insert(out.end(), pending_.begin(), pending_.begin() + release);
        pending_.erase(pending_.begin(), pending_.begin() + release);
    }
}

void SilenceTrimmer::finish(std::vector<BYTE>& out)
{
    if (!lead_done_) {
        // The utterance ended before anything that looked like speech turned up. Emit any
        // silence that was actually asked for, and whatever real audio was held, minus the
        // digital silence at its head.
        if (preserved_lead_ms_ > 0) {
            append_silence(out, preserved_lead_ms_ * kSampleRate / 1000);
            preserved_lead_ms_ = 0;
        }
        const auto* h = reinterpret_cast<const short*>(head_.data());
        const std::size_t n = head_.size() / sizeof(short);
        std::size_t first = 0;
        while (first < n && std::abs(static_cast<int>(h[first])) < kSilenceThreshold) {
            ++first;
        }
        if (first < n) {
            out.insert(out.end(), head_.begin() + first * sizeof(short), head_.end());
        }
        head_.clear();
        pending_.clear();
        return;
    }
    const std::size_t keep =
        (std::min)(pending_.size(), kKeepTailSamples * sizeof(short));
    out.insert(out.end(), pending_.begin(), pending_.begin() + keep);
    pending_.clear();
}

ProseHostClient::~ProseHostClient()
{
    stop();
}

std::string ProseHostClient::build_prefix(const SpeakRequest& request)
{
    // Firmware settings persist across utterances - the emulator models real hardware, and
    // the real hardware kept whatever it was last told. So every utterance restates the
    // complete parameter set rather than only what changed. Sending a partial set lets a
    // value set once leak into everything spoken afterwards, which showed up as an
    // utterance falling silent because a parameter from three utterances earlier was still
    // in force.
    //
    // Order matters only in that voice comes first: selecting a voice resets the tract, and
    // the rate, pitch and level that follow have to survive that.
    char buffer[256];
    std::string prefix;

    const auto append = [&](int value, wchar_t command) {
        _snprintf_s(buffer, _TRUNCATE, "\x1b[%d%c", value, static_cast<char>(command));
        prefix += buffer;
    };

    append(request.voice, L'V');
    append(request.rate, L'r');
    append(request.pitch, L'p');
    append(request.attenuation, L'a');

    for (const ParamDesc& param : extra_parameters()) {
        int value = param.default_value;
        for (const auto& [command, supplied] : request.extras) {
            if (command == param.command) {
                value = supplied;
            }
        }
        append((std::max)(param.min_value, (std::min)(param.max_value, value)), param.command);
    }
    return prefix;
}

bool ProseHostClient::start()
{
    std::wstring missing;
    if (!engine_files_present(&missing)) {
        PROSE_LOG_E("cannot start the emulator, missing %s", log_narrow(missing.c_str()).c_str());
        return false;
    }

    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE child_stdin_read = nullptr;
    HANDLE child_stdout_write = nullptr;

    if (!CreatePipe(&child_stdin_read, &stdin_write_, &sa, 0)) {
        PROSE_LOG_E("CreatePipe for stdin failed, error %lu", GetLastError());
        return false;
    }
    if (!CreatePipe(&stdout_read_, &child_stdout_write, &sa, 0)) {
        PROSE_LOG_E("CreatePipe for stdout failed, error %lu", GetLastError());
        CloseHandle(child_stdin_read);
        CloseHandle(stdin_write_);
        stdin_write_ = nullptr;
        return false;
    }

    // Our ends must not leak into the child, or the pipe never reports EOF.
    SetHandleInformation(stdin_write_, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stdout_read_, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = child_stdin_read;
    si.hStdOutput = child_stdout_write;
    si.hStdError = INVALID_HANDLE_VALUE;

    const std::wstring host = host_exe_path();
    const std::wstring roms = rom_dir();
    std::wstring command_line = L"\"" + host + L"\" \"" + roms + L"\"";
    std::vector<wchar_t> mutable_command_line(command_line.begin(), command_line.end());
    mutable_command_line.push_back(L'\0');

    PROCESS_INFORMATION pi = {};
    const BOOL created = CreateProcessW(host.c_str(), mutable_command_line.data(), nullptr,
                                        nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                        data_root().c_str(), &si, &pi);

    CloseHandle(child_stdin_read);
    CloseHandle(child_stdout_write);

    if (!created) {
        const DWORD error = GetLastError();
        PROSE_LOG_E("could not start %s, error %lu", log_narrow(host.c_str()).c_str(), error);
        close_handles();
        return false;
    }

    CloseHandle(pi.hThread);
    process_ = pi.hProcess;
    PROSE_LOG_I("started %s (pid %lu) with roms %s", log_narrow(host.c_str()).c_str(),
                pi.dwProcessId, log_narrow(roms.c_str()).c_str());

    shutting_down_ = false;
    reader_ = std::thread(&ProseHostClient::reader_loop, this, stdout_read_, process_);

    Message message;
    if (!pop(message, kStartupTimeoutMs)) {
        PROSE_LOG_E("the emulator did not report ready within %lu ms", kStartupTimeoutMs);
        stop_locked();
        return false;
    }
    if (message.type != kReady) {
        const std::string detail(message.payload.begin(), message.payload.end());
        PROSE_LOG_E("the emulator failed to start: %s",
                    message.error.empty() ? detail.c_str() : message.error.c_str());
        stop_locked();
        return false;
    }

    PROSE_LOG_I("emulator ready");
    return true;
}

void ProseHostClient::reader_loop(HANDLE pipe, HANDLE process)
{
    BYTE header[kHeaderSize];

    const auto read_exact = [&](BYTE* out, DWORD size) -> bool {
        DWORD total = 0;
        while (total < size) {
            DWORD got = 0;
            if (!ReadFile(pipe, out + total, size - total, &got, nullptr) || got == 0) {
                return false;
            }
            total += got;
        }
        return true;
    };

    for (;;) {
        if (!read_exact(header, kHeaderSize)) {
            break;
        }
        const DWORD magic = get_u32(header);
        const DWORD type = get_u32(header + 4);
        const DWORD generation = get_u32(header + 8);
        const DWORD size = get_u32(header + 12);

        if (magic != kMagic || size > kMaxPayload) {
            Message bad;
            bad.fatal = true;
            bad.error = "the emulator returned an invalid message";
            {
                std::lock_guard<std::mutex> lock(queue_mutex_);
                queue_.push_back(std::move(bad));
            }
            queue_cv_.notify_all();
            return;
        }

        Message message;
        message.type = type;
        message.generation = generation;
        if (size) {
            message.payload.resize(size);
            if (!read_exact(message.payload.data(), size)) {
                break;
            }
        }
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            queue_.push_back(std::move(message));
        }
        queue_cv_.notify_all();
    }

    // The pipe closed. Report it unless we are the ones tearing the process down.
    if (!shutting_down_.load()) {
        DWORD exit_code = 0;
        GetExitCodeProcess(process, &exit_code);
        Message eof;
        eof.fatal = true;
        eof.error = "the emulator stopped unexpectedly (exit code " +
                    std::to_string(exit_code) + ")";
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            queue_.push_back(std::move(eof));
        }
        queue_cv_.notify_all();
    }
}

bool ProseHostClient::send(DWORD type, DWORD generation, const void* payload, DWORD size)
{
    std::vector<BYTE> packet(kHeaderSize + size);
    put_u32(packet.data(), kMagic);
    put_u32(packet.data() + 4, type);
    put_u32(packet.data() + 8, generation);
    put_u32(packet.data() + 12, size);
    if (size && payload) {
        memcpy(packet.data() + kHeaderSize, payload, size);
    }

    std::lock_guard<std::mutex> lock(write_mutex_);
    if (!stdin_write_) {
        return false;
    }
    DWORD total = 0;
    while (total < packet.size()) {
        DWORD written = 0;
        if (!WriteFile(stdin_write_, packet.data() + total,
                       static_cast<DWORD>(packet.size()) - total, &written, nullptr) ||
            written == 0) {
            PROSE_LOG_E("writing to the emulator failed, error %lu", GetLastError());
            return false;
        }
        total += written;
    }
    return true;
}

bool ProseHostClient::pop(Message& out, DWORD timeout_ms)
{
    std::unique_lock<std::mutex> lock(queue_mutex_);
    if (!queue_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                            [this] { return !queue_.empty(); })) {
        return false;
    }
    out = std::move(queue_.front());
    queue_.pop_front();
    return true;
}

void ProseHostClient::drain()
{
    std::lock_guard<std::mutex> lock(queue_mutex_);
    queue_.clear();
}

void ProseHostClient::close_handles()
{
    if (stdin_write_) {
        CloseHandle(stdin_write_);
        stdin_write_ = nullptr;
    }
    if (stdout_read_) {
        CloseHandle(stdout_read_);
        stdout_read_ = nullptr;
    }
    if (process_) {
        CloseHandle(process_);
        process_ = nullptr;
    }
}

bool ProseHostClient::running() const
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!process_) {
        return false;
    }
    return WaitForSingleObject(process_, 0) == WAIT_TIMEOUT;
}

HRESULT ProseHostClient::ensure_ready()
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (process_ && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT) {
        return S_OK;
    }
    if (process_) {
        PROSE_LOG_W("the emulator had exited; restarting it");
    }
    stop_locked();
    return start() ? S_OK : E_FAIL;
}

void ProseHostClient::abort_current()
{
    abort_requested_ = true;
    const DWORD generation = active_generation_.load();
    if (generation != 0) {
        PROSE_LOG_D("asking the firmware to cancel generation %lu", generation);
        send(kCancel, generation, nullptr, 0);
    }
}

HRESULT ProseHostClient::speak(const SpeakRequest& request, SynthSink& sink)
{
    const HRESULT ready = ensure_ready();
    if (FAILED(ready)) {
        return ready;
    }

    const std::string body = to_firmware_ascii(request.text);
    if (body.empty()) {
        return S_OK;
    }
    const std::string payload = build_prefix(request) + body;

    DWORD generation = 0;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        generation = ++generation_;
    }
    abort_requested_ = false;
    active_generation_ = generation;
    drain();

    PROSE_LOG_D("speak gen=%lu voice=%d rate=%d pitch=%d atten=%d, %zu chars", generation,
                request.voice, request.rate, request.pitch, request.attenuation, body.size());

    if (!send(kSpeak, generation, payload.data(), static_cast<DWORD>(payload.size()))) {
        active_generation_ = 0;
        return E_FAIL;
    }

    bool cancelling = false;
    DWORD cancel_started = 0;
    DWORD last_progress = GetTickCount();
    ULONGLONG produced = 0;
    HRESULT result = S_OK;

    for (;;) {
        if (!cancelling && (abort_requested_.load() || sink.should_abort())) {
            PROSE_LOG_I("aborting generation %lu", generation);
            send(kCancel, generation, nullptr, 0);
            cancelling = true;
            cancel_started = GetTickCount();
            result = S_FALSE;
        }

        Message message;
        if (!pop(message, 250)) {
            if (cancelling && GetTickCount() - cancel_started > kCancelTimeoutMs) {
                // No acknowledgement. The usual reason is that the utterance had already
                // finished, so there was nothing left for the firmware to stop - it is
                // idle and perfectly healthy. Killing it here used to cost a restart on
                // the path between a keypress and the next thing spoken, for nothing.
                //
                // Leaving it running is safe: generations only ever increase, the next
                // speak() drains the queue first, and anything late is discarded by the
                // generation check below.
                PROSE_LOG_D("no cancel acknowledgement for generation %lu; the firmware is "
                            "idle, keeping it", generation);
                break;
            }
            if (!cancelling && !running()) {
                PROSE_LOG_E("the emulator exited mid-utterance");
                result = E_FAIL;
                break;
            }
            if (!cancelling && GetTickCount() - last_progress > kStallTimeoutMs) {
                // Some firmware commands can wedge the synthesiser past its own time
                // limit. Restarting is the only way back, and is far better than hanging
                // whatever application asked us to speak.
                PROSE_LOG_E("no message for %lu ms; restarting the wedged emulator",
                            kStallTimeoutMs);
                stop();
                result = E_FAIL;
                break;
            }
            continue;
        }

        last_progress = GetTickCount();

        if (message.fatal) {
            PROSE_LOG_E("%s", message.error.c_str());
            stop();
            result = E_FAIL;
            break;
        }
        if (message.generation != generation) {
            // Audio left over from an utterance that has already been cancelled.
            continue;
        }

        if (message.type == kAudio) {
            if (cancelling) {
                continue;
            }
            produced += message.payload.size();
            if (!sink.on_audio(message.payload.data(),
                               static_cast<DWORD>(message.payload.size()))) {
                // The sink asked to stop; let the next pass issue the cancel.
                abort_requested_ = true;
            }
        } else if (message.type == kDone) {
            PROSE_LOG_D("generation %lu done, %llu bytes", generation, produced);
            break;
        } else if (message.type == kCancelled) {
            PROSE_LOG_I("generation %lu cancelled by the firmware", generation);
            result = S_FALSE;
            break;
        } else if (message.type == kError) {
            const std::string detail(message.payload.begin(), message.payload.end());
            PROSE_LOG_E("the firmware reported an error: %s", detail.c_str());
            result = E_FAIL;
            break;
        }
    }

    active_generation_ = 0;
    return result;
}

void ProseHostClient::stop()
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    stop_locked();
}

void ProseHostClient::stop_locked()
{
    if (!process_ && !reader_.joinable()) {
        return;
    }
    shutting_down_ = true;

    if (process_ && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT) {
        send(kQuit, 0, nullptr, 0);
        if (WaitForSingleObject(process_, 1000) != WAIT_OBJECT_0) {
            PROSE_LOG_W("the emulator did not quit on request; terminating it");
            TerminateProcess(process_, 1);
            WaitForSingleObject(process_, 1000);
        }
    }

    // Closing our write end lets the reader see EOF and fall out of its loop.
    {
        std::lock_guard<std::mutex> write_lock(write_mutex_);
        if (stdin_write_) {
            CloseHandle(stdin_write_);
            stdin_write_ = nullptr;
        }
    }
    if (reader_.joinable()) {
        reader_.join();
    }
    close_handles();
    drain();
    active_generation_ = 0;
    PROSE_LOG_I("emulator stopped");
}

}  // namespace prose
