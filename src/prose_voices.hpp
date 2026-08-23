// The Prose 2000 voice catalogue and the firmware parameters the engine exposes.
//
// Everything here was established by measurement against the emulated firmware rather
// than from documentation - the NVDA add-on this project builds on ships only four of the
// firmware's seventeen commands and documents a single voice. See docs/PARAMETERS.md.

#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include <windows.h>
#include <objbase.h>

namespace prose {

// ---------------------------------------------------------------------------------------
// Voices. ESC[<n>V selects one; values above 2 clamp to voice 2. All three share the same
// letter-to-sound rules and therefore the same duration - only the vocal tract changes.
// ---------------------------------------------------------------------------------------
struct VoiceDesc {
    int firmware_voice = 0;      // the <n> in ESC[<n>V
    std::wstring token_name;     // registry-ish token id, no spaces
    std::wstring display_name;   // what a SAPI voice list shows
    std::wstring description;
    int nominal_f0 = 0;          // measured fundamental, for the log and the config UI

    [[nodiscard]] GUID mode_guid() const;
    [[nodiscard]] std::wstring sapi_language_attribute() const { return L"409;9"; }
    [[nodiscard]] LANGID sapi_lcid() const { return 0x0409; }  // en-US
    [[nodiscard]] std::wstring sapi_gender() const { return L"Male"; }
    [[nodiscard]] std::wstring sapi_age() const { return L"Adult"; }
};

[[nodiscard]] const std::vector<VoiceDesc>& voice_catalogue();

// Looks a voice up by token name, display name, or firmware index rendered as text.
[[nodiscard]] const VoiceDesc* find_voice(const std::wstring& name);
[[nodiscard]] const VoiceDesc* find_voice_by_index(int firmware_voice);

// ---------------------------------------------------------------------------------------
// Firmware parameters beyond rate/pitch/volume.
//
// Each of these was swept across 0..250 against the emulator and kept only where the
// effect was reproducible and safe. Commands t and L can hang the firmware past its
// utterance time limit, N emits a fixed 3.2s regardless of the text, and C, l and x do
// nothing measurable - none of them are exposed.
// ---------------------------------------------------------------------------------------
struct ParamDesc {
    const wchar_t* id;         // stable key used in the registry and on the command line
    const wchar_t* label;      // shown in the configuration utility
    const wchar_t* help;       // what the measurement showed it does
    wchar_t command;           // the firmware command letter
    int min_value;
    int max_value;
    int default_value;
    int step;                  // increment used by the configuration utility
};

[[nodiscard]] const std::vector<ParamDesc>& extra_parameters();
[[nodiscard]] const ParamDesc* find_parameter(const std::wstring& id);

// ---------------------------------------------------------------------------------------
// The three native controls SAPI maps onto directly.
// ---------------------------------------------------------------------------------------
inline constexpr int kRateMin = 50;       // words per minute
inline constexpr int kRateDefault = 150;
inline constexpr int kRateMax = 250;

inline constexpr int kPitchMin = 50;      // firmware units
inline constexpr int kPitchDefault = 85;
inline constexpr int kPitchMax = 200;

inline constexpr int kAttenMin = 0;       // 0 is loudest, 15 is quietest
inline constexpr int kAttenMax = 15;

// Maps a SAPI rate (-10..+10) onto the firmware's words-per-minute scale, keeping the
// firmware default at SAPI 0.
[[nodiscard]] int sapi_rate_to_firmware(int sapi_rate);

// Maps a SAPI pitch adjustment (-10..+10) onto the firmware's pitch scale, keeping the
// firmware default at SAPI 0.
[[nodiscard]] int sapi_pitch_to_firmware(int sapi_pitch);

// Maps a SAPI volume percentage (0..100) onto the firmware's inverted attenuation.
[[nodiscard]] int sapi_volume_to_firmware(int percent);

}  // namespace prose
