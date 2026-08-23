#include "prose_settings.hpp"

#include <algorithm>

#include "prose_log.hpp"
#include "prose_voices.hpp"

namespace prose {

const wchar_t* const kSettingsKeyPath = L"Software\\Prose 2000 SAPI5";

namespace {

[[nodiscard]] bool read_dword(HKEY key, const wchar_t* name, int& out)
{
    DWORD value = 0;
    DWORD size = sizeof(value);
    DWORD type = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(&value), &size) !=
            ERROR_SUCCESS ||
        type != REG_DWORD) {
        return false;
    }
    out = static_cast<int>(static_cast<LONG>(value));
    return true;
}

bool write_dword(HKEY key, const wchar_t* name, int value)
{
    const DWORD stored = static_cast<DWORD>(static_cast<LONG>(value));
    return RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&stored),
                          sizeof(stored)) == ERROR_SUCCESS;
}

}  // namespace

void Settings::clamp()
{
    rate_offset = (std::max)(-10, (std::min)(10, rate_offset));
    pitch_offset = (std::max)(-10, (std::min)(10, pitch_offset));
    volume_percent = (std::max)(0, (std::min)(100, volume_percent));

    for (const ParamDesc& param : extra_parameters()) {
        auto it = extras.find(param.id);
        if (it == extras.end()) {
            continue;
        }
        it->second = (std::max)(param.min_value, (std::min)(param.max_value, it->second));
    }
}

int Settings::extra(const std::wstring& id, int fallback) const
{
    const auto it = extras.find(id);
    return it == extras.end() ? fallback : it->second;
}

Settings load_settings()
{
    Settings settings;

    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kSettingsKeyPath, 0, KEY_QUERY_VALUE, &key) !=
        ERROR_SUCCESS) {
        // No key yet: a fresh install speaks exactly like the original hardware.
        return settings;
    }

    (void)read_dword(key, L"RateOffset", settings.rate_offset);
    (void)read_dword(key, L"PitchOffset", settings.pitch_offset);
    (void)read_dword(key, L"VolumePercent", settings.volume_percent);

    for (const ParamDesc& param : extra_parameters()) {
        int value = param.default_value;
        if (read_dword(key, param.id, value)) {
            settings.extras[param.id] = value;
        }
    }

    RegCloseKey(key);
    settings.clamp();
    return settings;
}

bool save_settings(const Settings& input)
{
    Settings settings = input;
    settings.clamp();

    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKeyPath, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        PROSE_LOG_E("could not open %s for writing", log_narrow(kSettingsKeyPath).c_str());
        return false;
    }

    bool ok = true;
    ok = write_dword(key, L"RateOffset", settings.rate_offset) && ok;
    ok = write_dword(key, L"PitchOffset", settings.pitch_offset) && ok;
    ok = write_dword(key, L"VolumePercent", settings.volume_percent) && ok;

    for (const ParamDesc& param : extra_parameters()) {
        ok = write_dword(key, param.id, settings.extra(param.id, param.default_value)) && ok;
    }

    RegCloseKey(key);
    PROSE_LOG_I("settings saved: rate%+d pitch%+d volume %d%%", settings.rate_offset,
                settings.pitch_offset, settings.volume_percent);
    return ok;
}

bool reset_settings()
{
    const LONG result = RegDeleteKeyW(HKEY_CURRENT_USER, kSettingsKeyPath);
    if (result != ERROR_SUCCESS && result != ERROR_FILE_NOT_FOUND) {
        PROSE_LOG_W("could not delete %s", log_narrow(kSettingsKeyPath).c_str());
        return false;
    }
    PROSE_LOG_I("settings reset to defaults");
    return true;
}

}  // namespace prose
