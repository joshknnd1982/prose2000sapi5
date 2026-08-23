// The Prose 2000 configuration utility.
//
// Accessibility notes, because they drive the design rather than decorating it:
//
//  * Every setting is a drop-down list, never a trackbar. A Win32 trackbar reports its
//    position to MSAA as a percentage, so a control offering -10..+10 announces "+5" as
//    "seventy-five percent". A drop-down list announces the text of the item, which is
//    the thing the user actually chose.
//  * Every control is created in tab order, each immediately preceded by its own static
//    label. Screen readers take a control's name from the static that precedes it in
//    z-order, so creating them in pairs is what makes the dialog readable.
//  * Every change is written to the registry the moment it is made. The SAPI engine
//    re-reads the settings at the start of each utterance, so a change is audible on the
//    very next thing spoken - there is no Apply button to miss and nothing to save on the
//    way out.

#include <string>
#include <vector>

#include <windows.h>
#include <commctrl.h>
#include <sapi.h>
#include <comdef.h>
#include <comip.h>

#include "prose_log.hpp"
#include "prose_paths.hpp"
#include "prose_sapi_helpers.hpp"
#include "prose_settings.hpp"
#include "prose_voices.hpp"

#include "prose_config_ids.h"

namespace {

_COM_SMARTPTR_TYPEDEF(ISpVoice, __uuidof(ISpVoice));
_COM_SMARTPTR_TYPEDEF(ISpObjectToken, __uuidof(ISpObjectToken));
_COM_SMARTPTR_TYPEDEF(IEnumSpObjectTokens, __uuidof(IEnumSpObjectTokens));

constexpr int kMargin = 12;
constexpr int kLabelHeight = 16;
constexpr int kComboHeight = 200;  // includes the dropped-down list
constexpr int kRowGap = 8;
constexpr int kControlWidth = 300;
constexpr int kLabelWidth = 300;

// One row of the dialog: a static label and the drop-down it names.
struct Row {
    int control_id = 0;
    HWND label = nullptr;
    HWND combo = nullptr;
    const prose::ParamDesc* param = nullptr;  // null for the four built-in rows
};

HFONT g_font = nullptr;
std::vector<Row> g_rows;
HWND g_status = nullptr;
prose::Settings g_settings;
bool g_loading = false;

ISpVoicePtr g_voice;

[[nodiscard]] std::wstring format_offset(int value)
{
    if (value == 0) {
        return L"Normal (0)";
    }
    wchar_t buffer[64];
    swprintf_s(buffer, L"%s %d (%+d)", value < 0 ? L"Slower by" : L"Faster by",
               value < 0 ? -value : value, value);
    return buffer;
}

[[nodiscard]] std::wstring format_pitch(int value)
{
    if (value == 0) {
        return L"Normal (0)";
    }
    wchar_t buffer[64];
    swprintf_s(buffer, L"%s %d (%+d)", value < 0 ? L"Lower by" : L"Higher by",
               value < 0 ? -value : value, value);
    return buffer;
}

void set_status(const std::wstring& text)
{
    if (g_status) {
        SetWindowTextW(g_status, text.c_str());
    }
}

HWND make_label(HWND parent, const std::wstring& text, int x, int y, int width)
{
    // SS_NOTIFY keeps the label reachable by MSAA hit testing; it is not a tab stop, which
    // is what lets the screen reader treat it as the name of the control that follows.
    HWND hwnd = CreateWindowExW(0, L"STATIC", text.c_str(),
                                WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOTIFY, x, y, width,
                                kLabelHeight, parent, nullptr, nullptr, nullptr);
    SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    return hwnd;
}

HWND make_combo(HWND parent, int id, int x, int y, int width)
{
    // CBS_DROPDOWNLIST rather than CBS_DROPDOWN: the value must come from the list, and a
    // read-only combo announces cleanly instead of behaving like an edit box.
    HWND hwnd = CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", L"",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
                                    CBS_DROPDOWNLIST | CBS_HASSTRINGS,
                                x, y, width, kComboHeight, parent,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr,
                                nullptr);
    SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    return hwnd;
}

