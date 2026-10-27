// Goose compiler — the Go backend's functions and program (definitions of
// CodeGenGo members, codegen_go.h): each specialization's interface, its
// body, the globals and their initialization (§11.1), main, and the
// generated file around them.
#pragma once

namespace goose {

// The live specializations, their Go names, the free variables each reaches
// (its callees' included) and whether it needs the data stack block, to a
// fixpoint over the call graph (CodeGen::CollectSpecs). A body needs the
// block wherever it puts anything on a data stack -- in this version, the
// line print renders -- or calls a function that does.
inline void CodeGenGo::CollectSpecs() {
    for (auto sp : ast.fnspecs)
        if (sp->live && sp->body) livespecs.push_back(sp);
    // Names: suffixed where the Goose name maps to more than one live
    // specialization (overloads or several instantiations).
    map<string_view, int> namecount;
    for (auto sp : livespecs) namecount[sp->sf->qname]++;
    for (auto sp : livespecs) {
        auto &si = sinfo[sp];
        auto base = Sanitize(sp->sf->ns, sp->sf->name);
        if (namecount[sp->sf->qname] > 1) Append(base, "_", sp->id);
        si.name = Unique(base);
    }
    unordered_map<FnSpec *, vector<pair<FnSpec *, const Call *>>> callees;
    for (auto sp : livespecs) {
        auto &si = sinfo[sp];
        auto &calls = callees[sp];
        set<const VarDef *> seen;
        set<pair<FnSpec *, const Call *>> seencalls;
        function<void(Node *)> walk = [&](Node *n) {
            if (!n) return;
            if (auto id = Is<Ident>(n)) {
                auto v = id->vdef;
                // A global initializer's locals have no ownerspec: every spec
                // naming one takes it as a free variable, up to
                // gsInitGlobals, which declares it.
                if (v && v->ownerspec != sp && !v->isglobal && seen.insert(v).second)
                    si.freevars.push_back(v);
            }
            if (auto c = Is<Call>(n)) {
                auto add = [&](FnSpec *k) {
                    pair<FnSpec *, const Call *> e { k, c->fvremap.empty() ? nullptr : c };
                    if (k && k != sp && sinfo.count(k) && seencalls.insert(e).second)
                        calls.push_back(e);
                };
                if (c->builtin < 0) add(c->spec);
                for (auto d : c->dispatch) add(d);
                for (auto &fs : c->fmtspecs) add(fs.second);
                // print renders its line on a data stack.
                if (c->builtin == B_PRINT) si.needssp = true;
            }
            RunChildren(n, walk);
        };
        walk(sp->body);
    }
    for (auto changed = true; changed;) {
        changed = false;
        for (auto sp : livespecs) {
            auto &si = sinfo[sp];
            set<const VarDef *> seen(si.freevars.begin(), si.freevars.end());
            for (auto [callee, via] : callees[sp]) {
                auto &ki = sinfo[callee];
                for (auto fv : ki.freevars) {
                    auto v = via ? via->FreeVarArg(fv) : fv;
                    if (v->ownerspec != sp && seen.insert(v).second) {
                        si.freevars.push_back(v);
                        changed = true;
                    }
                }
                if (ki.needssp && !si.needssp) {
                    si.needssp = true;
                    changed = true;
                }
            }
        }
    }
}

// A specialization's Go signature; names its parameters and free variables.
inline string CodeGenGo::Signature(FnSpec *sp) {
    auto &si = sinfo[sp];
    string params;
    auto add = [&](const string &p) { Append(params, params.empty() ? "" : ", ", p); };
    if (si.needssp) add("gs *GsStackBlock, gs_sp int64");
    if (sp->params.size() != sp->argtypes.size())
        Fail(sp->sf->line, "internal: parameter types");
    for (size_t i = 0; i < sp->params.size(); i++)
        add(cat(LocalName(sp->params[i]), " ", GoT(sp->argtypes[i], sp->sf->line)));
    for (auto fv : si.freevars) {
        add(cat(LocalName(fv), " *", GoT(fv->type, fv->line)));
        fvptr.insert(fv);
    }
    string rets;
    for (size_t i = 0; i < sp->rets.size(); i++)
        Append(rets, i ? ", " : "", GoT(sp->rets[i], sp->sf->line));
    if (sp->rets.size() > 1) rets = cat("(", rets, ")");
    return cat("func ", si.name, "(", params, ")", rets.empty() ? "" : " ", rets);
}

inline void CodeGenGo::ResetFnState() {
    body.clear();
    ind = 1;
    tmpn = 0;
    stknext = stkmax = 0;
    fnused.clear();
    fnnextsuffix.clear();
    vnames.clear();
    fvptr.clear();
    scopes.clear();
    dead = false;
    dry = 0;
}

// The identifiers a line reads, into uses: every one but the declared names
// of a declaration and the targets of a plain assignment, which Go does not
// count as uses, and none inside a string literal.
static void GoLineUses(string_view s, unordered_map<string, int> &uses) {
    auto isid = [](char c) { return isalnum((unsigned char)c) || c == '_'; };
    auto i = s.find_first_not_of('\t');
    if (i == string_view::npos) return;
    s = s.substr(i);
    // The identifiers a list of them at p spells, `a, b`, and where it ends.
    auto names = [&](size_t p) {
        for (;;) {
            if (p >= s.size() || !(isalpha((unsigned char)s[p]) || s[p] == '_')) return p;
            while (p < s.size() && isid(s[p])) p++;
            if (s.compare(p, 2, ", ") != 0) return p;
            p += 2;
        }
    };
    size_t from = 0;
    if (s.compare(0, 4, "var ") == 0) {
        from = names(4);
    } else if (s.compare(0, 4, "for ") == 0) {
        auto e = names(4);
        if (s.compare(e, 4, " := ") == 0) from = e;
    } else {
        auto e = names(0);
        if (e > 0 && (s.compare(e, 3, " = ") == 0 || s.compare(e, 4, " := ") == 0)) from = e;
    }
    for (auto p = from; p < s.size();) {
        auto c = s[p];
        if (c == '"') {
            for (p++; p < s.size() && s[p] != '"'; p++)
                if (s[p] == '\\') p++;
            p++;
        } else if (isalpha((unsigned char)c) || c == '_') {
            auto b = p;
            while (p < s.size() && isid(s[p])) p++;
            uses[string(s.substr(b, p - b))]++;
        } else if (isdigit((unsigned char)c)) {
            while (p < s.size() && isid(s[p])) p++;
        } else {
            p++;
        }
    }
}

// A finished body's declarations without their markers (Decl), each local
// nothing reads followed by a use of its own: Go rejects a local it never
// sees read, and an optimized body may well leave one, or only assign it.
inline string CodeGenGo::FinishBody(const string &b) {
    unordered_map<string, int> uses;
    vector<string_view> lines;
    for (size_t pos = 0; pos < b.size();) {
        auto nl = b.find('\n', pos);
        if (nl == string::npos) nl = b.size();
        lines.push_back(string_view(b).substr(pos, nl - pos));
        pos = nl + 1;
    }
    for (auto line : lines) GoLineUses(line.substr(0, line.find(DECLMARK[0])), uses);
    string out;
    out.reserve(b.size());
    for (auto line : lines) {
        auto m = line.find(DECLMARK[0]);
        if (m == string_view::npos) {
            Append(out, line, "\n");
            continue;
        }
        Append(out, line.substr(0, m), "\n");
        auto indent = line.substr(0, line.find_first_not_of('\t'));
        auto names = line.substr(m + 1);
        for (size_t p = 0; p <= names.size();) {
            auto e = names.find(',', p);
            if (e == string_view::npos) e = names.size();
            auto n = string(names.substr(p, e - p));
            if (!n.empty() && !uses[n]) Append(out, indent, "_ = ", n, "\n");
            p = e + 1;
        }
    }
    return out;
}

inline void CodeGenGo::EmitSpec(FnSpec *sp) {
    curspec = sp;
    curinfo = &sinfo[sp];
    ResetFnState();
    spexpr = curinfo->needssp ? "gs_sp" : "0";
    PushSc(SC_FN);
    auto sig = Signature(sp);
    for (auto st : sp->body->stmts) GenStmt(st);
    if (auto tail = sp->body->tail) {
        auto asvalue = !sp->rets.empty() && !IsVoidT(tail->exprtype);
        if (auto fi = Is<IfExpr>(tail); fi && !fi->elseb) asvalue = false;
        if (asvalue) {
            vector<Node *> vals = { tail };
            GenNormalReturn(vals);
        } else {
            GenStmt(tail);
        }
    }
    // Where Go's flow reaches the end of a function with results, the
    // checker found that it never does (an abort, say): Go needs a statement
    // it knows ends the flow.
    if (!dead && !sp->rets.empty()) {
        L("panic(GsUnreachable(", LocArgs(sp->sf->line), "))");
        dead = true;
    }
    PopSc();
    if (stkmax > 0 && !curinfo->needssp)
        Fail(sp->sf->line, cat("internal: ", curinfo->name, " uses data stacks without gs_sp"));
    stackown[sp] = std::max(stackown[sp], stkmax);
    Append(code, "// ", sp->sf->qname, ", line ", sp->sf->line.line, "\n", sig, " {\n",
           FinishBody(body), "}\n\n");
    curspec = nullptr;
    curinfo = nullptr;
}

// ------------------------------------------------------------------
// Globals (§11.1): package-level variables, initialized in declaration
// order by gsInitGlobals before main runs. A `let` global of a `const`
// scalar type with a literal initializer is never written (§9.5), and Go
// initializes it before anything runs, as the C backend's static data is.

inline bool CodeGenGo::StaticInit(Node *n, TypeExpr *t, string &out) {
    if (!n || !IsScalarT(t)) return false;
    if (auto i = Is<IntLit>(n); i && t->kind == TY_INT) {
        out = GoIntLit(i->val, i->uns, t, n->line);
        return true;
    }
    if (auto f = Is<FltLit>(n); f && t->kind == TY_FLT) {
        out = GoFltLit(f->val, t, n->line);
        return true;
    }
    if (auto b = Is<BoolLit>(n); b && t->kind == TY_BOOL) {
        out = b->val ? "true" : "false";
        return true;
    }
    return false;
}

// Whether evaluating n has no effect but its value, and cannot fail: a
// literal, a literal of literals, or a variable read. A negated literal is
// one too, but where --debug checks the negation of the least value of a
// signed type, which is left to run, and fails.
inline bool CodeGenGo::PureInit(Node *n) {
    if (!n) return true;
    if (Is<IntLit>(n) || Is<FltLit>(n) || Is<BoolLit>(n) || Is<StrLit>(n) || Is<NullLit>(n))
        return true;
    if (auto id = Is<Ident>(n)) return id->vdef != nullptr;
    if (auto u = Is<Unary>(n)) {
        if (u->op != T_MINUS) return false;
        if (Is<FltLit>(u->child)) return true;
        auto i = Is<IntLit>(u->child);
        auto ct = u->child->exprtype;
        if (!i || !ct || ct->kind != TY_INT) return false;
        auto s = ct->intstorage;
        return !debug || IsUnsigned(s) ||
               WrapStorage(i->val, s) != WrapStorage((int64_t)(1ull << (IntBits(s) - 1)), s);
    }
    if (Is<ArrayLit>(n) || Is<StructLit>(n)) {
        auto pure = true;
        RunChildren(n, [&](Node *c) { pure = pure && PureInit(c); });
        return pure;
    }
    return false;
}

// The globals the program can tell are there: each one live code names, and
// those its initializer names, and every one whose initializer has an effect
// (a call, a print, a check that can fail), which runs whether or not
// anything reads the global. The rest -- the tables of a module the program
// imports and does not use, say -- are left out.
inline void CodeGenGo::FindUsedGlobals() {
    unordered_map<const VarDef *, pair<VarDecl *, size_t>> declof;
    for (auto g : ast.globals)
        for (size_t i = 0; i < g->defs.size(); i++) declof[g->defs[i]] = { g, i };
    function<void(Node *)> mark = [&](Node *n) {
        if (!n) return;
        if (auto id = Is<Ident>(n); id && id->vdef && id->vdef->isglobal &&
                                    usedglobals.insert(id->vdef).second) {
            auto it = declof.find(id->vdef);
            if (it != declof.end() && it->second.second < it->second.first->inits.size())
                mark(it->second.first->inits[it->second.second]);
        }
        RunChildren(n, mark);
    };
    for (auto sp : livespecs) mark(sp->body);
    for (auto g : ast.globals) {
        auto pure = true;
        for (auto i : g->inits) pure = pure && PureInit(i);
        if (pure) continue;
        for (auto d : g->defs) usedglobals.insert(d);
        for (auto i : g->inits) mark(i);
    }
}

inline void CodeGenGo::EmitGlobalDecls() {
    FindUsedGlobals();
    for (auto g : ast.globals) {
        for (size_t di = 0; di < g->defs.size(); di++) {
            auto d = g->defs[di];
            if (!usedglobals.count(d)) continue;
            auto name = Unique(Sanitize(g->ns, d->name));
            gnames[d] = name;
            auto gt = GoT(d->type, g->line);
            string init;
            if (!d->isvar && d->type->cq && g->inits.size() == g->defs.size() &&
                StaticInit(g->inits[di], d->type, init)) {
                Append(gdecls, "var ", name, " ", gt, " = ", init, "\n");
                gstatic.insert(d);
            } else {
                Append(gdecls, "var ", name, " ", gt, "\n");
            }
        }
    }
}

inline void CodeGenGo::EmitGlobalInit() {
    curspec = nullptr;
    curinfo = nullptr;
    ResetFnState();
    spexpr = "0";
    PushSc(SC_FN);
    for (auto g : ast.globals) {
        if (g->inits.empty()) continue;
        if (g->inits.size() != g->defs.size())
            Unsupported(g->line, "a global binding several results of a call");
        PushSc(SC_STMT);
        for (size_t i = 0; i < g->defs.size(); i++) {
            auto d = g->defs[i];
            // Initialized at its declaration, or not there at all.
            if (gstatic.count(d) || !usedglobals.count(d)) continue;
            GenAny(g->inits[i], GoDst { GD_LVALUE, gnames[d], d->type });
        }
        // An initializer that never completes (a `loop {}`) leaves the ones
        // after it unreached, for Go as for the program.
        PopSc();
    }
    PopSc();
    stackown[nullptr] = std::max(stackown[nullptr], stkmax);
    Append(code, "// The globals' initializers, in declaration order (§11.1).\n"
                 "func gsInitGlobals(gs *GsStackBlock) {\n\t_ = gs\n", FinishBody(body), "}\n\n");
}

// The program's entry: once every function is emitted, the counts are
// final, and main reserves the main program's data stacks, initializes the
// globals and runs fn main(). A library has no main: its package's init
// initializes the globals, and only those. The Go program that imports it
// runs instead of its fn main(), if it has one.
inline void CodeGenGo::EmitProgram() {
    for (auto sf : ast.functions)
        if (sf->isexport) Unsupported(sf->line, "export fn");
    BoundStacksOf(*this);
    auto mainstacks = ProgramStacksOf(*this, MainRootsOf(*this));
    auto mainsf = ast.MainFunction();
    CheckStackLimitOf(*this, "the main program", mainstacks, mainsf ? mainsf->line : Line {});
    string call;
    if (!opts.library && mainsf && !mainsf->specs.empty() && sinfo.count(mainsf->specs[0])) {
        auto &mi = sinfo[mainsf->specs[0]];
        call = cat("\t", mi.name, "(", mi.needssp ? "gs, 0" : "", ")\n");
    }
    Append(code, opts.library ? "func init() {\n" : "func main() {\n",
           "\tdefer GsExitOnAbort()\n"
           "\tgs := NewStackBlock(", mainstacks, ", gsStackReserve, gsStackGap)\n"
           "\tgsInitGlobals(gs)\n", call, "}\n");
}

// The whole file: a header Go tools know generated code by, the package,
// the runtime configuration and the runtime, then the program.
inline string CodeGenGo::Source(const string &srcname) {
    // Go source is UTF-8 throughout, comments included: the file's name as
    // printable ASCII.
    string name = srcname;
    for (auto &c : name)
        if ((uint8_t)c < 32 || (uint8_t)c >= 127) c = '?';
    string s = cat("// Code generated by the Goose compiler from ", name, ". DO NOT EDIT.\n"
                   "//\n"
                   "// Names from the program carry a _g suffix, which keeps them clear of Go's\n"
                   "// keywords and of the runtime's gs and Gs names; a namespaced name ns::x is\n"
                   "// ns_x_g followed by the namespace's length.\n\n"
                   "package ", opts.package, "\n\n"
                   "import (\n\t\"math\"\n\t\"os\"\n\t\"strconv\"\n\t\"syscall\"\n\t\"unsafe\"\n)\n\n"
                   "// The runtime configuration the program was compiled with (--stack-reserve,\n"
                   "// -D GS_STACK_RESERVE, -D GS_STACK_GAP).\n"
                   "const (\n\tgsStackReserve = ", opts.stacks.reserve,
                   "\n\tgsStackGap     = ", opts.stacks.gap, "\n)\n\n");
    for (auto part : go_runtime_prologue) s += part;
    Append(s, "\n// ---- program ----\n\n", filedecls, filedecls.empty() ? "" : "\n", gdecls,
           gdecls.empty() ? "" : "\n", code);
    return s;
}

}  // namespace goose
