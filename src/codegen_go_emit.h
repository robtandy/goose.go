// Goose compiler — the Go backend's functions and program (definitions of
// CodeGenGo members, codegen_go.h): each specialization's interface, its
// body, the globals and their initialization (§11.1), main, and the
// generated file around them.
#pragma once

namespace goose {

// The live specializations, their Go names, the free variables each reaches
// (its callees' included) and whether it needs the data stack block, to a
// fixpoint over the call graph (CodeGen::CollectSpecs). A body needs the
// block and its index (needssp) wherever it puts anything on a data stack,
// which the C backend's rules decide, as they decide where C takes gs_sp:
// a parameter, a result or a node of a type that is not fixed-size (or one
// held on a stack, NeedsStack), a slice literal too large for a native
// value, a payload a match arm copies, the line print renders, a call's
// result built as a type of its own. It needs the block alone (needsgs)
// wherever it reaches a stack through what it is handed: a fat or pool
// reference, a global's stack. Either goes for a function that calls one
// that does.
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
    auto typed = [](TypeExpr *t) {
        return t && t->kind != TY_VOID && t->kind != TY_FN && t->kind != TY_GENERIC;
    };
    for (auto sp : livespecs) {
        auto &si = sinfo[sp];
        auto &calls = callees[sp];
        set<const VarDef *> seen;
        set<pair<FnSpec *, const Call *>> seencalls;
        for (auto pt : sp->argtypes) si.needssp |= NeedsStack(pt);
        for (auto rt : sp->rets) si.needssp |= NeedsStack(rt);
        for (auto pt : sp->argtypes) si.needsgs |= HoldsFatRef(pt);
        function<void(Node *)> walk = [&](Node *n) {
            if (!n) return;
            if (auto id = Is<Ident>(n)) {
                auto v = id->vdef;
                // A global initializer's locals have no ownerspec: every spec
                // naming one takes it as a free variable, up to
                // gsInitGlobals, which declares it.
                if (v && v->ownerspec != sp && !v->isglobal && seen.insert(v).second)
                    si.freevars.push_back(v);
                if (v && v->type && typed(v->type) && (IsBytesT(v->type) || HoldsFatRef(v->type)))
                    si.needsgs = true;
            }
            if (typed(n->exprtype)) {
                if (NeedsStack(n->exprtype)) si.needssp = true;
                if (HoldsFatRef(n->exprtype)) si.needsgs = true;
            }
            // A slice literal still constructs a fixed array to point at.
            if (auto al = Is<ArrayLit>(n); al && al->exprtype && al->exprtype->kind == TY_SLICE &&
                IsFix(al->exprtype->sub)) {
                auto count = GoLitCount(al);
                si.needssp |= count > NATIVE_VALUE_LIMIT /
                    std::max<int64_t>(FixedSize(al->exprtype->sub), 1);
            }
            // A payload bound by value is copied onto a stack of its own.
            if (auto m = Is<MatchExpr>(n))
                for (auto &arm : m->arms)
                    si.needssp |= arm.binder && typed(arm.binder->type) && NeedsStack(arm.binder->type);
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
                // A result is built as the call's own type (str() passed as
                // a slice, say) before it is fitted.
                for (auto rt : c->rettypes) si.needssp |= typed(rt) && NeedsStack(rt);
                // A limited receiver takes anything rendered structurally
                // from a builder of its own (EmitFormatInto).
                if (c->builtin == B_FORMAT) {
                    auto an = c->ArgNodes();
                    auto recv = an[0]->exprtype;
                    if (recv->kind == TY_REF) recv = recv->ref->sub;
                    if (recv->kind == TY_ARRAY && recv->arr->akind == A_LIMITED)
                        for (size_t i = 1; i < an.size(); i++)
                            si.needssp |= !SimpleText(GoFmtContext(c, an[i]), an[i]->exprtype);
                }
            }
            RunChildren(n, walk);
        };
        walk(sp->body);
        if (si.needssp) si.needsgs = true;
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
                    si.needsgs = true;
                    changed = true;
                }
                if (ki.needsgs && !si.needsgs) {
                    si.needsgs = true;
                    changed = true;
                }
            }
            for (auto fv : si.freevars)
                if (fv->type && typed(fv->type) && (IsBytesT(fv->type) || HoldsFatRef(fv->type)) &&
                    !si.needsgs) {
                    si.needsgs = true;
                    changed = true;
                }
        }
    }
}

