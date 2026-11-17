#include "platform/LauncherDiscovery.h"

#include <windows.h>
#include <shlobj.h>
#include <knownfolders.h>

#include <algorithm>
#include <cwctype>
#include <iterator>
#include <string>
#include <vector>

#include "platform/WinUtil.h"
#include "util/Json.h"
#include "util/Str.h"
#include "util/Vdf.h"

namespace pt {

namespace {

void normalizeSlashes(std::wstring& path) {
    for (wchar_t& c : path) {
        if (c == L'/') c = L'\\';
    }
    while (path.size() > 3 && path.back() == L'\\') {
        path.pop_back();
    }
}

}  // namespace

std::wstring cleanGameName(std::wstring_view name) {
    std::wstring buf;
    buf.reserve(name.size());
    for (wchar_t c : name) {
        if (c == L'\u00AE' || c == L'\u2122' || c == L'\u00A9') continue;
        buf.push_back(c);
    }
    std::wstring t = trim(buf);
    std::wstring out;
    out.reserve(t.size());
    bool space = false;
    for (wchar_t c : t) {
        if (std::iswspace(c)) {
            if (!space) {
                out.push_back(L' ');
                space = true;
            }
        } else {
            out.push_back(c);
            space = false;
        }
    }
    return out;
}

std::wstring steamRoot() {
    auto val = regString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath");
    if (!val || val->empty()) return {};
    std::wstring root = *val;
    normalizeSlashes(root);
    return root;
}

std::wstring steamCoverPath(const std::wstring& steamRoot, const std::wstring& appId) {
    if (steamRoot.empty() || appId.empty()) return {};
    std::wstring cacheDir = steamRoot + L"\\appcache\\librarycache";
    std::wstring appDir = cacheDir + L"\\" + appId;

    // Portrait art first (directly in <appid>\ or one level down in hashed folders), then the legacy flat file,
    // then the landscape header as a last resort (the UI crops it).
    if (dirExists(appDir)) {
        auto subdirs = listDir(appDir, L"*", true);
        for (const wchar_t* name : {L"library_600x900.jpg", L"library_capsule.jpg"}) {
            std::wstring direct = appDir + L"\\" + name;
            if (fileExists(direct)) return direct;
            for (const auto& sub : subdirs) {
                std::wstring p = appDir + L"\\" + sub + L"\\" + name;
                if (fileExists(p)) return p;
            }
        }
    }
    std::wstring legacy = cacheDir + L"\\" + appId + L"_library_600x900.jpg";
    if (fileExists(legacy)) return legacy;
    for (std::wstring p : {appDir + L"\\header.jpg", cacheDir + L"\\" + appId + L"_header.jpg"})
        if (fileExists(p)) return p;
    return {};
}

std::vector<std::wstring> steamLibraryFolders(const std::wstring& steamRoot) {
    std::vector<std::wstring> out;
    if (steamRoot.empty()) return out;

    auto addFolder = [&](std::wstring p) {
        normalizeSlashes(p);
        if (p.empty()) return;
        std::wstring lower = toLower(p);
        for (const auto& existing : out) {
            if (toLower(existing) == lower) return;
        }
        out.push_back(std::move(p));
    };

    addFolder(steamRoot);

    std::wstring vdfPath = steamRoot + L"\\steamapps\\libraryfolders.vdf";
    auto bytes = readFileBytes(vdfPath);
    if (!bytes) return out;

    try {
        VdfNode root = parseVdf(*bytes);
        const VdfNode* lf = root.find("libraryfolders");
        if (lf) {
            for (const auto& child : lf->children) {
                if (child.isSection) {
                    std::string p = child.get("path");
                    if (!p.empty()) addFolder(widen(p));
                } else if (!child.value.empty()) {
                    addFolder(widen(child.value));
                }
            }
        }
    } catch (...) {
        // malformed file skipped
    }
    return out;
}

std::vector<DiscoveredGame> parseSteamLibrary(const std::wstring& libraryFolder, const std::wstring& steamRoot) {
    std::vector<DiscoveredGame> out;
    if (libraryFolder.empty()) return out;

    std::wstring steamappsDir = libraryFolder;
    if (!endsWith(toLower(libraryFolder), L"\\steamapps")) {
        steamappsDir = libraryFolder + L"\\steamapps";
    }
    if (!dirExists(steamappsDir)) return out;

    std::wstring commonDir = steamappsDir + L"\\common";
    auto acfFiles = listDir(steamappsDir, L"appmanifest_*.acf", false);

    for (const auto& acfName : acfFiles) {
        try {
            std::wstring acfPath = steamappsDir + L"\\" + acfName;
            auto bytes = readFileBytes(acfPath);
            if (!bytes) continue;

            VdfNode root = parseVdf(*bytes);
            const VdfNode* appState = root.find("AppState");
            if (!appState) continue;

            std::string appId = appState->get("appid");
            std::string rawName = appState->get("name");
            std::string installDirName = appState->get("installdir");

            if (appId.empty() || rawName.empty() || installDirName.empty()) continue;
            if (appId == "228980") continue;

            std::wstring wideName = widen(rawName);
            std::wstring lowerName = toLower(wideName);
            if (startsWith(lowerName, L"proton") ||
                startsWith(lowerName, L"steam linux runtime") ||
                startsWith(lowerName, L"steamworks")) {
                continue;
            }

            std::wstring wideInstallDir = widen(installDirName);
            normalizeSlashes(wideInstallDir);
            std::wstring fullInstallDir;
            if (wideInstallDir.size() >= 2 && wideInstallDir[1] == L':') {
                fullInstallDir = wideInstallDir;
            } else {
                fullInstallDir = commonDir + L"\\" + wideInstallDir;
            }

            if (!dirExists(fullInstallDir)) continue;

            std::wstring cleanedName = cleanGameName(wideName);
            if (cleanedName.empty()) continue;

            std::wstring wideAppId = widen(appId);
            std::wstring cover = steamCoverPath(steamRoot, wideAppId);

            DiscoveredGame g;
            g.launcher = L"steam";
            g.appId = std::move(wideAppId);
            g.name = std::move(cleanedName);
            g.installDir = std::move(fullInstallDir);
            g.coverPath = std::move(cover);
            out.push_back(std::move(g));
        } catch (...) {
            // skip malformed acf
        }
    }
    return out;
}

std::vector<DiscoveredGame> discoverSteam() {
    std::vector<DiscoveredGame> out;
    try {
        std::wstring root = steamRoot();
        if (root.empty()) return out;
        auto folders = steamLibraryFolders(root);
        for (const auto& folder : folders) {
            auto games = parseSteamLibrary(folder, root);
            out.insert(out.end(), std::make_move_iterator(games.begin()), std::make_move_iterator(games.end()));
        }
    } catch (...) {
        // never throw
    }
    return out;
}

std::optional<DiscoveredGame> parseEpicManifest(std::string_view json) {
    try {
        Json j = Json::parse(json);
        if (!j.isObject()) return std::nullopt;

        if (j.has("bIsIncompleteInstall") && j["bIsIncompleteInstall"].asBool(false)) {
            return std::nullopt;
        }

        std::string appName = j["AppName"].asString();
        if (appName.empty()) return std::nullopt;

        std::string mainGameAppName = j["MainGameAppName"].asString();
        if (!mainGameAppName.empty() && mainGameAppName != appName) {
            return std::nullopt;
        }

        const auto& cats = j["AppCategories"];
        bool hasGames = false;
        if (cats.isArray()) {
            for (size_t i = 0; i < cats.size(); ++i) {
                if (cats[i].isString() && cats[i].asString() == "games") {
                    hasGames = true;
                    break;
                }
            }
        }
        if (!hasGames) return std::nullopt;

        std::string displayName = j["DisplayName"].asString();
        std::string installLoc = j["InstallLocation"].asString();
        if (displayName.empty() || installLoc.empty()) return std::nullopt;

        std::wstring name = cleanGameName(widen(displayName));
        if (name.empty()) return std::nullopt;

        std::wstring installDir = widen(installLoc);
        normalizeSlashes(installDir);

        DiscoveredGame g;
        g.launcher = L"epic";
        g.appId = widen(appName);
        g.name = std::move(name);
        g.installDir = std::move(installDir);
        g.coverPath = L"";
        return g;
    } catch (...) {
        return std::nullopt;
    }
}

std::vector<DiscoveredGame> discoverEpic() {
    std::vector<DiscoveredGame> out;
    try {
        PWSTR base = nullptr;
        std::wstring programData;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &base))) {
            programData = base;
            CoTaskMemFree(base);
        }
        if (programData.empty()) return out;

        std::wstring manifestsDir = programData + L"\\Epic\\EpicGamesLauncher\\Data\\Manifests";
        if (!dirExists(manifestsDir)) return out;

        auto files = listDir(manifestsDir, L"*.item", false);
        for (const auto& f : files) {
            try {
                std::wstring itemPath = manifestsDir + L"\\" + f;
                auto bytes = readFileBytes(itemPath);
                if (!bytes) continue;
                auto game = parseEpicManifest(*bytes);
                if (game && dirExists(game->installDir)) {
                    out.push_back(std::move(*game));
                }
            } catch (...) {
                // skip malformed item
            }
        }
    } catch (...) {
        // never throw
    }
    return out;
}

