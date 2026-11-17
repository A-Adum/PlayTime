#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/AccountManager.h"
#include "core/GameLibrary.h"
#include "core/PlaytimeCalculator.h"

namespace pt {

constexpr uint64_t kSteamId64Base = 76561197960265728ULL;

struct SteamUser {
    uint32_t accountId = 0;  // SteamID3 account number (userdata\<accountId>)
    uint64_t steamId64 = 0;
    std::wstring persona;    // display name from config\loginusers.vdf
};

// Users that have signed in to Steam on this PC (config\loginusers.vdf).
std::vector<SteamUser> steamUsers(const std::wstring& steamRoot);
// User currently signed in to the running Steam client (HKCU\Software\Valve\Steam\ActiveProcess\ActiveUser).
// This is the account a Steam-launched game runs under. nullopt when Steam is not running/signed in.
std::optional<SteamUser> activeSteamUser();
// Identity used to map sessions/imports to a PlayTime account: launcher "steam", externalId = accountId.
Identity steamIdentity(const SteamUser& user);

// Lifetime playtime per app (seconds) that Steam caches locally for one user:
// userdata\<accountId>\config\localconfig.vdf -> UserLocalConfigStore/Software/Valve/Steam/apps/<appid>/Playtime
// (minutes). Apps without a Playtime value are omitted.
std::map<std::wstring, int64_t> steamLocalPlaytime(const std::wstring& steamRoot, uint32_t accountId);

// Official Steam Web API IPlayerService/GetOwnedGames (needs the user's own Web API key; the profile's
// game details must be visible to that key). Synchronous WinHTTP request with a short timeout.
struct SteamWebGame {
    std::wstring name;
    int64_t seconds = 0;
};
struct SteamWebResult {
    bool ok = false;
    std::wstring error;
    std::map<std::wstring, SteamWebGame> games;  // appid -> game
};
SteamWebResult steamWebApiOwnedGames(const std::wstring& apiKey, const std::wstring& steamId64);

struct SteamImportLine {
    Id gameId = 0;
    std::wstring gameName;
    Id accountId = 0;
    std::wstring accountName;
    ImportResult result;
};

// Applies imported lifetime totals for one Steam user to games in the library that are linked to Steam
// app ids (via discovery) - or, when `names` is provided, whose name matches. The Steam account under each
// game is found by identifier or created ("Steam · <persona>"). Totals are reconciled, never added.
std::vector<SteamImportLine> applySteamImport(GameLibrary& library, AccountManager& accounts,
                                              PlaytimeCalculator& calc, const SteamUser& user,
                                              const std::map<std::wstring, int64_t>& secondsByApp,
                                              const std::map<std::wstring, std::wstring>& names = {});

}  // namespace pt
