// h2cpp.cpp - create, or bring up to date, a .cpp of stub definitions for the
// functions declared in a C++ header.
//
//   Build:  g++     -std=c++23 -O2 -Wall -Wextra -o h2cpp h2cpp.cpp
//           clang++ -std=c++23 -O2 -Wall -Wextra -o h2cpp h2cpp.cpp
//
//   Usage:  h2cpp foo.h                  stubs for everything, to stdout
//           h2cpp foo.h -o foo.cpp       create foo.cpp, or add only the
//                                        missing stubs to an existing one,
//                                        placed to follow the header's order
//
// This is a lightweight declaration scanner, not a full C++ parser.  It copes
// with namespaces, nested classes, extern "C", operators, conversion
// operators, constructors/destructors, default arguments, attributes, trailing
// return types, reference returns and common Qt macros.  Things that cannot
// (or must not) be defined in a .cpp are skipped: templates, inline/constexpr
// functions, = 0 / = default / = delete, functions with inline bodies, friends
// and Qt signals.

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <print>
#include <ranges>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr size_t npos = std::string::npos;

// ---------------------------------------------------------------- tokens ---

struct Token {
    std::string text;
    bool spaceBefore = false; // whitespace preceded it in the source
    size_t begin = 0; // byte offsets in the source
    size_t end = 0;
};
using Tokens = std::vector<Token>;
using NsPath = std::vector<std::string>; // namespace components; "" = anonymous

bool isIdentStart(char c)
{
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}
bool isIdentChar(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}
bool isIdent(std::string_view s)
{
    return !s.empty() && isIdentStart(s[0]);
}
bool isWordLike(std::string_view s)
{
    return !s.empty() && isIdentChar(s[0]);
}
bool isStringLit(std::string_view s)
{
    return s.ends_with('"');
}
bool isOpen(std::string_view s)
{
    return s == "(" || s == "[" || s == "{";
}
bool isClose(std::string_view s)
{
    return s == ")" || s == "]" || s == "}";
}

size_t skipQuoted(std::string_view src, size_t i)
{
    const char q = src[i++];
    while (i < src.size() && src[i] != q && src[i] != '\n') {
        i += (src[i] == '\\') ? 2 : 1;
    }
    return std::min(i + 1, src.size());
}

// Splits source into tokens, dropping comments and preprocessor directives.
// `directiveEnds`, if given, receives the offset just past each preprocessor line.
Tokens tokenize(std::string_view src, std::vector<size_t>* directiveEnds = nullptr)
{
    static constexpr std::string_view multi[]
        = { "...", "<=>", "->*", "::", "->", "<<", "<=", "==", "!=", "&&", "||",
            "++",  "--",  "+=",  "-=", "*=", "/=", "%=", "&=", "|=", "^=", ".*" };
    static const std::set<std::string_view> strPrefixes
        = { "L", "u", "U", "u8", "R", "LR", "uR", "UR", "u8R" };

    auto skipBlockComment = [&](size_t i) {
        const size_t e = src.find("*/", i + 2);
        return e == npos ? src.size() : e + 2;
    };

    Tokens toks;
    const size_t n = src.size();
    size_t i = 0;
    bool space = false, lineStart = true;

    while (i < n) {
        const char c = src[i];
        if (c == '\n') {
            space = lineStart = true;
            ++i;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(c)) || c == '\\') {
            space = true;
            ++i;
            continue;
        }
        if (src.substr(i, 2) == "//") {
            while (i < n && src[i] != '\n') {
                ++i;
            }
            space = true;
            continue;
        }
        if (src.substr(i, 2) == "/*") {
            i = skipBlockComment(i);
            space = true;
            continue;
        }
        if (c == '#' && lineStart) { // preprocessor line(s)
            while (i < n && src[i] != '\n') {
                if (src[i] == '\\') {
                    size_t j = i + 1;
                    while (j < n && src[j] == '\r') {
                        ++j;
                    }
                    if (j < n && src[j] == '\n') {
                        i = j + 1;
                        continue;
                    }
                }
                if (src.substr(i, 2) == "/*") {
                    i = skipBlockComment(i);
                    continue;
                }
                ++i;
            }
            if (directiveEnds) {
                directiveEnds->push_back(std::min(i + 1, n));
            }
            space = true;
            continue;
        }

        lineStart = false;
        const size_t start = i;
        if (isIdentStart(c)) {
            while (i < n && isIdentChar(src[i])) {
                ++i;
            }
            if (i < n && (src[i] == '"' || src[i] == '\'')
                && strPrefixes.contains(src.substr(start, i - start))) {
                if (src[i - 1] == 'R' && src[i] == '"') { // raw string
                    const size_t open = src.find('(', i);
                    if (open == npos) {
                        i = n;
                    } else {
                        const std::string delim
                            = std::format("){}\"", src.substr(i + 1, open - i - 1));
                        const size_t e = src.find(delim, open);
                        i = (e == npos) ? n : e + delim.size();
                    }
                } else {
                    i = skipQuoted(src, i);
                }
            }
        } else if (
            std::isdigit(static_cast<unsigned char>(c))
            || (c == '.' && i + 1 < n && std::isdigit(static_cast<unsigned char>(src[i + 1])))) {
            while (i < n) {
                const char d = src[i];
                if (isIdentChar(d) || d == '.' || d == '\'') {
                    ++i;
                } else if ((d == '+' || d == '-') && std::strchr("eEpP", src[i - 1])) {
                    ++i;
                } else {
                    break;
                }
            }
        } else if (c == '"' || c == '\'') {
            i = skipQuoted(src, i);
        } else {
            const auto m = std::ranges::find_if(
                multi, [&](std::string_view m) { return src.substr(i).starts_with(m); });
            i += (m != std::ranges::end(multi)) ? m->size() : 1;
        }
        toks.push_back({ std::string(src.substr(start, i - start)), space, start, i });
        space = false;
    }
    return toks;
}