std::vector<DiscoveredGame> discoverGog() {
    std::vector<DiscoveredGame> out;
    try {
        const wchar_t* gogKey = L"SOFTWARE\\GOG.com\\Games";
        auto subkeys = regSubkeys(HKEY_LOCAL_MACHINE, gogKey, RRF_SUBKEY_WOW6432KEY);
        for (const auto& id : subkeys) {
            std::wstring subkey = std::wstring(gogKey) + L"\\" + id;
            auto nameOpt = regString(HKEY_LOCAL_MACHINE, subkey.c_str(), L"gameName", RRF_SUBKEY_WOW6432KEY);
            auto pathOpt = regString(HKEY_LOCAL_MACHINE, subkey.c_str(), L"path", RRF_SUBKEY_WOW6432KEY);
            if (!nameOpt || !pathOpt || nameOpt->empty() || pathOpt->empty()) continue;

            std::wstring path = *pathOpt;
            normalizeSlashes(path);
            if (!dirExists(path)) continue;

            std::wstring name = cleanGameName(*nameOpt);
            if (name.empty()) continue;

            DiscoveredGame g;
            g.launcher = L"gog";
            g.appId = id;
            g.name = std::move(name);
            g.installDir = std::move(path);
            g.coverPath = L"";
            out.push_back(std::move(g));
        }
    } catch (...) {
        // never throw
    }
    return out;
}

