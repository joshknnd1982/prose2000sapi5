// Locating the emulator and its firmware, without the registry.
//
// Every path is derived from the location of the calling binary. The layout the installer
// produces is:
//
//   <root>\Prose2000SAPI5.dll       32-bit SAPI 5 engine
//   <root>\x64\Prose2000SAPI5.dll   64-bit SAPI 5 engine
//   <root>\ProseHost.exe            the emulator, 64-bit, spawned by both engines
//   <root>\roms\                    the six firmware images
//   <root>\Prose2000Config.exe      the configuration utility
//
// so the 64-bit DLL has to look one directory up. PROSE2000_DATA_DIR overrides the search
// entirely, which is what the renderer and the test harness use out of a build tree.

#pragma once

#include <string>
#include <vector>

#include <windows.h>

namespace prose {

// The six firmware images the emulator needs, in the order the host expects them.
[[nodiscard]] const std::vector<std::wstring>& rom_names();

// Directory containing the given module, without a trailing backslash. Pass nullptr for
// the running executable.
[[nodiscard]] std::wstring module_directory(HMODULE module);
[[nodiscard]] std::wstring own_module_directory();

// The directory holding ProseHost.exe and roms\. Empty if it could not be found.
// Resolved once and cached.
[[nodiscard]] const std::wstring& data_root();
void set_data_root(const std::wstring& root);

[[nodiscard]] std::wstring host_exe_path();   // <root>\ProseHost.exe
[[nodiscard]] std::wstring rom_dir();         // <root>\roms
[[nodiscard]] std::wstring config_exe_path(); // <root>\Prose2000Config.exe

// True when the host and all six ROMs are present. Reports the first missing file.
[[nodiscard]] bool engine_files_present(std::wstring* missing = nullptr);

[[nodiscard]] bool file_exists(const std::wstring& path);

}  // namespace prose
