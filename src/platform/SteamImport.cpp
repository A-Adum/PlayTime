#include "platform/SteamImport.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cwctype>
#include <string>
#include <utility>
#include <vector>

#include "platform/LauncherDiscovery.h"
#include "platform/WinUtil.h"
#include "util/Json.h"
#include "util/Str.h"
#include "util/Vdf.h"

namespace pt {

namespace {

struct ScopedWinHttpHandle {
    HINTERNET h = nullptr;
    ScopedWinHttpHandle(HINTERNET handle = nullptr) : h(handle) {}
    ~ScopedWinHttpHandle() {
        if (h) WinHttpCloseHandle(h);
    }
    ScopedWinHttpHandle(const ScopedWinHttpHandle&) = delete;
    ScopedWinHttpHandle& operator=(const ScopedWinHttpHandle&) = delete;
    ScopedWinHttpHandle(ScopedWinHttpHandle&& o) noexcept : h(o.h) { o.h = nullptr; }
    ScopedWinHttpHandle& operator=(ScopedWinHttpHandle&& o) noexcept {
        if (this != &o) {
            if (h) WinHttpCloseHandle(h);
            h = o.h;
            o.h = nullptr;
        }
        return *this;
    }
    operator HINTERNET() const { return h; }
};

}  // namespace

std::vector<SteamUser> steamUsers(const std::wstring& steamRoot) {
    std::vector<SteamUser> out;
    if (steamRoot.empty()) return out;

    std::wstring path = steamRoot + L"\\config\\loginusers.vdf";
    auto bytes = readFileBytes(path);
    if (!bytes) return out;

    try {
        VdfNode root = parseVdf(*bytes);
        const VdfNode* users = root.find("users");
        if (!users) return out;

        for (const auto& userNode : users->children) {
            uint64_t id64 = 0;
            try {
                id64 = std::stoull(userNode.key);
            } catch (...) {
                continue;
            }
            if (id64 <= kSteamId64Base) continue;
            uint32_t accountId = static_cast<uint32_t>(id64 - kSteamId64Base);
            std::string persona = userNode.get("PersonaName");
            out.push_back(SteamUser{accountId, id64, widen(persona)});
        }
    } catch (...) {
        // malformed file skipped
    }
    return out;
}

std::optional<SteamUser> activeSteamUser() {
    auto dword = regDword(HKEY_CURRENT_USER, L"Software\\Valve\\Steam\\ActiveProcess", L"ActiveUser");
    if (!dword || *dword == 0) return std::nullopt;

    uint32_t accountId = *dword;
    uint64_t id64 = kSteamId64Base + accountId;
    std::wstring persona;

    std::wstring root = steamRoot();
    if (!root.empty()) {
        auto users = steamUsers(root);
        for (const auto& u : users) {
            if (u.accountId == accountId) {
                persona = u.persona;
                break;
            }
        }
    }
    return SteamUser{accountId, id64, std::move(persona)};
}

Identity steamIdentity(const SteamUser& user) {
    return Identity{L"steam", std::to_wstring(user.accountId), user.persona};
}

std::map<std::wstring, int64_t> steamLocalPlaytime(const std::wstring& steamRoot, uint32_t accountId) {
    std::map<std::wstring, int64_t> out;
    if (steamRoot.empty() || accountId == 0) return out;

    std::wstring path = steamRoot + L"\\userdata\\" + std::to_wstring(accountId) + L"\\config\\localconfig.vdf";
    auto bytes = readFileBytes(path, 64u << 20);
    if (!bytes) return out;

    try {
        VdfNode root = parseVdf(*bytes);
        const VdfNode* apps = root.path({"UserLocalConfigStore", "Software", "Valve", "Steam", "apps"});
        if (!apps) return out;

        for (const auto& appNode : apps->children) {
            if (appNode.key.empty()) continue;
            std::string ptStr = appNode.get("Playtime");
            if (ptStr.empty()) continue;
            try {
                int64_t minutes = std::stoll(ptStr);
                if (minutes >= 0) {
                    out[widen(appNode.key)] = minutes * 60;
                }
            } catch (...) {
                // skip unparseable number
            }
        }
    } catch (...) {
        // malformed file skipped
    }
    return out;
}

SteamWebResult steamWebApiOwnedGames(const std::wstring& apiKey, const std::wstring& steamId64) {
    if (apiKey.size() != 32) {
        return {false, L"API key must be 32 hexadecimal characters", {}};
    }
    for (wchar_t c : apiKey) {
        if (!std::iswxdigit(c)) {
            return {false, L"API key must be 32 hexadecimal characters", {}};
        }
    }
    if (steamId64.size() != 17) {
        return {false, L"Steam ID must be 17 digits", {}};
    }
    for (wchar_t c : steamId64) {
        if (!std::iswdigit(c)) {
            return {false, L"Steam ID must be 17 digits", {}};
        }
    }

    ScopedWinHttpHandle hSession(WinHttpOpen(L"PlayTime/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!hSession) {
        return {false, L"Failed to initialize HTTP session: " + lastErrorMessage(), {}};
    }

    // 10s timeouts: resolve, connect, send, receive
    WinHttpSetTimeouts(hSession, 10000, 10000, 10000, 10000);

    ScopedWinHttpHandle hConnect(WinHttpConnect(hSession, L"api.steampowered.com", INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (!hConnect) {
        return {false, L"Failed to connect to Steam API: " + lastErrorMessage(), {}};
    }

    std::wstring path = L"/IPlayerService/GetOwnedGames/v1/?key=" + apiKey +
                        L"&steamid=" + steamId64 +
                        L"&include_appinfo=1&include_played_free_games=1&format=json";

    ScopedWinHttpHandle hRequest(WinHttpOpenRequest(hConnect, L"GET", path.c_str(), nullptr,
                                                   WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                   WINHTTP_FLAG_SECURE));
    if (!hRequest) {
        return {false, L"Failed to open HTTP request: " + lastErrorMessage(), {}};
    }

    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        return {false, L"Network request failed: " + lastErrorMessage(), {}};
    }

    if (!WinHttpReceiveResponse(hRequest, nullptr)) {
        return {false, L"Network response failed: " + lastErrorMessage(), {}};
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    if (!WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX)) {
        return {false, L"Failed to query HTTP status: " + lastErrorMessage(), {}};
    }

    if (statusCode == 401 || statusCode == 403) {
        return {false, L"Invalid or unauthorized Steam Web API key (HTTP " + std::to_wstring(statusCode) + L")", {}};
    }
    if (statusCode != 200) {
        return {false, L"HTTP error " + std::to_wstring(statusCode), {}};
    }

    std::string responseBody;
    for (;;) {
        DWORD bytesAvail = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &bytesAvail)) {
            return {false, L"Network read error: " + lastErrorMessage(), {}};
        }
        if (bytesAvail == 0) break;
        std::string buf(bytesAvail, '\0');
        DWORD bytesRead = 0;
        if (!WinHttpReadData(hRequest, buf.data(), bytesAvail, &bytesRead)) {
            return {false, L"Network read error: " + lastErrorMessage(), {}};
        }
        buf.resize(bytesRead);
        responseBody += buf;
    }

    try {
        Json root = Json::parse(responseBody);
        if (!root.isObject() || !root.has("response")) {
            return {false, L"Malformed API response", {}};
        }
        const auto& resp = root["response"];
        if (!resp.isObject()) {
            return {false, L"Malformed API response", {}};
        }
        if (resp.members().empty()) {
            return {false, L"Steam profile or game details are private", {}};
        }
        if (!resp.has("games")) {
            if (resp.has("game_count") && resp["game_count"].asInt(0) == 0) {
                return {true, L"", {}};
            }
            return {false, L"Steam profile or game details are private", {}};
        }

        const auto& gamesArr = resp["games"];
        if (!gamesArr.isArray()) {
            return {false, L"Malformed games list in API response", {}};
        }

        std::map<std::wstring, SteamWebGame> outGames;
        for (size_t i = 0; i < gamesArr.size(); ++i) {
            const auto& item = gamesArr[i];
            if (!item.isObject()) continue;

            std::wstring appId;
            if (item["appid"].isNumber()) {
                appId = std::to_wstring(item["appid"].asInt(0));
            } else {
                appId = widen(item["appid"].asString());
            }
            if (appId.empty()) continue;

            std::wstring name = widen(item["name"].asString());
            int64_t minutes = item["playtime_forever"].asInt(0);
            int64_t seconds = std::max<int64_t>(0, minutes * 60);

            outGames[appId] = SteamWebGame{std::move(name), seconds};
        }
        return {true, L"", std::move(outGames)};
    } catch (const std::exception& e) {
        return {false, L"Failed to parse API response: " + widen(e.what()), {}};
    }
}

std::vector<SteamImportLine> applySteamImport(GameLibrary& library, AccountManager& accounts,
                                              PlaytimeCalculator& calc, const SteamUser& user,
                                              const std::map<std::wstring, int64_t>& secondsByApp,
                                              const std::map<std::wstring, std::wstring>& names) {
    std::vector<SteamImportLine> out;
    auto launcherAppsList = library.launcherApps(L"steam");
    std::map<std::wstring, Id> appToGame;
    for (const auto& [appId, gameId] : launcherAppsList) {
        appToGame[appId] = gameId;
    }

    Identity identity = steamIdentity(user);
    std::wstring accName = L"Steam \u00B7 " + (user.persona.empty() ? std::to_wstring(user.accountId) : user.persona);

    for (const auto& [appId, seconds] : secondsByApp) {
        if (seconds <= 0) continue;

        Id gameId = 0;
        auto it = appToGame.find(appId);
        if (it != appToGame.end()) {
            gameId = it->second;
        } else if (!names.empty()) {
            auto nameIt = names.find(appId);
            if (nameIt != names.end() && !nameIt->second.empty()) {
                auto found = library.findByName(nameIt->second);
                if (found) {
                    gameId = *found;
                }
            }
        }

        if (gameId == 0) continue;

        auto gameOpt = library.game(gameId);
        if (!gameOpt) continue;

        Id targetAccountId = 0;
        std::wstring targetAccountName;
        auto gameAccounts = accounts.accounts(gameId);
        for (const auto& acc : gameAccounts) {
            auto idents = accounts.identifiers(acc.id);
            for (const auto& ident : idents) {
                if (ident.launcher == identity.launcher && ident.externalId == identity.externalId) {
                    targetAccountId = acc.id;
                    targetAccountName = acc.name;
                    break;
                }
            }
            if (targetAccountId != 0) break;
        }

        if (targetAccountId == 0) {
            targetAccountId = accounts.addAccount(gameId, accName, L"steam");
            targetAccountName = accName;
            accounts.addIdentifier(targetAccountId, identity);
        }

        ImportResult res = calc.reconcileImport(gameId, targetAccountId, seconds, L"steam");

        SteamImportLine line;
        line.gameId = gameId;
        line.gameName = gameOpt->name;
        line.accountId = targetAccountId;
        line.accountName = targetAccountName;
        line.result = res;
        out.push_back(std::move(line));
    }

    return out;
}

}  // namespace pt
