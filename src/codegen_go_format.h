// Goose compiler — gofmt's layout of the Go the backend writes (issue #20),
// so that `gofmt -l` finds nothing to change in a generated file.
//
// The program's Go (CodeGenGo::Source: everything after the runtime) is
// written a statement or a declaration to a line, with gofmt's indentation
// and its spacing of everything but binary operators, whose spacing gofmt
// works out from where an expression stands: `a + b` at the top of a
// statement, `f(a+b, c)` among several arguments, `x[i+1]` in an index, and
// a mix of precedences as `a*b + c` (go/printer's binaryExpr). That is not
// known where an expression is generated, inside out, so GoFormat lays the
// finished text out as go/printer prints the same Go:
//
// * each line's expressions are parsed as go/parser parses them, and every
//   binary operator is spaced as go/printer spaces it, from the depth and
//   the precedences around it; a pair of parentheses gofmt does not print
//   (the outer of two around one operand, those around a control clause's
//   expression) is left out, and a number is written as gofmt normalizes
//   it;
// * the fields of a struct, the specs of a const or var group, and the
//   comments that end consecutive lines are aligned in columns, as
//   text/tabwriter aligns the cells go/printer writes;
// * a run of blank lines is one, none opens or closes a struct or a group,
//   and at the package level a comment, or a declaration of another kind
//   than the one before it, follows the declaration before it after a blank
//   line, as go/printer's declList and comment placement have it.
//
// A line it does not parse is left as it is (test/run_go_tests.py checks
// every file it generates with gofmt -l). The runtime and the --include
// files are Go of their own, which the pass does not touch: the runtime's
// text is gofmt's already, and an --include file is spliced in as it is
// written (codegen_go_export.h).
#pragma once

namespace goose {

struct GoFmt {
    // ------------------------------------------------------------------
    // Tokens.

    // A token of a line: its text, gofmt's normalization of it where that
    // differs (a number), the blanks before it -- as written, then as gofmt
    // writes them -- and whether gofmt leaves it out (a parenthesis).
    struct Tok {
        string_view text;
        string norm;
        char kind = 0;   // i identifier, k keyword, n number, c rune, s string, o operator
        string ws;
        bool drop = false;
    };

    static bool Keyword(string_view s) {
        static const unordered_set<string_view> kw = {
            "break", "case", "chan", "const", "continue", "default", "defer", "else", "fallthrough", "for",
            "func", "go", "goto", "if", "import", "interface", "map", "package", "range", "return", "select",
            "struct", "switch", "type", "var",
        };
        return kw.count(s) != 0;
    }

    // The length of the operator or punctuation s starts with, 0 for none.
    static size_t OpLen(string_view s) {
        static const char *const ops[] = {
            "<<=", ">>=", "&^=", "...", "&&", "||", "<-", "++", "--", "==", "!=", "<=", ">=", ":=", "+=",
            "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<", ">>", "&^",
        };
        for (auto o : ops)
            if (s.substr(0, strlen(o)) == o) return strlen(o);
        return !s.empty() && s[0] && strchr("+-*/%&|^<>=!()[]{},;.:~", s[0]) ? 1 : 0;
    }

    // A number as gofmt writes it (go/printer's normalizedNumber): its base
    // prefix and exponent in lower case, and an integer imaginary literal
    // without leading zeros.
    static string NormNumber(string_view x) {
        string s(x);
        if (s.size() < 2) return s;
        auto p2 = s.substr(0, 2);
        auto lowerp = [&] {
            auto i = s.rfind('P');
            if (i != string::npos) s[i] = 'p';
        };
        if (p2 == "0X") {
            s[1] = 'x';
            lowerp();
        } else if (p2 == "0x") {
            lowerp();
        } else if (p2 == "0O" || p2 == "0B") {
            s[1] = (char)tolower((unsigned char)s[1]);
        } else if (p2 != "0o" && p2 != "0b") {
            if (auto i = s.rfind('E'); i != string::npos) {
                s[i] = 'e';
            } else if (s.back() == 'i' && s.find_first_of(".e") == string::npos) {
                s = s.substr(std::min(s.size() - 1, s.find_first_not_of("0_")));
                if (s == "i") s = "0i";
            }
        }
        return s;
    }

    // A line as written, its indentation, its tokens and a // comment that
    // ends it (or is all of it). `ok` is false for a line the lexer does not
    // take apart, which is left as it is: one inside, or starting, a raw
    // string or a /* */ comment over several lines, or holding a /* */
    // comment or text that is no Go token.
    struct Line {
        string_view text, indent, comment;
        vector<Tok> toks;
        bool ok = true;
    };

    // `state` carries a raw string (1) or a /* */ comment (2) that a line
    // leaves open into the next.
    static void Lex(Line &l, int &state) {
        auto s = l.text;
        size_t i = 0, n = s.size();
        if (state) {
            l.ok = false;
            auto e = s.find(state == 1 ? "`" : "*/");
            if (e == string_view::npos) return;
            i = e + (state == 1 ? 1 : 2);
            state = 0;
        }
        auto ib = s.find_first_not_of('\t');
        l.indent = s.substr(0, std::min(n, ib));
        if (l.ok) i = l.indent.size();
        string ws;
        auto isid = [](char c) { return isalnum((unsigned char)c) || c == '_' || (unsigned char)c >= 0x80; };
        while (i < n) {
            auto c = s[i];
            if (c == ' ' || c == '\t' || c == '\r') {
                ws += c;
                i++;
                continue;
            }
            auto b = i;
            Tok t;
            if (c == '/' && i + 1 < n && s[i + 1] == '/') {
                l.comment = s.substr(i);
                return;
            }
            if (c == '/' && i + 1 < n && s[i + 1] == '*') {
                // A /* */ comment: the line is left as it is.
                l.ok = false;
                auto e = s.find("*/", i + 2);
                if (e == string_view::npos) {
                    state = 2;
                    return;
                }
                i = e + 2;
                continue;
            }
            if (c == '`') {
                auto e = s.find('`', i + 1);
                if (e == string_view::npos) {
                    l.ok = false;
                    state = 1;
                    return;
                }
                i = e + 1;
                t.kind = 's';
            } else if (c == '"' || c == '\'') {
                for (i++; i < n && s[i] != c; i++)
                    if (s[i] == '\\') i++;
                if (i >= n) {
                    l.ok = false;
                    return;
                }
                i++;
                t.kind = c == '"' ? 's' : 'c';
            } else if (isdigit((unsigned char)c) || (c == '.' && i + 1 < n && isdigit((unsigned char)s[i + 1]))) {
                auto hex = c == '0' && i + 1 < n && (s[i + 1] == 'x' || s[i + 1] == 'X');
                for (i++; i < n; i++) {
                    auto d = s[i], e = s[i - 1];
                    if (isalnum((unsigned char)d) || d == '_' || d == '.') continue;
                    if ((d == '+' || d == '-') && (hex ? e == 'p' || e == 'P' : e == 'e' || e == 'E')) continue;
                    break;
                }
                t.kind = 'n';
            } else if (isid(c)) {
                while (i < n && isid(s[i])) i++;
                t.kind = Keyword(s.substr(b, i - b)) ? 'k' : 'i';
            } else if (auto ol = OpLen(s.substr(i))) {
                i += ol;
                t.kind = 'o';
            } else {
                l.ok = false;
                return;
            }
            t.text = s.substr(b, i - b);
            if (t.kind == 'n') {
                auto nn = NormNumber(t.text);
                if (nn != t.text) t.norm = nn;
            }
            t.ws = std::move(ws);
            ws.clear();
            l.toks.push_back(std::move(t));
        }
    }