HWND make_button(HWND parent, int id, const std::wstring& text, int x, int y, int width,
                 bool default_button = false)
{
    HWND hwnd = CreateWindowExW(0, L"BUTTON", text.c_str(),
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                    (default_button ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON),
                                x, y, width, 26, parent,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr,
                                nullptr);
    SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    return hwnd;
}

[[nodiscard]] Row* row_for(int id)
{
    for (Row& row : g_rows) {
        if (row.control_id == id) {
            return &row;
        }
    }
    return nullptr;
}

void populate_from_settings()
{
    g_loading = true;
    for (Row& row : g_rows) {
        int value = 0;
        if (row.param) {
            value = g_settings.extra(row.param->id, row.param->default_value);
        } else if (row.control_id == IDC_RATE) {
            value = g_settings.rate_offset;
        } else if (row.control_id == IDC_PITCH) {
            value = g_settings.pitch_offset;
        } else if (row.control_id == IDC_VOLUME) {
            value = g_settings.volume_percent;
        } else if (row.control_id == IDC_VOICE) {
            value = 0;  // preview voice is not persisted
        }

        const int count = static_cast<int>(SendMessageW(row.combo, CB_GETCOUNT, 0, 0));
        for (int i = 0; i < count; ++i) {
            const int item =
                static_cast<int>(SendMessageW(row.combo, CB_GETITEMDATA, i, 0));
            if (item == value) {
                SendMessageW(row.combo, CB_SETCURSEL, i, 0);
                break;
            }
        }
    }
    g_loading = false;
}

void save_now()
{
    if (prose::save_settings(g_settings)) {
        set_status(L"Saved. The change applies to the next thing spoken.");
    } else {
        set_status(L"Could not save the settings. See the log for details.");
    }
}

void on_selection_changed(int id)
{
    if (g_loading) {
        return;
    }
    Row* row = row_for(id);
    if (!row) {
        return;
    }
    const int index = static_cast<int>(SendMessageW(row->combo, CB_GETCURSEL, 0, 0));
    if (index == CB_ERR) {
        return;
    }
    const int value = static_cast<int>(SendMessageW(row->combo, CB_GETITEMDATA, index, 0));

    if (row->param) {
        g_settings.extras[row->param->id] = value;
    } else if (id == IDC_RATE) {
        g_settings.rate_offset = value;
    } else if (id == IDC_PITCH) {
        g_settings.pitch_offset = value;
    } else if (id == IDC_VOLUME) {
        g_settings.volume_percent = value;
    } else if (id == IDC_VOICE) {
        set_status(L"Preview voice selected.");
        return;  // not a persisted setting
    }
    save_now();
}

[[nodiscard]] int selected_voice()
{
    Row* row = row_for(IDC_VOICE);
    if (!row) {
        return 0;
    }
    const int index = static_cast<int>(SendMessageW(row->combo, CB_GETCURSEL, 0, 0));
    if (index == CB_ERR) {
        return 0;
    }
    return static_cast<int>(SendMessageW(row->combo, CB_GETITEMDATA, index, 0));
}

// Speaks a sample through the registered SAPI 5 stack, which is what the settings actually
// affect. Asynchronous so the dialog stays responsive and a second press interrupts.
void speak_sample()
{
    const prose::VoiceDesc* voice = prose::find_voice_by_index(selected_voice());
    if (!voice) {
        set_status(L"No such voice.");
        return;
    }

    if (!g_voice) {
        const HRESULT hr = g_voice.CreateInstance(CLSID_SpVoice);
        if (FAILED(hr) || !g_voice) {
            set_status(L"Speech is not available. Is the Prose 2000 voice installed?");
            PROSE_LOG_E("CreateInstance(SpVoice) failed %s",
                        prose::hresult_string(hr).c_str());
            return;
        }
    }

    // Find our token by name so the preview uses the voice chosen above rather than
    // whatever the system default happens to be.
    IEnumSpObjectTokensPtr tokens;
    if (SUCCEEDED(prose::sapi::enum_voice_tokens(&tokens)) && tokens) {
        ULONG count = 0;
        tokens->GetCount(&count);
        for (ULONG i = 0; i < count; ++i) {
            ISpObjectTokenPtr token;
            if (FAILED(tokens->Item(i, &token)) || !token) {
                continue;
            }
            std::wstring description;
            if (SUCCEEDED(prose::sapi::token_description(token, description)) &&
                voice->display_name == description) {
                g_voice->SetVoice(token);
                break;
            }
        }
    }

    const std::wstring sample =
        L"This is " + voice->display_name +
        L", speaking with your current settings. The quick brown fox jumps over the lazy dog.";

    g_voice->Speak(nullptr, SPF_PURGEBEFORESPEAK, nullptr);
    const HRESULT hr = g_voice->Speak(sample.c_str(), SPF_ASYNC | SPF_IS_NOT_XML, nullptr);
    if (FAILED(hr)) {
        set_status(L"The sample could not be spoken. See the log for details.");
        PROSE_LOG_E("ISpVoice::Speak failed %s", prose::hresult_string(hr).c_str());
    } else {
        set_status(L"Speaking a sample.");
    }
}