// Index of the bracket closing the one at `open` (any of ( [ { ).
size_t matchClose(const Tokens& t, size_t open)
{
    int depth = 0;
    for (size_t k = open; k < t.size(); ++k) {
        if (isOpen(t[k].text)) {
            ++depth;
        } else if (isClose(t[k].text) && --depth == 0) {
            return k;
        }
    }
    return t.empty() ? 0 : t.size() - 1;
}

Tokens slice(const Tokens& t, size_t b, size_t e)
{
    e = std::min(e, t.size());
    return b < e ? Tokens(t.begin() + b, t.begin() + e) : Tokens {};
}

bool contains(const Tokens& t, std::string_view s)
{
    return std::ranges::contains(t, s, &Token::text);
}

// Rebuilds text from tokens, keeping the source's spacing where sensible.
std::string join(const Tokens& t)
{
    std::string out;
    for (size_t k = 0; k < t.size(); ++k) {
        if (k > 0) {
            const std::string& prev = t[k - 1].text;
            const std::string& cur = t[k].text;
            const bool wanted = t[k].spaceBefore || (isWordLike(prev) && isWordLike(cur));
            const bool suppressed
                = prev == "(" || prev == "[" || cur == ")" || cur == "]" || cur == ",";
            if (wanted && !suppressed) {
                out += ' ';
            }
        }
        out += t[k].text;
    }
    return out;
}

// Spacing-insensitive form, used for comparing declarations.
std::string compact(const Tokens& t)
{
    std::string out;
    for (size_t k = 0; k < t.size(); ++k) {
        if (k > 0 && isWordLike(t[k - 1].text) && isWordLike(t[k].text)) {
            out += ' ';
        }
        out += t[k].text;
    }
    return out;
}

// (std::ranges::starts_with/ends_with need GCC 15; these keep GCC 14 happy.)
bool startsWith(const NsPath& path, const NsPath& prefix)
{
    return prefix.size() <= path.size()
        && std::ranges::equal(prefix, path | std::views::take(prefix.size()));
}

bool endsWith(const NsPath& path, const NsPath& suffix)
{
    return suffix.size() <= path.size()
        && std::ranges::equal(suffix, path | std::views::drop(path.size() - suffix.size()));
}

std::string joinNs(std::span<const std::string> parts)
{
    std::string out;
    for (const auto& p : parts) {
        out += (out.empty() ? "" : "::") + p;
    }
    return out;
}

// Removes [[attributes]], __attribute__((...)), __declspec(...), alignas(...).
Tokens clean(const Tokens& in)
{
    Tokens out;
    for (size_t k = 0; k < in.size(); ++k) {
        const std::string& s = in[k].text;
        if (s == "[" && k + 1 < in.size() && in[k + 1].text == "[") {
            k = matchClose(in, k);
            continue;
        }
        if ((s == "__attribute__" || s == "__declspec" || s == "alignas") && k + 1 < in.size()
            && in[k + 1].text == "(") {
            k = matchClose(in, k + 1);
            continue;
        }
        out.push_back(in[k]);
    }
    return out;
}

// Index just past any leading "template<...>" clauses (0 if none).
size_t afterTemplatePrefix(const Tokens& t)
{
    size_t k = 0;
    while (k + 1 < t.size() && t[k].text == "template" && t[k + 1].text == "<") {
        int angle = 0, paren = 0;
        for (k = k + 1; k < t.size(); ++k) {
            const std::string& s = t[k].text;
            if (s == "(") {
                ++paren;
            } else if (s == ")") {
                --paren;
            } else if (paren == 0 && s == "<") {
                ++angle;
            } else if (paren == 0 && s == ">" && --angle == 0) {
                ++k;
                break;
            }
        }
    }
    return k;
}

// Name in "class EXPORT_MACRO Name final : Base" style heads.
std::string className(const Tokens& t, size_t from)
{
    std::string name;
    for (size_t k = from; k < t.size(); ++k) {
        const std::string& s = t[k].text;
        if (s == ":" || s == "<" || s == "{") {
            break;
        }
        if (isIdent(s) && s != "final" && s != "class" && s != "struct") {
            name = s;
        }
    }
    return name;
}

// Splits a parameter list on top-level commas, dropping default arguments.
std::vector<Tokens> splitParams(const Tokens& t)
{
    std::vector<Tokens> parts(1);
    int depth = 0, angle = 0;
    bool inDefault = false;
    for (size_t k = 0; k < t.size(); ++k) {
        const std::string& s = t[k].text;
        if (isOpen(s)) {
            ++depth;
        } else if (isClose(s)) {
            --depth;
        } else if (depth == 0) {
            if (s == "<" && k > 0 && isIdent(t[k - 1].text)) {
                ++angle;
            } else if (s == ">" && angle > 0) {
                --angle;
            } else if (angle == 0 && s == ",") {
                parts.emplace_back();
                inDefault = false;
                continue;
            } else if (angle == 0 && s == "=") {
                inDefault = true;
            }
        }
        if (!inDefault) {
            parts.back().push_back(t[k]);
        }
    }
    if (parts.size() == 1 && (parts[0].empty() || compact(parts[0]) == "void")) {
        parts.clear();
    }
    return parts;
}