    // ------------------------------------------------------------------
    // Expressions, as go/parser builds them.

    enum Kind : uint8_t {
        X_IDENT, X_LIT, X_BINARY, X_UNARY, X_STAR, X_PAREN, X_SELECTOR, X_ASSERT, X_INDEX, X_SLICE, X_CALL,
        X_COMPOSITE, X_KEYVALUE, X_TYPE,
    };
    // An expression: its operator (a binary or unary one's, a parenthesis's
    // `(`), the `)` of a parenthesis, a binary operator's precedence, its
    // operands (-1 for one left out: a slice's index, a literal's type) and
    // a slice's colons. A type's `sub` says which: a array or slice, m map,
    // c chan, f func, s struct, i interface.
    struct Node {
        Kind kind;
        char sub = 0;
        int tok = -1, tok2 = -1, prec = 0;
        vector<int> kids, colons;
    };

    // go/token's precedence of a binary operator, 0 for any other token.
    static int BinPrec(string_view o) {
        if (o == "||") return 1;
        if (o == "&&") return 2;
        if (o == "==" || o == "!=" || o == "<" || o == "<=" || o == ">" || o == ">=") return 3;
        if (o == "+" || o == "-" || o == "|" || o == "^") return 4;
        if (o == "*" || o == "/" || o == "%" || o == "<<" || o == ">>" || o == "&" || o == "&^") return 5;
        return 0;
    }

    // The statement or declaration on one line, as go/parser parses it:
    // its expressions, each with the depth go/printer prints it at and
    // whether its parentheses go (a control clause's), and the block the
    // line opens, if any: { code, s a struct's fields, i an interface's,
    // c a const group, v a var group, m an import group, t a type group.
    // The boundaries of a field's or a spec's parts are kept for the
    // alignment: its names end at `names`, its type at `type`, and its
    // values start at `eq`.
    struct Root {
        int node, depth;
        bool strip;
    };
    struct Parser {
        vector<Tok> &t;
        vector<Node> nodes;
        vector<Root> roots;
        size_t p = 0, end;
        int lev = 0;   // go/parser's exprLev: below 0 in a control clause's header
        bool bad = false;
        char opens = 0;
        int names = -1, type = -1, eq = -1;
        bool tag = false;

        explicit Parser(vector<Tok> &toks) : t(toks), end(toks.size()) {}
        bool AtEnd() const { return p >= end; }
        bool Is(size_t i, string_view s) const {
            return i < end && (t[i].kind == 'o' || t[i].kind == 'k') && t[i].text == s;
        }
        bool At(string_view s) const { return Is(p, s); }
        bool AtKind(char k) const { return p < end && t[p].kind == k; }
        bool Accept(string_view s) {
            if (!At(s)) return false;
            p++;
            return true;
        }
        void Expect(string_view s) {
            if (!Accept(s)) Bad();
        }
        int Bad() {
            bad = true;
            p = end;
            return -1;
        }
        int Add(Node n) {
            nodes.push_back(std::move(n));
            return (int)nodes.size() - 1;
        }
        void Use(int n, int depth, bool strip = false) {
            if (n >= 0) roots.push_back({ n, depth, strip });
        }

