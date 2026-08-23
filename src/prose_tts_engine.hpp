// The SAPI 5 engine object: ISpTTSEngine plus ISpObjectWithToken.
//
// Identical source for both architectures, because the emulator is out of process either
// way. The firmware has no index-mark facility at all, so bookmarks and boundary events
// are recovered by splitting the utterance rather than by asking the engine where it got
// to - see the note above Speak().

#pragma once

#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>
#include <sapi.h>
#include <sapiddk.h>
#include <sperror.h>
#include <comdef.h>
#include <comip.h>

#include "prose_com.hpp"
#include "prose_host.hpp"
#include "prose_settings.hpp"
#include "prose_token.hpp"
#include "prose_voices.hpp"

namespace prose {
namespace sapi {

class __declspec(uuid("9f2d4c61-08b3-4a77-bd15-6c4e39a25c88")) ISpTTSEngineImpl :
    public ISpTTSEngine, public ISpObjectWithToken
{
public:
    ISpTTSEngineImpl();
    ~ISpTTSEngineImpl();

    ISpTTSEngineImpl(const ISpTTSEngineImpl&) = delete;
    ISpTTSEngineImpl& operator=(const ISpTTSEngineImpl&) = delete;

    STDMETHOD(Speak)(DWORD dwSpeakFlags, REFGUID rguidFormatId,
                     const WAVEFORMATEX* pWaveFormatEx, const SPVTEXTFRAG* pTextFragList,
                     ISpTTSEngineSite* pOutputSite) override;
    STDMETHOD(GetOutputFormat)(const GUID* pTargetFmtId, const WAVEFORMATEX* pTargetWaveFormatEx,
                               GUID* pOutputFormatId,
                               WAVEFORMATEX** ppCoMemOutputWaveFormatEx) override;

    STDMETHOD(SetObjectToken)(ISpObjectToken* pToken) override;
    STDMETHOD(GetObjectToken)(ISpObjectToken** ppToken) override;

protected:
    [[nodiscard]] void* get_interface(REFIID riid) noexcept
    {
        void* ptr = com::try_primary_interface<ISpTTSEngine>(this, riid);
        return ptr ? ptr : com::try_interface<ISpObjectWithToken>(this, riid);
    }

private:
    _COM_SMARTPTR_TYPEDEF(ISpObjectToken, __uuidof(ISpObjectToken));
    _COM_SMARTPTR_TYPEDEF(ISpDataKey, __uuidof(ISpDataKey));

    // A pending SAPI event, held until the audio it belongs to has been written and its
    // stream offset is therefore known.
    struct PendingEvent {
        SPEVENTENUM id = SPEI_WORD_BOUNDARY;
        std::wstring bookmark_text;
        LONG bookmark_number = 0;
        ULONG text_offset = 0;
        ULONG text_length = 0;
    };

    // One stretch of text spoken with a single set of firmware settings. A new segment is
    // started whenever the prosody changes or an event has to be timed.
    struct Segment {
        std::wstring text;
        SpeakRequest request;
        std::vector<PendingEvent> events_before;  // fire at this segment's start offset
        // Silence the caller explicitly asked for before any text in this segment. The
        // firmware's own padding is trimmed away, so this has to be put back deliberately.
        unsigned lead_silence_ms = 0;
    };

    void build_segments(const SPVTEXTFRAG* frags, ISpTTSEngineSite* site,
                        const Settings& settings, bool want_word_events,
                        bool want_sentence_events, std::vector<Segment>& out);
    [[nodiscard]] SpeakRequest request_for(const Settings& settings, int sapi_rate,
                                           int sapi_pitch) const;
    void fire(ISpTTSEngineSite* site, const PendingEvent& event, ULONGLONG offset);

    ISpObjectTokenPtr token_;
    VoiceDesc voice_;
    bool have_voice_ = false;

    std::unique_ptr<ProseHostClient> host_;

    // Booting the firmware takes about 90 ms. That is small, but it lands on the very
    // first thing the user hears, so it is done in the background as soon as a voice is
    // chosen rather than on the critical path of the first utterance.
    std::thread warmup_;
};

}  // namespace sapi
}  // namespace prose