// Index of the parameter's name within its tokens, if it has one:
// "int x" -> x, "const T& v" -> v, "char buf[16]" -> buf,
// "void (*cb)(int)" -> cb; "int", "const T&", "..." -> none.
std::optional<size_t> paramNameIndex(const Tokens& p)
{
    static const std::set<std::string_view> builtin
        = { "int",  "char", "short",   "long",    "unsigned", "signed",   "float", "double",
            "bool", "void", "wchar_t", "char8_t", "char16_t", "char32_t", "auto" };
    static const std::set<std::string_view> notAType
        = { "const", "volatile", "struct", "class", "enum", "union", "typename", "::" };

    // Function pointer / reference: "R (*name)(args)", "R (C::*name)(args)".
    for (size_t k = 0; k + 2 < p.size(); ++k) {
        if (p[k].text != "(") {
            continue;
        }
        size_t j = k + 1;
        while (j + 1 < p.size() && isIdent(p[j].text) && p[j + 1].text == "::") {
            j += 2;
        }
        if (j < p.size() && (p[j].text == "*" || p[j].text == "&" || p[j].text == "&&")) {
            ++j;
            while (j < p.size() && (p[j].text == "const" || p[j].text == "volatile")) {
                ++j;
            }
            if (j < p.size() && isIdent(p[j].text)) {
                return j;
            }
            return std::nullopt;
        }
    }

    // Skip trailing array bounds: "buf[16]", "grid[3][3]".
    size_t end = p.size();
    while (end > 0 && p[end - 1].text == "]") {
        int depth = 0;
        while (end > 0) {
            const std::string& s = p[--end].text;
            if (s == "]") {
                ++depth;
            } else if (s == "[" && --depth == 0) {
                break;
            }
        }
    }

    if (end < 2) {
        return std::nullopt;
    }
    const std::string& last = p[end - 1].text;
    const std::string& prev = p[end - 2].text;
    const bool prevIsType
        = isIdent(prev) || prev == "*" || prev == "&" || prev == "&&" || prev == ">";
    if (!isIdent(last) || builtin.contains(last) || notAType.contains(last)) {
        return std::nullopt;
    }

    // cv after something ("T* const name", "int const name") means a name
    // follows; cv with nothing before it ("const Foo") qualifies the type.
    if (prev == "const" || prev == "volatile") {
        size_t j = end - 2;
        while (j > 0 && (p[j].text == "const" || p[j].text == "volatile")) {
            --j;
        }
        const bool cvOnly = p[j].text == "const" || p[j].text == "volatile";
        return cvOnly ? std::nullopt : std::optional<size_t>(end - 1);
    }
    if (!notAType.contains(prev) && prevIsType) {
        return end - 1;
    }
    return std::nullopt;
}

// Parameter types with names removed, so "int x" matches "int".
std::string paramKey(const std::vector<Tokens>& params)
{
    std::string key;
    for (Tokens p : params) {
        if (const auto idx = paramNameIndex(p)) {
            p.erase(p.begin() + *idx);
        }
        key += (key.empty() ? "" : ",") + compact(p);
    }
    return key;
}

// cv/ref qualifiers only: noexcept, override etc. don't distinguish overloads.
std::string qualKey(const Tokens& quals)
{
    Tokens kept;
    for (size_t k = 0; k < quals.size(); ++k) {
        const std::string& s = quals[k].text;
        if (s == "noexcept" || s == "throw") {
            if (k + 1 < quals.size() && quals[k + 1].text == "(") {
                k = matchClose(quals, k + 1);
            }
            continue;
        }
        if (s == "override" || s == "final") {
            continue;
        }
        kept.push_back(quals[k]);
    }
    return compact(kept);
}

std::vector<std::string> nameParts(const Tokens& name)
{
    std::vector<std::string> parts(1);
    Tokens cur;
    for (const auto& tk : name) {
        if (tk.text == "::") {
            parts.back() = compact(cur);
            parts.emplace_back();
            cur.clear();
        } else {
            cur.push_back(tk);
        }
    }
    parts.back() = compact(cur);
    std::erase(parts, std::string {}); // leading "::" (global qualification)
    return parts;
}

// ------------------------------------------------------- declaration parse ---

enum class Mode {
    Header,
    Source
};

struct Signature {
    Tokens specs, ret, name, convType, quals, trailing;
    std::vector<Tokens> params;
    bool conversion = false;
    bool hasTrailing = false;
    bool spaceBeforeName = false;
};

// Picks apart one function declaration (Header) or definition head (Source).
// The error is a note to emit, or empty to skip silently.
std::expected<Signature, std::string> analyse(const Tokens& st, Mode mode, bool inClass)
{
    using std::unexpected;

    // 1. The parameter list: the first top-level '('.
    size_t p = npos, opIdx = npos;
    int paren = 0, angle = 0;
    for (size_t k = 0; k < st.size() && p == npos; ++k) {
        const std::string& s = st[k].text;
        if (paren == 0 && angle == 0 && s == "operator") {
            opIdx = k;
            size_t j = k + 1;
            if (j < st.size() && st[j].text == "(") {
                j += 2; // operator()
            }
            while (j < st.size() && st[j].text != "(") {
                ++j;
            }
            p = j;
        } else if (s == "(") {
            if (paren == 0 && angle == 0) {
                p = k;
            } else {
                ++paren;
            }
        } else if (s == ")") {
            --paren;
        } else if (paren == 0) {
            if (s == "<" && k > 0 && isIdent(st[k - 1].text)) {
                ++angle;
            } else if (s == ">" && angle > 0) {
                --angle;
            } else if (s == "=" && angle == 0) {
                return unexpected(""); // variable initialiser
            }
        }
    }
    if (p == npos || p >= st.size()) {
        return unexpected("");
    }
    const size_t q = matchClose(st, p);
    if (st[q].text != ")") {
        return unexpected("");
    }

    // Pointer to function / member function variable: "void (*cb)(int);"
    if (p + 1 < q) {
        const std::string& a = st[p + 1].text;
        if (a == "*" || a == "&" || a == "^") {
            return unexpected("");
        }
        if (p + 3 < q && st[p + 2].text == "::" && st[p + 3].text == "*") {
            return unexpected("");
        }
    }

    // 2. Function name (possibly ~Dtor, Qualified::name, operator...).
    size_t nameStart;
    if (opIdx != npos) {
        nameStart = opIdx;
    } else {
        if (p == 0 || !isIdent(st[p - 1].text)) {
            return unexpected("");
        }
        nameStart = p - 1;
        if (nameStart > 0 && st[nameStart - 1].text == "~") {
            --nameStart;
        }
    }
    while (nameStart >= 2 && st[nameStart - 1].text == "::" && isIdent(st[nameStart - 2].text)) {
        nameStart -= 2;
    }

    // 3. Declaration specifiers before the name; what's left is the return type.
    Signature sig;
    for (size_t k = 0; k < nameStart; ++k) {
        const std::string& s = st[k].text;
        if (s == "explicit" && k + 1 < nameStart && st[k + 1].text == "(") {
            k = matchClose(st, k + 1);
            continue;
        }
        if (s == "virtual" || s == "explicit") {
            continue;
        }
        if (s == "extern") {
            if (k + 1 < nameStart && isStringLit(st[k + 1].text)) {
                ++k;
            }
            continue;
        }
        if (s == "static") {
            if (!inClass) {
                sig.specs.push_back(st[k]);
            }
            continue;
        }
        if (s == "inline" || s == "constexpr" || s == "consteval") {
            if (mode == Mode::Header) {
                return unexpected(s + " function not stubbed (must be defined in the header)");
            }
            continue;
        }
        if (s == "friend" || s == "Q_SIGNAL") {
            return unexpected("");
        }
        sig.ret.push_back(st[k]);
    }

    // 4. Qualifiers after the parameter list.
    for (size_t k = q + 1; k < st.size(); ++k) {
        const std::string& s = st[k].text;
        if (s == "override" || s == "final") {
            continue;
        }
        if (s == ":" || s == "try") {
            break; // ctor init list (Source)
        }
        if (s == "=") { // = 0 / default / delete
            // In a .cpp, "X::~X() = default;" is a definition in its own right.
            const bool defaulted
                = k + 1 < st.size() && (st[k + 1].text == "default" || st[k + 1].text == "delete");
            if (mode == Mode::Source && defaulted) {
                break;
            }
            return unexpected("");
        }
        if (s == "->" && !sig.hasTrailing) {
            sig.hasTrailing = true;
            continue;
        }
        (sig.hasTrailing ? sig.trailing : sig.quals).push_back(st[k]);
    }

    sig.name = slice(st, nameStart, p);
    sig.params = splitParams(slice(st, p + 1, q));
    sig.conversion = opIdx != npos && sig.ret.empty();
    if (sig.conversion) {
        sig.convType = slice(st, opIdx + 1, p);
    }
    sig.spaceBeforeName = st[nameStart].spaceBefore;
    return sig;
}