        int Expr(int prec1 = 1) {
            auto x = Unary();
            for (;;) {
                if (bad) return -1;
                auto prec = AtKind('o') ? BinPrec(t[p].text) : 0;
                if (prec < prec1) return x;
                Node n { X_BINARY };
                n.tok = (int)p++;
                n.prec = prec;
                auto y = Expr(prec + 1);
                n.kids = { x, y };
                x = Add(std::move(n));
            }
        }
        int Unary() {
            if (bad || AtEnd()) return Bad();
            if (AtKind('o')) {
                auto s = t[p].text;
                if (s == "<-" && Is(p + 1, "chan")) return Primary();
                if (s == "*" || s == "+" || s == "-" || s == "!" || s == "^" || s == "&" || s == "<-" || s == "~") {
                    Node n { s == "*" ? X_STAR : X_UNARY };
                    n.tok = (int)p++;
                    n.kids = { Unary() };
                    return Add(std::move(n));
                }
            }
            return Primary();
        }
        int Primary() {
            auto x = Operand();
            for (;;) {
                if (bad) return -1;
                if (At(".")) {
                    if (Is(p + 1, "(")) {
                        p += 2;
                        Node n { X_ASSERT };
                        n.kids = { x, -1 };
                        if (!Accept("type")) {
                            lev++;
                            n.kids[1] = Type();
                            lev--;
                        }
                        Expect(")");
                        x = Add(std::move(n));
                        continue;
                    }
                    if (p + 1 >= end || t[p + 1].kind != 'i') return Bad();
                    p += 2;
                    Node n { X_SELECTOR };
                    n.kids = { x };
                    x = Add(std::move(n));
                } else if (At("[")) {
                    x = Index(x);
                } else if (At("(")) {
                    x = Call(x);
                } else if (At("{") && LiteralType(x)) {
                    x = Composite(x);
                } else {
                    return x;
                }
            }
        }
        // Whether a `{` after x opens a composite literal of type x, as
        // go/parser has it: a type name only outside a control clause's
        // header, an array, slice, struct or map type always.
        bool LiteralType(int x) const {
            auto u = x;
            while (u >= 0 && nodes[u].kind == X_PAREN) u = nodes[u].kids[0];
            if (u < 0) return false;
            auto &n = nodes[u];
            if (n.kind == X_IDENT || n.kind == X_SELECTOR || n.kind == X_INDEX) return lev >= 0;
            return n.kind == X_TYPE && (n.sub == 'a' || n.sub == 's' || n.sub == 'm');
        }
        int Operand() {
            if (bad || AtEnd()) return Bad();
            auto k = t[p].kind;
            if (k == 'i') {
                p++;
                return Add({ X_IDENT });
            }
            if (k == 'n' || k == 'c' || k == 's') {
                p++;
                return Add({ X_LIT });
            }
            if (At("(")) {
                Node n { X_PAREN };
                n.tok = (int)p++;
                lev++;
                n.kids = { Expr() };
                lev--;
                n.tok2 = (int)p;
                Expect(")");
                return Add(std::move(n));
            }
            return TypeLit();
        }
        // A type where only a type can stand: a type name, qualified and
        // instantiated, or a type literal.
        int Type() {
            if (bad || AtEnd()) return Bad();
            if (At("*")) {
                Node n { X_STAR };
                n.tok = (int)p++;
                n.kids = { Type() };
                return Add(std::move(n));
            }
            if (At("(")) {
                Node n { X_PAREN };
                n.tok = (int)p++;
                lev++;
                n.kids = { Type() };
                lev--;
                n.tok2 = (int)p;
                Expect(")");
                return Add(std::move(n));
            }
            if (AtKind('i')) {
                p++;
                auto x = Add({ X_IDENT });
                if (At(".") && p + 1 < end && t[p + 1].kind == 'i') {
                    p += 2;
                    Node n { X_SELECTOR };
                    n.kids = { x };
                    x = Add(std::move(n));
                }
                if (At("[")) {
                    Node n { X_INDEX };
                    n.tok = (int)p++;
                    lev++;
                    n.kids = { x, Type() };
                    while (Accept(","))
                        if (!At("]")) n.kids.push_back(Type());
                    lev--;
                    Expect("]");
                    x = Add(std::move(n));
                }
                return x;
            }
            return TypeLit();
        }
        // Past a run of tokens that opens with t[p] and closes with its
        // match.
        void Skip() {
            int depth = 0;
            do {
                if (AtEnd()) {
                    Bad();
                    return;
                }
                auto s = t[p].kind == 'o' ? t[p].text : string_view();
                if (s == "(" || s == "[" || s == "{") depth++;
                else if (s == ")" || s == "]" || s == "}") depth--;
                p++;
            } while (depth > 0);
        }
        bool StartsType(size_t i) const {
            if (i >= end) return false;
            if (t[i].kind == 'i') return true;
            for (auto s : { "*", "[", "(", "map", "chan", "func", "struct", "interface", "<-" })
                if (Is(i, s)) return true;
            return false;
        }
        int TypeLit() {
            if (bad || AtEnd()) return Bad();
            Node n { X_TYPE };
            if (Accept("[")) {
                n.sub = 'a';
                lev++;
                if (At("...") && Is(p + 1, "]")) p++;
                else if (!At("]")) n.kids.push_back(Expr());
                lev--;
                Expect("]");
                n.kids.push_back(Type());
            } else if (Accept("map")) {
                n.sub = 'm';
                Expect("[");
                lev++;
                n.kids.push_back(Type());
                lev--;
                Expect("]");
                n.kids.push_back(Type());
            } else if (At("chan") || (At("<-") && Is(p + 1, "chan"))) {
                n.sub = 'c';
                if (Accept("<-")) p++;
                else if (p++, At("<-")) p++;
                n.kids.push_back(Type());
            } else if (Accept("func")) {
                // A signature, which nothing in it changes: a function
                // literal, which a body follows, is left alone.
                n.sub = 'f';
                if (!At("(")) return Bad();
                Skip();
                if (At("(")) Skip();
                else if (StartsType(p) && !At("(")) n.kids.push_back(Type());
                if (At("{")) return Bad();
            } else if (At("struct") || At("interface")) {
                n.sub = t[p].text[0];
                p++;
                if (!At("{")) return Bad();
                if (p + 1 == end) {
                    // A struct or an interface that the lines below declare.
                    p++;
                    opens = n.sub;
                } else {
                    Skip();
                }
            } else {
                return Bad();
            }
            return Add(std::move(n));
        }
        int Index(int x) {
            Node n { X_INDEX };
            n.tok = (int)p++;
            lev++;
            auto a = At(":") ? -1 : Expr();
            n.kids = { x, a };
            if (At(",")) {
                // Several type arguments.
                while (Accept(","))
                    if (!At("]")) n.kids.push_back(Type());
            } else if (At(":")) {
                n.kind = X_SLICE;
                while (At(":") && n.colons.size() < 2) {
                    n.colons.push_back((int)p++);
                    n.kids.push_back(At(":") || At("]") ? -1 : Expr());
                }
            }
            lev--;
            Expect("]");
            return Add(std::move(n));
        }
        int Call(int fun) {
            Node n { X_CALL };
            n.tok = (int)p++;
            lev++;
            n.kids = { fun };
            while (!bad && !At(")")) {
                n.kids.push_back(Expr());
                Accept("...");
                if (!Accept(",")) break;
            }
            lev--;
            Expect(")");
            return Add(std::move(n));
        }
        int Composite(int type) {
            Node n { X_COMPOSITE };
            n.tok = (int)p++;
            if (AtEnd()) return Bad();   // A literal over several lines.
            lev++;
            n.kids = { type };
            while (!bad && !At("}")) {
                auto e = Element();
                if (Accept(":")) {
                    Node kv { X_KEYVALUE };
                    kv.kids = { e, Element() };
                    e = Add(std::move(kv));
                }
                n.kids.push_back(e);
                if (!Accept(",")) break;
            }
            lev--;
            Expect("}");
            return Add(std::move(n));
        }
        int Element() { return At("{") ? Composite(-1) : Expr(); }
        vector<int> ExprList() {
            vector<int> v = { Expr() };
            while (!bad && Accept(",")) v.push_back(Expr());
            return v;
        }