void restore_defaults(HWND dialog)
{
    prose::reset_settings();
    g_settings = prose::Settings{};
    populate_from_settings();
    set_status(L"All settings restored to their defaults.");
    // Move focus back to the first setting so a screen reader user is not left on a button
    // whose effect has already happened.
    if (!g_rows.empty()) {
        SetFocus(g_rows.front().combo);
    }
    (void)dialog;
}

void build_controls(HWND dialog)
{
    NONCLIENTMETRICSW metrics = {};
    metrics.cbSize = sizeof(metrics);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0);
    g_font = CreateFontIndirectW(&metrics.lfMessageFont);

    int y = kMargin;
    const int x = kMargin;

    const auto add_row = [&](int id, const std::wstring& label,
                             const prose::ParamDesc* param) -> Row& {
        Row row;
        row.control_id = id;
        row.param = param;
        row.label = make_label(dialog, label, x, y, kLabelWidth);
        y += kLabelHeight + 2;
        row.combo = make_combo(dialog, id, x, y, kControlWidth);
        y += 26 + kRowGap;
        g_rows.push_back(row);
        return g_rows.back();
    };

    // ---- preview voice -------------------------------------------------------------
    {
        Row& row = add_row(IDC_VOICE, L"&Voice to use when testing:", nullptr);
        for (const prose::VoiceDesc& voice : prose::voice_catalogue()) {
            const int index = static_cast<int>(SendMessageW(
                row.combo, CB_ADDSTRING, 0,
                reinterpret_cast<LPARAM>(voice.display_name.c_str())));
            SendMessageW(row.combo, CB_SETITEMDATA, index, voice.firmware_voice);
        }
    }

    // ---- rate ----------------------------------------------------------------------
    {
        Row& row = add_row(IDC_RATE, L"&Rate adjustment:", nullptr);
        for (int value = -10; value <= 10; ++value) {
            const std::wstring text = format_offset(value);
            const int index = static_cast<int>(SendMessageW(
                row.combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str())));
            SendMessageW(row.combo, CB_SETITEMDATA, index, value);
        }
    }

    // ---- pitch ---------------------------------------------------------------------
    {
        Row& row = add_row(IDC_PITCH, L"&Pitch adjustment:", nullptr);
        for (int value = -10; value <= 10; ++value) {
            const std::wstring text = format_pitch(value);
            const int index = static_cast<int>(SendMessageW(
                row.combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str())));
            SendMessageW(row.combo, CB_SETITEMDATA, index, value);
        }
    }

    // ---- volume --------------------------------------------------------------------
    {
        Row& row = add_row(IDC_VOLUME, L"Vol&ume:", nullptr);
        for (int value = 100; value >= 0; value -= 5) {
            wchar_t text[32];
            swprintf_s(text, L"%d percent", value);
            const int index = static_cast<int>(
                SendMessageW(row.combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text)));
            SendMessageW(row.combo, CB_SETITEMDATA, index, value);
        }
    }

    // ---- the firmware extras, straight from the catalogue --------------------------
    int next_id = IDC_EXTRA_FIRST;
    for (const prose::ParamDesc& param : prose::extra_parameters()) {
        std::wstring label = param.label;
        label += L" (firmware ";
        label.push_back(param.command);
        label += L"):";

        Row& row = add_row(next_id++, label, &param);
        for (int value = param.min_value; value <= param.max_value; value += param.step) {
            std::wstring text;
            if (param.min_value == 0 && param.max_value == 1) {
                text = value ? L"On (1)" : L"Off (0)";
            } else if (value == param.default_value) {
                text = std::to_wstring(value) + L" (default)";
            } else {
                text = std::to_wstring(value);
            }
            const int index = static_cast<int>(SendMessageW(
                row.combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str())));
            SendMessageW(row.combo, CB_SETITEMDATA, index, value);
        }
    }

    // ---- buttons -------------------------------------------------------------------
    y += 4;
    const int button_width = 140;
    make_button(dialog, IDC_TEST, L"&Test voice", x, y, button_width, true);
    make_button(dialog, IDC_DEFAULTS, L"Restore &defaults", x + button_width + 10, y,
                button_width);
    y += 26 + kRowGap;
    make_button(dialog, IDCANCEL, L"&Close", x, y, button_width);
    y += 26 + kRowGap;

    // ---- status line ---------------------------------------------------------------
    g_status = make_label(dialog, L"Ready.", x, y, kLabelWidth);
    y += kLabelHeight + kMargin;

    // Size the window around what was built.
    RECT client = {0, 0, kControlWidth + kMargin * 2, y};
    AdjustWindowRectEx(&client, static_cast<DWORD>(GetWindowLongPtrW(dialog, GWL_STYLE)),
                       FALSE, static_cast<DWORD>(GetWindowLongPtrW(dialog, GWL_EXSTYLE)));
    SetWindowPos(dialog, nullptr, 0, 0, client.right - client.left,
                 client.bottom - client.top, SWP_NOMOVE | SWP_NOZORDER);

    // Centre on the work area.
    RECT work = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    RECT window = {};
    GetWindowRect(dialog, &window);
    SetWindowPos(dialog, nullptr,
                 work.left + ((work.right - work.left) - (window.right - window.left)) / 2,
                 work.top + ((work.bottom - work.top) - (window.bottom - window.top)) / 2, 0,
                 0, SWP_NOSIZE | SWP_NOZORDER);
}