// ---------------------------------------------------------------- parser ---

struct Scope {
    enum Kind {
        Global,
        Namespace,
        Class,
        Transparent
    };
    Kind kind = Global;
    std::string name; // class name
    NsPath ns; // namespace components
    bool skipMembers = false; // class template / anonymous class
    bool inSignals = false; // Qt "signals:" section
    std::set<std::string> nestedTypes;
};

// A stub (or note) generated from the header.
struct Item {
    NsPath ns;
    std::string text;
    bool isNote = false;
    std::vector<std::string> qualName; // class path + function name
    std::string paramKey, qualKey;
    size_t paramCount = 0;
};

// A function definition found in an existing .cpp.
struct Definition {
    NsPath ns;
    std::vector<std::string> qualName;
    std::string paramKey, qualKey;
    size_t paramCount = 0;
    size_t before = 0; // insertion point just ahead of it
    size_t after = 0; // insertion point just after it
};

struct Options {
    std::string header, output;
    bool trailingReturn = false;
    std::set<std::string, std::less<>> ignoreMacros;
};

class Parser {
public:
    Parser(std::string_view src, Mode mode, const Options& opt)
        : src_(src)
        , t_(tokenize(src, &directiveEnds_))
        , mode_(mode)
        , opt_(opt)
    {
        run();
    }

    const std::vector<Item>& items() const
    {
        return items_;
    }
    const std::vector<Definition>& definitions() const
    {
        return defs_;
    }

private:
    void run();
    void push(const Token& tk);
    void handleBrace();
    void handleStatement(const Tokens& st);
    void buildStub(const Tokens& st);
    void recordDefinition(const Tokens& st, size_t closeIdx);
    void closeScope();
    void recordNestedType(Scope& sc, const Tokens& st);
    void addItem(Item item);
    void note(const std::string& why, const Tokens& st);
    bool isNestedType(const std::string& name) const;
    NsPath currentNs() const;
    std::vector<std::string> classPath() const;
    size_t lineAfter(size_t pos) const;

    std::string_view src_;
    std::vector<size_t> directiveEnds_;
    Tokens t_;
    Mode mode_;
    const Options& opt_;
    size_t i_ = 0;
    int depth_ = 0;
    Tokens stmt_;
    size_t stmtStart_ = 0; // index in t_ of stmt_'s first token
    std::vector<Scope> scopes_;
    std::vector<Item> items_;
    std::vector<Definition> defs_;
};

void Parser::push(const Token& tk)
{
    if (stmt_.empty()) {
        stmtStart_ = i_;
    }
    stmt_.push_back(tk);
}

void Parser::run()
{
    static const std::set<std::string_view> labelWords
        = { "public", "protected", "private", "signals", "slots", "Q_SIGNALS", "Q_SLOTS" };

    scopes_.assign(1, Scope {});
    while (i_ < t_.size()) {
        const Token& tk = t_[i_];
        const std::string& s = tk.text;

        if (depth_ > 0) { // inside ( ) [ ] or an initialiser { }
            if (isOpen(s)) {
                ++depth_;
            } else if (isClose(s)) {
                --depth_;
            }
            push(tk);
            ++i_;
            continue;
        }
        if (stmt_.empty()) {
            if (s == ";") {
                ++i_;
                continue;
            }
            if (s == "Q_DECLARE_FLAGS" && i_ + 2 < t_.size() && t_[i_ + 1].text == "(") {
                if (scopes_.back().kind == Scope::Class) {
                    scopes_.back().nestedTypes.insert(t_[i_ + 2].text);
                }
                i_ = matchClose(t_, i_ + 1) + 1;
                continue;
            }
        }
        if (opt_.ignoreMacros.contains(s)) { // Q_OBJECT, Q_PROPERTY(...), export macros...
            ++i_;
            if (i_ < t_.size() && t_[i_].text == "(") {
                i_ = matchClose(t_, i_) + 1;
            }
            continue;
        }
        if (s == "(" || s == "[") {
            ++depth_;
            push(tk);
            ++i_;
            continue;
        }
        if (s == ";") {
            ++i_;
            handleStatement(clean(stmt_));
            stmt_.clear();
            continue;
        }
        if (s == ":" && scopes_.back().kind == Scope::Class && !stmt_.empty()) {
            bool label = true, signals = false;
            for (const auto& w : stmt_) {
                label = label && labelWords.contains(w.text);
                signals = signals || w.text == "signals" || w.text == "Q_SIGNALS";
            }
            if (label) {
                scopes_.back().inSignals = signals;
                stmt_.clear();
                ++i_;
                continue;
            }
        }
        if (s == "}") {
            ++i_;
            stmt_.clear();
            closeScope();
            continue;
        }
        if (s == "{") {
            handleBrace();
            continue;
        }
        push(tk);
        ++i_;
    }
}