        // A simple statement (go/parser's parseSimpleStmt): x an expression,
        // = an assignment, r a range clause, + an increment or decrement,
        // < a send; 0 for none.
        struct Simple {
            char kind = 0;
            vector<int> lhs, rhs;
        };
        Simple ParseSimple(bool rangeok) {
            Simple s;
            if (rangeok && Accept("range")) {
                s.kind = 'r';
                s.rhs = { Expr() };
                return s;
            }
            s.lhs = ExprList();
            if (bad) return s;
            static const unordered_set<string_view> assign = {
                "=", ":=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>=", "&^=",
            };
            if (AtKind('o') && assign.count(t[p].text)) {
                p++;
                if (rangeok && Accept("range")) {
                    s.kind = 'r';
                    s.rhs = { Expr() };
                    return s;
                }
                s.kind = '=';
                s.rhs = ExprList();
            } else if (Accept("++") || Accept("--")) {
                s.kind = '+';
            } else if (Accept("<-")) {
                s.kind = '<';
                s.rhs = { Expr() };
            } else {
                s.kind = s.lhs.size() == 1 ? 'x' : 0;
            }
            return s;
        }
        // Its expressions at the depths go/printer's stmt prints them at.
        void UseSimple(const Simple &s) {
            switch (s.kind) {
                case 'x': Use(s.lhs[0], 1); break;
                case '=': {
                    auto d = s.lhs.size() > 1 && s.rhs.size() > 1 ? 2 : 1;
                    for (auto x : s.lhs) Use(x, d);
                    for (auto x : s.rhs) Use(x, d);
                    break;
                }
                case 'r':
                    for (auto x : s.lhs) Use(x, 1);
                    Use(s.rhs[0], 1, true);
                    break;
                case '+': Use(s.lhs[0], 2); break;
                case '<':
                    Use(s.lhs[0], 1);
                    Use(s.rhs[0], 1);
                    break;
                default: Bad();
            }
        }

        // A control clause's header up to its block's `{`, which ends the
        // line (go/parser's parseIfHeader, parseSwitchStmt, parseForStmt).
        void Header(const char *kw) {
            auto save = lev;
            lev = -1;
            Simple init, cond, post;
            auto three = false, hasinit = false;
            string_view k = kw;
            if (k == "for" && At("{")) {
                // for {
            } else if (k == "for") {
                if (!At(";")) cond = ParseSimple(true);
                if (cond.kind != 'r' && Accept(";")) {
                    three = true;
                    init = cond;
                    cond = {};
                    if (!At(";")) cond = ParseSimple(false);
                    Expect(";");
                    if (!At("{")) post = ParseSimple(false);
                }
            } else if (!(k == "switch" && At("{"))) {
                if (!At(";")) cond = ParseSimple(false);
                if (Accept(";")) {
                    hasinit = true;
                    init = cond;
                    cond = {};
                    if (!At("{")) cond = ParseSimple(false);
                }
            }
            lev = save;
            if (bad) return;
            if (hasinit || (three && init.kind)) UseSimple(init);
            if (cond.kind == 'x') {
                // A type switch's guard is a statement of its own; any
                // other expression loses its parentheses.
                auto x = cond.lhs[0];
                auto guard = nodes[x].kind == X_ASSERT && nodes[x].kids[1] < 0;
                Use(x, 1, !guard);
            } else if (cond.kind == 'r' || (k == "switch" && cond.kind == '=')) {
                UseSimple(cond);
            } else if (cond.kind || (k == "if" && !cond.kind)) {
                Bad();
            }
            if (post.kind) UseSimple(post);
            Expect("{");
            opens = '{';
        }

        // A const or var spec: names, a type and values.
        void ValueSpec() {
            do {
                if (!AtKind('i')) {
                    Bad();
                    return;
                }
                p++;
            } while (Accept(","));
            names = (int)p;
            if (!AtEnd() && !At("=")) Use(Type(), 1);
            type = (int)p;
            if (At("=")) {
                eq = (int)p++;
                for (auto x : ExprList()) Use(x, 1);
            }
        }
        // A type spec: its name, type parameters and type.
        void TypeSpec() {
            if (!AtKind('i')) {
                Bad();
                return;
            }
            p++;
            if (At("[") && p + 1 < end && t[p + 1].kind == 'i' && StartsType(p + 2)) Skip();
            Accept("=");
            Use(Type(), 1);
        }
        // A struct's field: names and a type, or a type of its own, and a
        // tag.
        void Field() {
            auto named = AtKind('i') && (Is(p + 1, ",") || (StartsType(p + 1) && !Is(p + 1, "(")));
            if (named) {
                do {
                    if (!AtKind('i')) {
                        Bad();
                        return;
                    }
                    p++;
                } while (Accept(","));
                names = (int)p;
            }
            Use(Type(), 1);
            type = (int)p;
            if (AtKind('s')) {
                p++;
                tag = true;
            }
        }

