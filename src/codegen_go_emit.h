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
// that does. GoTakesNoStack (codegen_go_cycles.h), which decides before
// the backend runs which helpers it may inline into a recursive cycle,
// follows the same rules for needssp, and must keep up with them.
inline void CodeGenGo::CollectSpecs() {
    for (auto sp : ast.fnspecs)
        if (sp->live && sp->body) livespecs.push_back(sp);
    // Names: suffixed where the Goose name maps to more than one live
    // specialization (overloads or several instantiations).
    map<string_view, int> namecount;
    for (auto sp : livespecs) namecount[sp->sf->qname]++;
    for (auto sp : livespecs) {
        specnum[sp] = (int)numspec.size();
        numspec.push_back(sp);
        sinfo[sp].hasrf = !sp->needs.empty();
        for (auto t : sp->needs)
            if (!fromids.count(t)) fromids[t] = (int)fromids.size() + 1;
    }
    for (auto sp : livespecs) {
        auto &si = sinfo[sp];
        auto base = Sanitize(sp->sf->ns, sp->sf->name);
        if (namecount[sp->sf->qname] > 1) Append(base, "_", sp->id);
        si.name = Unique(base);
        // A standard library function reports its failures where the
        // program called it (AtCaller): but for an export's, whose Go
        // signature is its host's, and main, which has no caller.
        si.located = ast.StdlibFile(sp->sf->line.fileidx) && !sp->sf->isexport &&
                     sp->sf != ast.MainFunction();
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
            // A value adapted to a fixed-mode ADT from a variable-mode one, a
            // reference result's pointee included, is built as that first
            // (GenAdtAdapted).
            if (auto from = AdtFrom(n)) si.needssp |= NeedsStack(from);
            if (auto c = Is<Call>(n)) {
                auto add = [&](FnSpec *k) {
                    pair<FnSpec *, const Call *> e { k, c->fvremap.empty() ? nullptr : c };
                    if (k && k != sp && sinfo.count(k) && seencalls.insert(e).second)
                        calls.push_back(e);
                };
                if (c->builtin < 0) add(c->spec);
                // An extern fn calling the program's own export back.
                if (c->builtin < 0) add(ExportOfExtern(c->spec));
                for (auto d : c->dispatch) add(d);
                for (auto &fs : c->fmtspecs) add(fs.second);
                // print renders its line on a data stack, and thread_spawn
                // assembles its packet on one; thread_wait asks the block
                // which worker is waiting.
                if (c->builtin == B_PRINT || c->builtin == B_THREAD_SPAWN ||
                    c->builtin == B_THREAD_SPAWN_CONTAINED)
                    si.needssp = true;
                if (c->builtin == B_THREAD_WAIT) si.needsgs = true;
                // An extern fn appending to a builder is handed the block.
                if (c->builtin < 0 && c->spec && c->spec->sf->isextern && ExternTakesGs(c->spec))
                    si.needsgs = true;
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
        for (auto [k, via] : calls) calleesof[sp].push_back(k);
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
        // A root owner's descriptor reaches the stacks of all its components.
        if (IsRoot(x)) return true;
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
    else add(cat(GSPARAM, specnum.at(sp), GSPARAM));
    // The parameters a body can take the address of (FinishBody).
    ptypes.clear();
    auto param = [&](const string &n, const string &gt) {
        add(cat(n, " ", gt));
        ptypes.push_back({ n, gt });
    };
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
            param(pn, "GsPref");
        } else if (IsResz(pt)) {
            GoT(pt, sp->sf->line);
            auto sn = Unique2(cat(pn, "_stk"));
            param(pn, RzGoT(pt, sp->sf->line));
            add(cat(sn, " int64"));
            vstk[vd] = sn;
        } else if (IsBytesT(pt)) {
            GoT(pt, sp->sf->line);
            param(pn, "unsafe.Pointer");
        } else if (sp->sf->isthread && IsLargeFixed(pt)) {
            // A thread_fn's large value is the worker's copy in its spawn
            // packet, at its address (EmitThreadThunk), as C passes one.
            param(pn, cat("*", GoT(pt, sp->sf->line)));
            fvptr.insert(vd);
        } else {
            param(pn, ParamT(pt, sp->sf->line));
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
    curzeros.clear();
    curgorets.clear();
    for (size_t i = 0; i < sp->rets.size(); i++) {
        auto rt = sp->rets[i];
        if (IsBytesT(rt)) {
            GoT(rt, sp->sf->line);
            add(cat("gs_dst", i, " int64"));
            // The element-run twin returns its run's count.
            if (emiter && i == 0) {
                Append(rets, nrets++ ? ", " : "", "int64");
                curzeros.push_back("0");
                curgorets.push_back("int64");
                continue;
            }
            if (!IsResz(rt)) continue;
            auto gt = IsFrameObj(rt) ? RzGoT(rt, sp->sf->line) : string("int64");
            Append(rets, nrets++ ? ", " : "", gt);
            curzeros.push_back(IsFrameObj(rt) ? cat(gt, "{}") : string("0"));
            curgorets.push_back(gt);
            continue;
        }
        // A result holding relative references is built where it is
        // received, so that a link from it to the receiver's storage is
        // measured from there (C's result location, NeedsResultLocation):
        // its caller hands the place's address.
        if (NeedsResultLocation(rt)) {
            GoT(rt, sp->sf->line);
            add(cat("gs_r", i, " unsafe.Pointer"));
            continue;
        }
        Append(rets, nrets++ ? ", " : "", ParamT(rt, sp->sf->line));
        curzeros.push_back(ZeroOf(rt, sp->sf->line));
        curgorets.push_back(ParamT(rt, sp->sf->line));
    }
    if (nrets > 1) rets = cat("(", rets, ")");
    if (si.located) add("gs_at *GsLoc");
    return cat("func ", emiter ? ernames[sp] : si.name, "(", params, ")", rets.empty() ? "" : " ", rets);
}

// The element-run twin of a specialization (C.3, CodeGen::EnsureEr): its name,
// queued for emission, or "" where it can have none.
inline string CodeGenGo::EnsureEr(FnSpec *sp) {
    if (auto it = ernames.find(sp); it != ernames.end()) return it->second;
    auto rt = sp->rets.size() == 1 ? sp->rets[0] : nullptr;
    auto ok = rt && rt->kind == TY_ARRAY && rt->arr->akind == A_VAR && sp->body && sinfo.count(sp) &&
              !fromids.count(sp);
    if (!ok) return ernames[sp] = "";
    auto name = Unique(cat(sinfo[sp].name, "_er"));
    ernames[sp] = name;
    erqueue.push_back(sp);
    return name;
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
    refvars.clear();
    refplaces.clear();
    scopes.clear();
    dead = false;
    dry = 0;
    vstk.clear();
    vpool.clear();
    nrvo.clear();
    openat.clear();
    dsttop0.clear();
    dstcounts.clear();
    ltypes.clear();
    ptypes.clear();
    fdstsaves.clear();
    curzeros.clear();
    curgorets.clear();
}

// The identifiers a line reads, into uses: every one but the declared names
// of a declaration and the targets of a plain assignment, which Go does not
// count as uses, and none inside a string literal.
static void GoLineUses(string_view s, unordered_map<string, int> &uses) {
    auto isid = [](char c) { return isalnum((unsigned char)c) || c == '_'; };
    auto i = s.find_first_not_of('\t');
    if (i == string_view::npos) return;
    s = s.substr(i);
    // The identifiers a list of them at p spells, `a, b`, and where it ends;
    // with bases, a target selecting a field (`h.Len`, `h.a.b`) as well,
    // whose variable Go counts as used, into bases.
    auto names = [&](size_t p, vector<string_view> *bases) {
        for (;;) {
            if (p >= s.size() || !(isalpha((unsigned char)s[p]) || s[p] == '_')) return p;
            auto b = p;
            while (p < s.size() && isid(s[p])) p++;
            auto e = p;
            while (bases && p + 1 < s.size() && s[p] == '.' &&
                   (isalpha((unsigned char)s[p + 1]) || s[p + 1] == '_')) {
                for (p++; p < s.size() && isid(s[p]);) p++;
            }
            if (bases && p != e) bases->push_back(s.substr(b, e - b));
            if (s.compare(p, 2, ", ") != 0) return p;
            p += 2;
        }
    };
    size_t from = 0;
    if (s.compare(0, 4, "var ") == 0) {
        from = names(4, nullptr);
    } else if (s.compare(0, 4, "for ") == 0) {
        auto e = names(4, nullptr);
        if (s.compare(e, 4, " := ") == 0) from = e;
    } else {
        vector<string_view> bases;
        auto e = names(0, &bases);
        if (e > 0 && (s.compare(e, 3, " = ") == 0 || s.compare(e, 4, " := ") == 0)) {
            from = e;
            for (auto b : bases) uses[string(b)]++;
        }
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
// outside its string literals, into addressed: not where a load or a store
// takes it for itself alone, `&` marked (CodeGenGo::TMARK, TransientAddr).
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

// A line's code with each identifier `ren` maps renamed, outside its string
// literals and comments, but for a field selector's: the first of the pair
// where it is used, the second where `&` takes its address (and nothing
// selects a part of it after), marked or not (TMARK).
static string GoRename(string_view s, const unordered_map<string, pair<string, string>> &ren) {
    auto isid = [](char c) { return isalnum((unsigned char)c) || c == '_'; };
    string out;
    out.reserve(s.size() + 16);
    for (size_t p = 0; p < s.size();) {
        auto c = s[p];
        // The identifier `&` (or `&` and its mark) takes the address of.
        auto at = p + 1 < s.size() && s[p + 1] == CodeGenGo::TMARK[0] ? p + 2 : p + 1;
        if (c == '"' || c == '\'') {
            auto b = p;
            for (p++; p < s.size() && s[p] != c; p++)
                if (s[p] == '\\') p++;
            p++;
            out.append(s.substr(b, p - b));
        } else if (c == '/' && p + 1 < s.size() && s[p + 1] == '/') {
            out.append(s.substr(p));
            break;
        } else if (c == '&' && at < s.size() && (isalpha((unsigned char)s[at]) || s[at] == '_') &&
                   (p == 0 || s[p - 1] != '&')) {
            auto b = at, e = b;
            while (e < s.size() && isid(s[e])) e++;
            auto it = ren.find(string(s.substr(b, e - b)));
            if (it == ren.end()) {
                out.append(s.substr(p, e - p));
            } else if (e < s.size() && (s[e] == '.' || s[e] == '[')) {
                Append(out, "&", it->second.first);
            } else {
                out += it->second.second;
            }
            p = e;
        } else if (isalpha((unsigned char)c) || c == '_') {
            auto b = p;
            while (p < s.size() && isid(s[p])) p++;
            auto id = s.substr(b, p - b);
            auto it = b > 0 && s[b - 1] == '.' ? ren.end() : ren.find(string(id));
            if (it == ren.end()) out.append(id);
            else out += it->second.first;
        } else if (isdigit((unsigned char)c)) {
            auto b = p;
            while (p < s.size() && isid(s[p])) p++;
            out.append(s.substr(b, p - b));
        } else {
            out += c;
            p++;
        }
    }
    return out;
}

// A finished body's declarations without their markers (Decl), each local
// nothing reads followed by a use of its own: Go rejects a local it never
// sees read, and an optimized body may well leave one, or only assign it.
//
// Where `frames`, a body whose function takes the block (gs): each local or
// parameter whose address it takes lives in the function's frame on the
// fixed-frame stack instead of on Go's stack (codegen_go.h, ltypes). Its
// declaration becomes a pointer to its slot, which every use dereferences;
// the frame is taken as the body starts, its slots' offsets constants Go
// works out from their types, and given back at every return (the values
// returned read first) and where the body's end is reached (FRAMEEND).
// `rtypes` are the function's Go result types; `framed` reports a frame.
inline string CodeGenGo::FinishBody(const string &b, bool frames, const vector<string> &rtypes,
                                    bool *framed) {
    unordered_map<string, int> uses;
    unordered_set<string> addressed;
    vector<string_view> lines;
    for (size_t pos = 0; pos < b.size();) {
        auto nl = b.find('\n', pos);
        if (nl == string::npos) nl = b.size();
        lines.push_back(string_view(b).substr(pos, nl - pos));
        pos = nl + 1;
    }
    auto trimmed = [](string_view line) { return line.substr(std::min(line.size(), line.find_first_not_of('\t'))); };
    // A root owner's descriptor that no other line names is never read: its
    // lines go (ROOTMARK, BindLocal), and the addresses of the components
    // they take with them, which leaves those locals Go's own.
    if (b.find(ROOTMARK[0]) != string::npos) {
        unordered_map<string, int> named;
        for (auto line : lines)
            if (line.find(ROOTMARK[0]) == string_view::npos)
                GoLineUses(line.substr(0, line.find(DECLMARK[0])), named);
        vector<string_view> kept;
        for (auto line : lines) {
            auto m = line.find(ROOTMARK[0]);
            if (m == string_view::npos) kept.push_back(line);
            else if (named[string(line.substr(m + 1))]) kept.push_back(line.substr(0, m));
        }
        lines = std::move(kept);
    }
    for (auto line : lines) {
        if (trimmed(line) == FRAMEEND) continue;
        GoLineUses(line.substr(0, line.find(DECLMARK[0])), uses);
        GoLineAddressed(line.substr(0, line.find(DECLMARK[0])), addressed);
    }
    // The frame's slots: the address-taken locals the declarations name, in
    // their order, then the parameters.
    vector<pair<string, string>> slots;   // (name, Go type)
    unordered_map<string, int> slotof;
    unordered_map<string, pair<string, string>> ren;
    if (frames) {
        for (auto line : lines) {
            auto m = line.find(DECLMARK[0]);
            if (m == string_view::npos) continue;
            auto names = line.substr(m + 1);
            for (size_t p = 0; p <= names.size();) {
                auto e = names.find(',', p);
                if (e == string_view::npos) e = names.size();
                auto n = string(names.substr(p, e - p));
                p = e + 1;
                auto t = ltypes.find(n);
                if (n.empty() || t == ltypes.end() || slotof.count(n)) continue;
                if (!addressed.count(n) && !largegts.count(t->second)) continue;
                slotof[n] = (int)slots.size();
                slots.push_back({ n, t->second });
                ren[n] = { cat("(*", n, ")"), n };
            }
        }
        for (auto &[n, t] : ptypes) {
            if (!addressed.count(n) || slotof.count(n)) continue;
            auto pn = cat("gs_p", slots.size());
            slotof[n] = (int)slots.size();
            slots.push_back({ n, t });
            ren[n] = { cat("(*", pn, ")"), pn };
        }
    }
    if (framed) *framed = !slots.empty();
    string out;
    out.reserve(b.size());
    auto slotptr = [&](int k, const string &t) { return cat("(*", t, ")(unsafe.Add(gs_fr, gs_fo", k, "))"); };
    if (!slots.empty()) {
        // Each slot 8-aligned, and of a byte at least, so that no two
        // variables share an address (§3.8).
        out += "\tconst (\n\t\tgs_fo0 = 0\n";
        for (size_t k = 0; k < slots.size(); k++)
            Append(out, "\t\tgs_fo", k + 1, " = gs_fo", k, " + (max(unsafe.Sizeof(*new(", slots[k].second,
                   ")), 1)+7)&^7\n");
        Append(out, "\t)\n\tgs_fr := gs.Frame\n\tif uintptr(gs.FrameEnd)-uintptr(gs_fr) < gs_fo", slots.size(),
               " {\n\t\tGsFrameOverflow()\n\t}\n\tgs.Frame = unsafe.Add(gs_fr, gs_fo", slots.size(), ")\n");
        for (auto &[n, t] : ptypes) {
            auto it = slotof.find(n);
            if (it == slotof.end()) continue;
            auto pn = ren[n].second;
            Append(out, "\t", pn, " := ", slotptr(it->second, t), "\n\t*", pn, " = ", n, "\n");
        }
    }
    for (auto line : lines) {
        auto indent = line.substr(0, line.find_first_not_of('\t'));
        if (trimmed(line) == FRAMEEND) {
            if (!slots.empty()) Append(out, indent, "gs.Frame = gs_fr\n");
            continue;
        }
        auto m = line.find(DECLMARK[0]);
        auto code = line.substr(0, m);
        auto body = trimmed(code);
        string marks = m == string_view::npos ? string() : string(line.substr(m + 1));
        if (!slots.empty()) {
            string done;
            if (body.compare(0, 4, "var ") == 0) {
                // var NAME TYPE [= INIT]
                auto ne = body.find(' ', 4);
                auto n = string(body.substr(4, ne == string_view::npos ? string_view::npos : ne - 4));
                if (auto it = slotof.find(n); it != slotof.end() && ne != string_view::npos) {
                    auto rest = body.substr(ne + 1);
                    auto eq = rest.find(" = ");
                    auto t = string(rest.substr(0, eq));
                    Append(done, indent, n, " := ", slotptr(it->second, t), "\n", indent, "*", n, " = ",
                           eq == string_view::npos ? cat("*new(", t, ")") : GoRename(rest.substr(eq + 3), ren), "\n");
                }
            } else if (auto as = body.find(" := "); as != string_view::npos && m != string_view::npos) {
                // N1, N2 := X: a slot's name receives through a temporary.
                auto lhs = body.substr(0, as);
                string nl, after;
                auto any = false;
                for (size_t p = 0; p <= lhs.size();) {
                    auto e = lhs.find(", ", p);
                    if (e == string_view::npos) e = lhs.size();
                    auto n = string(lhs.substr(p, e - p));
                    p = e + 2;
                    if (auto it = slotof.find(n); it != slotof.end()) {
                        auto v = cat("gs_v", it->second);
                        Append(after, indent, n, " := ", slotptr(it->second, slots[it->second].second), "\n",
                               indent, "*", n, " = ", v, "\n");
                        for (size_t q = 0; (q = marks.find(n, q)) != string::npos;) {
                            auto endq = q + n.size();
                            if ((q == 0 || marks[q - 1] == ',') && (endq == marks.size() || marks[endq] == ',')) {
                                marks.replace(q, n.size(), v);
                                break;
                            }
                            q = endq;
                        }
                        n = v;
                        any = true;
                    }
                    Append(nl, nl.empty() ? "" : ", ", n);
                }
                if (any) Append(done, indent, nl, " := ", GoRename(body.substr(as + 4), ren), "\n", after);
            }
            if (done.empty()) {
                auto rc = GoRename(body, ren);
                if (rc == "return" || rc.compare(0, 7, "return ") == 0) {
                    // The values first, into temporaries of the results'
                    // types, then the frame given back.
                    if (rc == "return" || rtypes.empty()) {
                        Append(done, indent, "gs.Frame = gs_fr\n", indent, rc, "\n");
                    } else {
                        string rs;
                        Append(done, indent, "{\n");
                        for (size_t i = 0; i < rtypes.size(); i++) {
                            Append(rs, i ? ", " : "", "gs_r", i);
                            Append(done, indent, "\tvar gs_r", i, " ", rtypes[i], "\n");
                        }
                        Append(done, indent, "\t", rs, " = ", rc.substr(7), "\n", indent, "\tgs.Frame = gs_fr\n",
                               indent, "\treturn ", rs, "\n", indent, "}\n");
                    }
                } else {
                    Append(done, indent, rc, "\n");
                }
            }
            out += done;
        } else {
            Append(out, code, "\n");
        }
        if (m == string_view::npos) continue;
        for (size_t p = 0; p <= marks.size();) {
            auto e = marks.find(',', p);
            if (e == string::npos) e = marks.size();
            auto n = marks.substr(p, e - p);
            if (!n.empty() && !uses[n] && !slotof.count(n)) Append(out, indent, "_ = ", n, "\n");
            p = e + 1;
        }
    }
    // The marks of the addresses a load or a store takes for itself alone.
    out.erase(std::remove(out.begin(), out.end(), TMARK[0]), out.end());
    return out;
}

inline void CodeGenGo::EmitSpec(FnSpec *sp, bool er) {
    curspec = sp;
    curinfo = &sinfo[sp];
    emiter = er;
    ResetFnState();
    spexpr = curinfo->needssp ? "gs_sp" : "0";
    PushSc(SC_FN);
    auto sig = Signature(sp);
    // The count each resizable result returns (the frame object, whole), in
    // the frame; the named results built at their destinations (§7.3).
    dstcounts.assign(sp->rets.size(), "");
    for (size_t i = 0; i < sp->rets.size(); i++) {
        if (!IsResz(sp->rets[i]) && !(er && i == 0)) continue;
        dstcounts[i] = Unique2(cat("gs_rl", i));
        Decl(dstcounts[i], IsFrameObj(sp->rets[i]) ? RzGoT(sp->rets[i], sp->sf->line) : "int64");
    }
    // In the element-run form a named result is not built at the
    // destination: the copy at the return is the cost of the run (§7.3).
    if (!er) DetectNrvo(sp);
    if (fromids.count(sp)) {
        // A long-distance return's target: its non-fixed results' channels
        // name its destinations while it runs, for the values in flight.
        EnsureFromChannels(sp);
        auto tid = fromids[sp];
        for (size_t i = 0; i < sp->rets.size(); i++) {
            // A result built where it is received names its receiver.
            if (NeedsResultLocation(sp->rets[i])) {
                auto sav = T();
                auto lret = GlName(cat("gsLret_", tid, "_", i));
                Decl(sav, "unsafe.Pointer", lret);
                L(lret, " = gs_r", i);
                fdstsaves.push_back(cat(lret, " = ", sav));
                continue;
            }
            if (!IsBytesT(sp->rets[i])) continue;
            auto sav = T();
            auto fdst = GlName(cat("gsFdst_", tid, "_", i));
            Decl(sav, "int64", fdst);
            L(fdst, " = gs_dst", i);
            fdstsaves.push_back(cat(fdst, " = ", sav));
        }
    }
    // A body that only chooses among its values, as std's min and max do
    // where nothing is inlined: one return of the choice
    // (codegen_go_select.h).
    if (!GenSelectReturn(sp)) {
        GenStmts(sp->body->stmts);
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
    }
    // Where Go's flow reaches the end of a function with results, the
    // checker found that it never does (an abort, say): Go needs a statement
    // it knows ends the flow.
    if (!dead && !sp->rets.empty()) {
        L("panic(GsUnreachable(", LocArgs(sp->sf->line), "))");
        dead = true;
    }
    PopSc();
    L(FRAMEEND);
    if (stkmax > 0 && !curinfo->needssp)
        Fail(sp->sf->line, cat("internal: ", curinfo->name, " uses data stacks without gs_sp"));
    stackown[sp] = std::max(stackown[sp], stkmax);
    auto framed = false;
    auto fb = FinishBody(body, true, curgorets, &framed);
    if (framed) hasframe.insert(sp);
    // A located function's checks report its caller's place, read once.
    if (curinfo->located && fb.find("gs_atf") != string::npos)
        fb = cat("\tgs_atf, gs_atl := gs_at.File, gs_at.Line\n", fb);
    if (UsesGl(fb)) glusers.insert(sp);
    if (fb.find("gsGStk_") != string::npos) gstkusers.insert(sp);
    // So does one naming a root global whose components are on stacks of
    // their own, whose descriptor a call it makes may reach them through.
    if (!gstkusers.count(sp)) {
        auto reach = false;
        function<void(Node *)> walk = [&](Node *n) {
            if (!n || reach) return;
            if (auto id = Is<Ident>(n); id && id->vdef && id->vdef->isglobal && id->vdef->type &&
                                        IsRoot(id->vdef->type) && NeedsStack(id->vdef->type))
                reach = true;
            RunChildren(n, walk);
        };
        walk(sp->body);
        if (reach) gstkusers.insert(sp);
    }
    Append(code, "// ", sp->sf->qname, ", line ", sp->sf->line.line, er ? ", its element-run twin" : "", "\n",
           sig, " {\n", fb, "}\n\n");
    curspec = nullptr;
    curinfo = nullptr;
    emiter = false;
}

// ------------------------------------------------------------------
// Globals (§11.1): package-level variables, initialized in declaration
// order by gsInitGlobals before main runs. A `let` global of a `const`
// scalar type with a literal initializer is never written (§9.5), and Go
// initializes it before anything runs, as the C backend's static data is;
// and so is every other global C makes static data whose value Go can
// spell as a constant, a table of them say (StaticConst, issue #62).

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

// The value of a global the C backend makes static data (cstatic), n its
// initializer, as a Go constant expression of t's Go type, where Go can spell
// one (issue #62): an integer, a float or a bool literal, and a fixed array
// or a native struct of those, built of literals, a string's bytes, fills
// and the static globals they name. Go's linker lays such a global out with
// its value, so that it holds it before anything runs, a library's start
// included, and gsInitGlobals has nothing of it to do. A value Go has no
// constant for -- a NaN, an infinity, a negative zero -- and one held as
// the bytes of its C layout (GR_BYTES), which hold its fields in the
// target's byte order, are left to gsInitGlobals.
inline bool CodeGenGo::StaticConst(Node *n, TypeExpr *t, string &out, bool elem) {
    if (!n || !t) return false;
    if (auto id = Is<Ident>(n)) {
        auto it = id->vdef ? cstatic.find(id->vdef) : cstatic.end();
        return it != cstatic.end() && it->second != n && StaticConst(it->second, t, out, elem);
    }
    switch (t->kind) {
        case TY_INT: {
            auto i = Is<IntLit>(n);
            if (!i || t->intstorage == IS_VARINT || t->ix) return false;
            out = i->uns || (i->val < 0 && t->intstorage == IS_U64) ? cat((uint64_t)i->val) : cat(i->val);
            return true;
        }
        case TY_FLT: {
            auto f = Is<FltLit>(n);
            if (!f) return false;
            auto v = f->val;
            auto f32 = t->fltstorage == FS_F32;
            if (v != v || std::isinf(v) || (f32 && std::fabs(v) >= std::ldexp(2.0 - std::ldexp(1.0, -24), 127)) ||
                ((f32 ? (float)v == 0 : v == 0) && std::signbit(v)))
                return false;
            out.clear();
            CatOne(out, v);
            return true;
        }
        case TY_BOOL: {
            auto b = Is<BoolLit>(n);
            if (!b) return false;
            out = b->val ? "true" : "false";
            return true;
        }
        case TY_ARRAY: {
            if (t->arr->akind != A_FIXED) return false;
            auto k = ArrSize(t->arr);
            auto sub = t->arr->sub;
            vector<string> elems;
            if (auto s = Is<StrLit>(n)) {
                if (!IsU8(sub) || (int64_t)s->val.size() != k) return false;
                for (auto c : s->val) elems.push_back(cat((int)(uint8_t)c));
            } else if (auto al = Is<ArrayLit>(n)) {
                if (al->fillval) {
                    string e;
                    if (!StaticConst(al->fillval, sub, e, true)) return false;
                    elems.assign((size_t)k, e);
                } else {
                    if ((int64_t)al->elems.size() != k) return false;
                    for (auto x : al->elems) {
                        string e;
                        if (!StaticConst(x, sub, e, true)) return false;
                        elems.push_back(e);
                    }
                }
            } else {
                return false;
            }
            // An element's type is the array's: Go takes it as said.
            out = elem ? string("{") : cat(GoT(t, n->line), "{");
            for (size_t i = 0; i < elems.size(); i++) Append(out, i ? ", " : "", elems[i]);
            out += "}";
            return true;
        }
        case TY_STRUCT: {
            auto sl = Is<StructLit>(n);
            auto si = SI(t);
            if (!sl || sl->sinst != si || TI(t, n->line).rep != GR_NATIVE) return false;
            string fields;
            for (size_t fi = 0; fi < si->st->fields.size(); fi++) {
                if (si->st->fields[fi].ispad) continue;
                string v;
                // A field's value names its type where it is a composite.
                if (!StaticConst(sl->InitFor((int)fi), si->ftypes[fi], v, false)) return false;
                Append(fields, fields.empty() ? "" : ", ", Sanitize(si->st->fields[fi].name), ": ", v);
            }
            out = cat(elem ? string() : TI(t, n->line).name, "{", fields, "}");
            return true;
        }
        default: return false;
    }
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
    set<TypeExpr *> namedtypes;
    function<void(Node *)> mark = [&](Node *n) {
        if (!n) return;
        auto use = [&](VarDef *v) {
            if (!usedglobals.insert(v).second) return;
            auto it = declof.find(v);
            if (it != declof.end() && it->second.second < it->second.first->inits.size())
                mark(it->second.first->inits[it->second.second]);
        };
        if (auto id = Is<Ident>(n); id && id->vdef && id->vdef->isglobal) use(id->vdef);
        // The arrays the types it names name too: an index type's, a pool a
        // relative reference measures from, a root owner's component's,
        // which is there with its owner.
        NodeNamedArrays(n, namedtypes, [&](VarDef *a, bool) {
            if (!a || !a->isglobal) return;
            use(a->componentowner ? a->componentowner : a);
        });
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
        // such global, a root owner's component included.
        for (auto d : g->defs) pure = pure && !IsBytesT(d->type) && !(IsRoot(d->type) && NeedsStack(d->type));
        if (pure) continue;
        for (auto d : g->defs) usedglobals.insert(d);
        for (auto i : g->inits) mark(i);
    }
    // A root owner's components are there wherever it is.
    for (auto g : ast.globals)
        for (auto d : g->defs)
            if (usedglobals.count(d) && d->type && IsRoot(d->type))
                for (auto c : d->components)
                    if (c) usedglobals.insert(c);
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
    // A global of a program with instances is a member of the program
    // instance (gsGlobals), but for read-only static data, which every
    // instance shares (§6.3); a dedicated stack is the stack of the block
    // the instance is on, whose index is the instance's too where the program
    // has workers, whose blocks lay their stacks out each its own way
    // (codegen_go_threads.h).
    auto var = [&](const VarDef *d, const string &name, const string &gt) {
        if (instances && !cstatic.count(d)) {
            Append(glfields, "\t", name, " ", gt, "\n");
            return GlName(name);
        }
        Append(gdecls, "var ", name, " ", gt, "\n");
        return name;
    };
    // A global's place, or a root owner's component's, which is a global of
    // its own: a value on a data stack is its header (or base) and a
    // dedicated stack, which follows the main program's in its block
    // (gsInitGlobals sets it up).
    auto onstack = [&](VarDef *d, const string &name, Line ln) {
        GoT(d->type, ln);
        auto stk = Unique(cat("gsGStk_", name));
        gnames[d] = var(d, name, IsResz(d->type) ? RzGoT(d->type, ln) : "unsafe.Pointer");
        gstks[d] = workers ? var(d, stk, "int64") : stk;
    };
    for (auto g : ast.globals) {
        for (size_t di = 0; di < g->defs.size(); di++) {
            auto d = g->defs[di];
            if (!usedglobals.count(d)) continue;
            auto name = Unique(Sanitize(g->ns, d->name));
            if (IsRoot(d->type)) {
                // A root owner (CodeGen::EmitGlobalDecls): its descriptor,
                // and each component a global of its own, owner_field.
                gnames[d] = var(d, name, GoT(d->type, g->line));
                for (auto c : d->components) {
                    if (!c) continue;
                    string cname(c->name);
                    for (auto &ch : cname)
                        if (ch == '.') ch = '_';
                    auto cn = Unique(Sanitize(g->ns, cname));
                    if (IsBytesT(c->type)) onstack(c, cn, g->line);
                    else gnames[c] = var(c, cn, GoT(c->type, g->line));
                }
                continue;
            }
            if (IsBytesT(d->type)) {
                // A header (or base) and a dedicated stack, which follows the
                // main program's in its block (gsInitGlobals sets it up).
                if (di >= g->inits.size())
                    Unsupported(g->line, cat("a global of type ", TypeText(d->type), " without an initializer"));
                GoT(d->type, g->line);
                auto stk = Unique(cat("gsGStk_", name));
                gnames[d] = var(d, name, IsResz(d->type) ? RzGoT(d->type, g->line) : "unsafe.Pointer");
                gstks[d] = workers ? var(d, stk, "int64") : stk;
                if (d->reusable) {
                    auto fln = Unique(cat(name, "_fl"));
                    auto flstk = Unique(cat("gsGStk_", fln));
                    gpools[d] = { var(d, fln, "GsRhdr"), workers ? var(d, flstk, "int64") : flstk };
                }
                continue;
            }
            if (PrefVar(d)) {
                gnames[d] = var(d, name, "GsPref");
                continue;
            }
            auto gt = GoT(d->type, g->line);
            string init;
            if (!d->isvar && d->type->cq && g->inits.size() == g->defs.size() &&
                StaticInit(g->inits[di], d->type, init)) {
                Append(gdecls, "var ", name, " ", gt, " = ", init, "\n");
                gnames[d] = name;
                gstatic.insert(d);
            } else if (auto cs = cstatic.find(d); cs != cstatic.end() && StaticConst(cs->second, d->type, init)) {
                // Static data, as C's is: a constant composite literal,
                // which the linker lays out (issue #62).
                Append(gdecls, "var ", name, " = ", init, "\n");
                gnames[d] = name;
                gstatic.insert(d);
            } else {
                gnames[d] = var(d, name, gt);
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
            InitGlobalValue(d, g->inits[i]);
        }
        // An initializer that never completes (a `loop {}`) leaves the ones
        // after it unreached, for Go as for the program.
        PopSc();
    }
    PopSc();
    L(FRAMEEND);
    stackown[nullptr] = std::max(stackown[nullptr], stkmax);
    auto fb = FinishBody(body, true);
    UsesGl(fb);
    initempty = fb.find_first_not_of(" \t\n") == string::npos;
    initat = code.size();
    Append(code, "// The globals' initializers, in declaration order (§11.1).\n"
                 "func gsInitGlobals(gs *GsStackBlock) {\n\t_ = gs\n", fb, "}\n\n");
    initlen = code.size() - initat;
}

// A global's value, from its initializer (CodeGen::InitGlobalValue): a root
// owner's components each as a global of its own, in declaration order, and
// its descriptor the references to them.
inline void CodeGenGo::InitGlobalValue(VarDef *d, Node *init) {
    if (IsRoot(d->type)) {
        auto sl = Is<StructLit>(init);
        if (!sl || !sl->sinst || !sl->sinst->st->isroot)
            Fail(d->line, cat("internal: the root owner ", d->name, " is not built by its literal"));
        auto si = SI(d->type);
        for (auto c : d->components) {
            if (!c) continue;
            auto i = c->componentfield;
            InitGlobalValue(c, sl->InitFor(i));
            auto cl = VarLoc(c, init->line);
            auto ref = c->type->kind == TY_REF ? RefOfLoc(cl) : RefValOf(cl, init->line);
            L(gnames[d], ".", Sanitize(si->st->fields[i].name), " = ", ref);
        }
        return;
    }
    if (IsBytesT(d->type)) {
        InitGlobalStack(d);
        GenConstruct(init, gstks[d], d->type, IsResz(d->type) ? RzLenLv(d->type, gnames[d]) : "");
        return;
    }
    if (PrefVar(d)) {
        L(gnames[d], " = ", GenPrefVal(init));
        return;
    }
    if (IsLargeFixed(d->type) && (Is<ArrayLit>(init) || Is<StructLit>(init) || Is<StrLit>(init))) {
        // Built where it stays. Where the C backend's initializer builds it,
        // it takes a data stack for the value first.
        if (!cstatic.count(d)) AllocStk();
        auto dst = AtVar(gnames[d], d->type);
        if (auto sl = Is<StrLit>(init)) StrLitInto(sl, dst);
        else LitInto(init, dst, true);
        return;
    }
    GenAny(init, GoDst { GD_LVALUE, gnames[d], d->type });
}

// A global's dedicated stack, and its pool's freelist's, set up where its
// initializer runs (CodeGen::InitGlobalStack): its header at the stack's
// base.
inline void CodeGenGo::InitGlobalStack(VarDef *d) {
    auto stk = gstks[d];
    // Main's instance names the stacks of the main program's layout, which
    // the constants gsGStk_<name> give (EmitProgram).
    auto index = [&](const string &s) {
        auto raw = workers ? s.substr(6) : s;
        gstkorder.push_back(s);
        gstkfields.push_back(raw);
        if (workers) L(s, " = ", raw);
    };
    globalregions++;
    index(stk);
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
        index(p.second);
        L(p.first, ".Base = ", Top(p.second));
        L(p.first, ".Len = 0");
    }
}

// The program's entry: once every function is emitted, the counts are
// final, and main reserves the main program's data stacks, initializes the
// globals and runs fn main(). A library has no main, and no init: the first
// call of an export fn's entry point starts it (gsLibStart, issue #18),
// which initializes the globals, and only those, on the package's block,
// so that importing the package does nothing. The Go program that imports
// it runs instead of its fn main(), if it has one. A library with nothing
// to start -- no global to initialize, no worker to spawn -- has no start
// (issue #19): its entry points call the functions directly.
inline void CodeGenGo::EmitProgram() {
    BoundStacksOf(*this);
    auto mainstacks = ProgramStacksOf(*this, MainRootsOf(*this));
    auto mainsf = ast.MainFunction();
    CheckStackLimitOf(*this, "the main program", mainstacks, mainsf ? mainsf->line : Line {});
    // The counts are final: the workers' thunks, each opening its program's
    // block, and the most regions the main program and any one worker hold,
    // which give hardware_threads() its cap (gs_rt_init).
    int64_t workerregions = 0;
    for (auto &[name, sp] : WorkerEntries()) {
        auto stacks = ProgramStacksOf(*this, { sp });
        CheckStackLimitOf(*this, cat("thread program ", name), stacks, sp->sf->line);
        EmitThreadThunk(sp, stacks);
        workerregions = std::max(workerregions, stacks + thunkregions[sp]);
    }
    string call;
    if (!opts.library && mainsf && !mainsf->specs.empty() && sinfo.count(mainsf->specs[0])) {
        auto msp = mainsf->specs[0];
        auto &mi = sinfo[msp];
        call = cat("\t", mi.name, "(", mi.needssp ? "gs, 0" : mi.needsgs ? "gs" : GsArgPh(msp), ")\n");
    }
    // The globals' dedicated stacks follow the main program's in its block,
    // where main's instance has them (and an export's call's, in a program
    // with instances).
    if (!gstkfields.empty()) {
        Append(gdecls, "\n// The dedicated data stacks of the globals that live on one, which follow\n"
                       "// the main program's own in its block.\nconst (\n");
        for (size_t i = 0; i < gstkfields.size(); i++)
            Append(gdecls, "\t", gstkfields[i], " = ", mainstacks + (int64_t)i, "\n");
        if (instances) Append(gdecls, "\tgsGStkEnd = ", mainstacks + globalregions, "\n");
        Append(gdecls, ")\n");
    }
    // Under GS_STACK_STATS the program reports, as it ends, the stacks it
    // took, as C's gs_stack_stats does.
    string stats;
    if (opts.stackstats && !opts.library) stats = cat("\tgsProg.StackStats(gs, ", mainstacks, ")\n");
    // In a program with instances, main's block runs main's instance, which
    // the export fns' entry points copy their globals from.
    string inst;
    if (instances) inst = "\tgs.Gl = unsafe.Pointer(&gsGlobalsMain)\n\tgsMainBlock = gs\n";
    auto start = cat("\tgsProg.Start(", opts.stacks.budget, ", ", mainstacks + globalregions, ", ", workerregions, ")\n"
                     "\tgs := gsProg.NewStackBlock(", mainstacks + globalregions, ", gsStackReserve, gsStackGap)\n",
                     inst, "\tgsInitGlobals(gs)\n");
    libstart = opts.library && (instances || !initempty || globalregions > 0);
    if (opts.library && !libstart) {
        // Nothing to start, and no gsInitGlobals for a start to call.
        code.erase(initat, initlen);
    } else if (!opts.library) {
        Append(code, "func main() {\n"
                     "\tdefer GsExitOnAbort()\n"
                     "\tdebug.SetPanicOnFault(true)\n",
               start, call, stats, "}\n");
    } else {
        // The host owns the process, its goroutines' fault handling and its
        // memory: the start runs on the goroutine of the call that makes it,
        // turns panic on fault on for itself only, and never exits the
        // process. A start that panics leaves what it panicked with for every
        // call.
        Append(code, "// The library's start (issue #18), which the first call of an export fn's\n"
                     "// entry point makes (gsLibStart), once, rather than the package's init: the\n"
                     "// runtime, the package's block and the globals. Importing the package maps\n"
                     "// no memory, sets no goroutine's fault handling and runs no initializer.\n"
                     "// Where the start panics, an abort of an initializer's included, it exits\n"
                     "// no process: that call and every later one panic with what it did, a\n"
                     "// *GsError for an abort.\n"
                     "var gsLib struct {\n\tonce sync.Once\n\tok   uint32 // 1 once the start has completed, read atomically.\n"
                     "\tfail any    // What it panicked with where it did not.\n}\n\n"
                     "// gsLibStart makes sure the library has started: once it has, an atomic\n"
                     "// load, which keeps the smallest entry points within what Go inlines.\n"
                     "func gsLibStart() {\n\tif atomic.LoadUint32(&gsLib.ok) == 0 {\n\t\tgsLibStartSlow()\n\t}\n}\n\n"
                     "// gsLibStartSlow starts the library where no call has yet, or waits for\n"
                     "// the call that is, and panics as the start did where that failed.\n"
                     "func gsLibStartSlow() {\n\tgsLib.once.Do(gsLibInit)\n\tif atomic.LoadUint32(&gsLib.ok) == 0 {\n"
                     "\t\tpanic(gsLib.fail)\n\t}\n}\n\n"
                     "// gsLibInit is the start, on the goroutine of the call making it, whose\n"
                     "// panic on fault it turns on for the start only.\n"
                     "func gsLibInit() {\n"
                     "\tdefer func() {\n\t\tif atomic.LoadUint32(&gsLib.ok) == 0 {\n"
                     "\t\t\tgsLib.fail = GsAbortOf(recover())\n\t\t\tpanic(gsLib.fail)\n\t\t}\n\t}()\n"
                     "\tdefer debug.SetPanicOnFault(debug.SetPanicOnFault(true))\n",
               start, "\tatomic.StoreUint32(&gsLib.ok, 1)\n}\n");
    }
    // Every body is emitted, and the counts are final: the export fns' entry
    // points can say what they hand on.
    ComputeGsNeeds();
    EmitExports();
}

// The place of a call's block argument where the callee may yet take it.
inline string CodeGenGo::GsArgPh(FnSpec *sp) {
    phcalls[curspec].insert(sp);
    return cat(GSARG, specnum.at(sp), GSARG);
}

// Once every specialization's body is emitted: one takes the block where it
// did already, where its frame holds a local, and where it calls one that
// takes it.
inline void CodeGenGo::ComputeGsNeeds() {
    auto &needs = gsneeds;
    needs.assign(numspec.size(), 0);
    for (size_t i = 0; i < numspec.size(); i++)
        needs[i] = sinfo[numspec[i]].needsgs || hasframe.count(numspec[i]) || glusers.count(numspec[i]);
    for (auto changed = true; changed;) {
        changed = false;
        for (auto &[sp, callees] : phcalls) {
            if (!sp || !specnum.count(sp) || needs[specnum[sp]]) continue;
            for (auto k : callees)
                if (needs[specnum[k]]) {
                    needs[specnum[sp]] = 1;
                    changed = true;
                    break;
                }
        }
    }
}

// Every call's and signature's place for the block is filled, as
// ComputeGsNeeds found.
inline void CodeGenGo::ResolveGsArgs() {
    if (gsneeds.size() != numspec.size()) ComputeGsNeeds();
    auto &needs = gsneeds;
    auto fill = [&](string &text) {
        if (text.find(GSARG[0]) == string::npos && text.find(GSPARAM[0]) == string::npos) return;
        string out;
        out.reserve(text.size());
        for (size_t p = 0; p < text.size();) {
            auto c = text[p];
            if (c != GSARG[0] && c != GSPARAM[0]) {
                out += c;
                p++;
                continue;
            }
            auto e = text.find(c, p + 1);
            auto n = stoi(text.substr(p + 1, e - p - 1));
            p = e + 1;
            auto rest = text.compare(p, 2, ", ") == 0;
            if (needs[n]) {
                out += c == GSARG[0] ? "gs" : "gs *GsStackBlock";
            } else if (rest) {
                p += 2;
            }
        }
        text = std::move(out);
    };
    fill(code);
    fill(bcode);
    fill(eqcode);
}

// --go-build's expression in go/build/constraint's syntax -- tags (letters,
// digits, _ and .) under !, && and ||, and parentheses -- into canon as
// gofmt writes a //go:build line, which parenthesizes an && inside an ||
// and an || inside an && (constraint.Expr's String); false, with why,
// where it is not one.
static bool GoBuildConstraint(string_view text, string &canon, string &why) {
    struct Parser {
        string_view s;
        size_t i = 0;
        string_view tok;
        bool istag = false;
        string err;
        int size = 0;
        struct Expr {
            char op;   // t a tag, ! not, & and, | or
            string_view tag;
            int x, y;
        };
        vector<Expr> e;
        int Fail(string m) {
            if (err.empty()) err = std::move(m);
            tok = {};
            istag = false;
            return -1;
        }
        int New(char op, int x, int y, string_view tag = {}) {
            if (!err.empty()) return -1;
            e.push_back({ op, tag, x, y });
            return (int)e.size() - 1;
        }
        void Lex() {
            istag = false;
            while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
            if (i >= s.size()) {
                tok = {};
                return;
            }
            auto c = s[i];
            if (c == '(' || c == ')' || c == '!') {
                tok = s.substr(i++, 1);
                return;
            }
            if (c == '&' || c == '|') {
                if (i + 1 >= s.size() || s[i + 1] != c) {
                    Fail(cat("invalid syntax at ", string(1, c)));
                    return;
                }
                tok = s.substr(i, 2);
                i += 2;
                return;
            }
            auto b = i;
            while (i < s.size() && (isalnum((unsigned char)s[i]) || s[i] == '_' || s[i] == '.' ||
                                    (unsigned char)s[i] >= 0x80))
                i++;
            if (i == b) {
                Fail((unsigned char)c < 32 ? string("invalid syntax at a control character")
                                           : cat("invalid syntax at ", string(1, c)));
                return;
            }
            tok = s.substr(b, i - b);
            istag = true;
        }
        int Or() {
            auto x = And();
            while (err.empty() && tok == "||") x = New('|', x, And());
            return x;
        }
        int And() {
            auto x = Not();
            while (err.empty() && tok == "&&") x = New('&', x, Not());
            return x;
        }
        int Not() {
            if (++size > 1000) return Fail("build expression too large");
            Lex();
            if (!err.empty()) return -1;
            if (tok == "!") {
                Lex();
                if (tok == "!") return Fail("double negation not allowed");
                auto x = Atom();
                return New('!', x, -1);
            }
            return Atom();
        }
        int Atom() {
            if (!err.empty()) return -1;
            if (tok == "(") {
                auto x = Or();
                if (err == "unexpected end of expression") err = "missing close paren";
                if (!err.empty()) return -1;
                if (tok != ")") return Fail("missing close paren");
                Lex();
                return x;
            }
            if (!istag) return Fail(tok.empty() ? string("unexpected end of expression") : cat("unexpected token ", tok));
            auto t = tok;
            Lex();
            return New('t', -1, -1, t);
        }
        string Text(int x) const {
            auto &n = e[x];
            auto arg = [&](int k, char paren) {
                auto a = Text(k);
                return e[k].op == paren ? cat("(", a, ")") : a;
            };
            switch (n.op) {
                case 't': return string(n.tag);
                case '!': {
                    auto a = Text(n.x);
                    return cat("!", e[n.x].op == '&' || e[n.x].op == '|' ? cat("(", a, ")") : a);
                }
                case '&': return cat(arg(n.x, '|'), " && ", arg(n.y, '|'));
                default: return cat(arg(n.x, '&'), " || ", arg(n.y, '&'));
            }
        }
    };
    Parser p;
    p.s = text;
    auto x = p.Or();
    if (p.err.empty() && !p.tok.empty()) p.Fail(cat("unexpected token ", p.tok));
    if (!p.err.empty()) {
        why = p.err;
        return false;
    }
    canon = p.Text(x);
    return true;
}

// An --include file's Go as the generated file holds it, its package clause
// and imports gone (GoSplitInclude): each run of lines between blank ones
// under a //line directive naming the file and the run's first line, which
// keeps every position Go reports in it -- an error's, a panic's frame's --
// the file's own, whatever blank lines gofmt collapses. A run that starts
// with a comment at the package level that a declaration follows, its
// documentation, takes the directive at its end instead, after an empty
// comment line, which is where gofmt keeps a directive in a doc comment
// (go/printer's formatDocComment); a run of nothing but comments needs
// none. Each run follows a blank line, as text for the end of the file.
static string GoIncludeLines(const string &name, const string &body) {
    // Each line: whether it is blank, outside a raw string and a /* */
    // comment; nothing but comments; inside a raw string at its end, which
    // keeps the blanks there; a comment at its start; a // comment and
    // nothing else.
    struct L {
        size_t b, e;
        bool blank, comments, rawend, comment1, slashes;
    };
    vector<L> lines;
    auto state = 0;   // 1 in a raw string, 2 in a /* */ comment
    for (size_t pos = 0; pos <= body.size();) {
        auto nl = body.find('\n', pos);
        if (nl == string::npos) nl = body.size();
        L l { pos, nl, false, false, false, false, false };
        auto start = state;
        auto code = false, text = false;
        l.comment1 = !state && (body.compare(pos, 2, "//") == 0 || body.compare(pos, 2, "/*") == 0);
        auto first = std::min(body.find_first_not_of(" \t", pos), nl);
        l.slashes = !state && body.compare(first, 2, "//") == 0;
        for (auto i = pos; i < nl;) {
            auto c = body[i];
            if (state == 1) {
                if (c == '`') state = 0;
                code = true;
                i++;
            } else if (state == 2) {
                text = true;
                if (body.compare(i, 2, "*/") == 0) {
                    state = 0;
                    i += 2;
                } else {
                    i++;
                }
            } else if (c == ' ' || c == '\t' || c == '\r') {
                i++;
            } else if (body.compare(i, 2, "//") == 0) {
                text = true;
                break;
            } else if (body.compare(i, 2, "/*") == 0) {
                text = true;
                state = 2;
                i += 2;
            } else {
                code = true;
                if (c == '`') {
                    state = 1;
                    i++;
                } else if (c == '"' || c == '\'') {
                    for (i++; i < nl && body[i] != c; i++)
                        if (body[i] == '\\') i++;
                    i++;
                } else {
                    i++;
                }
            }
        }
        l.blank = !start && !code && !text;
        l.comments = !code && start != 1 && !l.blank;
        l.rawend = state == 1;
        lines.push_back(l);
        pos = nl + 1;
    }
    // A line as gofmt keeps it: without the blanks that end it.
    auto line = [&](const L &l) {
        auto e = l.e;
        if (!l.rawend)
            while (e > l.b && (body[e - 1] == ' ' || body[e - 1] == '\t' || body[e - 1] == '\r')) e--;
        return cat(body.substr(l.b, e - l.b), "\n");
    };
    // A directive in a comment (go/printer's isDirective): //line, //extern,
    // //export, or //word:word.
    auto directive = [&](const L &l) {
        string_view c(body.data() + l.b, l.e - l.b);
        if (c.substr(0, 2) != "//") return false;
        c.remove_prefix(2);
        if (c.rfind("line ", 0) == 0 || c.rfind("extern ", 0) == 0 || c.rfind("export ", 0) == 0) return true;
        auto colon = c.find(':');
        if (colon == 0 || colon == string_view::npos || colon + 1 >= c.size()) return false;
        for (size_t i = 0; i <= colon + 1; i++)
            if (i != colon && !(islower((unsigned char)c[i]) || isdigit((unsigned char)c[i]))) return false;
        return true;
    };
    string out;
    for (size_t a = 0; a < lines.size();) {
        if (lines[a].blank) {
            a++;
            continue;
        }
        auto b = a;
        while (b < lines.size() && !lines[b].blank) b++;
        out += "\n";
        auto m = a;
        while (m < b && lines[m].comments) m++;
        if (m == b) {
            // Comments alone.
        } else if (!lines[a].comment1) {
            Append(out, "//line ", name, ":", a + 1, "\n");
        } else {
            // A doc comment, when a declaration at the package level
            // follows it, which go/doc/comment lays out where it is all //
            // comments: the directive after the text, an empty comment line
            // between them.
            auto top = lines[m].e > lines[m].b && body[lines[m].b] != ' ' && body[lines[m].b] != '\t';
            auto slashes = true;
            for (; a < m; a++) {
                slashes = slashes && lines[a].slashes;
                out += line(lines[a]);
            }
            auto last = line(lines[m - 1]);
            if (top && slashes && !directive(lines[m - 1]) && last != "//\n") out += "//\n";
            Append(out, "//line ", name, ":", m + 1, "\n");
        }
        for (; a < b; a++) out += line(lines[a]);
    }
    return out;
}

// The settings a package's runtime file records, which every program
// compiled into the package must share (--go-runtime-file): what the
// runtime is to its files, the names it gives the runtime's, and the
// configuration its blocks are made with.
inline string GoRuntimeSettings(const GoOptions &o, uint64_t framereserve) {
    return cat("runtime=", o.runtime == GO_RT_IMPORT ? "import" : o.runtime == GO_RT_FULL ? "full" : "embed",
               " names=", o.unexported ? "unexported" : "exported", " stack-reserve=", o.stacks.reserve,
               " stack-gap=", o.stacks.gap, " frame-reserve=", framereserve);
}

// The options a file's Go depends on, as one command line gives them, which
// its header records (issue #21): each spelled one way, in one order, and
// only where it is not the default, but for the target and the level --
// whatever spelling the compile had (-o out.go for --target=go, a -D for
// --debug or --stack-reserve, an option's default given), so that two
// compiles that say the same write the same file, which goose --check
// compares a file with. The --include files are named as the file's //line
// directives name them and the runtime file as the header does, relative
// to the file; an argument a shell would split or read otherwise is quoted.
// What does not change the Go is left out: the other -D, which are C's,
// and --stdlib, whose place is the machine's.
inline string GoHeaderFlags(const GoOptions &o, uint64_t framereserve, uint64_t defaultframe) {
    auto arg = [](const string &s) {
        auto plain = !s.empty() && all_of(s.begin(), s.end(), [](char c) {
            return isalnum((unsigned char)c) || string_view("_@%+=:,./-").find(c) != string_view::npos;
        });
        if (plain) return s;
        string q = "'";
        for (auto c : s) q += c == '\'' ? string("'\\''") : string(1, c);
        return q + "'";
    };
    // A size as --stack-reserve and --frame-reserve take one: in the largest
    // unit it is a whole number of.
    auto size = [](uint64_t n) {
        for (auto [shift, unit] : { pair { 40, "T" }, pair { 30, "G" }, pair { 20, "M" }, pair { 10, "K" } })
            if (n && n % (1ull << shift) == 0) return cat(n >> shift, unit);
        return cat(n);
    };
    string s = cat("--target=go -O", o.optlevel);
    if (o.debug) s += " --debug";
    if (o.nobce) s += " --no-bce";
    if (o.library) s += " --library";
    if (o.package != (o.library ? "goose" : "main")) Append(s, " --go-package ", o.package);
    if (!o.build.empty()) Append(s, " --go-build ", arg(o.build));
    if (o.runtime != GO_RT_EMBED) Append(s, " --go-runtime=", o.runtime == GO_RT_IMPORT ? "import" : "full");
    if (!o.runtimefile.empty()) Append(s, " --go-runtime-file ", arg(o.runtimefile));
    if (o.unexported) s += " --go-unexported";
    if (o.nowithstack) s += " --go-no-withstack";
    // The runtime's configuration: --stack-reserve's where it takes the
    // size, and the -D that set it otherwise.
    auto &st = o.stacks, &def = o.defaults;
    if (st.reserve != def.reserve) {
        if (st.reserve >= (64ull << 10) && st.reserve <= (1ull << 48))
            Append(s, " --stack-reserve ", size(st.reserve));
        else
            Append(s, " -DGS_STACK_RESERVE=", st.reserve);
    }
    if (st.gap != def.gap) Append(s, " -DGS_STACK_GAP=", st.gap);
    if (st.budget != def.budget) Append(s, " -DGS_STACK_BUDGET=", st.budget);
    if (st.maxstacks != def.maxstacks) Append(s, " -DGS_MAX_STACKS=", st.maxstacks);
    if (o.stackstats) s += " -DGS_STACK_STATS=1";
    if (framereserve != defaultframe) Append(s, " --frame-reserve ", size(framereserve));
    for (auto &inc : o.includes) Append(s, " --include ", arg(inc.name));
    return s;
}

// The whole file: a build constraint where --go-build gives one, a header
// Go tools know generated code by, which records what wrote the file (the
// compiler's version, the runtime's revision and the options, issue #21),
// the package, the imports, the runtime configuration and the runtime --
// the part the program uses, all of it, or none, where it imports the
// runtime's package (codegen_go_runtime.h) -- then the program, laid out
// as gofmt lays it out (GoFormat), and the --include files. With --go-runtime-file the configuration and the
// runtime go to that file instead (runtimefiletext), which the package's
// programs share, and the program's own names carry its file's name.
inline string CodeGenGo::Source(const string &srcname) {
    auto &rt = GoRuntime();
    auto imp = opts.runtime == GO_RT_IMPORT, full = opts.runtime == GO_RT_FULL;
    auto shared = !opts.runtimefile.empty();
    // Go source is UTF-8 throughout, comments included: the file's name as
    // printable ASCII.
    auto ascii = [](string s) {
        for (auto &c : s)
            if ((uint8_t)c < 32 || (uint8_t)c >= 127) c = '?';
        return s;
    };
    string name = ascii(srcname);
    // A program with instances keeps its globals in them: one with workers,
    // and one whose export fns' calls reach a global's own data stack.
    string inst;
    if (workers)
        inst = "// The globals of a program instance (§11.1, §11.2), which every thread program has\n"
               "// of its own: main's is gsGlobalsMain, a worker's the copy its spawn took. Read-only\n"
               "// static data is not among them, which every instance shares as it is.\n";
    else if (instances)
        inst = "// The globals of a program instance (§11.1), which every block an export fn's call\n"
               "// runs on has of its own, a copy of the package's, whose values on data stacks are on\n"
               "// the block's: main's is gsGlobalsMain, on gsMainBlock. Read-only static data is not\n"
               "// among them, which every instance shares as it is.\n";
    if (instances)
        inst += cat("type gsGlobals struct {\n", glfields, "}\n\nvar gsGlobalsMain gsGlobals\n\n"
                    "// The block main's program runs, whose stacks hold main's instance's values.\n"
                    "var gsMainBlock *GsStackBlock\n\n");
    if (!qdecls.empty()) inst += cat("// The queues (§11.2), one per element type.\n", qdecls, "\n");
    if (usesos)
        inst += cat("// os.resource_dir's directory where it is not the executable's: the entry\n"
                    "// source's, for goose run.\nconst gsJitResourceDir = ", GoStr(opts.resourcedir), "\n\n");
    // The program's place in the runtime, where its Go names it: its
    // configuration, which the blocks it makes have, its pool, and whether
    // it is a library.
    auto progdecl = [&](bool library) {
        return cat("// The program's place in the runtime: the configuration its blocks are made with,\n"
                   "// the pool of them its export fns' entry points take theirs from, and the cap on\n"
                   "// its workers", library ? "; and it is a library, which never exits the process" : "", ".\n"
                   "var gsProg = GsProgram{StackReserve: gsStackReserve, StackGap: gsStackGap, FrameReserve: "
                   "gsFrameReserve", library ? ", Library: true" : "", "}\n\n");
    };
    // The package's own, in its runtime file, which the runtime's block API
    // makes blocks for (NewStackBlock, AcquireStackBlock), of the sizes the
    // package's programs share: each program has one of its own too.
    auto pkgprogdecl = string("// The package's place in the runtime, whose blocks the runtime's block API makes\n"
                              "// (NewStackBlock, AcquireStackBlock), of the configuration its programs share; each\n"
                              "// program has a place of its own too.\n"
                              "var gsProg = GsProgram{StackReserve: gsStackReserve, StackGap: gsStackGap, FrameReserve: "
                              "gsFrameReserve}\n\n");
    auto program = [&](bool withprog) {
        return cat("// ---- program ----\n\n", filedecls, filedecls.empty() ? "" : "\n",
                   locdecls.empty() ? "" : "// The places the standard library's functions report failures at: where the\n"
                                            "// program called them.\n",
                   locdecls, locdecls.empty() ? "" : "\n", withprog ? progdecl(opts.library) : "",
                   tdecls, inst, gdecls, gdecls.empty() ? "" : "\n", rfdecls, rfdecls.empty() ? "" : "\n", code,
                   xcode.empty() ? "" : "// ---- export fns' entry points ----\n\n", xcode, eqcode, bcode, ldstcode);
    };
    // What the program's Go and the --include files' use of the runtime's,
    // by the names the runtime declares them by; and of Go's packages.
    // The package the program's Go may name besides the runtime's: math/bits,
    // which std's popcount, clz and ctz call (codegen_go_export.h).
    auto known = rt.imports;
    known["bits"] = "math/bits";
    GoUses uses;
    GoScanUses(program(false), known, uses);
    auto proguses = uses;
    for (auto &[iname, ibody] : incbodies) {
        GoUses inc;
        GoScanUses(ibody, {}, inc);
        uses.refs.insert(inc.refs.begin(), inc.refs.end());
        uses.sels.insert(inc.sels.begin(), inc.sels.end());
    }
    auto refs = uses.refs, sels = uses.sels;
    // The block type of --go-unexported's NameWithStack entry points, and
    // the pool's two that make one, which the package exports for them: an
    // alias of the runtime's own.
    auto alias = opts.unexported && !imp && !opts.nowithstack && !entrynames.empty();
    string aliastext;
    if (alias) {
        aliastext = "// StackBlock is the block of data stacks an export fn's NameWithStack entry point\n"
                    "// runs on, which a caller may keep across calls.\ntype StackBlock = GsStackBlock\n\n";
        if (hostblocks)
            aliastext += "// AcquireStackBlock is a block of at least numStacks empty data stacks from the\n"
                         "// runtime's pool, which allocates and reserves nothing once the pool holds one: hand\n"
                         "// it back with ReleaseStackBlock, and until then it is the caller's alone.\n"
                         "func AcquireStackBlock(numStacks int) *StackBlock { return gsProg.AcquireStackBlock(numStacks) }\n\n"
                         "// ReleaseStackBlock rewinds gs, a block AcquireStackBlock gave, and gives it back to\n"
                         "// the pool: nothing may use what its stacks hold afterwards.\n"
                         "func ReleaseStackBlock(gs *StackBlock) { gsProg.ReleaseStackBlock(gs) }\n\n";
        GoUses au;
        GoScanUses(aliastext, {}, au);
        // Its own names are no uses of the runtime's.
        for (auto n : { "StackBlock", "AcquireStackBlock", "ReleaseStackBlock" }) au.refs.erase(n);
        refs.insert(au.refs.begin(), au.refs.end());
        sels.insert(au.sels.begin(), au.sels.end());
    }
    // What a host of a library uses besides its entry points: the error an
    // abort panics with, and where an entry point runs on a block the
    // caller holds, the blocks the runtime makes.
    if (opts.library && !opts.unexported) {
        refs.insert("GsError");
        if (hostblocks) {
            for (auto n : { "NewStackBlock", "AcquireStackBlock", "ReleaseStackBlock" }) refs.insert(n);
            for (auto n : { "Close", "Reset", "EnsureStacks" }) sels.insert(n);
        }
    }
    // The names the runtime gets in a file, by the names it declares them
    // by: an internal one's, or with --go-unexported every one's, unexported,
    // in its comments as well; or the package's, where the file imports it.
    unordered_map<string, string> rtcode, rtwords;
    for (auto &n : rt.toplevel) {
        if (imp) {
            if (isupper((unsigned char)n[0])) rtcode[n] = cat(GO_RUNTIME_NAME, ".", n);
        } else if (rt.internal.count(n) || (opts.unexported && isupper((unsigned char)n[0]))) {
            rtcode[n] = rtwords[n] = GoUnexport(n);
        }
    }
    // A runtime file already there: its settings must be this compile's,
    // and what it holds of the runtime it goes on holding, for the
    // package's other programs.
    auto settings = GoRuntimeSettings(opts, FrameReserve());
    if (shared && opts.runtimeexists) {
        auto &old = opts.runtimeold;
        auto at = old.find("// goose-runtime: ");
        if (at == string::npos)
            throw CompileError { cat("--go-runtime-file ", opts.runtimefile, ": the file is no runtime file the Goose "
                                     "compiler wrote; remove it, or name another") };
        auto line = old.substr(at + 18, old.find('\n', at) - at - 18);
        if (line != settings)
            throw CompileError { cat("--go-runtime-file ", opts.runtimefile, " holds the runtime of programs compiled "
                                     "with ", line, ", and this one is compiled with ", settings, ": the programs of a "
                                     "package share its runtime, and so its settings (remove the file to start over)") };
        auto oldsrc = GoParseDecls(old);
        for (auto &d : oldsrc.decls)
            if (d.kind == "package" && (d.names.empty() || d.names[0] != opts.package))
                throw CompileError { cat("--go-runtime-file ", opts.runtimefile, " is the runtime of package ",
                                         d.names.empty() ? string("?") : d.names[0], ", and this file's package is ",
                                         opts.package) };
        // Every name it uses, by the name the runtime declares it by.
        unordered_map<string, string> canon;
        for (auto &[from, to] : rtcode) canon[to] = from;
        GoUses ou;
        GoScanUses(old, {}, ou);
        for (auto &n : ou.refs) {
            auto it = canon.find(n);
            refs.insert(it != canon.end() ? it->second : n);
        }
        sels.insert(ou.sels.begin(), ou.sels.end());
        sels.insert(ou.refs.begin(), ou.refs.end());
        if (ou.refs.count("StackBlock") && !alias && !imp && opts.unexported) {
            alias = true;
            aliastext = "// StackBlock is the block of data stacks an export fn's NameWithStack entry point\n"
                        "// runs on, which a caller may keep across calls.\ntype StackBlock = GsStackBlock\n\n";
        }
        if (ou.refs.count("AcquireStackBlock") && opts.unexported && !imp && aliastext.find("func Acquire") == string::npos)
            aliastext += "// AcquireStackBlock is a block of at least numStacks empty data stacks from the\n"
                         "// runtime's pool, which allocates and reserves nothing once the pool holds one: hand\n"
                         "// it back with ReleaseStackBlock, and until then it is the caller's alone.\n"
                         "func AcquireStackBlock(numStacks int) *StackBlock { return gsProg.AcquireStackBlock(numStacks) }\n\n"
                         "// ReleaseStackBlock rewinds gs, a block AcquireStackBlock gave, and gives it back to\n"
                         "// the pool: nothing may use what its stacks hold afterwards.\n"
                         "func ReleaseStackBlock(gs *StackBlock) { gsProg.ReleaseStackBlock(gs) }\n\n";
    }
    // The program's place in the runtime, its own where its Go names it, and
    // the package's where the runtime's API does: one and the same in a
    // package of one program.
    auto progvar = uses.refs.count("gsProg") != 0;
    auto aliasprog = alias && aliastext.find("gsProg") != string::npos;
    auto pkgprog = false;
    vector<GoRuntimeModel::Ref> decls;
    for (;;) {
        auto r = refs;
        if (progvar || pkgprog) r.insert("GsProgram");
        decls = imp ? vector<GoRuntimeModel::Ref> {} : GoRuntimeReach(r, sels, full);
        auto rtprog = aliasprog;
        for (auto d : decls) rtprog = rtprog || rt.D(d).refs.count("gsProg");
        auto &have = shared ? pkgprog : progvar;
        if (rtprog && !have) {
            have = true;
            continue;
        }
        break;
    }
    // In a package of several programs, the program's own names carry its
    // file's name (x_tag_g), which keeps them apart from the others'; with
    // --go-unexported, those that Go would export are unexported, but for
    // the types its entry points name, which a host names too. The entry
    // points keep their names.
    auto prog = program(progvar);
    unordered_map<string, string> prename;
    {
        auto ps = GoParseDecls(prog);
        set<string> declared;
        for (auto &d : ps.decls)
            for (auto &n : d.names) declared.insert(n);
        for (auto &n : declared) {
            if (entrynames.count(n) || n == "main" || n == "init" || n == "_") continue;
            auto m = n;
            if (shared && !opts.tag.empty()) {
                auto g = m.rfind("_g");
                auto tail = g == string::npos ? string() : m.substr(g + 2);
                auto ours = g != string::npos && all_of(tail.begin(), tail.end(), [](char c) {
                    return isdigit((unsigned char)c) || c == '_';
                });
                m = ours ? cat(m.substr(0, g), "_", opts.tag, "_g", tail) : cat(m, "_", opts.tag);
            }
            if (opts.unexported && isupper((unsigned char)m[0]) && !sigtypes.count(n)) {
                auto u = cat("_", m);
                while (declared.count(u)) u = cat("_", u);
                m = u;
            }
            if (m != n) prename[n] = m;
        }
    }
    // Go's checks of the program's names against the runtime's: no rename
    // may land on one.
    for (auto &[from, to] : prename)
        if (rt.toplevel.count(to) || GoRuntimeName(to))
            Fail(Line {}, cat("internal: the program's ", from, " renamed to the runtime's ", to));
    // The renames, then gofmt's layout of the program's Go. No mark of an
    // address a load or a store takes for itself alone is left in it
    // (TMARK): FinishBody drops each.
    if (prog.find(TMARK[0]) != string::npos) Fail(Line {}, "internal: a marked address outside a function's body");
    prog = GoFormat(GoRenameIdents(prog, rtcode, prename, imp ? unordered_map<string, string> {} : rtwords));
    if (imp) {
        // A name the program's Go uses that the runtime keeps to itself is
        // one the package cannot reach.
        for (auto &n : proguses.refs)
            if (rt.toplevel.count(n) && !isupper((unsigned char)n[0]))
                Fail(Line {}, cat("internal: the Go backend names the runtime's ", n, ", which it does not export"));
    }
    // The --include files' Go, the runtime's names and the program's in it
    // renamed as the program's are.
    vector<string> incs;
    for (auto &[iname, ibody] : incbodies) {
        auto n = ascii(iname);
        incs.push_back(cat("// ---- --include ", n, " ----\n",
                           GoIncludeLines(n, GoRenameIdents(ibody, rtcode, prename, {}))));
    }
    // The runtime, and the package's configuration: in this file, or in the
    // package's runtime file.
    string config = cat("// The runtime configuration the package's programs were compiled with\n"
                        "// (--stack-reserve, -D GS_STACK_RESERVE, -D GS_STACK_GAP, --frame-reserve).\n"
                        "const (\n\tgsStackReserve = ", opts.stacks.reserve, "\n\tgsStackGap     = ", opts.stacks.gap,
                        "\n\tgsFrameReserve = ", FrameReserve(), "\n)\n\n");
    string runtime;
    if (!decls.empty())
        runtime = cat("// ---- runtime ----\n"
                      "// The Goose runtime for Go, the package ", GO_RUNTIME_PATH, "\n"
                      "// (runtime/go): ",
                      full ? "all of it (--go-runtime=full)"
                      : shared ? "the part of it the package's programs use"
                               : "the part of it the program uses",
                      ", which builds with nothing\n"
                      "// but the Go standard library. Its names start with gs or Gs, which no name\n"
                      "// from the program does (those end in _g), and are the runtime's own: a file\n"
                      "// that holds it may not declare them.\n\n",
                      GoRuntimeText(decls, rtcode, rtwords));
    // The alias's own names are the package's, which no rename touches.
    auto aliascode = rtcode, aliaswords = rtwords;
    for (auto n : { "StackBlock", "AcquireStackBlock", "ReleaseStackBlock" }) {
        aliascode.erase(n);
        aliaswords.erase(n);
    }
    string pkgsection = cat(config, pkgprog ? GoRenameIdents(pkgprogdecl, rtcode, {}, rtwords) : "",
                            alias ? GoRenameIdents(aliastext, aliascode, {}, aliaswords) : "", runtime);
    // The imports a text needs: the packages its Go names, and the runtime's
    // where it is imported.
    auto importsof = [&](const vector<string> &texts, const vector<string> &rtonly,
                         const vector<pair<string, string>> &extra) {
        vector<pair<string, string>> imports;
        auto add = [&](const string &n, const string &path) {
            // A name that is the package's own is no name.
            auto nm = n == GoPathName(path) ? string() : n;
            for (auto &[in, ip] : imports)
                if (ip == path && in == nm) return;
            imports.push_back({ nm, path });
        };
        for (auto &text : texts) {
            GoUses u;
            GoScanUses(text, known, u);
            for (auto &p : u.pkgs) add(p, known.at(p));
        }
        // The runtime's package, where the file imports it, and the Go of
        // an --include file names it too, which imports what else it uses.
        if (imp) {
            vector<string> all = texts;
            all.insert(all.end(), rtonly.begin(), rtonly.end());
            for (auto &text : all) {
                GoUses u;
                GoScanUses(text, { { GO_RUNTIME_NAME, GO_RUNTIME_PATH } }, u);
                if (u.pkgs.count(GO_RUNTIME_NAME)) add(GO_RUNTIME_NAME, GO_RUNTIME_PATH);
            }
        }
        for (auto &[n, p] : extra) add(n, p);
        sort(imports.begin(), imports.end(), [](auto &a, auto &b) {
            return a.second != b.second ? a.second < b.second : a.first < b.first;
        });
        string imps;
        for (auto &[n, p] : imports) Append(imps, "\t", n, n.empty() ? "" : " ", GoStr(p), "\n");
        return imps.empty() ? string() : cat("import (\n", imps, ")\n\n");
    };
    // What wrote the file (issue #21): after the line Go's tools know
    // generated code by, which stays as it was, the compiler's version and
    // the runtime's revision, and the options the Go depends on, which
    // write it again; the runtime file has the first.
    auto stamp = cat("// goose-version: ", ascii(opts.version.empty() ? string("unknown") : opts.version),
                     " (runtime/go ", GoRuntimeRevision(), ")\n");
    auto header = cat("// Code generated by the Goose compiler from ", name, ". DO NOT EDIT.\n"
                      "//\n", stamp,
                      "// goose-flags: ", ascii(GoHeaderFlags(opts, FrameReserve(), DefaultFrameReserve())), "\n"
                      "//\n"
                      "// Names from the program carry a _g suffix, which keeps them clear of Go's\n"
                      "// keywords and of the runtime's gs and Gs names; a namespaced name ns::x is\n"
                      "// ns_x_g followed by the namespace's length.",
                      shared ? cat(" In a package whose programs share a\n// runtime file (", ascii(opts.runtimefile),
                                   "), they carry their file's name before the suffix\n// too: x_", opts.tag, "_g.")
                             : string(),
                      imp ? cat("\n//\n// The runtime is the package ", GO_RUNTIME_PATH, ",\n// imported as ",
                                GO_RUNTIME_NAME, " (--go-runtime=import).")
                          : string(),
                      "\n\n");
    auto build = opts.build.empty() ? string() : cat("//go:build ", opts.build, "\n");
    // The parts of a file, a blank line between each two, as gofmt keeps
    // them; the --include files last, so that the //line directives naming
    // each hold to its end.
    auto join = [](const vector<string> &parts) {
        string out;
        for (auto p : parts) {
            while (!p.empty() && p.back() == '\n') p.pop_back();
            if (p.empty()) continue;
            Append(out, out.empty() ? "" : "\n\n", p);
        }
        return out + "\n";
    };
    vector<string> parts = { build, header, cat("package ", opts.package) };
    if (!shared) {
        parts.push_back(importsof({ pkgsection, prog }, incs, incimports));
        parts.push_back(pkgsection);
    } else {
        parts.push_back(importsof({ prog }, incs, incimports));
        runtimefiletext = join({ cat("// Code generated by the Goose compiler. DO NOT EDIT.\n//\n", stamp, "//\n"
                                     "// The Goose runtime of package ", opts.package, "'s programs (--go-runtime-file),\n"
                                     "// which they share: what they use of it, and its configuration. A compile of\n"
                                     "// one of them writes it again, with what the others use of it still in it,\n"
                                     "// and requires the settings this line records to be its own.\n"
                                     "// goose-runtime: ", settings),
                                 cat("package ", opts.package), importsof({ pkgsection }, {}, {}), pkgsection });
    }
    parts.push_back(prog);
    parts.insert(parts.end(), incs.begin(), incs.end());
    return join(parts);
}

}  // namespace goose