void Parser::closeScope()
{
    if (scopes_.size() <= 1) {
        return;
    }
    const bool wasClass = scopes_.back().kind == Scope::Class;
    scopes_.pop_back();
    if (wasClass) { // skip "} instance;" trailer
        while (i_ < t_.size() && t_[i_].text != ";") {
            i_ = isOpen(t_[i_].text) ? matchClose(t_, i_) + 1 : i_ + 1;
        }
    }
}

void Parser::handleBrace()
{
    const Tokens st = clean(stmt_);
    Scope& cur = scopes_.back();

    // namespace [a::b] {
    const size_t k = (!st.empty() && st[0].text == "inline") ? 1 : 0;
    if (k < st.size() && st[k].text == "namespace") {
        Scope sc;
        sc.kind = Scope::Namespace;
        for (size_t j = k + 1; j < st.size(); ++j) {
            if (isIdent(st[j].text) && st[j].text != "inline") {
                sc.ns.push_back(st[j].text);
            }
        }
        if (sc.ns.empty()) {
            sc.ns.push_back(""); // anonymous
        }
        scopes_.push_back(sc);
        ++i_;
        stmt_.clear();
        return;
    }
    // extern "C" {
    if (st.size() == 2 && st[0].text == "extern" && isStringLit(st[1].text)) {
        Scope sc;
        sc.kind = Scope::Transparent;
        sc.skipMembers = cur.skipMembers;
        scopes_.push_back(sc);
        ++i_;
        stmt_.clear();
        return;
    }
    // enum / typedef'd aggregate: skip the body, finish the statement at ';'
    if (!st.empty() && (st[0].text == "enum" || st[0].text == "typedef")) {
        if (st[0].text == "enum" && cur.kind == Scope::Class) {
            const std::string nm = className(st, 1);
            if (!nm.empty()) {
                cur.nestedTypes.insert(nm);
            }
        }
        i_ = matchClose(t_, i_) + 1;
        return;
    }
    // class / struct / union definition
    const size_t head = afterTemplatePrefix(st);
    if (head < st.size() && !contains(st, "(")
        && (st[head].text == "class" || st[head].text == "struct" || st[head].text == "union")) {
        Scope sc;
        sc.kind = Scope::Class;
        sc.name = className(st, head + 1);
        sc.skipMembers = cur.skipMembers || head > 0 || sc.name.empty();
        if (head > 0 && !cur.skipMembers) {
            note(
                std::format(
                    "members of class template {} not stubbed (templates belong in headers)",
                    sc.name),
                {});
        }
        if (cur.kind == Scope::Class && !sc.name.empty()) {
            cur.nestedTypes.insert(sc.name);
        }
        scopes_.push_back(sc);
        ++i_;
        stmt_.clear();
        return;
    }
    // Function with a body - or a brace inside a constructor's
    // member-initialiser list ("Foo() : a{1}, b{2} {").
    if (contains(st, "(")) {
        bool initList = false, afterParen = false;
        for (const auto& tk : st) {
            if (tk.text == ")") {
                afterParen = true;
            }
            if (afterParen && tk.text == ":") {
                initList = true;
            }
        }
        const std::string& last = st.back().text;
        if (initList && (isIdent(last) || last == ">")) {
            push(t_[i_++]);
            depth_ = 1;
            return;
        }
        const size_t close = matchClose(t_, i_);
        if (mode_ == Mode::Source) {
            recordDefinition(st, close);
        }
        i_ = close + 1;
        stmt_.clear();
        return;
    }
    // Brace initialiser on a variable, etc.
    push(t_[i_++]);
    depth_ = 1;
}

void Parser::recordNestedType(Scope& sc, const Tokens& st)
{
    const std::string& f = st[0].text;
    if (f == "using" && st.size() >= 3 && st[2].text == "=") {
        sc.nestedTypes.insert(st[1].text);
    } else if (f == "typedef") {
        for (size_t k = 0; k + 2 < st.size(); ++k) {
            if (st[k].text == "(" && st[k + 1].text == "*" && isIdent(st[k + 2].text)) {
                sc.nestedTypes.insert(st[k + 2].text);
                return;
            }
        }
        const auto last = std::ranges::find_if(
            st | std::views::reverse, [](const Token& t) { return isIdent(t.text); });
        if (last != (st | std::views::reverse).end()) {
            sc.nestedTypes.insert(last->text);
        }
    } else if (
        (f == "class" || f == "struct" || f == "union" || f == "enum") && !contains(st, "(")) {
        const std::string nm = className(st, 1);
        if (!nm.empty()) {
            sc.nestedTypes.insert(nm);
        }
    }
}

void Parser::handleStatement(const Tokens& st)
{
    if (st.empty()) {
        return;
    }
    if (mode_ == Mode::Source) {
        // Only "= default;" / "= delete;" definitions end in ';' - plain
        // declarations (forward declarations, prototypes) don't count.
        const bool defaulted = st.size() >= 3 && st[st.size() - 2].text == "="
            && (st.back().text == "default" || st.back().text == "delete");
        if (defaulted) {
            recordDefinition(st, i_ - 1); // i_ - 1 is the ';'
        }
        return;
    }
    Scope& cur = scopes_.back();
    if (cur.kind == Scope::Class) {
        recordNestedType(cur, st);
    }
    if (cur.skipMembers || cur.inSignals) {
        return;
    }

    static const std::set<std::string_view> ignoreFirst = { "typedef",       "using",     "friend",
                                                            "static_assert", "namespace", "asm",
                                                            "Q_SIGNAL",      "concept" };
    const std::string& f = st[0].text;
    if (ignoreFirst.contains(f)) {
        return;
    }
    if ((f == "class" || f == "struct" || f == "union" || f == "enum") && !contains(st, "(")) {
        return;
    }
    if (f == "template") {
        if (contains(st, "(")) {
            note("template function not stubbed (templates belong in headers)", st);
        }
        return;
    }
    buildStub(st);
}

