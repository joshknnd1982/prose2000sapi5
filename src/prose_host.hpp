// The client for ProseHost.exe: process lifetime plus the framed stdio protocol.
//
// ProseHost.exe is a 64-bit console program that imports only KERNEL32 and msvcrt. A
// 32-bit caller can spawn it perfectly well under WOW64, so both architectures of the SAPI
// engine use this same out-of-process design and there is no 32-bit helper anywhere in
// this project.
//
// Wire format, little endian, repeated for every message in both directions:
//
//   uint32 magic       'PR2K' (0x4B325250)
//   uint32 messageType
//   uint32 generation  echoed back on every reply, so stale audio can be discarded
//   uint32 payloadSize
//   byte   payload[payloadSize]

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>
#include <mmreg.h>

namespace prose {

// The firmware always produces this, and it is not configurable.
inline constexpr DWORD kSampleRate = 10000;
inline constexpr WORD kBitsPerSample = 16;
inline constexpr WORD kChannels = 1;

void fill_output_format(WAVEFORMATEX& wfx);

// Scales 16-bit PCM in place. The firmware's own attenuation is coarse (16 steps), so
// SAPI's finer volume percentage is applied to the samples on top of it.
void apply_volume(void* samples, std::size_t bytes, int percent);

// Anything quieter than this counts as silence when looking for the end of an utterance.
inline constexpr int kSilenceThreshold = 64;

// Removes the silence the firmware pads every utterance with.
//
// This is the whole of the perceived speech latency. Synthesis itself delivers its first
// audio message 15-47 ms after being asked, but the firmware puts 0.23 s to 1.08 s of
// silence at the head of what it returns, scaling with the length of the utterance - so a
// screen reader announcing "Documents folder" waits nearly half a second to be heard.
//
// Finding the head is not as simple as "first sample above a threshold". The firmware
// emits a brief fixed marker - peak 247, RMS around 198 - at the point the pad ends, and
// on longer utterances a further stretch of true digital silence follows it before any
// speech. A single-sample test stops on that marker and then treats the silence behind it
// as an internal pause worth keeping, which trims almost nothing.
//
// So the head is found on windowed energy instead: locate the first window that is
// unambiguously speech, then walk back over any quieter windows that lead into it so the
// attack of the first phoneme is not clipped.
inline constexpr std::size_t kGateWindowSamples = 100;  // 10 ms
inline constexpr double kSpeechRms = 600.0;   // clearly speech, well above the 198 marker
inline constexpr double kOnsetRms = 100.0;    // quiet enough to be the attack of a word

// The trimming streams: once the head is found audio is passed through as it arrives, and
// a run of silence at the tail is held back until something louder follows, so an internal
// pause survives but a trailing pad does not.
class SilenceTrimmer {
public:
    // Silence that was actually asked for (a SAPI silence fragment at the start of the
    // utterance) and must therefore be reproduced rather than trimmed away.
    void set_preserved_lead_ms(unsigned ms);

    // Appends the audio that should really be played to `out`.
    void process(const void* data, std::size_t bytes, std::vector<BYTE>& out);

    // Call at the end of an utterance. Emits a short tail so the last phoneme is not cut.
    void finish(std::vector<BYTE>& out);

    [[nodiscard]] std::size_t trimmed_lead_bytes() const { return trimmed_lead_bytes_; }

private:
    // Looks for the speech onset in head_. Returns true and sets `offset` (in bytes) when
    // it is found.
    [[nodiscard]] bool find_onset(std::size_t& offset) const;

    bool lead_done_ = false;
    unsigned preserved_lead_ms_ = 0;
    std::size_t trimmed_lead_bytes_ = 0;
    std::vector<BYTE> head_;     // audio held while the speech onset is still being sought
    std::vector<BYTE> pending_;  // a run of silence that may yet turn out to be internal
};

// Receives one utterance. Every call happens on the thread that called speak().
class SynthSink {
public:
    virtual ~SynthSink() = default;

    // Return false to abort the rest of the utterance.
    virtual bool on_audio(const void* data, DWORD size) = 0;

    // Polled between messages so a caller can abort without waiting for more audio.
    virtual bool should_abort() { return false; }
};

// One utterance, with the firmware values already resolved.
struct SpeakRequest {
    std::wstring text;     // ASCII-safe text; control codes are added by the client
    int voice = 0;         // ESC[<n>V
    int rate = 150;        // ESC[<n>r, words per minute
    int pitch = 85;        // ESC[<n>p
    int attenuation = 0;   // ESC[<n>a, 0 is loudest
    std::vector<std::pair<wchar_t, int>> extras;  // firmware command letter and value
};

class ProseHostClient {
public:
    ProseHostClient() = default;
    ~ProseHostClient();

    ProseHostClient(const ProseHostClient&) = delete;
    ProseHostClient& operator=(const ProseHostClient&) = delete;

    // Starts the emulator if it is not already running and waits for its ready message.
    HRESULT ensure_ready();

    // Renders one utterance, streaming audio into the sink. Returns S_FALSE when the
    // utterance was aborted rather than completed.
    HRESULT speak(const SpeakRequest& request, SynthSink& sink);

    // Safe to call from any thread, including while speak() is running.
    void abort_current();

    void stop();

    [[nodiscard]] bool running() const;

private:
    struct Message {
        DWORD type = 0;
        DWORD generation = 0;
        std::vector<BYTE> payload;
        bool fatal = false;          // the reader hit EOF or a protocol error
        std::string error;
    };

    // start() and stop_locked() both assume state_mutex_ is already held by the caller;
    // stop() is the public wrapper that takes it. Keeping the distinction explicit is what
    // stops ensure_ready() from deadlocking against its own failure path.
    bool start();
    void stop_locked();
    void reader_loop(HANDLE pipe, HANDLE process);
    bool send(DWORD type, DWORD generation, const void* payload, DWORD size);
    bool pop(Message& out, DWORD timeout_ms);
    void drain();
    void close_handles();

    [[nodiscard]] static std::string build_prefix(const SpeakRequest& request);

    HANDLE process_ = nullptr;
    HANDLE stdin_write_ = nullptr;
    HANDLE stdout_read_ = nullptr;
    std::thread reader_;

    mutable std::mutex state_mutex_;
    std::mutex write_mutex_;

    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<Message> queue_;

    DWORD generation_ = 0;
    std::atomic<DWORD> active_generation_{0};
    std::atomic<bool> abort_requested_{false};
    std::atomic<bool> shutting_down_{false};
};

}  // namespace prose
