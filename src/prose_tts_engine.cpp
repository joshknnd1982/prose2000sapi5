#include "prose_tts_engine.hpp"

#include <algorithm>
#include <cstdlib>
#include <cwctype>
#include <new>

#include "prose_log.hpp"
#include "prose_paths.hpp"

namespace prose {
namespace sapi {
namespace {

// Audio is handed to SAPI in pieces this size so a stop request is noticed promptly rather
// than after a whole sentence has already been passed over.
constexpr ULONG kWriteChunkBytes = 4096;

[[nodiscard]] bool is_word_char(wchar_t c)
{
    return iswalnum(static_cast<wint_t>(c)) != 0 || c == L'\'' || c == L'-';
}

[[nodiscard]] int clamp_sapi(int value)
{
    return (std::max)(-10, (std::min)(10, value));
}

// Feeds one segment into ISpTTSEngineSite and counts what was accepted.
class SiteSink : public SynthSink
{
public:
    SiteSink(ISpTTSEngineSite* site, ProseHostClient* host, int volume_percent,
             unsigned preserved_lead_ms)
        : site_(site), host_(host), volume_percent_(volume_percent)
    {
        trimmer_.set_preserved_lead_ms(preserved_lead_ms);
    }

    bool on_audio(const void* data, DWORD size) override
    {
        // Strip the firmware's padding before anything else. This is what makes the voice
        // respond promptly rather than a third of a second late.
        trimmed_.clear();
        trimmer_.process(data, size, trimmed_);
        return emit(trimmed_);
    }

    // Releases the short tail the trimmer holds back. Must be called once per utterance.
    bool finish()
    {
        trimmed_.clear();
        trimmer_.finish(trimmed_);
        return emit(trimmed_);
    }

    bool should_abort() override { return aborted_ || check_actions(); }

    [[nodiscard]] ULONGLONG bytes_written() const { return bytes_written_; }
    [[nodiscard]] bool aborted() const { return aborted_; }

private:
    // Writes one buffer to SAPI in pieces, honouring an abort between them.
    bool emit(std::vector<BYTE>& buffer)
    {
        if (buffer.empty()) {
            return !aborted_;
        }

        // The firmware's own attenuation is 16 coarse steps, so SAPI's finer percentage is
        // applied to the samples here instead.
        if (volume_percent_ < 100) {
            apply_volume(buffer.data(), buffer.size(), volume_percent_);
        }

        const BYTE* p = buffer.data();
        ULONG remaining = static_cast<ULONG>(buffer.size());
        while (remaining > 0) {
            if (check_actions()) {
                return false;
            }
            const ULONG want = (std::min)(remaining, kWriteChunkBytes);
            ULONG written = 0;
            const HRESULT hr = site_->Write(p, want, &written);
            if (FAILED(hr)) {
                PROSE_LOG_E("ISpTTSEngineSite::Write failed %s", hresult_string(hr).c_str());
                aborted_ = true;
                return false;
            }
            if (written == 0 || written > want) {
                PROSE_LOG_E("ISpTTSEngineSite::Write accepted %lu of %lu bytes", written, want);
                aborted_ = true;
                return false;
            }
            bytes_written_ += written;
            remaining -= written;
            p += written;
        }
        return true;
    }

    bool check_actions()
    {
        if (aborted_) {
            return true;
        }
        const DWORD actions = site_->GetActions();
        if (actions & SPVES_ABORT) {
            PROSE_LOG_I("SAPI asked to abort");
            aborted_ = true;
        } else if (actions & SPVES_SKIP) {
            // The firmware cannot skip ahead, so report that nothing was skipped and stop.
            PROSE_LOG_I("SAPI asked to skip; the firmware has no skip, reporting none");
            site_->CompleteSkip(0);
            aborted_ = true;
        }
        if (aborted_ && host_) {
            host_->abort_current();
        }
        return aborted_;
    }