        // A line of a function's body.
        void Stmt() {
            if (AtKind('i') && Is(p + 1, ":") && p + 2 == end) {
                p += 2;   // A label.
                return;
            }
            if (AtKind('k')) {
                auto k = t[p].text;
                if (k == "if" || k == "for" || k == "switch") {
                    p++;
                    Header(k == "if" ? "if" : k == "for" ? "for" : "switch");
                    return;
                }
                if (k == "select") {
                    p++;
                    Expect("{");
                    opens = '{';
                    return;
                }
                if (k == "case") {
                    p++;
                    auto save = p;
                    auto list = ExprList();
                    if (!bad && At(":")) {
                        for (auto x : list) Use(x, 1);
                    } else {
                        // A select's communication.
                        bad = false;
                        p = save;
                        roots.clear();
                        UseSimple(ParseSimple(false));
                    }
                    Expect(":");
                    return;
                }
                if (k == "default") {
                    p++;
                    Expect(":");
                    return;
                }
                if (k == "else") {
                    p++;
                    if (Accept("if")) {
                        Header("if");
                    } else {
                        Expect("{");
                        opens = '{';
                    }
                    return;
                }
                if (k == "return") {
                    p++;
                    if (!AtEnd())
                        for (auto x : ExprList()) Use(x, 1);
                    return;
                }
                if (k == "break" || k == "continue" || k == "goto") {
                    p++;
                    if (AtKind('i')) p++;
                    return;
                }
                if (k == "fallthrough") {
                    p++;
                    return;
                }
                if (k == "go" || k == "defer") {
                    p++;
                    Use(Expr(), 1);
                    return;
                }
                if (k == "var" || k == "const" || k == "type") {
                    Decl();
                    return;
                }
            }
            if (At("{")) {
                p++;
                opens = '{';
                return;
            }
            UseSimple(ParseSimple(false));
        }
        // A declaration's line, at the package level or in a function.
        void Decl() {
            auto k = t[p++].text;
            if (Accept("(")) {
                opens = k == "const" ? 'c' : k == "var" ? 'v' : k == "import" ? 'm' : 't';
                return;
            }
            if (k == "type") TypeSpec();
            else ValueSpec();
            names = type = eq = -1;
        }
    };

    // ------------------------------------------------------------------
    // Spacing, as go/printer's expr1 and binaryExpr print an expression.

    struct Spacer {
        vector<Tok> &t;
        vector<Node> &n;

        void Blank(int tok, bool b) {
            if (tok >= 0 && (size_t)tok < t.size()) t[tok].ws = b ? " " : "";
        }
        static int Reduce(int depth) { return depth > 1 ? depth - 1 : 1; }
        int DiffPrec(int x, int prec) const { return x >= 0 && n[x].kind == X_BINARY && n[x].prec == prec ? 0 : 1; }
        // go/printer's walkBinary.
        void WalkBinary(int x, bool &has4, bool &has5, int &problem) const {
            auto &e = n[x];
            has4 = has4 || e.prec == 4;
            has5 = has5 || e.prec == 5;
            auto l = e.kids[0], r = e.kids[1];
            if (l >= 0 && n[l].kind == X_BINARY && n[l].prec >= e.prec) WalkBinary(l, has4, has5, problem);
            if (r < 0) return;
            auto &y = n[r];
            if (y.kind == X_BINARY) {
                if (y.prec > e.prec) WalkBinary(r, has4, has5, problem);
            } else if (y.kind == X_STAR) {
                if (t[e.tok].text == "/") problem = 5;
            } else if (y.kind == X_UNARY) {
                auto s = cat(t[e.tok].text, t[y.tok].text);
                if (s == "/*" || s == "&&" || s == "&^") problem = 5;
                else if (s == "++" || s == "--") problem = std::max(problem, 4);
            }
        }
        // go/printer's cutoff: the precedence below which a binary
        // operator gets blanks around it.
        int Cutoff(int x, int depth) const {
            auto has4 = false, has5 = false;
            auto problem = 0;
            WalkBinary(x, has4, has5, problem);
            if (problem > 0) return problem + 1;
            if (has4 && has5) return depth == 1 ? 5 : 4;
            return depth == 1 ? 6 : 4;
        }
        void Walk(int x, int depth) {
            if (x < 0) return;
            auto &e = n[x];
            switch (e.kind) {
                case X_BINARY: {
                    auto blank = e.prec < Cutoff(x, depth);
                    Walk(e.kids[0], depth + DiffPrec(e.kids[0], e.prec));
                    Blank(e.tok, blank);
                    Blank(e.tok + 1, blank);
                    Walk(e.kids[1], depth + 1);
                    break;
                }
                case X_KEYVALUE:
                case X_STAR:
                case X_TYPE:
                    for (auto k : e.kids) Walk(k, 1);
                    break;
                case X_UNARY:
                case X_SELECTOR: Walk(e.kids[0], depth); break;
                case X_PAREN: {
                    auto k = e.kids[0];
                    if (k >= 0 && n[k].kind == X_PAREN) {
                        // Parentheses around a parenthesized operand: gofmt
                        // prints the inner ones only.
                        t[e.tok].drop = t[e.tok2].drop = true;
                        Walk(k, depth);
                    } else {
                        Walk(k, Reduce(depth));
                    }
                    break;
                }
                case X_ASSERT:
                    Walk(e.kids[0], depth);
                    Walk(e.kids[1], 1);
                    break;
                case X_INDEX:
                    Walk(e.kids[0], 1);
                    for (size_t i = 1; i < e.kids.size(); i++) Walk(e.kids[i], depth + 1);
                    break;
                case X_SLICE: {
                    Walk(e.kids[0], 1);
                    // Blanks around the colons at the top of an expression
                    // where an index of several is a binary expression.
                    auto blanks = false;
                    if (depth <= 1) {
                        auto count = 0;
                        auto binary = false;
                        for (size_t i = 1; i < e.kids.size(); i++)
                            if (e.kids[i] >= 0) {
                                count++;
                                binary = binary || n[e.kids[i]].kind == X_BINARY;
                            }
                        blanks = count > 1 && binary;
                    }
                    for (size_t i = 1; i < e.kids.size(); i++) {
                        if (i > 1) {
                            auto c = e.colons[i - 2];
                            Blank(c, blanks && e.kids[i - 1] >= 0);
                            Blank(c + 1, blanks && e.kids[i] >= 0);
                        }
                        Walk(e.kids[i], depth + 1);
                    }
                    break;
                }
                case X_CALL:
                    if (e.kids.size() > 2) depth++;
                    for (auto k : e.kids) Walk(k, depth);
                    break;
                case X_COMPOSITE:
                    Walk(e.kids[0], depth);
                    for (size_t i = 1; i < e.kids.size(); i++) Walk(e.kids[i], 1);
                    break;
                default: break;
            }
        }
        // go/printer's stripParens: a control clause's expression without
        // the parentheses around it, but where they keep a composite
        // literal of a type name from reading as the clause's block.
        bool TypedLiteral(int x) const {
            if (x < 0 || n[x].kind == X_PAREN) return false;
            if (n[x].kind == X_COMPOSITE) {
                auto k = n[x].kids[0];
                while (k >= 0 && n[k].kind == X_SELECTOR) k = n[k].kids[0];
                return k >= 0 && n[k].kind == X_IDENT;
            }
            for (auto k : n[x].kids)
                if (TypedLiteral(k)) return true;
            return false;
        }
        int Strip(int x) {
            while (x >= 0 && n[x].kind == X_PAREN && !TypedLiteral(n[x].kids[0])) {
                t[n[x].tok].drop = t[n[x].tok2].drop = true;
                x = n[x].kids[0];
            }
            return x;
        }
    };