// Whether a value of type `t` carries a data stack inside it: a fat
// reference, or one reachable from the value (CodeGen::HoldsFatRef).
inline bool CodeGenGo::HoldsFatRef(TypeExpr *t) {
    set<const void *> open;
    function<bool(TypeExpr *)> rec = [&](TypeExpr *x) -> bool {
        switch (x->kind) {
            case TY_REF: return IsFatRef(x) || rec(x->ref->sub);
            case TY_SLICE: return rec(x->sub);
            case TY_ARRAY: return rec(x->arr->sub);
            case TY_STRUCT: case TY_ENUM: case TY_VARIANT:
                for (auto &run : FieldRuns(x)) {
                    if (!open.insert(run.ftypes).second) continue;
                    for (auto ft : *run.ftypes) if (ft && rec(ft)) return true;
                }
                return false;
            default: return false;
        }
    };
    return rec(t);
}

// The Go type of a parameter or a result: one of a fixed-size value of more
// than NATIVE_VALUE_LIMIT bytes the C backend passes through a copy on a data
// stack (codegen_calls.h), which this version does not hold.
inline string CodeGenGo::ParamT(TypeExpr *t, Line ln) {
    if (IsLargeFixed(t))
        Unsupported(ln, cat("a parameter or result of ", TypeText(t), " (", FixedSize(t),
                            " bytes: C passes one of more than ", NATIVE_VALUE_LIMIT,
                            " through a copy on a data stack)"));
    return GoT(t, ln);
}

// A specialization's Go signature (C.3 as the Go backend has it); names its
// parameters and free variables. The block and its index lead, where the
// function takes them; then the declared parameters (a by-value resizable
// is its header and its stack, a pool parameter a GsPref, a bytes value its
// base); then the free variables, each a pointer to the caller's (a
// resizable one's header, with its stack); then, per result that is not
// fixed-size, the stack it is built on, gs_dst<i>. The Go results are the
// fixed results and each resizable result's count (or frame object), in
// order: a bytes result is where its stack's top was.
inline string CodeGenGo::Signature(FnSpec *sp) {
    auto &si = sinfo[sp];
    string params;
    auto add = [&](const string &p) { Append(params, params.empty() ? "" : ", ", p); };
    if (si.needssp) add("gs *GsStackBlock, gs_sp int64");
    else if (si.needsgs) add("gs *GsStackBlock");
    if (sp->params.size() != sp->argtypes.size())
        Fail(sp->sf->line, "internal: parameter types");
    for (size_t i = 0; i < sp->params.size(); i++) {
        auto pt = sp->argtypes[i];
        auto vd = sp->params[i];
        if (IsRelRef(pt))
            Unsupported(sp->sf->line, cat("a parameter of the relative reference type ",
                                          TypeText(pt)));
        auto pn = LocalName(vd);
        if (IsPoolParam(sp, i)) {
            GoT(pt, sp->sf->line);
            add(cat(pn, " GsPref"));
        } else if (IsResz(pt)) {
            GoT(pt, sp->sf->line);
            auto sn = Unique2(cat(pn, "_stk"));
            add(cat(pn, " ", RzGoT(pt, sp->sf->line), ", ", sn, " int64"));
            vstk[vd] = sn;
        } else if (IsBytesT(pt)) {
            GoT(pt, sp->sf->line);
            add(cat(pn, " unsafe.Pointer"));
        } else {
            add(cat(pn, " ", ParamT(pt, sp->sf->line)));
        }
    }
    for (auto fv : si.freevars) {
        auto fn = LocalName(fv);
        if (fv->reusable) {
            GoT(fv->type, fv->line);
            add(cat(fn, " GsPref"));
        } else if (IsResz(fv->type)) {
            auto sn = Unique2(cat(fn, "_stk"));
            add(cat(fn, " *", RzGoT(fv->type, fv->line), ", ", sn, " int64"));
            vstk[fv] = sn;
            fvptr.insert(fv);
        } else if (IsBytesT(fv->type)) {
            GoT(fv->type, fv->line);
            add(cat(fn, " unsafe.Pointer"));
        } else {
            // A pool-shaped reference variable is a GsPref (PrefVar).
            add(cat(fn, " *", PrefVar(fv) ? string("GsPref") : GoT(fv->type, fv->line)));
            fvptr.insert(fv);
        }
    }
    string rets;
    auto nrets = 0;
    for (size_t i = 0; i < sp->rets.size(); i++) {
        auto rt = sp->rets[i];
        if (IsBytesT(rt)) {
            GoT(rt, sp->sf->line);
            add(cat("gs_dst", i, " int64"));
            if (!IsResz(rt)) continue;
            Append(rets, nrets++ ? ", " : "", IsFrameObj(rt) ? RzGoT(rt, sp->sf->line) : "int64");
            continue;
        }
        // A result holding relative references is built where it is
        // received, so that a link from it to the receiver's storage is
        // measured from there (C's result location, NeedsResultLocation):
        // a Go result is a copy.
        if (rt->kind != TY_REF && IsFix(rt) && HasRelRef(rt))
            Unsupported(sp->sf->line, cat("a function result holding relative references (",
                                          TypeText(rt), ")"));
        Append(rets, nrets++ ? ", " : "", ParamT(rt, sp->sf->line));
    }
    if (nrets > 1) rets = cat("(", rets, ")");
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
    vstk.clear();
    vpool.clear();
    nrvo.clear();
    openat.clear();
    dsttop0.clear();
    dstcounts.clear();
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
            // A field selector (`x.f_g`) names no local.
            if (b == 0 || s[b - 1] != '.') uses[string(s.substr(b, p - b))]++;
        } else if (isdigit((unsigned char)c)) {
            while (p < s.size() && isid(s[p])) p++;
        } else {
            p++;
        }
    }
}

