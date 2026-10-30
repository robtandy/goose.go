// Goose compiler — the runtime the Go backend writes (issue #19): runtime/go,
// the package github.com/robtandy/goose.go/runtime/go, whose source the
// compiler's build embeds (cmake/go_runtime.cmake, go_runtime_files), so
// that there is one copy of it. A file the backend writes holds the part of
// it the program uses (--go-runtime=embed, the default), all of it
// (--go-runtime=full), or none, importing the package instead
// (--go-runtime=import); the runtime can also go to a file of its own,
// which the programs of a package share (--go-runtime-file).
//
// What the part a program uses is comes from reading the Go: the runtime's
// top-level declarations (GoParseDecls), each with the names it declares and
// the names its text uses, and the names the program's Go uses; a
// declaration is written where a name the program uses reaches it, a method
// where its type is written and its name is used after a dot (Error, which
// the error interface asks for, always). The same reading renames the
// runtime's names, in the runtime and in the program alike: a name the
// runtime marks //goose:internal, which it exports only for a file that
// imports it, is unexported in a file that holds it (GsEscape is gsEscape),
// --go-unexported unexports them all (GsSlice is gsSlice), and a file that
// imports the runtime names them as the package's (gsrt.GsSlice).
//
// The text is gofmt's, and every rename keeps it so: unexporting a name
// changes no length, and the program's Go, which a rename lengthens, is laid
// out afterwards (GoFormat).
#pragma once

#include "goose_go_runtime.h"