bool Parser::isNestedType(const std::string& name) const
{
    return std::ranges::any_of(scopes_, [&](const Scope& sc) {
        return sc.kind == Scope::Class && sc.nestedTypes.contains(name);
    });
}

NsPath Parser::currentNs() const
{
    NsPath ns;
    for (const auto& sc : scopes_) {
        if (sc.kind == Scope::Namespace) {
            ns.insert(ns.end(), sc.ns.begin(), sc.ns.end());
        }
    }
    return ns;
}

std::vector<std::string> Parser::classPath() const
{
    std::vector<std::string> path;
    for (const auto& sc : scopes_) {
        if (sc.kind == Scope::Class) {
            path.push_back(sc.name);
        }
    }
    return path;
}

size_t Parser::lineAfter(size_t pos) const
{
    const size_t nl = src_.find('\n', pos);
    return nl == npos ? src_.size() : nl + 1;
}

void Parser::recordDefinition(const Tokens& st, size_t closeIdx)
{
    const Scope& cur = scopes_.back();
    if (cur.skipMembers || st.empty() || st[0].text == "template") {
        return;
    }
    const auto sig = analyse(st, Mode::Source, cur.kind == Scope::Class);
    if (!sig) {
        return;
    }

    Definition d;
    d.ns = currentNs();
    d.qualName = classPath();
    std::ranges::copy(nameParts(sig->name), std::back_inserter(d.qualName));
    d.paramKey = paramKey(sig->params);
    d.qualKey = qualKey(sig->quals);
    d.paramCount = sig->params.size();
    // Just after whatever precedes it (token or #directive), so that any
    // comment above the definition stays attached to it.
    d.before = stmtStart_ > 0 ? lineAfter(t_[stmtStart_ - 1].end) : 0;
    for (const size_t e : directiveEnds_) {
        if (e <= t_[stmtStart_].begin) {
            d.before = std::max(d.before, e);
        }
    }
    d.after = lineAfter(t_[closeIdx].end);
    defs_.push_back(std::move(d));
}

void Parser::buildStub(const Tokens& st)
{
    const Scope& cur = scopes_.back();
    const bool inClass = cur.kind == Scope::Class;

    const auto sig = analyse(st, Mode::Header, inClass);
    if (!sig) {
        if (!sig.error().empty()) {
            note(sig.error(), st);
        }
        return;
    }

    // Return type.
    Tokens retType;
    bool useTrailing = false;
    if (sig->hasTrailing) {
        if (compact(sig->ret) != "auto") {
            return; // e.g. deduction guide
        }
        retType = sig->trailing;
        useTrailing = true;
    } else {
        retType = sig->conversion ? sig->convType : sig->ret;
    }
    if (retType.empty() && (!inClass || sig->name.back().text != cur.name)) {
        return; // not a ctor/dtor: macro?
    }
    const std::string retText = join(retType);
    if (retText == "auto" || retText == "decltype(auto)") {
        note("deduced return type cannot be stubbed out of line", st);
        return;
    }

    // Nested class types in a leading return type aren't in scope outside
    // the class, so switch to trailing-return syntax when one is used.
    if (inClass && !useTrailing && !sig->conversion && !retType.empty()) {
        useTrailing = opt_.trailingReturn;
        for (size_t k = 0; k < retType.size() && !useTrailing; ++k) {
            useTrailing = isIdent(retType[k].text) && (k == 0 || retType[k - 1].text != "::")
                && isNestedType(retType[k].text);
        }
    }

    // Assemble the definition.
    std::string qual;
    for (const auto& c : classPath()) {
        qual += c + "::";
    }
    const std::string name = qual + join(sig->name);

    std::string params;
    for (const auto& p : sig->params) {
        params += (params.empty() ? "" : ", ") + join(p);
    }
    const std::string quals = sig->quals.empty() ? "" : " " + join(sig->quals);

    std::string head;
    if (useTrailing) {
        head = std::format("auto {}({}){} -> {}", name, params, quals, retText);
    } else {
        Tokens lead = sig->specs;
        if (!sig->conversion) {
            lead.insert(lead.end(), retType.begin(), retType.end());
        }
        head = join(lead);
        if (!head.empty() && (sig->spaceBeforeName || isWordLike(lead.back().text))) {
            head += ' ';
        }
        head += std::format("{}({}){}", name, params, quals);
    }

    // Silence unused-parameter warnings inside the stub itself, so the
    // suppression goes away when the body is replaced.
    std::string body = "    // TODO (auto-generated stub)\n";
    for (const auto& p : sig->params) {
        if (const auto idx = paramNameIndex(p)) {
            body += std::format("    (void){}; // TODO - remove\n", p[*idx].text);
        }
    }

    if (!retType.empty() && retText != "void") {
        const std::string& last = retType.back().text;
        if (last == "&" || last == "&&") {
            body += std::format(
                "    static {} stub_return_value_{{}};\n",
                join(slice(retType, 0, retType.size() - 1)));
            body += (last == "&")
                ? "    return stub_return_value_;\n"
                : std::format("    return static_cast<{}>(stub_return_value_);\n", retText);
        } else {
            body += "    return {};\n";
        }
    }

    Item item;
    item.text = std::format("{}\n{{\n{}}}\n", head, body);
    item.qualName = classPath();
    std::ranges::copy(nameParts(sig->name), std::back_inserter(item.qualName));
    item.paramKey = paramKey(sig->params);
    item.qualKey = qualKey(sig->quals);
    item.paramCount = sig->params.size();
    addItem(std::move(item));
}

void Parser::addItem(Item item)
{
    item.ns = currentNs();
    items_.push_back(std::move(item));
}

