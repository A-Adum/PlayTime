#pragma once
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/Model.h"

namespace pt {

// Installed-game discovery from local launcher metadata only: no network, no credentials.
// Supported: Steam (libraryfolders.vdf + appmanifest_*.acf), Epic Games (Manifests\*.item),
// GOG Galaxy (registry), Ubisoft Connect (registry), Xbox app (<drive>:\XboxGames\*\Content).
std::vector<DiscoveredGame> discoverInstalledGames();

std::vector<DiscoveredGame> discoverSteam();
std::vector<DiscoveredGame> discoverEpic();
std::vector<DiscoveredGame> discoverGog();
std::vector<DiscoveredGame> discoverUbisoft();
std::vector<DiscoveredGame> discoverXbox();

// Steam install root from HKCU\Software\Valve\Steam\SteamPath (normalized backslashes), empty if absent.
std::wstring steamRoot();
// Every steamapps library folder listed in <steamRoot>\steamapps\libraryfolders.vdf (plus the root itself).
std::vector<std::wstring> steamLibraryFolders(const std::wstring& steamRoot);
// Installed games of one Steam library folder (parses appmanifest_*.acf). Skips tools/redistributables.
std::vector<DiscoveredGame> parseSteamLibrary(const std::wstring& libraryFolder, const std::wstring& steamRoot);
// Portrait artwork from Steam's local library cache, empty if none is cached.
std::wstring steamCoverPath(const std::wstring& steamRoot, const std::wstring& appId);
// Parses one Epic *.item manifest. Returns nullopt for incomplete installs, DLC and non-game entries.
std::optional<DiscoveredGame> parseEpicManifest(std::string_view json);

// Removes trademark symbols and surrounding whitespace from launcher display names.
std::wstring cleanGameName(std::wstring_view name);

}  // namespace pt
