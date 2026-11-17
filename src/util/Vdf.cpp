#include "util/Vdf.h"

#include <cctype>

namespace pt {

namespace {
bool ieq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
    return true;
}

struct Lexer {
    std::string_view s;
    size_t i = 0;
    enum Tok { End, Str, Open, Close };

    Tok next(std::string& out) {
        for (;;) {
            while (i < s.size() && std::isspace((unsigned char)s[i])) ++i;
            if (i + 1 < s.size() && s[i] == '/' && s[i + 1] == '/') {
                while (i < s.size() && s[i] != '\n') ++i;
                continue;
            }
            break;
        }
        if (i >= s.size()) return End;
        char c = s[i];
        if (c == '{') { ++i; return Open; }
        if (c == '}') { ++i; return Close; }
        out.clear();
        if (c == '"') {
            ++i;
            while (i < s.size() && s[i] != '"') {
                if (s[i] == '\\' && i + 1 < s.size()) {
                    char e = s[i + 1];
                    out += e == 'n' ? '\n' : e == 't' ? '\t' : e;
                    i += 2;
                } else {
                    out += s[i++];
                }
            }
            ++i;
            return Str;
        }
        while (i < s.size() && !std::isspace((unsigned char)s[i]) && s[i] != '{' && s[i] != '}' && s[i] != '"')
            out += s[i++];
        // Conditional suffixes like [$WIN32] are skipped.
        return Str;
    }
};

void parseInto(Lexer& lx, VdfNode& parent, int depth) {
    std::string tok;
    while (true) {
        auto t = lx.next(tok);
        if (t == Lexer::End || t == Lexer::Close) return;
        if (t != Lexer::Str) continue;
        VdfNode node;
        node.key = tok;
        std::string val;
        auto t2 = lx.next(val);
        if (t2 == Lexer::Open) {
            node.isSection = true;
            if (depth < 64) parseInto(lx, node, depth + 1);
        } else if (t2 == Lexer::Str) {
            node.value = std::move(val);
            // Skip an optional conditional like [$WIN32].
            size_t save = lx.i;
            std::string peek;
            if (lx.next(peek) != Lexer::Str || peek.empty() || peek[0] != '[') lx.i = save;
        } else {
            parent.children.push_back(std::move(node));
            return;
        }
        parent.children.push_back(std::move(node));
    }
}
}  // namespace

const VdfNode* VdfNode::find(std::string_view k) const {
    for (auto& c : children)
        if (ieq(c.key, k)) return &c;
    return nullptr;
}

const VdfNode* VdfNode::path(std::initializer_list<std::string_view> keys) const {
    const VdfNode* n = this;
    for (auto k : keys) {
        n = n->find(k);
        if (!n) return nullptr;
    }
    return n;
}

std::string VdfNode::get(std::string_view k, std::string_view def) const {
    auto n = find(k);
    return n && !n->isSection ? n->value : std::string(def);
}

VdfNode parseVdf(std::string_view text) {
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF) text.remove_prefix(3);
    VdfNode root;
    root.isSection = true;
    Lexer lx{text};
    parseInto(lx, root, 0);
    return root;
}

}  // namespace pt