void Parser::note(const std::string& why, const Tokens& st)
{
    std::string decl = join(st);
    if (decl.size() > 70) {
        decl = decl.substr(0, 67) + "...";
    }
    Item item;
    item.isNote = true;
    item.text = std::format("// h2cpp: {}{}\n", why, decl.empty() ? "" : ":\n//   " + decl);
    addItem(std::move(item));
}

// ------------------------------------------------------------- rendering ---

std::string nsLabel(std::span<const std::string> chunk)
{
    return chunk.size() == 1 && chunk[0].empty() ? "anonymous namespace"
                                                 : "namespace " + joinNs(chunk);
}

// Renders items in order, opening/closing namespaces as needed. Output starts
// and ends inside namespace `base`; every item's namespace must lie within it.
std::string renderItems(std::span<const Item* const> items, const NsPath& base)
{
    std::string out;
    std::vector<NsPath> blocks; // namespaces opened here, innermost last
    NsPath cur = base;

    auto closeBlock = [&] {
        out += std::format("\n}}  // {}\n", nsLabel(blocks.back()));
        cur.resize(cur.size() - blocks.back().size());
        blocks.pop_back();
    };

    for (const Item* item : items) {
        while (!blocks.empty() && !startsWith(item->ns, cur)) {
            closeBlock();
        }
        for (size_t k = cur.size(); k < item->ns.size();) {
            NsPath chunk; // anonymous namespaces open alone
            if (item->ns[k].empty()) {
                chunk.push_back(item->ns[k++]);
            } else {
                while (k < item->ns.size() && !item->ns[k].empty()) {
                    chunk.push_back(item->ns[k++]);
                }
            }
            out += chunk[0].empty() ? "\nnamespace {\n"
                                    : std::format("\nnamespace {} {{\n", joinNs(chunk));
            cur.insert(cur.end(), chunk.begin(), chunk.end());
            blocks.push_back(std::move(chunk));
        }
        out += "\n" + item->text;
    }
    while (!blocks.empty()) {
        closeBlock();
    }
    return out;
}

std::string baseName(std::string_view path)
{
    return fs::path(path).filename().string();
}

std::string freshFile(const Options& opt, const std::vector<Item>& items)
{
    std::string out = std::format(
        "// Stub implementations generated by h2cpp from {0}\n"
        "#include \"{0}\"\n",
        baseName(opt.header));
    std::vector<const Item*> all;
    for (const auto& item : items) {
        all.push_back(&item);
    }
    return out + renderItems(all, {});
}

// ----------------------------------------------------------------- merge ---

const Definition*
findDefinition(const std::vector<Item>& items, const Item& h, const std::vector<Definition>& defs)
{
    NsPath full = h.ns;
    full.insert(full.end(), h.qualName.begin(), h.qualName.end());

    // A definition matches if its qualified name is a suffix of the header's
    // (so "using namespace x;" style files still match) with the same cv/ref.
    auto nameMatches = [&](const Definition& d) {
        NsPath df = d.ns;
        df.insert(df.end(), d.qualName.begin(), d.qualName.end());
        return df.size() >= h.qualName.size() && endsWith(full, df) && d.qualKey == h.qualKey;
    };

    for (const auto& d : defs) {
        if (nameMatches(d) && d.paramKey == h.paramKey) {
            return &d;
        }
    }

    // Parameter spellings can legitimately differ (aliases, "using namespace
    // std"...), so if there's no overload ambiguity, settle for arity.
    const auto sameArity = std::ranges::count_if(items, [&](const Item& o) {
        return !o.isNote && o.ns == h.ns && o.qualName == h.qualName && o.qualKey == h.qualKey
            && o.paramCount == h.paramCount;
    });
    if (sameArity == 1) {
        for (const auto& d : defs) {
            if (nameMatches(d) && d.paramCount == h.paramCount) {
                return &d;
            }
        }
    }
    return nullptr;
}

struct MergeResult {
    std::string text;
    size_t added = 0;
};

// Inserts stubs for header functions missing from `src`, each placed after
// its nearest predecessor (in header order) that the file already defines.
MergeResult
merge(std::string src, const std::vector<Item>& items, const std::vector<Definition>& defs)
{
    if (!src.empty() && !src.ends_with('\n')) {
        src += '\n';
    }

    std::vector<const Definition*> found(items.size(), nullptr);
    for (size_t i = 0; i < items.size(); ++i) {
        if (!items[i].isNote) {
            found[i] = findDefinition(items, items[i], defs);
        }
    }

    struct Group {
        size_t pos;
        NsPath base;
        std::vector<const Item*> items;
    };
    std::vector<Group> groups;
    size_t added = 0;

    for (size_t i = 0; i < items.size(); ++i) {
        const Item& item = items[i];
        if (item.isNote || found[i]) {
            continue;
        }

        std::optional<std::pair<size_t, NsPath>> anchor;
        for (size_t j = i; j-- > 0;) {
            if (found[j]) {
                if (startsWith(item.ns, found[j]->ns)) {
                    anchor.emplace(found[j]->after, found[j]->ns);
                }
                break;
            }
        }
        if (!anchor) {
            for (size_t j = i + 1; j < items.size(); ++j) {
                if (found[j]) {
                    if (startsWith(item.ns, found[j]->ns)) {
                        anchor.emplace(found[j]->before, found[j]->ns);
                    }
                    break;
                }
            }
        }
        if (!anchor) {
            anchor.emplace(src.size(), NsPath {});
        }

        if (!groups.empty() && groups.back().pos == anchor->first
            && groups.back().base == anchor->second) {
            groups.back().items.push_back(&item);
        } else {
            groups.push_back({ anchor->first, anchor->second, { &item } });
        }
        ++added;
    }

    // Insert back to front so earlier offsets stay valid; for equal offsets
    // the later group goes in first, leaving header order intact.
    std::ranges::stable_sort(groups, {}, &Group::pos);
    for (const auto& g : groups | std::views::reverse) {
        src.insert(g.pos, renderItems(g.items, g.base));
    }
    return { std::move(src), added };
}

