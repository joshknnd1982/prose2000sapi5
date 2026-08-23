#include "prose_voices.hpp"

#include <algorithm>
#include <cmath>

namespace prose {
namespace {

// One stable mode GUID per voice. SAPI hands the token back to the engine, and the mode
// tells the engine which firmware voice the token meant without parsing display names.
// Generated once and fixed: changing them would orphan any saved voice selection.
constexpr GUID kModeGuids[] = {
    {0x7c1e5a20, 0x9d34, 0x4f61, {0xb2, 0x0a, 0x51, 0xc8, 0x3e, 0x77, 0x10, 0x01}},
    {0x7c1e5a20, 0x9d34, 0x4f61, {0xb2, 0x0a, 0x51, 0xc8, 0x3e, 0x77, 0x10, 0x02}},
    {0x7c1e5a20, 0x9d34, 0x4f61, {0xb2, 0x0a, 0x51, 0xc8, 0x3e, 0x77, 0x10, 0x03}},
};

[[nodiscard]] bool iequals(const std::wstring& a, const std::wstring& b)
{
    return _wcsicmp(a.c_str(), b.c_str()) == 0;
}

}  // namespace

GUID VoiceDesc::mode_guid() const
{
    const std::size_t index = static_cast<std::size_t>(firmware_voice);
    return index < std::size(kModeGuids) ? kModeGuids[index] : kModeGuids[0];
}

const std::vector<VoiceDesc>& voice_catalogue()
{
    // Built once. The names keep the default voice called plainly "Prose 2000" so an
    // existing SAPI voice selection naming it keeps working.
    static const std::vector<VoiceDesc> catalogue = {
        {0, L"Prose2000", L"Prose 2000",
         L"The default Prose 2000 voice, as shipped on the original hardware.", 130},
        {1, L"Prose2000Deep", L"Prose 2000 Deep",
         L"A lower, heavier variant of the Prose 2000 voice.", 105},
        {2, L"Prose2000High", L"Prose 2000 High",
         L"A higher, lighter variant of the Prose 2000 voice.", 170},
    };
    return catalogue;
}

const VoiceDesc* find_voice(const std::wstring& name)
{
    if (name.empty()) {
        return nullptr;
    }
    for (const VoiceDesc& voice : voice_catalogue()) {
        if (iequals(name, voice.token_name) || iequals(name, voice.display_name)) {
            return &voice;
        }
    }
    // A bare number is accepted so the command-line tools can say "-voice 2".
    wchar_t* end = nullptr;
    const long index = wcstol(name.c_str(), &end, 10);
    if (end && *end == L'\0') {
        return find_voice_by_index(static_cast<int>(index));
    }
    return nullptr;
}

const VoiceDesc* find_voice_by_index(int firmware_voice)
{
    for (const VoiceDesc& voice : voice_catalogue()) {
        if (voice.firmware_voice == firmware_voice) {
            return &voice;
        }
    }
    return nullptr;
}

const std::vector<ParamDesc>& extra_parameters()
{
    // Ranges and defaults are the measured-safe ones. "default_value" is the value that
    // reproduces the firmware's untouched behaviour, so a fresh install sounds exactly
    // like the original hardware until the user changes something.
    static const std::vector<ParamDesc> params = {
        {L"expression", L"Expression",
         L"Widens the intonation contour and lowers the fundamental as it rises. "
         L"0 leaves the firmware untouched; 250 is nearly monotone and much slower.",
         L'g', 0, 250, 0, 5},
        {L"phrasing", L"Phrasing",
         L"Lengthens phrase boundaries without changing the fundamental. "
         L"0 leaves the firmware untouched; 250 adds roughly half again to the duration.",
         L's', 0, 250, 0, 5},
        {L"softness", L"Softness",
         L"A timbre toggle. 1 gives a slightly softer delivery than the default 0.",
         L'i', 0, 1, 0, 1},
        // ESC[NI ("tone") is deliberately absent. It sounds fine on the utterance that
        // sets it and then returns nothing at all for every second utterance afterwards,
        // and no combination of following commands clears it - only restarting the
        // firmware does. A control that can silence a screen reader is not worth having.
        // tools/prose_safety.py is the test that caught it; run it before adding any more.
        {L"emphasis", L"Emphasis",
         L"A toggle. 0 slows the delivery and lowers the fundamental; 1 is the default.",
         L'P', 0, 1, 1, 1},
    };
    return params;
}

const ParamDesc* find_parameter(const std::wstring& id)
{
    for (const ParamDesc& param : extra_parameters()) {
        if (iequals(id, param.id)) {
            return &param;
        }
    }
    return nullptr;
}

int sapi_rate_to_firmware(int sapi_rate)
{
    sapi_rate = (std::max)(-10, (std::min)(10, sapi_rate));
    // SAPI's rate is logarithmic: +10 is about three times normal, -10 about a third.
    // The firmware's 50..250 span is narrower than that, so the result is clamped.
    const double scaled = kRateDefault * std::pow(3.0, sapi_rate / 10.0);
    return (std::max)(kRateMin, (std::min)(kRateMax, static_cast<int>(scaled + 0.5)));
}

int sapi_pitch_to_firmware(int sapi_pitch)
{
    sapi_pitch = (std::max)(-10, (std::min)(10, sapi_pitch));
    // An octave either way about the firmware default, clamped to what it accepts. The
    // two halves are scaled separately because the default sits low in the range.
    if (sapi_pitch <= 0) {
        const double span = kPitchDefault - kPitchMin;
        return kPitchDefault + static_cast<int>(span * sapi_pitch / 10.0 - 0.5);
    }
    const double span = kPitchMax - kPitchDefault;
    return kPitchDefault + static_cast<int>(span * sapi_pitch / 10.0 + 0.5);
}

int sapi_volume_to_firmware(int percent)
{
    percent = (std::max)(0, (std::min)(100, percent));
    // Attenuation runs backwards: 0 is full level and 15 is quietest.
    return static_cast<int>((100 - percent) * kAttenMax / 100.0 + 0.5);
}

}  // namespace prose