// The identifiers whose address a line takes (`&x`, `&x.f`, `&x[i]`),
// outside its string literals, into addressed.
static void GoLineAddressed(string_view s, unordered_set<string> &addressed) {
    auto isid = [](char c) { return isalnum((unsigned char)c) || c == '_'; };
    for (size_t p = 0; p < s.size(); p++) {
        auto c = s[p];
        if (c == '"') {
            for (p++; p < s.size() && s[p] != '"'; p++)
                if (s[p] == '\\') p++;
        } else if (c == '&' && p + 1 < s.size() && (isalpha((unsigned char)s[p + 1]) || s[p + 1] == '_') &&
                   (p == 0 || s[p - 1] != '&')) {
            auto b = p + 1, e = b;
            while (e < s.size() && isid(s[e])) e++;
            addressed.insert(string(s.substr(b, e - b)));
            p = e - 1;
        }
    }
}

// A finished body's declarations without their markers (Decl), each local
// nothing reads followed by a use of its own: Go rejects a local it never
// sees read, and an optimized body may well leave one, or only assign it.
// A local is kept alive where its scope is left (KEEPMARK) only where the
// function takes its address.
inline string CodeGenGo::FinishBody(const string &b) {
    unordered_map<string, int> uses;
    unordered_set<string> addressed;
    vector<string_view> lines;
    for (size_t pos = 0; pos < b.size();) {
        auto nl = b.find('\n', pos);
        if (nl == string::npos) nl = b.size();
        lines.push_back(string_view(b).substr(pos, nl - pos));
        pos = nl + 1;
    }
    for (auto line : lines) {
        if (line.find(KEEPMARK[0]) != string_view::npos) continue;
        GoLineUses(line.substr(0, line.find(DECLMARK[0])), uses);
        GoLineAddressed(line.substr(0, line.find(DECLMARK[0])), addressed);
    }
    string out;
    out.reserve(b.size());
    for (auto line : lines) {
        if (auto k = line.find(KEEPMARK[0]); k != string_view::npos) {
            auto n = string(line.substr(k + 1));
            if (addressed.count(n)) Append(out, line.substr(0, k), "runtime.KeepAlive(&", n, ")\n");
            continue;
        }
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
    // The count each resizable result returns (the frame object, whole), in
    // the frame; the named results built at their destinations (§7.3).
    dstcounts.assign(sp->rets.size(), "");
    for (size_t i = 0; i < sp->rets.size(); i++) {
        if (!IsResz(sp->rets[i])) continue;
        dstcounts[i] = Unique2(cat("gs_rl", i));
        Decl(dstcounts[i], IsFrameObj(sp->rets[i]) ? RzGoT(sp->rets[i], sp->sf->line) : "int64");
    }
    DetectNrvo(sp);
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
        // A large value the C backend builds on a data stack takes one
        // there whether or not anything reads it, which both backends count
        // (--stacks).
        for (auto d : g->defs) pure = pure && (!IsLargeFixed(d->type) || cstatic.count(d));
        // So does a value on a stack of its own, which C reserves for every
        // such global.
        for (auto d : g->defs) pure = pure && !IsBytesT(d->type);
        if (pure) continue;
        for (auto d : g->defs) usedglobals.insert(d);
        for (auto i : g->inits) mark(i);
    }
}