namespace goose {

// The runtime's package, and the name a file that imports it gives it.
constexpr const char *GO_RUNTIME_PATH = "github.com/robtandy/goose.go/runtime/go";
constexpr const char *GO_RUNTIME_NAME = "gsrt";

// ------------------------------------------------------------------
// Go source as the backend reads it: tokens, enough of them to tell an
// identifier from what a string or a comment holds, a selector from a name,
// and a top-level declaration's extent.

struct GoTok {
    enum Kind : uint8_t { IDENT, PUNCT, LIT, COMMENT, NL };
    Kind kind;
    uint32_t b, e;   // Its bytes in the text.
    uint32_t line;   // The line it starts on, from 0.
};

static vector<GoTok> GoTokenize(string_view s) {
    vector<GoTok> toks;
    uint32_t line = 0;
    size_t i = 0, n = s.size();
    auto idchar = [](unsigned char c) { return isalnum(c) || c == '_' || c >= 0x80; };
    auto push = [&](GoTok::Kind k, size_t b, size_t e, uint32_t l) {
        toks.push_back({ k, (uint32_t)b, (uint32_t)e, l });
    };
    while (i < n) {
        auto c = (unsigned char)s[i];
        auto b = i;
        auto l0 = line;
        if (c == '\n') {
            push(GoTok::NL, i, i + 1, line++);
            i++;
        } else if (c == ' ' || c == '\t' || c == '\r') {
            i++;
        } else if (c == '/' && i + 1 < n && s[i + 1] == '/') {
            while (i < n && s[i] != '\n') i++;
            push(GoTok::COMMENT, b, i, l0);
        } else if (c == '/' && i + 1 < n && s[i + 1] == '*') {
            for (i += 2; i < n && !(s[i] == '*' && i + 1 < n && s[i + 1] == '/'); i++)
                if (s[i] == '\n') line++;
            i = std::min(i + 2, n);
            push(GoTok::COMMENT, b, i, l0);
        } else if (c == '"' || c == '\'') {
            for (i++; i < n && s[i] != (char)c && s[i] != '\n'; i++)
                if (s[i] == '\\') i++;
            if (i < n && s[i] == (char)c) i++;
            push(GoTok::LIT, b, i, l0);
        } else if (c == '`') {
            for (i++; i < n && s[i] != '`'; i++)
                if (s[i] == '\n') line++;
            if (i < n) i++;
            push(GoTok::LIT, b, i, l0);
        } else if (isdigit(c) || (c == '.' && i + 1 < n && isdigit((unsigned char)s[i + 1]))) {
            // A number, its exponent's sign included: e for a decimal one, p
            // for any, which a hex digit e is not.
            auto hex = c == '0' && i + 1 < n && (s[i + 1] == 'x' || s[i + 1] == 'X');
            for (i++; i < n; i++) {
                auto d = (unsigned char)s[i];
                if (isalnum(d) || d == '_' || d == '.') continue;
                auto p = s[i - 1];
                if ((d == '+' || d == '-') && i + 1 < n && isdigit((unsigned char)s[i + 1]) &&
                    (((p == 'e' || p == 'E') && !hex) || p == 'p' || p == 'P'))
                    continue;
                break;
            }
            push(GoTok::LIT, b, i, l0);
        } else if (idchar(c)) {
            while (i < n && idchar((unsigned char)s[i])) i++;
            push(GoTok::IDENT, b, i, l0);
        } else if (c == '.' && i + 2 < n && s[i + 1] == '.' && s[i + 2] == '.') {
            i += 3;
            push(GoTok::PUNCT, b, i, l0);
        } else {
            i++;
            push(GoTok::PUNCT, b, i, l0);
        }
    }
    return toks;
}

// The token before and after toks[i] that a comment or a line break is not:
// -1 for none.
static int GoPrevTok(const vector<GoTok> &toks, int i) {
    for (i--; i >= 0; i--)
        if (toks[i].kind != GoTok::COMMENT && toks[i].kind != GoTok::NL) return i;
    return -1;
}

static int GoNextTok(const vector<GoTok> &toks, int i) {
    for (i++; i < (int)toks.size(); i++)
        if (toks[i].kind != GoTok::COMMENT && toks[i].kind != GoTok::NL) return i;
    return -1;
}

static bool GoTokIs(string_view s, const GoTok &t, string_view text) {
    return t.e - t.b == text.size() && s.compare(t.b, t.e - t.b, text) == 0;
}

// Whether the identifier toks[i] follows a dot: a field, a method, or a
// package's member, rather than a name of the file's scope.
static bool GoIsSelector(string_view s, const vector<GoTok> &toks, int i) {
    auto p = GoPrevTok(toks, i);
    return p >= 0 && GoTokIs(s, toks[p], ".");
}

// Whether the identifier toks[i] is the name a method declaration gives its
// method, after its receiver: `func (r *T) Name(`, which no name of the
// file's scope is either (where a func literal's result type, `func(x T)
// R {`, is one).
static bool GoIsMethodName(string_view s, const vector<GoTok> &toks, int i) {
    auto p = GoPrevTok(toks, i);
    if (p < 0 || !GoTokIs(s, toks[p], ")")) return false;
    auto n = GoNextTok(toks, i);
    if (n < 0 || !GoTokIs(s, toks[n], "(")) return false;
    int depth = 0;
    for (; p >= 0; p--) {
        if (toks[p].kind != GoTok::PUNCT) continue;
        if (GoTokIs(s, toks[p], ")")) depth++;
        else if (GoTokIs(s, toks[p], "(") && --depth == 0) break;
    }
    auto f = p > 0 ? GoPrevTok(toks, p) : -1;
    return f >= 0 && toks[f].kind == GoTok::IDENT && GoTokIs(s, toks[f], "func");
}

// A top-level declaration of Go source: its kind (func, type, var, const,
// import or package), the names it declares at the package level, or, for
// a method, its receiver's type and its name; its text, from the first line
// of its doc comment through its last line and the line break after it;
// and the names its text uses, apart (refs) from those after a dot (sels),
// and the imported packages (pkgs, by the names a file's imports give
// them).
struct GoDecl {
    string kind;
    vector<string> names;
    string recv, method;
    size_t b = 0, e = 0;    // Its text.
    size_t db = 0;          // Where the declaration, its doc comment past, starts.
    int section = -1;       // The comment it follows, in GoSource::sections.
    bool internal = false;  // //goose:internal: exported only for a file that imports it.
    set<string> refs, sels, pkgs;
    vector<pair<string, string>> imports;   // An import declaration's (name, path).
};

struct GoSource {
    string text;
    vector<GoTok> toks;
    vector<GoDecl> decls;
    // The comments at the package level that no declaration has for its own
    // documentation, which head the declarations after them: a section's.
    vector<pair<size_t, size_t>> sections;
    map<string, string> imports;   // Name -> path, from its import declarations.
};

// The path's last element: the name a package's own name is, for the
// standard library's (math/bits is bits).
static string GoPathName(string_view path) {
    auto slash = path.find_last_of('/');
    return string(slash == string_view::npos ? path : path.substr(slash + 1));
}

// Reads text, Go source laid out as gofmt lays it out (or as the backend
// writes its Go, a declaration from the start of a line), into its
// top-level declarations.
static GoSource GoParseDecls(string text) {
    GoSource src;
    src.text = std::move(text);
    string_view s = src.text;
    src.toks = GoTokenize(s);
    auto &toks = src.toks;
    // Each line's start, and what it holds: nothing (a blank line), only
    // comments, code, or the inside of a token that began on a line above.
    vector<size_t> lstart = { 0 };
    for (size_t i = 0; i < s.size(); i++)
        if (s[i] == '\n') lstart.push_back(i + 1);
    auto nlines = lstart.size();
    enum { BLANK, COMMENTS, CODE, COVERED };
    vector<int> lkind(nlines, BLANK);
    for (auto &t : toks) {
        if (t.kind == GoTok::NL) continue;
        auto &k = lkind[t.line];
        if (t.kind == GoTok::COMMENT) {
            if (k == BLANK) k = COMMENTS;
        } else {
            k = CODE;
        }
        auto last = t.line;
        for (auto i = t.b; i < t.e; i++)
            if (s[i] == '\n') lkind[++last] = COVERED;
    }
    auto lineend = [&](uint32_t l) { return l + 1 < nlines ? lstart[l + 1] : s.size(); };
    // The first token of each line.
    vector<int> first(nlines, -1);
    for (int i = 0; i < (int)toks.size(); i++)
        if (toks[i].kind != GoTok::NL && first[toks[i].line] < 0) first[toks[i].line] = i;
    static const set<string_view> keywords = { "func", "type", "var", "const", "import", "package" };
    vector<pair<uint32_t, uint32_t>> declines;   // Each declaration's first and last line.
    int depth = 0;
    for (int i = 0; i < (int)toks.size(); i++) {
        auto &t = toks[i];
        auto word = string_view(s).substr(t.b, t.e - t.b);
        if (t.kind == GoTok::PUNCT) {
            if (word == "(" || word == "[" || word == "{") depth++;
            else if (word == ")" || word == "]" || word == "}") depth--;
            continue;
        }
        if (depth != 0 || t.kind != GoTok::IDENT || first[t.line] != i || !keywords.count(word)) continue;
        GoDecl d;
        d.kind = string(word);
        // Through the line break where a statement ends at depth 0, as Go's
        // semicolons do.
        int j = i + 1, dd = 0, last = i;
        for (; j < (int)toks.size(); j++) {
            auto &u = toks[j];
            auto w = string_view(s).substr(u.b, u.e - u.b);
            if (u.kind == GoTok::PUNCT) {
                if (w == "(" || w == "[" || w == "{") dd++;
                else if (w == ")" || w == "]" || w == "}") dd--;
            }
            if (u.kind == GoTok::NL && dd == 0) {
                auto &p = toks[last];
                auto pw = string_view(s).substr(p.b, p.e - p.b);
                if (p.kind == GoTok::IDENT || p.kind == GoTok::LIT || pw == ")" || pw == "]" || pw == "}") break;
            }
            if (u.kind != GoTok::COMMENT && u.kind != GoTok::NL) last = j;
        }
        auto lastline = toks[std::min(j, (int)toks.size() - 1)].line;
        if (j >= (int)toks.size()) lastline = toks[last].line;
        // Its doc comment: the lines of nothing but comments right above it.
        auto docline = t.line;
        while (docline > 0 && lkind[docline - 1] == COMMENTS) docline--;
        d.b = lstart[docline];
        d.db = lstart[t.line];
        d.e = lineend(lastline);
        for (auto l = docline; l < t.line; l++) {
            auto line = string_view(s).substr(lstart[l], lineend(l) - lstart[l]);
            if (line.substr(0, 16) == "//goose:internal") d.internal = true;
        }
        // What it declares.
        auto ident = [&](int k) { return k >= 0 && toks[k].kind == GoTok::IDENT; };
        auto text = [&](int k) { return string(s.substr(toks[k].b, toks[k].e - toks[k].b)); };
        auto n1 = GoNextTok(toks, i);
        auto groupnames = [&](int open, int close) {
            // The names starting each line of a group, and those after a
            // comma there.
            for (int k = open + 1; k < close; k++) {
                if (toks[k].kind != GoTok::IDENT || first[toks[k].line] != k) continue;
                int dk = 0;
                for (int m = open + 1; m < k; m++) {
                    auto w = string_view(s).substr(toks[m].b, toks[m].e - toks[m].b);
                    if (toks[m].kind == GoTok::PUNCT) {
                        if (w == "(" || w == "[" || w == "{") dk++;
                        else if (w == ")" || w == "]" || w == "}") dk--;
                    }
                }
                if (dk != 0) continue;
                d.names.push_back(text(k));
                for (auto m = GoNextTok(toks, k); m >= 0 && GoTokIs(s, toks[m], ","); m = GoNextTok(toks, m)) {
                    m = GoNextTok(toks, m);
                    if (!ident(m)) break;
                    d.names.push_back(text(m));
                }
            }
        };
        auto matching = [&](int open) {
            int dk = 0;
            for (int m = open; m < (int)toks.size(); m++) {
                if (toks[m].kind != GoTok::PUNCT) continue;
                auto w = string_view(s).substr(toks[m].b, toks[m].e - toks[m].b);
                if (w == "(" || w == "[" || w == "{") dk++;
                else if ((w == ")" || w == "]" || w == "}") && --dk == 0) return m;
            }
            return (int)toks.size();
        };
        if (d.kind == "func" && n1 >= 0) {
            if (GoTokIs(s, toks[n1], "(")) {
                auto close = matching(n1);
                for (int m = n1 + 1; m < close; m++) {
                    if (GoTokIs(s, toks[m], "[")) break;
                    if (ident(m)) d.recv = text(m);
                }
                auto m = GoNextTok(toks, close);
                if (ident(m)) d.method = text(m);
            } else if (ident(n1)) {
                d.names.push_back(text(n1));
            }
        } else if ((d.kind == "type" || d.kind == "var" || d.kind == "const") && n1 >= 0) {
            if (GoTokIs(s, toks[n1], "(")) {
                groupnames(n1, matching(n1));
            } else if (ident(n1)) {
                d.names.push_back(text(n1));
                if (d.kind != "type")
                    for (auto m = GoNextTok(toks, n1); m >= 0 && GoTokIs(s, toks[m], ","); m = GoNextTok(toks, m)) {
                        m = GoNextTok(toks, m);
                        if (!ident(m)) break;
                        d.names.push_back(text(m));
                    }
            }
        } else if (d.kind == "import") {
            for (int m = i + 1; m <= j && m < (int)toks.size(); m++) {
                if (toks[m].kind != GoTok::LIT || s[toks[m].b] != '"') continue;
                auto path = text(m);
                path = path.substr(1, path.size() - 2);
                auto p = GoPrevTok(toks, m);
                auto name = p > i && ident(p) ? text(p) : GoPathName(path);
                d.imports.push_back({ name, path });
                src.imports[name] = path;
            }
        } else if (d.kind == "package" && ident(n1)) {
            d.names.push_back(text(n1));
        }
        // What it uses.
        for (int m = i + 1; m <= j && m < (int)toks.size(); m++) {
            if (toks[m].kind != GoTok::IDENT) continue;
            auto w = text(m);
            if (!d.method.empty() && w == d.method && GoIsMethodName(s, toks, m)) continue;
            if (GoIsSelector(s, toks, m)) {
                d.sels.insert(w);
                continue;
            }
            d.refs.insert(w);
            auto nx = GoNextTok(toks, m);
            if (nx >= 0 && GoTokIs(s, toks[nx], ".")) d.pkgs.insert(w);
        }
        for (auto &nm : d.names) d.refs.erase(nm);
        declines.push_back({ docline, lastline });
        src.decls.push_back(std::move(d));
        i = std::min(j, (int)toks.size() - 1);
        depth = 0;
    }
    // The comments at the package level that are no declaration's own: each
    // heads the declarations after it.
    size_t di = 0;
    for (uint32_t l = 0; l < nlines; l++) {
        while (di < declines.size() && declines[di].second < l) di++;
        if (lkind[l] != COMMENTS || (di < declines.size() && declines[di].first <= l)) continue;
        auto e = l;
        while (e + 1 < nlines && lkind[e + 1] == COMMENTS) e++;
        // A package clause's documentation is the clause's.
        if (di < declines.size() && declines[di].first == e + 1) {
            l = e;
            continue;
        }
        src.sections.push_back({ lstart[l], lineend(e) });
        for (auto k = di; k < src.decls.size(); k++) src.decls[k].section = (int)src.sections.size() - 1;
        l = e;
    }
    for (auto &d : src.decls)
        if (!d.pkgs.empty()) {
            set<string> pkgs;
            for (auto &p : d.pkgs)
                if (src.imports.count(p)) pkgs.insert(p);
            d.pkgs = std::move(pkgs);
        }
    return src;
}

// Renames the identifiers of Go source: those that follow no dot by `code`,
// every one by `every`, and the words of its comments by `words`.
static string GoRenameIdents(string_view s, const unordered_map<string, string> &code,
                             const unordered_map<string, string> &every,
                             const unordered_map<string, string> &words) {
    if (code.empty() && every.empty() && words.empty()) return string(s);
    auto toks = GoTokenize(s);
    string out;
    out.reserve(s.size() + s.size() / 8);
    size_t at = 0;
    for (int i = 0; i < (int)toks.size(); i++) {
        auto &t = toks[i];
        if (t.kind == GoTok::IDENT) {
            auto w = string(s.substr(t.b, t.e - t.b));
            auto it = every.find(w);
            const string *to = it != every.end() ? &it->second : nullptr;
            if (!to && !code.empty()) {
                auto c = code.find(w);
                if (c != code.end() && !GoIsSelector(s, toks, i) && !GoIsMethodName(s, toks, i)) to = &c->second;
            }
            if (to) {
                out.append(s.substr(at, t.b - at));
                out += *to;
                at = t.e;
            }
        } else if (t.kind == GoTok::COMMENT && !words.empty()) {
            for (size_t p = t.b; p < t.e;) {
                auto c = (unsigned char)s[p];
                if (!(isalpha(c) || c == '_') || (p > t.b && (isalnum((unsigned char)s[p - 1]) || s[p - 1] == '_'))) {
                    p++;
                    continue;
                }
                auto q = p;
                while (q < t.e && (isalnum((unsigned char)s[q]) || s[q] == '_')) q++;
                auto it = words.find(string(s.substr(p, q - p)));
                if (it != words.end()) {
                    out.append(s.substr(at, p - at));
                    out += it->second;
                    at = q;
                }
                p = q;
            }
        }
    }
    out.append(s.substr(at));
    return out;
}

// The names, and the names after a dot, that Go source uses, and of those
// the packages it names (an identifier a dot follows) among `known`.
struct GoUses {
    set<string> refs, sels, pkgs;
};

static void GoScanUses(string_view s, const map<string, string> &known, GoUses &uses) {
    auto toks = GoTokenize(s);
    for (int i = 0; i < (int)toks.size(); i++) {
        if (toks[i].kind != GoTok::IDENT) continue;
        auto w = string(s.substr(toks[i].b, toks[i].e - toks[i].b));
        if (GoIsSelector(s, toks, i) || GoIsMethodName(s, toks, i)) {
            uses.sels.insert(w);
            continue;
        }
        uses.refs.insert(w);
        auto nx = GoNextTok(toks, i);
        if (nx >= 0 && GoTokIs(s, toks[nx], ".") && known.count(w)) uses.pkgs.insert(w);
    }
}

// The name unexported: its first letter lowercase.
static string GoUnexport(const string &n) {
    string s = n;
    if (!s.empty()) s[0] = (char)tolower((unsigned char)s[0]);
    return s;
}

// ------------------------------------------------------------------
// The runtime, read once from what the build embedded.

struct GoRuntimeModel {
    vector<GoSource> files;
    vector<string> names;   // runtime/go's file names, in files' order.
    // A declaration by file and index; the declarations of each top-level
    // name, and the methods of each type.
    using Ref = pair<int, int>;
    unordered_map<string, vector<Ref>> byname;
    unordered_map<string, vector<Ref>> methods;
    set<string> toplevel, internal;
    map<string, string> imports;   // The packages the runtime imports, by name.
    const GoDecl &D(Ref r) const { return files[r.first].decls[r.second]; }
};

static const GoRuntimeModel &GoRuntime() {
    static const GoRuntimeModel rt = [] {
        GoRuntimeModel m;
        for (auto &f : go_runtime_files) {
            string text;
            for (int i = 0; i < f.nparts; i++) text += f.parts[i];
            text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
            m.names.push_back(f.name);
            m.files.push_back(GoParseDecls(std::move(text)));
        }
        for (int fi = 0; fi < (int)m.files.size(); fi++) {
            auto &src = m.files[fi];
            for (auto &[n, p] : src.imports) m.imports[n] = p;
            for (int di = 0; di < (int)src.decls.size(); di++) {
                auto &d = src.decls[di];
                if (d.kind == "package" || d.kind == "import") continue;
                if (!d.recv.empty()) {
                    m.methods[d.recv].push_back({ fi, di });
                    continue;
                }
                for (auto &n : d.names) {
                    if (n == "_") continue;
                    m.byname[n].push_back({ fi, di });
                    m.toplevel.insert(n);
                    if (d.internal) m.internal.insert(n);
                }
            }
        }
        // Unexporting a name must not make it another's: the renames that
        // unexported names (GoUnexport) are one to one.
        for (auto &n : m.toplevel)
            if (isupper((unsigned char)n[0]) && m.toplevel.count(GoUnexport(n)))
                throw CompileError { cat("internal: the Go runtime declares both ", n, " and ", GoUnexport(n)) };
        return m;
    }();
    return rt;
}

// Whether a name is the runtime's, in any of the forms a file gives it: as
// it declares it, unexported, or as --go-unexported's alias of the block
// type names it (StackBlock).
static bool GoRuntimeName(string_view n) {
    auto &rt = GoRuntime();
    string s(n);
    if (rt.toplevel.count(s) || s == "StackBlock") return true;
    for (auto &t : rt.toplevel)
        if (GoUnexport(t) == s) return true;
    return false;
}

// The runtime's declarations a file needs, in the runtime's order: those
// the names in `refs` reach, a type's methods where their names are in
// `sels` (a method is reached by its name after a dot), and the methods of
// the error interface. Names the runtime does not declare -- the program's,
// Go's -- reach nothing; those the file declares itself are not in refs.
// `full`: every declaration.
static vector<GoRuntimeModel::Ref> GoRuntimeReach(set<string> refs, set<string> sels, bool full) {
    auto &rt = GoRuntime();
    vector<GoRuntimeModel::Ref> out;
    set<GoRuntimeModel::Ref> in;
    if (full) {
        for (int fi = 0; fi < (int)rt.files.size(); fi++)
            for (int di = 0; di < (int)rt.files[fi].decls.size(); di++) {
                auto &k = rt.files[fi].decls[di].kind;
                if (k != "package" && k != "import") out.push_back({ fi, di });
            }
        return out;
    }
    vector<string> work(refs.begin(), refs.end());
    set<string> types;   // The types written, whose methods sels reach.
    vector<string> selwork(sels.begin(), sels.end());
    auto add = [&](GoRuntimeModel::Ref r) {
        if (!in.insert(r).second) return;
        auto &d = rt.D(r);
        for (auto &n : d.refs)
            if (refs.insert(n).second) work.push_back(n);
        for (auto &n : d.sels)
            if (sels.insert(n).second) selwork.push_back(n);
        if (d.kind == "type")
            for (auto &n : d.names) {
                types.insert(n);
                auto it = rt.methods.find(n);
                if (it == rt.methods.end()) continue;
                for (auto m : it->second) {
                    auto &md = rt.D(m);
                    if (sels.count(md.method) || md.method == "Error" || md.method == "String") work.push_back("\x01" + n + "." + md.method);
                }
            }
    };
    while (!work.empty() || !selwork.empty()) {
        if (!work.empty()) {
            auto n = work.back();
            work.pop_back();
            if (n[0] == '\x01') {
                // A method of a type written: Type.Method.
                auto dot = n.find('.');
                auto t = n.substr(1, dot - 1), m = n.substr(dot + 1);
                for (auto r : rt.methods.at(t))
                    if (rt.D(r).method == m) add(r);
                continue;
            }
            auto it = rt.byname.find(n);
            if (it != rt.byname.end())
                for (auto r : it->second) add(r);
            continue;
        }
        auto m = selwork.back();
        selwork.pop_back();
        for (auto &t : types) {
            auto it = rt.methods.find(t);
            if (it == rt.methods.end()) continue;
            for (auto r : it->second)
                if (rt.D(r).method == m) add(r);
        }
    }
    for (int fi = 0; fi < (int)rt.files.size(); fi++)
        for (int di = 0; di < (int)rt.files[fi].decls.size(); di++)
            if (in.count({ fi, di })) out.push_back({ fi, di });
    return out;
}

// A declaration's text as a file holds it: without its //goose:internal
// directive, and the empty comment line gofmt puts before one.
static string GoDeclText(const GoSource &src, const GoDecl &d) {
    string_view t = string_view(src.text).substr(d.b, d.e - d.b);
    if (!d.internal) return string(t);
    string out;
    vector<string_view> doc;
    auto docend = d.db - d.b;
    for (size_t p = 0; p < docend;) {
        auto nl = t.find('\n', p);
        auto line = t.substr(p, nl + 1 - p);
        p = nl + 1;
        if (line.substr(0, 16) == "//goose:internal") continue;
        doc.push_back(line);
    }
    while (!doc.empty() && (doc.back() == "//\n")) doc.pop_back();
    for (auto l : doc) out += l;
    out += t.substr(docend);
    return out;
}

// The runtime's declarations `decls` (GoRuntimeReach) as a file holds them,
// each section's comment before its first one written, renamed by `code`
// and in their comments by `words` (GoRenameIdents).
static string GoRuntimeText(const vector<GoRuntimeModel::Ref> &decls, const unordered_map<string, string> &code,
                            const unordered_map<string, string> &words) {
    auto &rt = GoRuntime();
    string out;
    int lastfile = -1, lastsection = -1;
    for (auto r : decls) {
        auto &src = rt.files[r.first];
        auto &d = rt.D(r);
        if (r.first != lastfile || d.section != lastsection) {
            if (d.section >= 0 && (r.first != lastfile || d.section != lastsection)) {
                auto [b, e] = src.sections[d.section];
                Append(out, out.empty() ? "" : "\n", string_view(src.text).substr(b, e - b));
            }
            lastfile = r.first;
            lastsection = d.section;
        }
        Append(out, out.empty() ? "" : "\n", GoDeclText(src, d));
    }
    return GoRenameIdents(out, code, {}, words);
}

}  // namespace goose