std::vector<DiscoveredGame> discoverUbisoft() {
    std::vector<DiscoveredGame> out;
    try {
        const wchar_t* ubiKey = L"SOFTWARE\\Ubisoft\\Launcher\\Installs";
        auto subkeys = regSubkeys(HKEY_LOCAL_MACHINE, ubiKey, RRF_SUBKEY_WOW6432KEY);
        for (const auto& id : subkeys) {
            std::wstring subkey = std::wstring(ubiKey) + L"\\" + id;
            auto dirOpt = regString(HKEY_LOCAL_MACHINE, subkey.c_str(), L"InstallDir", RRF_SUBKEY_WOW6432KEY);
            if (!dirOpt || dirOpt->empty()) continue;

            std::wstring dir = *dirOpt;
            normalizeSlashes(dir);
            if (!dirExists(dir)) continue;

            std::wstring folderName = fileNameOf(dir);
            std::wstring name = cleanGameName(folderName);
            if (name.empty()) continue;

            DiscoveredGame g;
            g.launcher = L"ubisoft";
            g.appId = id;
            g.name = std::move(name);
            g.installDir = std::move(dir);
            g.coverPath = L"";
            out.push_back(std::move(g));
        }
    } catch (...) {
        // never throw
    }
    return out;
}

std::vector<DiscoveredGame> discoverXbox() {
    std::vector<DiscoveredGame> out;
    try {
        DWORD drives = GetLogicalDrives();
        for (int i = 0; i < 26; ++i) {
            if (!(drives & (1 << i))) continue;
            wchar_t rootPath[] = {static_cast<wchar_t>(L'A' + i), L':', L'\\', L'\0'};
            if (GetDriveTypeW(rootPath) != DRIVE_FIXED) continue;

            std::wstring xboxDir = std::wstring(1, static_cast<wchar_t>(L'A' + i)) + L":\\XboxGames";
            if (!dirExists(xboxDir)) continue;

            auto folders = listDir(xboxDir, L"*", true);
            for (const auto& folder : folders) {
                std::wstring contentDir = xboxDir + L"\\" + folder + L"\\Content";
                if (!dirExists(contentDir)) continue;

                std::wstring name = cleanGameName(folder);
                // The Xbox app also installs launchers (e.g. "Minecraft Launcher"); those are not games.
                if (name.empty() || toLower(name).find(L"launcher") != std::wstring::npos) continue;

                DiscoveredGame g;
                g.launcher = L"xbox";
                g.appId = folder;
                g.name = std::move(name);
                g.installDir = std::move(contentDir);
                g.coverPath = L"";
                out.push_back(std::move(g));
            }
        }
    } catch (...) {
        // never throw
    }
    return out;
}

std::vector<DiscoveredGame> discoverInstalledGames() {
    std::vector<DiscoveredGame> out;
    auto s = discoverSteam();
    out.insert(out.end(), std::make_move_iterator(s.begin()), std::make_move_iterator(s.end()));
    auto e = discoverEpic();
    out.insert(out.end(), std::make_move_iterator(e.begin()), std::make_move_iterator(e.end()));
    auto g = discoverGog();
    out.insert(out.end(), std::make_move_iterator(g.begin()), std::make_move_iterator(g.end()));
    auto u = discoverUbisoft();
    out.insert(out.end(), std::make_move_iterator(u.begin()), std::make_move_iterator(u.end()));
    auto x = discoverXbox();
    out.insert(out.end(), std::make_move_iterator(x.begin()), std::make_move_iterator(x.end()));
    return out;
}

}  // namespace pt