    // Tokens a..b of a line as gofmt writes them, without the blanks
    // before the first; a token gofmt leaves out hands its blanks on.
    static string Render(const vector<Tok> &t, size_t a, size_t b) {
        string s, carry;
        auto first = true;
        for (auto i = a; i < b; i++) {
            auto &k = t[i];
            if (k.drop) {
                if (carry.empty()) carry = k.ws;
                continue;
            }
            if (!first) s += k.ws.empty() ? carry : k.ws;
            carry.clear();
            s += k.norm.empty() ? string(k.text) : k.norm;
            first = false;
        }
        return s;
    }

    // ------------------------------------------------------------------
    // Lines.

    // The blocks a line can be in.
    enum Ctx : uint8_t { C_TOP, C_CODE, C_STRUCT, C_IFACE, C_CONST, C_VAR, C_IMPORT, C_TYPE, C_OTHER };
    struct Block {
        Ctx ctx;
        int id;
        int open = 0;   // C_OTHER: the brackets open in it
    };

    // A line as it is written out: its indentation, the cells text/tabwriter
    // aligns (each with whether a vtab, which leaves a column of nothing but
    // empty cells empty, or a tab ends it), and the rest. A field's or a
    // spec's parts are kept until the block it is in is complete, which
    // says how they make cells.
    struct Out {
        string indent, trail;
        vector<string> cells;
        vector<bool> soft;
        bool blank = false, comment = false, endnote = false;   // endnote: a comment ends the line
        bool brk = false, brkafter = false;   // no alignment with the line before, or after
        int block = -1;                        // the block it is in
        Ctx ctx = C_TOP;
        char form = 0;                         // f a field, s a spec
        string names, type, tag, values, note;
        bool hastype = false;
        // The line starts at the package level; it opens a struct, an
        // interface or a group, or closes one; the declaration at the
        // package level it starts, and the one it ends (f func, v var,
        // c const, t type, i import).
        bool top = false, opens = false, closes = false;
        char decl = 0, declend = 0;
    };

    // The cells of one line as go/printer writes them into its tabwriter:
    // text, then a separator between cells, VT for a vtab or HT for a tab
    // (bytes no Go text holds, so that a tab in a comment stays one).
    static constexpr char VT = '\x01', HT = '\x02';
    static void Cells(Out &o, const string &stream) {
        o.cells.clear();
        o.soft.clear();
        string cur;
        for (auto c : stream) {
            if (c == VT || c == HT) {
                o.cells.push_back(cur);
                o.soft.push_back(c == VT);
                cur.clear();
            } else {
                cur += c;
            }
        }
        o.trail = cur;
    }

    // The width of text in runes, which text/tabwriter counts.
    static int Runes(const string &s) {
        int w = 0;
        for (auto c : s) w += ((unsigned char)c & 0xC0) != 0x80;
        return w;
    }

    // text/tabwriter's layout of lines o[a..b), consecutive lines with cells
    // (gofmt's: padding 1, blanks, DiscardEmptyColumns): each column is as
    // wide as the widest of its cells in a block of consecutive lines with
    // a cell there, and one more; a block of cells that are all empty and
    // end in \v is as wide as nothing.
    static void Tabwrite(vector<Out> &o, size_t a, size_t b) {
        vector<vector<int>> widths(b - a);
        size_t maxcells = 0;
        for (auto i = a; i < b; i++) maxcells = std::max(maxcells, o[i].cells.size());
        for (size_t col = 0; col < maxcells; col++)
            for (auto i = a; i < b;) {
                if (col >= o[i].cells.size()) {
                    i++;
                    continue;
                }
                auto j = i;
                auto width = 0;
                auto discard = true;
                for (; j < b && col < o[j].cells.size(); j++) {
                    width = std::max(width, Runes(o[j].cells[col]) + 1);
                    if (!o[j].cells[col].empty() || !o[j].soft[col]) discard = false;
                }
                if (discard) width = 0;
                for (auto k = i; k < j; k++) widths[k - a].push_back(width);
                i = j;
            }
        for (auto i = a; i < b; i++) {
            string s;
            for (size_t col = 0; col < o[i].cells.size(); col++) {
                auto &c = o[i].cells[col];
                s += c;
                s.append((size_t)std::max(0, widths[i - a][col] - Runes(c)), ' ');
            }
            o[i].trail = s + o[i].trail;
            o[i].cells.clear();
        }
    }

