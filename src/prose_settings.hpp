// User settings shared by the SAPI 5 engine and the configuration utility.
//
// Stored per user under HKCU so the utility needs no administrator rights, and re-read by
// the engine at the start of every utterance so a change made in the utility is audible on
// the very next thing spoken rather than after a restart.

#pragma once

#include <map>
#include <string>

#include <windows.h>

namespace prose {

// HKCU\Software\Prose 2000 SAPI5
extern const wchar_t* const kSettingsKeyPath;

struct Settings {
    // Offsets applied on top of whatever SAPI itself asks for, so the utility acts like a
    // trim control rather than fighting the application's own rate and volume sliders.
    int rate_offset = 0;      // -10..+10, in SAPI rate units
    int pitch_offset = 0;     // -10..+10, in SAPI pitch units
    int volume_percent = 100; // 0..100, scales the application's volume

    // Firmware extras, keyed by ParamDesc::id. Absent entries mean "use the default".
    std::map<std::wstring, int> extras;

    // Values are clamped to the ranges the firmware actually accepts.
    void clamp();

    [[nodiscard]] int extra(const std::wstring& id, int fallback) const;
};

// Reads the current settings. Never throws; missing or malformed values fall back to the
// defaults, so a corrupted key degrades to stock behaviour rather than silence.
[[nodiscard]] Settings load_settings();

// Writes the settings back. Returns false if the key could not be written.
bool save_settings(const Settings& settings);

// Removes every stored value, returning the engine to its defaults.
bool reset_settings();

}  // namespace prose