INT_PTR CALLBACK dialog_proc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message) {
        case WM_INITDIALOG: {
            g_settings = prose::load_settings();
            build_controls(dialog);
            populate_from_settings();

            std::wstring missing;
            if (!prose::engine_files_present(&missing)) {
                set_status(L"Warning: the Prose 2000 engine files were not found.");
                PROSE_LOG_E("engine files missing: %s",
                            prose::log_narrow(missing.c_str()).c_str());
            }
            if (!g_rows.empty()) {
                SetFocus(g_rows.front().combo);
            }
            return FALSE;  // focus was set here
        }

        case WM_COMMAND: {
            const int id = LOWORD(wparam);
            const int code = HIWORD(wparam);

            if (code == CBN_SELCHANGE) {
                on_selection_changed(id);
                return TRUE;
            }
            switch (id) {
                case IDC_TEST:
                    speak_sample();
                    return TRUE;
                case IDC_DEFAULTS:
                    restore_defaults(dialog);
                    return TRUE;
                case IDCANCEL:
                    EndDialog(dialog, 0);
                    return TRUE;
                default:
                    break;
            }
            break;
        }

        case WM_CLOSE:
            EndDialog(dialog, 0);
            return TRUE;

        case WM_DESTROY:
            if (g_voice) {
                g_voice->Speak(nullptr, SPF_PURGEBEFORESPEAK, nullptr);
                g_voice = nullptr;
            }
            if (g_font) {
                DeleteObject(g_font);
                g_font = nullptr;
            }
            return TRUE;

        default:
            break;
    }
    return FALSE;
}

}  // namespace

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int)
{
    prose::log_init(L"config");
    PROSE_LOG_I("configuration utility starting");

    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(co)) {
        MessageBoxW(nullptr, L"COM could not be started.", L"Prose 2000", MB_ICONERROR | MB_OK);
        return 1;
    }

    INITCOMMONCONTROLSEX controls = {sizeof(controls), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);

    DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_CONFIG), nullptr, dialog_proc, 0);

    PROSE_LOG_I("configuration utility closing");
    CoUninitialize();
    prose::log_shutdown();
    return 0;
}
