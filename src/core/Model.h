#pragma once
#include <cstdint>
#include <string>

namespace pt {

using Id = int64_t;
// Account id 0 means "Unassigned" (stored as NULL in the database).
constexpr Id kUnassigned = 0;

struct Game {
    Id id = 0;
    std::wstring name;
    std::wstring coverPath;
    int64_t createdAt = 0;
    Id preferredAccount = 0;  // 0 = auto-detect; otherwise sessions go to this account
};

struct Account {
    Id id = 0;
    Id gameId = 0;
    std::wstring name;
    std::wstring launcher;  // "steam", "epic", "xbox", ... or empty
    int64_t createdAt = 0;
};

// A launcher-provided identifier that reliably maps a running game to an account.
struct AccountIdentifier {
    Id id = 0;
    Id accountId = 0;
    std::wstring launcher;
    std::wstring externalId;
    std::wstring displayName;
};

struct Executable {
    Id id = 0;
    Id gameId = 0;
    std::wstring exeName;   // lowercase file name, e.g. "rocketleague.exe"
    std::wstring fullPath;  // optional normalized full path constraint
};

enum class RuleKind { InstallDir, Exclude };

struct TrackingRule {
    Id id = 0;
    Id gameId = 0;  // 0 = global rule
    RuleKind kind = RuleKind::InstallDir;
    std::wstring pattern;  // InstallDir: normalized directory; Exclude: glob on exe name, or on path if it contains '\'
    std::wstring launcher;  // discovery origin (InstallDir only)
    std::wstring appId;
};

enum class SessionSource { Auto, Manual };

struct Session {
    Id id = 0;
    Id gameId = 0;
    Id accountId = kUnassigned;
    int64_t startUtc = 0;
    int64_t endUtc = 0;
    int64_t duration = 0;  // active seconds (monotonic, excludes sleep)
    SessionSource source = SessionSource::Auto;
    bool open = false;       // still running (checkpointed)
    bool recovered = false;  // closed by crash recovery
    bool edited = false;     // manually modified
    std::wstring exeName;
    std::wstring exePath;
    uint32_t pid = 0;
    int64_t procCreated = 0;  // FILETIME of the tracked process, to recognise it after a restart
};

// Who is playing, as reported by local launcher metadata. Empty launcher = unknown.
struct Identity {
    std::wstring launcher;
    std::wstring externalId;
    std::wstring displayName;
    bool valid() const { return !launcher.empty() && !externalId.empty(); }
};

// An installed game found in local launcher metadata.
struct DiscoveredGame {
    std::wstring launcher;  // "steam", "epic", "gog", "xbox", "ubisoft"
    std::wstring appId;
    std::wstring name;
    std::wstring installDir;
    std::wstring coverPath;  // local artwork if the launcher caches one
};

}  // namespace pt
