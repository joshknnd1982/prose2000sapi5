#include "prose_paths.hpp"

#include <mutex>

#include "prose_log.hpp"

namespace prose {
namespace {

std::mutex g_root_mutex;
std::wstring g_root;
bool g_root_resolved = false;

[[nodiscard]] std::wstring strip_trailing_slash(std::wstring s)
{
    while (!s.empty() && (s.back() == L'\\' || s.back() == L'/')) {
        s.pop_back();
    }
    return s;
}

[[nodiscard]] std::wstring parent_of(const std::wstring& dir)
{
    const std::size_t pos = dir.find_last_of(L'\\');
    if (pos == std::wstring::npos || pos < 2) {
        return {};
    }
    return dir.substr(0, pos);
}

[[nodiscard]] bool looks_like_data_root(const std::wstring& dir)
{
    if (dir.empty()) {
        return false;
    }
    if (!file_exists(dir + L"\\ProseHost.exe")) {
        return false;
    }
    // One ROM is enough to tell a real install from a directory that merely happens to
    // contain a similarly named executable; the full check lives in engine_files_present.
    return file_exists(dir + L"\\roms\\" + rom_names().front());
}

[[nodiscard]] std::wstring env_value(const wchar_t* name)
{
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = GetEnvironmentVariableW(name, buf, static_cast<DWORD>(std::size(buf)));
    if (n == 0 || n >= std::size(buf)) {
        return {};
    }
    return strip_trailing_slash(std::wstring(buf, n));
}

void resolve_root()
{
    const std::wstring from_env = env_value(L"PROSE2000_DATA_DIR");
    if (!from_env.empty()) {
        g_root = from_env;
        PROSE_LOG_I("data root from PROSE2000_DATA_DIR: %s", log_narrow(g_root.c_str()).c_str());
        if (!looks_like_data_root(g_root)) {
            PROSE_LOG_W("PROSE2000_DATA_DIR does not contain ProseHost.exe and roms\\");
        }
        return;
    }

    const std::wstring here = own_module_directory();
    std::wstring candidate = here;
    for (int depth = 0; depth < 3 && !candidate.empty(); ++depth) {
        if (looks_like_data_root(candidate)) {
            g_root = candidate;
            PROSE_LOG_I("data root resolved to %s", log_narrow(g_root.c_str()).c_str());
            return;
        }
        candidate = parent_of(candidate);
    }

    PROSE_LOG_E("could not find ProseHost.exe near %s", log_narrow(here.c_str()).c_str());
    g_root.clear();
}

}  // namespace

const std::vector<std::wstring>& rom_names()
{
    static const std::vector<std::wstring> names = {
        L"v3.4.1__2000__2.u22",
        L"v3.4.1__2000__3.u45",
        L"v3.4.1__2000__0.u21",
        L"v3.4.1__2000__1.u44",
        L"v3.12__8-9-88__dsp_prog.u29",
        L"v3.12__8-9-88__dsp_data.u29",
    };
    return names;
}

bool file_exists(const std::wstring& path)
{
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring module_directory(HMODULE module)
{
    std::vector<wchar_t> buf(MAX_PATH);
    for (;;) {
        const DWORD n = GetModuleFileNameW(module, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) {
            return {};
        }
        if (n < buf.size() - 1) {
            break;
        }
        buf.resize(buf.size() * 2);
    }
    std::wstring path(buf.data());
    const std::size_t pos = path.find_last_of(L'\\');
    return pos == std::wstring::npos ? std::wstring() : path.substr(0, pos);
}

std::wstring own_module_directory()
{
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&own_module_directory), &self)) {
        self = nullptr;
    }
    return module_directory(self);
}

const std::wstring& data_root()
{
    std::lock_guard<std::mutex> lock(g_root_mutex);
    if (!g_root_resolved) {
        resolve_root();
        g_root_resolved = true;
    }
    return g_root;
}

void set_data_root(const std::wstring& root)
{
    {
        std::lock_guard<std::mutex> lock(g_root_mutex);
        g_root = strip_trailing_slash(root);
        g_root_resolved = true;
    }
    PROSE_LOG_I("data root overridden to %s", log_narrow(g_root.c_str()).c_str());
}

std::wstring host_exe_path()
{
    const std::wstring& root = data_root();
    return root.empty() ? std::wstring() : root + L"\\ProseHost.exe";
}

std::wstring rom_dir()
{
    const std::wstring& root = data_root();
    return root.empty() ? std::wstring() : root + L"\\roms";
}

std::wstring config_exe_path()
{
    const std::wstring& root = data_root();
    return root.empty() ? std::wstring() : root + L"\\Prose2000Config.exe";
}

bool engine_files_present(std::wstring* missing)
{
    const std::wstring host = host_exe_path();
    if (host.empty() || !file_exists(host)) {
        if (missing) {
            *missing = host.empty() ? L"ProseHost.exe" : host;
        }
        return false;
    }
    const std::wstring roms = rom_dir();
    for (const std::wstring& name : rom_names()) {
        const std::wstring path = roms + L"\\" + name;
        if (!file_exists(path)) {
            if (missing) {
                *missing = path;
            }
            return false;
        }
    }
    return true;
}

}  // namespace prose