    // The cells of the fields of a struct (go/printer's fieldList) and of
    // the specs of a group (valueSpec, with keepTypeColumn), once the block
    // is complete: the lines of `members`.
    static void BlockCells(vector<Out> &o, const vector<size_t> &members, Ctx ctx) {
        if (ctx == C_STRUCT) {
            // A struct of one field is written on one line's terms.
            auto sep = members.size() > 1 ? VT : ' ';
            for (auto i : members) {
                auto &f = o[i];
                string s;
                int extra;
                if (!f.names.empty()) {
                    s = cat(f.names, string(1, sep), f.type);
                    extra = 1;
                } else {
                    s = f.type;
                    extra = 2;
                }
                if (!f.tag.empty()) {
                    if (!f.names.empty() && sep == VT) s += sep;
                    s += cat(string(1, sep), f.tag);
                    extra = 0;
                }
                if (!f.note.empty()) {
                    // A comment after a vtab, or else after a tab.
                    auto hassep = false;
                    for (; extra > 0; extra--) {
                        s += sep;
                        hassep = hassep || sep == VT;
                    }
                    if (!hassep) {
                        while (!s.empty() && s.back() == ' ') s.pop_back();
                        s += HT;
                    }
                    s += f.note;
                }
                Cells(f, s);
            }
            return;
        }
        // A const or var group's specs; that of a group of one is written
        // as a declaration of its own is.
        vector<bool> keep(members.size(), false);
        if (members.size() > 1) {
            // keepTypeColumn: a run of specs with values keeps its type
            // column where one of them has a type.
            auto populate = [&](size_t i, size_t j, bool k) {
                if (k)
                    for (; i < j; i++) keep[i] = true;
            };
            auto i0 = -1;
            auto keeptype = false;
            for (size_t i = 0; i < members.size(); i++) {
                auto &s = o[members[i]];
                if (!s.values.empty()) {
                    if (i0 < 0) {
                        i0 = (int)i;
                        keeptype = false;
                    }
                } else if (i0 >= 0) {
                    populate((size_t)i0, i, keeptype);
                    i0 = -1;
                }
                if (s.hastype) keeptype = true;
            }
            if (i0 >= 0) populate((size_t)i0, members.size(), keeptype);
        }
        for (size_t k = 0; k < members.size(); k++) {
            auto &s = o[members[k]];
            string str = s.names;
            if (members.size() == 1) {
                if (s.hastype) str += cat(" ", s.type);
                if (!s.values.empty()) str += cat(" ", s.values);
                if (!s.note.empty()) str += cat(string(1, HT), s.note);
            } else {
                auto extra = 3;
                if (s.hastype || keep[k]) {
                    str += VT;
                    extra--;
                }
                if (s.hastype) str += s.type;
                if (!s.values.empty()) {
                    str += cat(string(1, VT), s.values);
                    extra--;
                }
                if (!s.note.empty()) {
                    str.append((size_t)extra, VT);
                    str += s.note;
                }
            }
            Cells(s, str);
        }
    }

    // ------------------------------------------------------------------
    // The pass.

    // The brackets a line's tokens from `from` on open, less those they
    // close.
    static int Opened(const vector<Tok> &t, size_t from) {
        auto open = 0;
        for (auto i = from; i < t.size(); i++)
            if (t[i].kind == 'o') {
                auto c = t[i].text;
                if (c == "(" || c == "[" || c == "{") open++;
                else if (c == ")" || c == "]" || c == "}") open--;
            }
        return open;
    }