// ------------------------------------------------------------------ main ---

void printUsage(std::FILE* f)
{
    std::print(
        f,
        "Usage: h2cpp [options] header.h\n"
        "\n"
        "Writes stub definitions for every function declared in a C++ header.\n"
        "Non-void functions return a value-initialised result ({{}}), or a\n"
        "reference to a static default object for reference return types.\n"
        "\n"
        "Options:\n"
        "  -o, --output FILE       write to FILE instead of stdout. If FILE exists,\n"
        "                          only stubs it lacks are added, in header order\n"
        "  -t, --trailing-return   use 'auto f() -> T' for every member function\n"
        "                          (helps when return types are inherited nested types)\n"
        "  -i, --ignore MACRO      treat MACRO (and any (...) after it) as noise,\n"
        "                          e.g. -i MYLIB_EXPORT; may be repeated\n"
        "  -h, --help              show this help\n");
}

std::optional<std::string> readFile(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return std::move(ss).str();
}

bool writeFile(const fs::path& path, std::string_view text)
{
    fs::path tmp = path;
    tmp += ".h2cpp-tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!(out << text)) {
            return false;
        }
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(tmp, ec);
    }
    return !ec;
}

// Parses the command line; returns an exit code if the program should stop.
std::optional<int> parseArgs(std::span<char* const> args, Options& opt)
{
    auto fail = [](std::string_view msg) {
        std::println(stderr, "h2cpp: {}\nTry 'h2cpp --help' for more information.", msg);
        return 2;
    };

    for (size_t a = 0; a < args.size(); ++a) {
        const std::string_view arg = args[a];
        auto value = [&]() -> std::optional<std::string_view> {
            if (a + 1 >= args.size()) {
                return std::nullopt;
            }
            return args[++a];
        };

        if (arg == "-h" || arg == "--help") {
            printUsage(stdout);
            return 0;
        } else if (arg == "-o" || arg == "--output") {
            const auto v = value();
            if (!v) {
                return fail(std::format("{} needs a file name", arg));
            }
            opt.output = *v;
        } else if (arg.starts_with("--output=")) {
            opt.output = arg.substr(9);
        } else if (arg == "-t" || arg == "--trailing-return") {
            opt.trailingReturn = true;
        } else if (arg == "-i" || arg == "--ignore") {
            const auto v = value();
            if (!v) {
                return fail(std::format("{} needs a macro name", arg));
            }
            opt.ignoreMacros.emplace(*v);
        } else if (arg.starts_with('-') && arg.size() > 1) {
            return fail(std::format("unknown option '{}'", arg));
        } else if (!opt.header.empty()) {
            return fail("only one header may be given");
        } else {
            opt.header = arg;
        }
    }
    if (opt.header.empty()) {
        return fail("no header file given");
    }
    return std::nullopt;
}

} // namespace

int main(int argc, char* argv[])
{
    Options opt;
    opt.ignoreMacros = { "Q_OBJECT",
                         "Q_GADGET",
                         "Q_NAMESPACE",
                         "Q_NAMESPACE_EXPORT",
                         "Q_PROPERTY",
                         "Q_ENUM",
                         "Q_ENUMS",
                         "Q_ENUM_NS",
                         "Q_FLAG",
                         "Q_FLAGS",
                         "Q_FLAG_NS",
                         "Q_DISABLE_COPY",
                         "Q_DISABLE_COPY_MOVE",
                         "Q_DISABLE_MOVE",
                         "Q_DECLARE_PRIVATE",
                         "Q_DECLARE_PRIVATE_D",
                         "Q_DECLARE_PUBLIC",
                         "Q_INTERFACES",
                         "Q_CLASSINFO",
                         "Q_PLUGIN_METADATA",
                         "Q_DECLARE_METATYPE",
                         "Q_DECLARE_OPERATORS_FOR_FLAGS",
                         "Q_DECLARE_INTERFACE",
                         "Q_INVOKABLE",
                         "Q_SCRIPTABLE",
                         "Q_SLOT",
                         "Q_REVISION",
                         "QT_BEGIN_NAMESPACE",
                         "QT_END_NAMESPACE",
                         "QT_FORWARD_DECLARE_CLASS",
                         "QML_ELEMENT",
                         "QML_NAMED_ELEMENT",
                         "QML_SINGLETON",
                         "QML_UNCREATABLE",
                         "QML_ANONYMOUS" };

    if (const auto rc = parseArgs(std::span(argv + 1, argv + argc), opt)) {
        return *rc;
    }

    const auto header = readFile(opt.header);
    if (!header) {
        std::println(stderr, "h2cpp: cannot read {}", opt.header);
        return 1;
    }
    const auto items = Parser(*header, Mode::Header, opt).items();
    const auto stubCount = std::ranges::count(items, false, &Item::isNote);

    if (opt.output.empty()) {
        std::print("{}", freshFile(opt, items));
        return 0;
    }

    const fs::path out = opt.output;
    std::error_code ec;
    if (fs::exists(out) && fs::equivalent(out, opt.header, ec)) {
        std::println(stderr, "h2cpp: refusing to overwrite the header itself");
        return 1;
    }

    if (!fs::exists(out)) {
        if (!writeFile(out, freshFile(opt, items))) {
            std::println(stderr, "h2cpp: cannot write {}", opt.output);
            return 1;
        }
        std::println(stderr, "h2cpp: created {} with {} stub(s)", opt.output, stubCount);
        return 0;
    }

    const auto existing = readFile(out);
    if (!existing) {
        std::println(stderr, "h2cpp: cannot read {}", opt.output);
        return 1;
    }
    const Parser source(*existing, Mode::Source, opt);
    auto [text, added] = merge(*existing, items, source.definitions());
    if (added == 0) {
        std::println(stderr, "h2cpp: {} is up to date", opt.output);
        return 0;
    }
    if (!writeFile(out, text)) {
        std::println(stderr, "h2cpp: cannot write {}", opt.output);
        return 1;
    }
    std::println(stderr, "h2cpp: added {} stub(s) to {}", added, opt.output);
    return 0;
}