inline void CodeGenGo::EmitGlobalDecls() {
    // The globals the C backend makes static data, in declaration order, as
    // it finds them (a static one's initializer may name an earlier one).
    for (auto g : ast.globals)
        for (size_t di = 0; di < g->defs.size(); di++) {
            auto d = g->defs[di];
            if (!d->isvar && d->type && d->type->cq && di < g->inits.size() &&
                g->inits.size() == g->defs.size() && CStaticInit(g->inits[di], d->type))
                cstatic[d] = g->inits[di];
        }
    FindUsedGlobals();
    for (auto g : ast.globals) {
        for (size_t di = 0; di < g->defs.size(); di++) {
            auto d = g->defs[di];
            if (!usedglobals.count(d)) continue;
            auto name = Unique(Sanitize(g->ns, d->name));
            gnames[d] = name;
            if (IsBytesT(d->type)) {
                // A header (or base) and a dedicated stack, which follows the
                // main program's in its block (gsInitGlobals sets it up).
                if (di >= g->inits.size())
                    Unsupported(g->line, cat("a global of type ", TypeText(d->type), " without an initializer"));
                GoT(d->type, g->line);
                auto stk = Unique(cat("gsGStk_", name));
                gstks[d] = stk;
                Append(gdecls, "var ", name, " ", IsResz(d->type) ? RzGoT(d->type, g->line) : "unsafe.Pointer",
                       "\n");
                if (d->reusable) {
                    auto fln = Unique(cat(name, "_fl"));
                    auto flstk = Unique(cat("gsGStk_", fln));
                    gpools[d] = { fln, flstk };
                    Append(gdecls, "var ", fln, " GsRhdr\n");
                }
                continue;
            }
            if (PrefVar(d)) {
                Append(gdecls, "var ", name, " GsPref\n");
                continue;
            }
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
            auto init = g->inits[i];
            if (IsBytesT(d->type)) {
                InitGlobalStack(d);
                GenConstruct(init, gstks[d], d->type, IsResz(d->type) ? RzLenLv(d->type, gnames[d]) : "");
                continue;
            }
            if (PrefVar(d)) {
                L(gnames[d], " = ", GenPrefVal(init));
                continue;
            }
            if (IsLargeFixed(d->type) && (Is<ArrayLit>(init) || Is<StructLit>(init) || Is<StrLit>(init))) {
                // Built where it stays. Where the C backend's initializer
                // builds it, it takes a data stack for the value first.
                if (!cstatic.count(d)) AllocStk();
                auto dst = AtVar(gnames[d], d->type);
                if (auto sl = Is<StrLit>(init)) StrLitInto(sl, dst);
                else LitInto(init, dst, true);
                continue;
            }
            GenAny(init, GoDst { GD_LVALUE, gnames[d], d->type });
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

// A global's dedicated stack, and its pool's freelist's, set up where its
// initializer runs (CodeGen::InitGlobalStack): its header at the stack's
// base.
inline void CodeGenGo::InitGlobalStack(VarDef *d) {
    auto stk = gstks[d];
    globalregions++;
    gstkorder.push_back(stk);
    if (IsResz(d->type) && IsFrameObj(d->type)) {
        // The tail header is set when the value is constructed.
    } else if (IsResz(d->type)) {
        L(gnames[d], ".Base = ", Top(stk));
        L(gnames[d], ".Len = 0");
    } else {
        L(gnames[d], " = ", Top(stk));
    }
    if (d->reusable) {
        auto &p = gpools[d];
        globalregions++;
        gstkorder.push_back(p.second);
        L(p.first, ".Base = ", Top(p.second));
        L(p.first, ".Len = 0");
    }
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
        call = cat("\t", mi.name, "(", mi.needssp ? "gs, 0" : mi.needsgs ? "gs" : "", ")\n");
    }
    // The globals' dedicated stacks follow the main program's in its block.
    if (!gstkorder.empty()) {
        Append(gdecls, "\n// The dedicated data stacks of the globals that live on one, which follow\n"
                       "// the main program's own in its block.\nconst (\n");
        for (size_t i = 0; i < gstkorder.size(); i++)
            Append(gdecls, "\t", gstkorder[i], " = ", mainstacks + (int64_t)i, "\n");
        Append(gdecls, ")\n");
    }
    Append(code, opts.library ? "func init() {\n" : "func main() {\n",
           "\tdefer GsExitOnAbort()\n"
           "\tdebug.SetPanicOnFault(true)\n"
           "\tgs := NewStackBlock(", mainstacks + globalregions, ", gsStackReserve, gsStackGap)\n"
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
                   "import (\n\t\"math\"\n\t\"os\"\n",
                   code.find("runtime.KeepAlive(") != string::npos ? "\t\"runtime\"\n" : "",
                   "\t\"runtime/debug\"\n\t\"strconv\"\n\t\"syscall\"\n\t\"unsafe\"\n)\n\n"
                   "// The runtime configuration the program was compiled with (--stack-reserve,\n"
                   "// -D GS_STACK_RESERVE, -D GS_STACK_GAP).\n"
                   "const (\n\tgsStackReserve = ", opts.stacks.reserve,
                   "\n\tgsStackGap     = ", opts.stacks.gap, "\n)\n\n");
    for (auto part : go_runtime_prologue) s += part;
    Append(s, "\n// ---- program ----\n\n", filedecls, filedecls.empty() ? "" : "\n", tdecls, gdecls,
           gdecls.empty() ? "" : "\n", code, eqcode);
    return s;
}

}  // namespace goose
