#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace pt {

// Valve KeyValues (text VDF/ACF) node. Lookups are case-insensitive, as in Steam.
struct VdfNode {
    std::string key;
    std::string value;  // empty for sections
    std::vector<VdfNode> children;
    bool isSection = false;

    const VdfNode* find(std::string_view k) const;
    // Follows a path of keys; returns nullptr when any segment is missing.
    const VdfNode* path(std::initializer_list<std::string_view> keys) const;
    std::string get(std::string_view k, std::string_view def = {}) const;
};

// Parses text VDF. Returns a synthetic root whose children are the top-level entries.
// Malformed trailing content is ignored (Steam files are occasionally truncated mid-write).
VdfNode parseVdf(std::string_view text);

}  // namespace pt