    static string Format(const string &src) {
        vector<string_view> lines;
        for (size_t pos = 0; pos < src.size();) {
            auto nl = src.find('\n', pos);
            if (nl == string::npos) nl = src.size();
            lines.push_back(string_view(src).substr(pos, nl - pos));
            pos = nl + 1;
        }
        vector<Out> out;
        out.reserve(lines.size());
        vector<Block> stack;
        auto nextid = 0;
        auto state = 0;
        char curdecl = 0;   // The declaration at the package level the lines are in.
        for (auto text : lines) {
            Line l;
            l.text = text;
            auto inside = state != 0;   // A raw string's or a /* */ comment's line.
            Lex(l, state);
            Out o;
            o.top = stack.empty();
            auto in = [&] {
                o.ctx = stack.empty() ? C_TOP : stack.back().ctx;
                o.block = stack.empty() ? -1 : stack.back().id;
            };
            in();
            // Trailing blanks go, as gofmt drops them, but in a raw string.
            auto trimmed = text;
            if (state != 1)
                while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\t' || trimmed.back() == '\r'))
                    trimmed.remove_suffix(1);
            // A line left as it is, and the brackets it opens kept count of
            // in a block of its own.
            auto asis = [&](size_t from) {
                o = Out { o.indent, string(trimmed.substr(o.indent.size())) };
                o.top = stack.empty();
                in();
                o.brk = o.brkafter = true;
                return Opened(l.toks, from);
            };
            if (!l.ok || (!stack.empty() && stack.back().ctx == C_OTHER)) {
                auto open = asis(0);
                o.blank = !inside && trimmed.find_first_not_of(" \t") == string_view::npos;
                if (!stack.empty() && stack.back().ctx == C_OTHER) {
                    stack.back().open += open;
                    if (stack.back().open <= 0) stack.pop_back();
                } else if (open > 0) {
                    stack.push_back({ C_OTHER, nextid++, open });
                }
            } else if (l.toks.empty()) {
                // Blank, or a comment of its own.
                o.indent = string(l.indent);
                o.blank = l.comment.empty();
                o.comment = !o.blank;
                o.trail = string(l.comment);
                while (!o.trail.empty() && (o.trail.back() == ' ' || o.trail.back() == '\t')) o.trail.pop_back();
                o.brk = o.brkafter = true;
            } else {
                o.indent = string(l.indent);
                auto &t = l.toks;
                // A line that closes the block it is in, and what follows on
                // it.
                size_t start = 0;
                if (t[0].kind == 'o' && (t[0].text == "}" || t[0].text == ")")) {
                    if (!stack.empty()) {
                        auto c = stack.back().ctx;
                        o.closes = c != C_CODE;
                        stack.pop_back();
                    }
                    in();
                    o.brk = o.brkafter = true;
                    start = 1;
                }
                Parser ps(t);
                ps.p = start;
                auto kw = start < t.size() && t[start].kind == 'k' ? t[start].text : string_view();
                size_t body = 0;   // Where a function's body on the line starts.
                if (start == t.size()) {
                    // `}` or `)` alone.
                } else if (start > 0) {
                    if (kw == "else") ps.Stmt();
                    else ps.Bad();
                } else if (o.ctx == C_TOP) {
                    if (kw == "package") {
                        ps.p = t.size();
                    } else if (kw == "import") {
                        o.decl = 'i';
                        ps.p++;
                        if (ps.Accept("(")) ps.opens = 'm';
                        else ps.p = t.size();
                    } else if (kw == "func") {
                        // A function's signature, which nothing in it
                        // changes, and its body: below, after a section
                        // break (go/printer's declList), or on the line,
                        // after a vtab (funcBody).
                        o.decl = 'f';
                        ps.p = t.size();
                        if (t.back().kind == 'o' && t.back().text == "{") {
                            ps.opens = '{';
                            o.brk = true;
                        } else if (t.back().kind == 'o' && t.back().text == "}") {
                            auto depth = 0;
                            for (auto i = t.size(); i-- > 0;) {
                                if (t[i].kind != 'o') continue;
                                if (t[i].text == "}") depth++;
                                else if (t[i].text == "{" && --depth == 0) {
                                    body = i;
                                    break;
                                }
                            }
                        }
                    } else if (kw == "var" || kw == "const" || kw == "type") {
                        o.decl = kw[0];
                        ps.Decl();
                    } else {
                        ps.Bad();
                    }
                } else if (o.ctx == C_CODE) {
                    if (kw == "case" || kw == "default" || (t.size() == 2 && t[0].kind == 'i' && t[1].text == ":"))
                        o.brk = o.brkafter = true;
                    ps.Stmt();
                } else if (o.ctx == C_STRUCT) {
                    ps.Field();
                    o.form = 'f';
                } else if (o.ctx == C_CONST || o.ctx == C_VAR) {
                    ps.ValueSpec();
                    o.form = 's';
                } else {
                    // An interface's methods, an import group's paths, a
                    // type group's specs: as they are.
                    ps.p = t.size();
                }
                if (!ps.bad && !ps.AtEnd()) ps.Bad();
                if (ps.bad) {
                    auto decl = o.decl;
                    auto closes = o.closes;
                    if (auto open = asis(start); open > 0) stack.push_back({ C_OTHER, nextid++, open });
                    o.decl = decl;
                    o.closes = closes;
                } else {
                    Spacer sp { t, ps.nodes };
                    for (auto &r : ps.roots) sp.Walk(r.strip ? sp.Strip(r.node) : r.node, r.depth);
                    string note(l.comment);
                    while (!note.empty() && (note.back() == ' ' || note.back() == '\t')) note.pop_back();
                    o.endnote = !note.empty();
                    if (o.form == 'f') {
                        // Its parts, which make cells once the struct is
                        // complete.
                        auto ne = ps.names >= 0 ? (size_t)ps.names : start;
                        if (ps.names >= 0) o.names = Render(t, start, ne);
                        o.type = Render(t, ne, (size_t)ps.type);
                        if (ps.tag) o.tag = Render(t, (size_t)ps.type, t.size());
                        o.note = note;
                    } else if (o.form == 's') {
                        o.names = Render(t, start, (size_t)ps.names);
                        o.hastype = ps.type > ps.names;
                        if (o.hastype) o.type = Render(t, (size_t)ps.names, (size_t)ps.type);
                        if (ps.eq >= 0) o.values = Render(t, (size_t)ps.eq, t.size());
                        o.note = note;
                    } else if (body) {
                        o.cells = { Render(t, 0, body) };
                        o.soft = { true };
                        o.trail = Render(t, body, t.size());
                        if (!note.empty()) {
                            o.cells.push_back(o.trail);
                            o.soft.push_back(false);
                            o.trail = note;
                        }
                    } else {
                        auto code = Render(t, 0, t.size());
                        if (note.empty()) {
                            o.trail = code;
                        } else {
                            o.cells = { code };
                            o.soft = { false };
                            o.trail = note;
                        }
                    }
                    if (ps.opens) {
                        Ctx c = C_CODE;
                        switch (ps.opens) {
                            case 's': c = C_STRUCT; break;
                            case 'i': c = C_IFACE; break;
                            case 'c': c = C_CONST; break;
                            case 'v': c = C_VAR; break;
                            case 'm': c = C_IMPORT; break;
                            case 't': c = C_TYPE; break;
                            default: break;
                        }
                        o.opens = c != C_CODE;
                        o.brkafter = true;
                        stack.push_back({ c, nextid++ });
                    }
                }
            }
            // The declaration at the package level the line starts or
            // ends.
            if (o.top && o.decl) curdecl = o.decl;
            if (stack.empty() && curdecl) {
                o.declend = curdecl;
                curdecl = 0;
            }
            out.push_back(std::move(o));
        }
        // The cells of each struct's fields and each group's specs.
        unordered_map<int, vector<size_t>> blocks;
        for (size_t i = 0; i < out.size(); i++)
            if (out[i].form) blocks[out[i].block].push_back(i);
        for (auto &[id, members] : blocks) BlockCells(out, members, out[members[0]].ctx);
        // Alignment, over each run of consecutive lines with cells.
        for (size_t i = 0; i < out.size();) {
            if (out[i].cells.empty()) {
                i++;
                continue;
            }
            auto j = i + 1;
            while (j < out.size() && !out[j].cells.empty() && !out[j].brk && !out[j - 1].brkafter &&
                   out[j].indent == out[i].indent && out[j].block == out[i].block)
                j++;
            Tabwrite(out, i, j);
            i = j;
        }
        // The blank lines.
        string s;
        s.reserve(src.size() + src.size() / 16);
        const Out *prev = nullptr;   // The last line written.
        for (size_t i = 0; i < out.size(); i++) {
            auto &o = out[i];
            if (o.blank) {
                // One at a time, none at a struct's or a group's start or
                // end, and none at the start or the end of the text.
                if (!prev || prev->blank || prev->opens) continue;
                auto next = i + 1;
                while (next < out.size() && out[next].blank) next++;
                if (next == out.size() || out[next].closes) continue;
                s += '\n';
                prev = &o;
                continue;
            }
            // At the package level, a comment follows code after a blank
            // line, and so does a declaration of another kind than the one
            // before it -- but for code a comment ends, whose line break is
            // all gofmt keeps of those it means to write after it
            // (go/printer's writeCommentSuffix).
            if (prev && !prev->blank && !prev->comment && !prev->endnote && o.top &&
                (o.comment || (o.decl && prev->declend && prev->declend != o.decl)))
                s += '\n';
            Append(s, o.indent, o.trail, "\n");
            prev = &o;
        }
        return s;
    }
};

// The program's Go as gofmt lays it out (GoFmt::Format).
inline string GoFormat(const string &src) { return GoFmt::Format(src); }

}  // namespace goose