    ISpTTSEngineSite* site_;
    ProseHostClient* host_;
    ULONGLONG bytes_written_ = 0;
    int volume_percent_ = 100;
    SilenceTrimmer trimmer_;
    std::vector<BYTE> trimmed_;
    bool aborted_ = false;
};

}  // namespace

ISpTTSEngineImpl::ISpTTSEngineImpl() = default;

ISpTTSEngineImpl::~ISpTTSEngineImpl()
{
    // The warm-up holds a raw pointer to host_, so it has to finish before anything here
    // is destroyed.
    if (warmup_.joinable()) {
        warmup_.join();
    }
}

SpeakRequest ISpTTSEngineImpl::request_for(const Settings& settings, int sapi_rate,
                                           int sapi_pitch) const
{
    SpeakRequest request;
    request.voice = voice_.firmware_voice;
    request.rate = sapi_rate_to_firmware(clamp_sapi(sapi_rate + settings.rate_offset));
    request.pitch = sapi_pitch_to_firmware(clamp_sapi(sapi_pitch + settings.pitch_offset));
    // The firmware attenuator is left at full and the level is applied to the samples
    // instead, because 16 steps is too coarse for a volume slider to feel continuous.
    request.attenuation = kAttenMin;

    for (const ParamDesc& param : extra_parameters()) {
        request.extras.push_back(
            {param.command, settings.extra(param.id, param.default_value)});
    }
    return request;
}

void ISpTTSEngineImpl::fire(ISpTTSEngineSite* site, const PendingEvent& pending,
                            ULONGLONG offset)
{
    SPEVENT event = {};
    event.eEventId = pending.id;
    event.ulStreamNum = 0;
    event.ullAudioStreamOffset = offset;

    if (pending.id == SPEI_TTS_BOOKMARK) {
        event.elParamType = SPET_LPARAM_IS_STRING;
        event.lParam = reinterpret_cast<LPARAM>(pending.bookmark_text.c_str());
        event.wParam = static_cast<WPARAM>(pending.bookmark_number);
    } else {
        event.elParamType = SPET_LPARAM_IS_UNDEFINED;
        event.lParam = static_cast<LPARAM>(pending.text_offset);
        event.wParam = static_cast<WPARAM>(pending.text_length);
    }

    const HRESULT hr = site->AddEvents(&event, 1);
    if (FAILED(hr)) {
        PROSE_LOG_W("AddEvents failed %s", hresult_string(hr).c_str());
    }
}

void ISpTTSEngineImpl::build_segments(const SPVTEXTFRAG* frags, ISpTTSEngineSite* site,
                                      const Settings& settings, bool want_word_events,
                                      bool want_sentence_events, std::vector<Segment>& out)
{
    long site_rate = 0;
    site->GetRate(&site_rate);

    std::vector<PendingEvent> pending;
    Segment current;
    bool have_current = false;
    int current_rate = 0;
    int current_pitch = 0;

    // Starts a new segment, which is how an event gets an exact audio offset: everything
    // before it has already been rendered, so the offset is simply what has been written.
    const auto flush = [&]() {
        if (have_current && !current.text.empty()) {
            out.push_back(std::move(current));
        }
        current = Segment{};
        have_current = false;
    };

    for (const SPVTEXTFRAG* frag = frags; frag; frag = frag->pNext) {
        const int rate = clamp_sapi(static_cast<int>(site_rate) + frag->State.RateAdj);
        const int pitch = clamp_sapi(frag->State.PitchAdj.MiddleAdj);

        if (have_current && (rate != current_rate || pitch != current_pitch)) {
            flush();
        }
        if (!have_current) {
            current = Segment{};
            current.request = request_for(settings, rate, pitch);
            current.events_before = std::move(pending);
            pending.clear();
            current_rate = rate;
            current_pitch = pitch;
            have_current = true;
        }

        switch (frag->State.eAction) {
            case SPVA_Bookmark: {
                // A bookmark has to land at an exact instant, so it closes the current
                // segment and opens the next one. Applications use these to track speech
                // position, and an approximate offset would defeat the purpose.
                PendingEvent event;
                event.id = SPEI_TTS_BOOKMARK;
                if (frag->ulTextLen > 0 && frag->pTextStart) {
                    event.bookmark_text.assign(frag->pTextStart, frag->ulTextLen);
                    event.bookmark_number = wcstol(event.bookmark_text.c_str(), nullptr, 10);
                }
                flush();
                pending.push_back(std::move(event));
                break;
            }

            case SPVA_Silence: {
                if (frag->State.SilenceMSecs > 0) {
                    if (current.text.empty()) {
                        // Nothing has been spoken in this segment yet, so this silence
                        // lands at the head - exactly where the trimmer strips the
                        // firmware's padding. Record it so it can be put back as real
                        // silence rather than being cut along with the pad.
                        current.lead_silence_ms += frag->State.SilenceMSecs;
                    } else {
                        // Inside the segment: the firmware has no pause command, so this
                        // is spoken as a run of spaces and survives trimming because the
                        // trimmer only holds back silence that nothing follows.
                        const int spaces = (std::min)(
                            40, 1 + static_cast<int>(frag->State.SilenceMSecs) / 60);
                        current.text.append(static_cast<std::size_t>(spaces), L' ');
                    }
                }
                break;
            }

            case SPVA_SpellOut: {
                // Spelling is done here rather than by the firmware: ESC[2N looked like a
                // spell mode but emits a fixed 3.2 seconds whatever it is given, so the
                // letters are simply separated instead.
                if (frag->ulTextLen > 0 && frag->pTextStart) {
                    for (ULONG i = 0; i < frag->ulTextLen; ++i) {
                        current.text.push_back(frag->pTextStart[i]);
                        current.text.push_back(L' ');
                        current.text.push_back(L' ');
                    }
                }
                break;
            }

            case SPVA_Speak:
            case SPVA_Pronounce:
            default: {
                if (frag->ulTextLen == 0 || !frag->pTextStart) {
                    break;
                }
                if (want_sentence_events) {
                    PendingEvent event;
                    event.id = SPEI_SENTENCE_BOUNDARY;
                    event.text_offset = frag->ulTextSrcOffset;
                    event.text_length = frag->ulTextLen;
                    if (!current.text.empty()) {
                        flush();
                        current.request = request_for(settings, current_rate, current_pitch);
                        have_current = true;
                    }
                    current.events_before.push_back(std::move(event));
                }

                if (want_word_events) {
                    // Word events are timed by interpolation, not by splitting: a separate
                    // utterance per word inflates the audio by about 93% because the
                    // firmware pads every utterance, and the result is unlistenable. The
                    // offsets are approximate; the audio is right.
                    const wchar_t* text = frag->pTextStart;
                    ULONG i = 0;
                    while (i < frag->ulTextLen) {
                        while (i < frag->ulTextLen &&
                               iswspace(static_cast<wint_t>(text[i]))) {
                            current.text.push_back(text[i]);
                            ++i;
                        }
                        const ULONG start = i;
                        while (i < frag->ulTextLen &&
                               !iswspace(static_cast<wint_t>(text[i]))) {
                            ++i;
                        }
                        if (start >= i) {
                            continue;
                        }
                        ULONG word_start = start;
                        while (word_start < i && !is_word_char(text[word_start])) {
                            ++word_start;
                        }
                        ULONG word_end = i;
                        while (word_end > word_start && !is_word_char(text[word_end - 1])) {
                            --word_end;
                        }
                        if (word_start < word_end) {
                            PendingEvent event;
                            event.id = SPEI_WORD_BOUNDARY;
                            event.text_offset = frag->ulTextSrcOffset + word_start;
                            event.text_length = word_end - word_start;
                            // Recorded against the character position it starts at, so the
                            // renderer can place it proportionally within the segment.
                            event.bookmark_number =
                                static_cast<LONG>(current.text.size());
                            current.events_before.push_back(std::move(event));
                        }
                        current.text.append(text + start, i - start);
                    }
                } else {
                    current.text.append(frag->pTextStart, frag->ulTextLen);
                }
                break;
            }
        }
    }

    flush();

    // A bookmark at the very end has nothing after it; give it an empty tail segment so it
    // still fires, at the end of the stream.
    if (!pending.empty()) {
        Segment tail;
        tail.request = request_for(settings, current_rate, current_pitch);
        tail.events_before = std::move(pending);
        out.push_back(std::move(tail));
    }
}

STDMETHODIMP ISpTTSEngineImpl::SetObjectToken(ISpObjectToken* pToken)
{
    if (!pToken) {
        return E_INVALIDARG;
    }

    try {
        ISpDataKeyPtr attributes;
        if (FAILED(pToken->OpenKey(L"Attributes", &attributes)) || !attributes) {
            PROSE_LOG_E("SetObjectToken: the token has no Attributes key");
            return E_INVALIDARG;
        }

        const VoiceDesc* voice = nullptr;
        utils::out_ptr<wchar_t> value(CoTaskMemFree);

        if (SUCCEEDED(attributes->GetStringValue(kAttrVoiceIndex, value.address())) &&
            value.get()) {
            voice = find_voice_by_index(_wtoi(value.get()));
        }
        if (!voice &&
            SUCCEEDED(attributes->GetStringValue(kAttrTokenName, value.address())) &&
            value.get()) {
            voice = find_voice(value.get());
        }
        if (!voice &&
            SUCCEEDED(attributes->GetStringValue(L"Name", value.address())) && value.get()) {
            // A token written by hand, where the display name is all there is to go on.
            voice = find_voice(value.get());
        }

        if (!voice) {
            PROSE_LOG_E("SetObjectToken: could not work out which voice the token means");
            return SPERR_NOT_FOUND;
        }

        voice_ = *voice;
        have_voice_ = true;
        token_ = pToken;
        PROSE_LOG_I("voice set to '%s' (firmware voice %d, about %d Hz)",
                    log_narrow(voice_.display_name.c_str()).c_str(), voice_.firmware_voice,
                    voice_.nominal_f0);

        // Start the emulator now, off the caller's thread, so the first utterance does not
        // pay for booting the firmware.
        if (!host_) {
            host_ = std::make_unique<ProseHostClient>();
        }
        if (!warmup_.joinable()) {
            ProseHostClient* host = host_.get();
            warmup_ = std::thread([host] {
                if (FAILED(host->ensure_ready())) {
                    PROSE_LOG_W("the emulator did not pre-start; the first utterance will "
                                "start it instead");
                }
            });
        }
        return S_OK;
    }
    catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    }
    catch (...) {
        return E_UNEXPECTED;
    }
}

STDMETHODIMP ISpTTSEngineImpl::GetObjectToken(ISpObjectToken** ppToken)
{
    if (!ppToken) {
        return E_POINTER;
    }
    *ppToken = nullptr;
    if (!token_) {
        return E_UNEXPECTED;
    }
    token_.AddRef();
    *ppToken = token_.GetInterfacePtr();
    return S_OK;
}

STDMETHODIMP ISpTTSEngineImpl::GetOutputFormat(const GUID* /*pTargetFmtId*/,
                                               const WAVEFORMATEX* /*pTargetWaveFormatEx*/,
                                               GUID* pOutputFormatId,
                                               WAVEFORMATEX** ppCoMemOutputWaveFormatEx)
{
    if (!pOutputFormatId || !ppCoMemOutputWaveFormatEx) {
        return E_POINTER;
    }
    *ppCoMemOutputWaveFormatEx = nullptr;
    *pOutputFormatId = SPDFID_WaveFormatEx;

    auto* wfx = static_cast<WAVEFORMATEX*>(CoTaskMemAlloc(sizeof(WAVEFORMATEX)));
    if (!wfx) {
        return E_OUTOFMEMORY;
    }

    // The firmware is fixed at 10 kHz / 16-bit / mono and offers no choice, so this is
    // answered without starting the emulator and voice selection stays cheap.
    fill_output_format(*wfx);
    *ppCoMemOutputWaveFormatEx = wfx;
    return S_OK;
}

STDMETHODIMP ISpTTSEngineImpl::Speak(DWORD dwSpeakFlags, REFGUID /*rguidFormatId*/,
                                     const WAVEFORMATEX* /*pWaveFormatEx*/,
                                     const SPVTEXTFRAG* pTextFragList,
                                     ISpTTSEngineSite* pOutputSite)
{
    if (!pTextFragList || !pOutputSite) {
        return E_INVALIDARG;
    }
    if (!have_voice_) {
        PROSE_LOG_E("Speak called before a voice token was set");
        return SPERR_UNINITIALIZED;
    }

    try {
        if (!host_) {
            host_ = std::make_unique<ProseHostClient>();
        }
        const HRESULT ready = host_->ensure_ready();
        if (FAILED(ready)) {
            PROSE_LOG_E("the emulator is not available: %s", hresult_string(ready).c_str());
            return ready;
        }

        // Re-read on every utterance so a change made in the configuration utility is
        // audible on the very next thing spoken, with no restart.
        const Settings settings = load_settings();

        ULONGLONG event_interest = 0;
        pOutputSite->GetEventInterest(&event_interest);
        const bool want_word = (event_interest & SPFEI(SPEI_WORD_BOUNDARY)) != 0;
        const bool want_sentence = (event_interest & SPFEI(SPEI_SENTENCE_BOUNDARY)) != 0;

        USHORT site_volume = 100;
        pOutputSite->GetVolume(&site_volume);
        const int volume_percent =
            (std::max)(0, (std::min)(100, static_cast<int>(site_volume) *
                                              settings.volume_percent / 100));

        PROSE_LOG_I("Speak: flags=0x%lX volume=%u interest=0x%llX voice='%s'", dwSpeakFlags,
                    site_volume, event_interest,
                    log_narrow(voice_.display_name.c_str()).c_str());

        std::vector<Segment> segments;
        build_segments(pTextFragList, pOutputSite, settings, want_word, want_sentence,
                       segments);
        if (segments.empty()) {
            PROSE_LOG_I("Speak: nothing to say");
            return S_OK;
        }

        ULONGLONG stream_offset = 0;
        for (Segment& segment : segments) {
            const DWORD actions = pOutputSite->GetActions();
            if (actions & SPVES_ABORT) {
                break;
            }
            if (actions & SPVES_SKIP) {
                pOutputSite->CompleteSkip(0);
                break;
            }

            // Everything queued for this point is fired before a byte of it is written,
            // so the offsets are exact for bookmarks and sentence starts.
            for (const PendingEvent& event : segment.events_before) {
                if (event.id != SPEI_WORD_BOUNDARY) {
                    fire(pOutputSite, event, stream_offset);
                }
            }

            if (segment.text.empty()) {
                continue;
            }

            SiteSink sink(pOutputSite, host_.get(), volume_percent, segment.lead_silence_ms);
            segment.request.text = segment.text;
            const HRESULT hr = host_->speak(segment.request, sink);
            // Releases the tail the trimmer was holding back, so the last phoneme is not
            // left unplayed. Counted before produced is read.
            sink.finish();
            const ULONGLONG produced = sink.bytes_written();

            // Word boundaries are spread across the audio this segment actually produced,
            // in proportion to where each word started in the text.
            if (produced > 0 && !segment.text.empty()) {
                for (const PendingEvent& event : segment.events_before) {
                    if (event.id != SPEI_WORD_BOUNDARY) {
                        continue;
                    }
                    const double fraction =
                        static_cast<double>(event.bookmark_number) /
                        static_cast<double>(segment.text.size());
                    ULONGLONG offset =
                        stream_offset + static_cast<ULONGLONG>(produced * fraction);
                    offset &= ~static_cast<ULONGLONG>(1);  // keep it on a sample boundary
                    fire(pOutputSite, event, offset);
                }
            }

            stream_offset += produced;

            if (FAILED(hr)) {
                PROSE_LOG_E("segment failed: %s", hresult_string(hr).c_str());
                return hr;
            }
            if (hr == S_FALSE || sink.aborted()) {
                break;
            }
        }

        PROSE_LOG_I("Speak finished, %llu bytes", stream_offset);
        return S_OK;
    }
    catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    }
    catch (...) {
        PROSE_LOG_E("Speak threw an unexpected exception");
        return E_UNEXPECTED;
    }
}

}  // namespace sapi
}  // namespace prose
