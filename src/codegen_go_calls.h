// Goose compiler — the Go backend's calls (definitions of CodeGenGo members,
// codegen_go.h): calls to specializations, the free variables a nested
// function reaches (§7.5), function values spliced inline (§7.6), and the
// builtins of the scalar core: print (§3.7), assert, abort and exit (§9.3),
// default and copy.
#pragma once

namespace goose {

// The arguments the callee's parameters take (Call::ArgNodes): the
// function-value arguments that follow them are compile-time only.
inline vector<Node *> CodeGenGo::CallArgNodes(Call *c, size_t nparams) {
    auto an = c->ArgNodes();
    an.resize(nparams);
    return an;
}

// A free variable of the callee, from the caller: the address of its own
// variable, or the pointer it was handed itself. A resizable one is its
// header's address and its stack, a reusable pool its GsPref, a bytes value
// its base (CodeGen::EmitFvArg).
inline string CodeGenGo::FreeVarArg(const VarDef *fv) {
    auto it = vnames.find(fv);
    if (it == vnames.end()) Fail(fv->line, cat("internal: free variable ", fv->name, " not in scope"));
    auto name = it->second;
    auto ours = fv->ownerspec == curspec;
    if (fv->reusable) {
        if (!ours || !vpool.count(fv)) return name;
        auto &p = vpool[fv];
        auto t = T();
        Decl(t, "GsPref", cat("GsPref{&", name, ", ", vstk[fv], ", &", p.first, ", ", p.second, "}"));
        return t;
    }
    if (fv->type && IsResz(fv->type))
        return cat(fvptr.count(fv) ? name : cat("&", name), ", ", vstk[fv]);
    if (fv->type && IsBytesT(fv->type)) return name;
    return fvptr.count(fv) ? name : cat("&", name);
}

// Whether a specialization's calls take the fixed-size form, every argument
// and result a Go value: no parameter or result of a non-fixed type, and no
// pool parameter.
inline bool CodeGenGo::PlainSig(FnSpec *sp) {
    for (size_t i = 0; i < sp->argtypes.size(); i++)
        if (IsBytesT(sp->argtypes[i]) || IsPoolParam(sp, i)) return false;
    for (auto rt : sp->rets)
        if (IsBytesT(rt)) return false;
    return true;
}

// The block and the stack index a call hands its callee, where it takes
// them.
inline string CodeGenGo::StackArgs(FnSpec *sp, Line ln) {
    auto &ki = sinfo[sp];
    if (ki.needssp) {
        NoStackAcrossCycleCall(sp, ln);
        NoteStackCall(sp, ln);
        return cat("gs, ", SpTop());
    }
    return ki.needsgs ? "gs" : "";
}

// The Go call of a specialization, its arguments evaluated: the data stack
// block and the first index free here where the callee needs them, the
// declared parameters, then the free variables. An argument read before a
// later one that runs statements is taken into a temporary first (§2).
inline string CodeGenGo::SpecCallX(Call *c, FnSpec *sp) {
    if (!sinfo.count(sp)) Fail(c->line, cat("internal: no Go function for ", sp->sf->name));
    auto &ki = sinfo[sp];
    auto an = CallArgNodes(c, sp->params.size());
    vector<bool> later(an.size() + 1, false);
    for (size_t i = an.size(); i-- > 0;) later[i] = later[i + 1] || HasStmts(an[i]);
    vector<string> args;
    for (size_t i = 0; i < an.size(); i++) {
        auto pt = sp->argtypes[i];
        auto x = Coerce(GenX(an[i]), an[i]->exprtype, pt, an[i]->line);
        if (later[i + 1] && !IsGoConst(x)) x = Snapshot(pt, x);
        args.push_back(x);
    }
    for (auto fv : ki.freevars) args.push_back(FreeVarArg(c->FreeVarArg(fv)));
    auto list = StackArgs(sp, c->line);
    for (auto &a : args) Append(list, list.empty() ? "" : ", ", a);
    return cat(ki.name, "(", list, ")");
}

// A call of a specialization by the full calling convention (C.3,
// CodeGen::EmitSpecCall), emitted here: a by-value resizable or bytes
// argument is built on a stack of its own, a pool parameter takes a GsPref,
// and each non-fixed result is built at its destination's top -- d0 for the
// first, alldst's for every one where given, a temporary's stack otherwise
// -- the callee handed that stack (gs_dst<i>). Returns one entry per
// result: a fixed one's Go value, a bytes one's base, a resizable one's
// temporary header, or "" where its count went to its destination's.
inline vector<string> CodeGenGo::EmitSpecCall(Call *c, FnSpec *sp, const GoDst &d0,
                                              vector<GoDst> *alldst) {
    if (!sinfo.count(sp)) Fail(c->line, cat("internal: no Go function for ", sp->sf->name));
    auto &ki = sinfo[sp];
    auto an = CallArgNodes(c, sp->params.size());
    vector<bool> later(an.size() + 1, false);
    for (size_t i = an.size(); i-- > 0;) later[i] = later[i + 1] || HasStmts(an[i]);
    vector<string> args;
    for (size_t i = 0; i < an.size(); i++) {
        auto pt = sp->argtypes[i];
        auto node = an[i];
        if (IsPoolParam(sp, i)) {
            args.push_back(GenPrefVal(node));
            continue;
        }
        if (IsResz(pt)) {
            // By value: constructed into a fresh slot, the header (or frame
            // object) passed with its stack (§7.2, C.3).
            string stk;
            auto h = RzTemp(pt, stk);
            GenConstruct(node, stk, pt, RzLenLv(pt, h));
            args.push_back(h);
            args.push_back(stk);
            continue;
        }
        if (IsBytesT(pt)) {
            string stk;
            auto base = BytesTemp(stk);
            GenConstruct(node, stk, pt);
            args.push_back(base);
            continue;
        }
        auto x = Coerce(GenX(node), node->exprtype, pt, node->line);
        if (later[i + 1] && !IsGoConst(x) && x != "nil") x = Snapshot(pt, x);
        args.push_back(x);
    }
    for (auto fv : ki.freevars) args.push_back(FreeVarArg(c->FreeVarArg(fv)));
    vector<string> retex(sp->rets.size());
    // Where each result the Go call returns goes: a resizable result's count
    // to its receiver (counts[i]), a fixed one to a temporary.
    vector<string> counts(sp->rets.size());
    for (size_t i = 0; i < sp->rets.size(); i++) {
        auto rt = sp->rets[i];
        GoDst dd = alldst && i < alldst->size() ? (*alldst)[i] : (i == 0 ? d0 : GoDst {});
        if (IsResz(rt)) {
            string stk;
            if (dd.k == GD_STACK && !dd.lenlv.empty()) {
                stk = dd.s;
                counts[i] = dd.lenlv;
            } else {
                auto h = RzTemp(rt, stk);
                counts[i] = RzLenLv(rt, h);
                retex[i] = h;
            }
            args.push_back(stk);
        } else if (IsBytesT(rt)) {
            string stk;
            if (dd.k == GD_STACK) {
                stk = dd.s;
                retex[i] = T();
                Decl(retex[i], "unsafe.Pointer", Top(stk));
            } else {
                retex[i] = BytesTemp(stk);
            }
            args.push_back(stk);
        }
    }
    auto list = StackArgs(sp, c->line);
    for (auto &a : args) Append(list, list.empty() ? "" : ", ", a);
    auto call = cat(ki.name, "(", list, ")");
    string names, marks;
    vector<pair<string, string>> assigns;
    for (size_t i = 0; i < sp->rets.size(); i++) {
        auto rt = sp->rets[i];
        if (IsBytesT(rt) && !IsResz(rt)) continue;
        auto t = T();
        Append(names, names.empty() ? "" : ", ", t);
        Append(marks, marks.empty() ? "" : ",", t);
        if (IsResz(rt)) assigns.push_back({ counts[i], t });
        else retex[i] = t;
    }
    if (names.empty()) L(call);
    else L(names, " := ", call, DECLMARK, marks);
    for (auto &[lv, t] : assigns) L(lv, " = ", t);
    return retex;
}

// A call's results as EmitSpecCall gives them, whatever is called: a
// builtin, a spliced function value, a case-function dispatch or a
// specialization (CodeGen::EmitCall).
inline vector<string> CodeGenGo::GenCallVals(Call *c, const GoDst &d0, vector<GoDst> *alldst) {
    if (c->builtin >= 0) return GenRzBuiltin(c, d0);
    if (c->fvbody) {
        auto et = c->fvbody->exprtype;
        if (d0.k == GD_STACK || IsVoidT(et)) {
            GenFvCall(c, d0);
            return { "" };
        }
        if (IsResz(et)) return { GenRzTmp(c).hdr };
        auto t = T();
        Decl(t, LocalT(c->exprtype, c->line));
        GenFvCall(c, GoDst { GD_LVALUE, t, c->exprtype });
        return { t };
    }
    if (!c->dispatch.empty()) {
        for (auto rt : c->dispatch[0]->rets)
            if (IsBytesT(rt)) Unsupported(c->line, "a case-function dispatch with a non-fixed result");
        return GenDispatch(c);
    }
    if (!c->spec) Fail(c->line, "internal: unresolved call");
    if (c->spec->sf->isextern) Unsupported(c->line, "a call to an extern fn");
    if (PlainSig(c->spec)) {
        auto sp = c->spec;
        auto call = SpecCallX(c, sp);
        vector<string> res;
        if (sp->rets.empty()) {
            L(call);
            return res;
        }
        string names, marks;
        for (size_t i = 0; i < sp->rets.size(); i++) {
            auto t = T();
            res.push_back(t);
            Append(names, i ? ", " : "", t);
            Append(marks, i ? "," : "", t);
        }
        L(names, " := ", call, DECLMARK, marks);
        return res;
    }
    return EmitSpecCall(c, c->spec, d0, alldst);
}

// The first result as its receiver takes it (CodeGen::CallVal0): a
// reference received in a value context reads its pointee, and a fixed
// value converts to the type the receiver wants.
inline string CodeGenGo::CallVal0(Call *c, const string &r0, TypeExpr *want) {
    auto rt = c->rettypes.empty() ? nullptr : c->rettypes[0];
    if (c->builtin >= 0 || c->fvbody || !c->dispatch.empty()) rt = c->exprtype;
    auto et = want ? want : c->exprtype;
    if (!rt || !et) return r0;
    if (IsBytesT(rt) || (rt->kind == TY_REF && rt->ref->lenstorage < 0 && IsBytesT(rt->ref->sub)))
        return r0;
    return CoerceVal(r0, rt, et, c->line);
}

// The first result as a location, in the representation its type arrives
// in: a header for a resizable, a base for a bytes value, a Go value
// otherwise; a reference is its pointee (CodeGen::CallResLoc).
inline CodeGenGo::GoLoc CodeGenGo::CallResLoc(Call *c, const string &r0) {
    auto rt = c->rettypes.empty() ? c->exprtype : c->rettypes[0];
    if (c->builtin >= 0 || c->fvbody || !c->dispatch.empty()) rt = c->exprtype;
    if (IsPlainRef(rt)) {
        auto sub = rt->ref->sub;
        if (IsResz(sub)) return FatRefLoc(r0, sub);
        if (IsBytesT(sub)) {
            GoLoc l;
            l.t = sub;
            l.addr = r0;
            return l;
        }
        return AtTyped(r0, sub);
    }
    if (IsResz(rt)) return RzTempLoc(rt, r0, "");
    if (IsBytesT(rt)) {
        GoLoc l;
        l.t = rt;
        l.addr = r0;
        return l;
    }
    return AtVar(r0, rt);
}

// The targets that take a call's first result of n into t, and drop the rest.
static string FirstResult(const string &t, size_t n) {
    auto s = t;
    for (size_t i = 1; i < n; i++) s += ", _";
    return s;
}

// A case-function call (§8.2): every argument evaluated once, in order,
// then the scrutinee's tag picking the case, which takes its variant's
// payload as its own type; every case's results land in the same
// temporaries, which are returned. A fixed-mode ADT, also behind a
// reference, dispatches only to cases taking the payload by value (§3.5).
inline vector<string> CodeGenGo::GenDispatch(Call *c) {
    auto ln = c->line;
    auto sp0 = c->dispatch[0];
    auto an = CallArgNodes(c, sp0->params.size());
    auto pos = c->dispatcharg;
    auto sn = an[pos];
    auto enumtype = sn->exprtype;
    auto isref = enumtype->kind == TY_REF;
    if (isref) enumtype = enumtype->ref->sub;
    if (enumtype->kind != TY_ENUM || enumtype->enu->varmode)
        Unsupported(ln, cat("a case-function dispatch over ", TypeText(enumtype)));
    auto ei = EIOf(enumtype);
    auto &ti = TI(enumtype, ln);
    GoLoc e;
    vector<string> args(an.size());
    for (size_t i = 0; i < an.size(); i++) {
        if ((int)i == pos) {
            if (isref) {
                e = AtTyped(GenPure(sn), enumtype);
            } else {
                // A copy, taken before the later arguments run.
                e = AtVar(Snapshot(enumtype, CoerceVal(GenX(sn), sn->exprtype, enumtype, ln)), enumtype);
            }
            continue;
        }
        auto pt = sp0->argtypes[i];
        auto x = Coerce(GenX(an[i]), an[i]->exprtype, pt, an[i]->line);
        if (!IsGoConst(x) && x != "nil") x = Snapshot(pt, x);
        args[i] = x;
    }
    vector<string> res;
    for (auto rt : sp0->rets) {
        auto t = T();
        Decl(t, LocalT(rt, ln));
        res.push_back(t);
    }
    auto chain = [&] {
        auto tag = T();
        Decl(tag, ti.rep == GR_TAG ? ti.name : IntGoT(TagStore(ei->en)), TagOf(e, ln));
        auto nv = ei->en->variants.size();
        for (size_t vi = 0; vi < nv; vi++) {
            auto sp = c->dispatch[vi];
            auto &ki = sinfo[sp];
            auto last = vi + 1 == nv;
            auto open = !vi ? cat("if ", tag, " == 0 {")
                            : last && !debug ? string("} else {") : cat("} else if ", tag, " == ", vi, " {");
            if (!vi) L(open);
            else LS(open);
            ind++;
            PushSc(SC_PLAIN);
            auto vt = VariantType(enumtype, (int)vi);
            auto pt = sp->argtypes[pos];
            if (pt->kind == TY_REF) Fail(ln, "internal: a fixed-mode payload dispatched by reference");
            auto varg = ti.voffs.empty() || ti.voffs[vi] < 0 ? ZeroOf(vt, ln) : PayloadLoc(e, (int)vi, ln).lv;
            string list;
            if (ki.needssp) {
                NoStackAcrossCycleCall(sp, ln);
                NoteStackCall(sp, ln);
                list = cat("gs, ", SpTop());
            }
            for (size_t i = 0; i < an.size(); i++)
                Append(list, list.empty() ? "" : ", ", (int)i == pos ? Coerce(varg, vt, pt, ln) : args[i]);
            for (auto fv : ki.freevars) Append(list, list.empty() ? "" : ", ", FreeVarArg(c->FreeVarArg(fv)));
            auto call = cat(ki.name, "(", list, ")");
            if (res.empty()) {
                L(call);
            } else {
                string names;
                for (size_t i = 0; i < res.size(); i++) Append(names, i ? ", " : "", res[i]);
                L(names, " = ", call);
            }
            PopSc();
            ind--;
        }
        if (debug) {
            // A debug build's check of a tag no variant has (GS_UNREACHABLE).
            LS("} else {");
            ind++;
            LS("panic(GsTagError(", LocArgs(ln), "))");
            ind--;
        }
        LS("}");
        dead = false;
    };
    if (Unreached()) GenUnreached(chain);
    else chain();
    return res;
}

// A call as a value: made at this point, its first result in a temporary.
inline string CodeGenGo::GenCall(Call *c) {
    if (c->builtin >= 0 || c->fvbody) return CtlValX(c);
    if (!c->dispatch.empty()) {
        auto res = GenDispatch(c);
        if (res.empty()) Fail(c->line, "internal: a call without results as a value");
        return Coerce(res[0], c->dispatch[0]->rets[0], c->exprtype, c->line);
    }
    if (!c->spec) Fail(c->line, "internal: unresolved call");
    auto sp = c->spec;
    if (sp->sf->isextern) Unsupported(c->line, "a call to an extern fn");
    if (sp->rets.empty()) Fail(c->line, "internal: a call without results as a value");
    if (!PlainSig(sp)) {
        auto rets = EmitSpecCall(c, sp, GoDst {}, nullptr);
        if (IsBytesT(sp->rets[0])) Fail(c->line, "internal: a non-fixed result as a Go value");
        return CallVal0(c, rets[0], c->exprtype);
    }
    auto call = SpecCallX(c, sp);
    auto t = T();
    if (sp->rets.size() > 1) {
        // A call in an ordinary value context yields its first result.
        GoT(sp->rets[0], c->line);
        L(FirstResult(t, sp->rets.size()), " := ", call, DECLMARK, t);
    } else {
        Decl(t, LocalT(sp->rets[0], c->line), call);
    }
    return Coerce(t, sp->rets[0], c->exprtype, c->line);
}

inline void CodeGenGo::GenCallAny(Call *c, const GoDst &d) {
    // A result bound for a stack slot (a branch's value, say) is constructed
    // there as anywhere else (Call::CgAny).
    if (d.k == GD_STACK) {
        GenConstruct(c, d.s, d.t, d.lenlv);
        return;
    }
    if (c->builtin >= 0) { GenBuiltin(c, d); return; }
    if (c->fvbody) { GenFvCall(c, d); return; }
    if (!c->dispatch.empty()) {
        auto res = GenDispatch(c);
        if (d.k == GD_DISCARD || res.empty()) return;
        auto want = d.t ? d.t : c->exprtype;
        auto rt = c->dispatch[0]->rets[0];
        if (IsRelRef(want)) {
            auto pt = ast.RefTo(want->ref->sub, c->line, want->ref->optional);
            auto v = Snapshot(pt, Coerce(res[0], rt, pt, c->line));
            EmitRelStore(AtVar(d.s, want), v, NonNullRef(c), false, c->line);
            return;
        }
        L(d.s, " = ", Coerce(res[0], rt, want, c->line));
        return;
    }
    if (!c->spec) Fail(c->line, "internal: unresolved call");
    auto sp = c->spec;
    if (sp->sf->isextern) Unsupported(c->line, "a call to an extern fn");
    if (!PlainSig(sp)) {
        auto rets = EmitSpecCall(c, sp, d, nullptr);
        if (d.k == GD_DISCARD || rets.empty() || IsBytesT(sp->rets[0]) || Unreached()) return;
        auto want = d.t ? d.t : c->exprtype;
        if (d.pool) {
            L(d.s, " = ", rets[0]);
            return;
        }
        if (IsRelRef(want)) {
            auto pt = ast.RefTo(want->ref->sub, c->line, want->ref->optional);
            auto v = Snapshot(pt, CallVal0(c, rets[0], pt));
            EmitRelStore(AtVar(d.s, want), v, NonNullRef(c), false, c->line);
            return;
        }
        auto x = CallVal0(c, rets[0], want);
        if (x != d.s) L(d.s, " = ", x);
        return;
    }
    auto call = SpecCallX(c, sp);
    // A call's results may go unused, as an expression statement.
    if (d.k == GD_DISCARD || sp->rets.empty()) {
        L(call);
        return;
    }
    auto want = d.t ? d.t : c->exprtype;
    string r = call;
    if (sp->rets.size() > 1) {
        r = T();
        L(FirstResult(r, sp->rets.size()), " := ", call, DECLMARK, r);
    }
    if (IsRelRef(want)) {
        // A reference into a relative slot: the plain one, encoded there.
        auto pt = ast.RefTo(want->ref->sub, c->line, want->ref->optional);
        auto v = Snapshot(pt, Coerce(r, sp->rets[0], pt, c->line));
        EmitRelStore(AtVar(d.s, want), v, NonNullRef(c), false, c->line);
        return;
    }
    L(d.s, " = ", Coerce(r, sp->rets[0], want, c->line));
}

// A multi-name binding from one call (`let a, b = f();`): each result into
// its variable, through temporaries of the callee's types where those are
// not the variables'.
inline void CodeGenGo::GenCallInto(Call *c, const vector<VarDef *> &defs) {
    if (!c->dispatch.empty()) {
        auto rets = c->dispatch[0]->rets;
        if (rets.size() != defs.size()) Fail(c->line, "internal: result count of a binding");
        auto res = GenDispatch(c);
        for (size_t i = 0; i < defs.size(); i++)
            Decl(LocalName(defs[i]), LocalT(defs[i]->type, defs[i]->line),
                 Coerce(res[i], rets[i], defs[i]->type, c->line));
        return;
    }
    if (c->fvbody || !c->spec || (c->builtin >= 0 && c->builtin != B_FROM_BYTES))
        Unsupported(c->line, "a multiple binding from this call");
    auto sp = c->spec;
    if (c->builtin >= 0 || !PlainSig(sp)) {
        // Each result where a declaration of its variable alone would build
        // it: a non-fixed one on a stack of its own (DeclStackLocal), a
        // fixed one in its variable (VarDecl::CgStmt).
        vector<GoDst> dsts;
        for (auto d : defs) {
            if (IsBytesT(d->type)) {
                dsts.push_back(DeclStackLocal(d, true));
            } else {
                auto name = LocalName(d);
                Decl(name, LocalT(d->type, d->line));
                dsts.push_back(GoDst { GD_LVALUE, name, d->type });
            }
        }
        auto rets = GenCallVals(c, dsts.empty() ? GoDst {} : dsts[0], &dsts);
        if (Unreached()) return;
        auto rtypes = c->builtin >= 0 ? c->rettypes : sp->rets;
        for (size_t i = 0; i < dsts.size() && i < rets.size(); i++) {
            if (dsts[i].k != GD_LVALUE || rets[i].empty()) continue;
            auto x = CoerceVal(rets[i], rtypes[i], dsts[i].t, c->line);
            if (x != dsts[i].s) L(dsts[i].s, " = ", x);
        }
        return;
    }
    if (sp->sf->isextern) Unsupported(c->line, "a call to an extern fn");
    if (sp->rets.size() != defs.size()) Fail(c->line, "internal: result count of a binding");
    auto same = true;
    for (size_t i = 0; i < defs.size(); i++) {
        GoT(defs[i]->type, defs[i]->line);
        same = same && SameGoT(sp->rets[i], defs[i]->type);
    }
    auto call = SpecCallX(c, sp);
    string names, marks;
    vector<string> temps;
    for (size_t i = 0; i < defs.size(); i++) {
        auto n = same ? LocalName(defs[i]) : T();
        temps.push_back(n);
        Append(names, i ? ", " : "", n);
        Append(marks, i ? "," : "", n);
    }
    L(names, " := ", call, DECLMARK, marks);
    if (same) return;
    for (size_t i = 0; i < defs.size(); i++)
        Decl(LocalName(defs[i]), LocalT(defs[i]->type, defs[i]->line),
             Coerce(temps[i], sp->rets[i], defs[i]->type, c->line));
}

// Calling a function value: the checked body instance is spliced inline
// (§7.6), its parameters ordinary locals of this function.
inline void CodeGenGo::GenFvCall(Call *c, const GoDst &d) {
    auto et = c->fvbody->exprtype;
    auto wantsval = !IsVoidT(et) && d.k != GD_DISCARD;
    if (Unreached()) return GenUnreached([&] { GenFvCall(c, d); });
    PushSc(SC_PLAIN);
    L("{");
    ind++;
    for (size_t i = 0; i < c->fvparams.size(); i++)
        BindLocal(c->fvparams[i], i < c->args.size() ? c->args[i] : nullptr);
    for (auto st : c->fvbody->stmts) GenStmt(st);
    if (c->fvbody->tail) {
        if (wantsval && d.k == GD_STACK) GenConstruct(c->fvbody->tail, d.s, d.t ? d.t : c->exprtype, d.lenlv);
        else if (wantsval) GenAny(c->fvbody->tail, GoDst { GD_LVALUE, d.s, d.t ? d.t : c->exprtype });
        else GenAny(c->fvbody->tail, GoDst {});
    }
    PopSc();
    ind--;
    LS("}");
}

// ------------------------------------------------------------------
// Builtins.

// The format overloads the call's argument is rendered with, which this
// version renders none of (CodeGen::FmtContext, FmtSpecFor).
static Call *GoFmtContext(Call *c, Node *arg) {
    if (!c || c->fmtcontexts.empty()) return c;
    auto args = c->ArgNodes();
    for (size_t i = 0; i < args.size(); i++)
        if (args[i] == arg && i < c->fmtcontexts.size()) return c->fmtcontexts[i];
    return c;
}

inline void CodeGenGo::GenBuiltin(Call *c, const GoDst &d) {
    auto an = c->ArgNodes();
    auto ln = c->line;
    switch ((BuiltinKind)c->builtin) {
        case B_PRINT: EmitPrint(c); return;
        case B_ASSERT: {
            auto x = GenTruth(an[0]);
            if (Unreached()) return GenUnreached([&] { GenBuiltin(c, d); });
            L("if !", x, " {");
            ind++;
            L("GsAbort(GsEAssert, ", LocArgs(ln), ")");
            ind--;
            LS("}");
            return;
        }
        case B_ABORT: {
            auto s = Is<StrLit>(an[0]);
            if (!s) Unsupported(ln, "abort with a message that is not a string literal");
            L("GsAbortMsg(", GoStr(s->val), ", ", LocArgs(ln), ")");
            return;
        }
        case B_EXIT: {
            auto x = GenX(an[0]);
            L("GsExit(", Coerce(x, an[0]->exprtype, ast.inttypes[IS_I64], ln), ")");
            return;
        }
        case B_DEFAULT: {
            // All-zero bytes are every fixed type's default (§4.2), which is
            // Go's zero value of its representation, but where a field
            // declares a default of its own: the checker built that as a
            // literal (Call::defaultinit).
            auto t = c->rettypes.empty() ? c->exprtype : c->rettypes[0];
            GoT(t, ln);
            auto want = d.t ? d.t : c->exprtype;
            if (!c->defaultinit) {
                if (d.k == GD_DISCARD) return;
                if (IsRelRef(want)) {
                    L(d.s, " = 0");
                    return;
                }
                L(d.s, " = ", Coerce(ZeroOf(t, ln), t, want, ln));
                return;
            }
            auto v = T();
            Decl(v, LocalT(t, ln));
            if (t->kind == TY_ARRAY) {
                auto av = ArrayView(AtVar(v, t), ln);
                auto i = T();
                L("for ", i, " := int64(0); ", i, " < ", ArrSize(t->arr), "; ", i, "++ {");
                ind++;
                PushSc(SC_PLAIN);
                GenAny(c->defaultinit, GoDst { GD_LVALUE, ElemAt(av, i, ln).lv, t->arr->sub });
                PopSc();
                ind--;
                LS("}");
            } else {
                GenAny(c->defaultinit, GoDst { GD_LVALUE, v, t });
            }
            if (d.k != GD_DISCARD) L(d.s, " = ", Coerce(v, t, want, ln));
            return;
        }
        case B_COPY: {
            // A copy is the value the place holds (§4.1).
            auto want = d.t ? d.t : c->exprtype;
            if (IsScalarT(an[0]->exprtype)) {
                GenAny(an[0], GoDst { d.k, d.s, want });
                return;
            }
            auto rt = c->rettypes.empty() ? c->exprtype : c->rettypes[0];
            auto x = LoadLoc(GenLoc(an[0]), rt, ln);
            if (want && want->kind == TY_SLICE && rt->kind != TY_SLICE) {
                // A slice destination views the copy, not its source.
                auto v = Snapshot(rt, x);
                x = LoadLoc(AtVar(v, rt), want, ln);
            } else {
                x = Coerce(x, rt, want, ln);
            }
            if (d.k == GD_DISCARD) {
                if (!IsGoName(x)) L("_ = ", x);
                return;
            }
            if (x != d.s) L(d.s, " = ", x);
            return;
        }
        case B_STR: {
            // A fixed-size slot (a slice, say): built on a temporary of its
            // own type, and read as the slot's.
            auto res = EmitStr(c, an, GoDst {}, ln);
            if (d.k == GD_DISCARD || res.empty() || Unreached()) return;
            auto want = d.t ? d.t : c->exprtype;
            auto x = LoadLoc(RzTempLoc(GrowU8(), res[0], ""), want, ln);
            if (x != d.s) L(d.s, " = ", x);
            return;
        }
        case B_FORMAT: {
            auto rt = an[0]->exprtype;
            while (rt->kind == TY_REF) rt = rt->ref->sub;
            auto l = IsResz(rt) ? RzRecvLoc(an[0]) : RecvLoc(an[0]);
            for (size_t i = 1; i < an.size(); i++) {
                if (Unreached()) break;
                EmitFormatInto(l, an[i], ln, c);
            }
            return;
        }
        case B_LEN: case B_CAP: case B_INDEX_OF: case B_PUSH: case B_APPEND: case B_POP:
        case B_POP_N: case B_CLEAR: case B_RESIZE: case B_ALLOC_INDEX: case B_ALLOC_REF:
        case B_ALLOC_SLICE: case B_REALLOC_SLICE: case B_FREE_SLICE: case B_FREE: {
            auto x = GenArrayBuiltin(c);
            if (x.empty() || Unreached()) return;
            auto want = d.t ? d.t : c->exprtype;
            auto rt = c->rettypes.empty() ? c->exprtype : c->rettypes[0];
            if (d.k == GD_DISCARD) {
                if (!IsGoName(x) && !IsGoConst(x)) L("_ = ", x);
                return;
            }
            if (want && IsRelRef(want)) {
                auto v = Snapshot(rt, x);
                EmitRelStore(AtVar(d.s, want), v, NonNullRef(c), false, ln);
                return;
            }
            x = Coerce(x, rt, want, ln);
            if (x != d.s) L(d.s, " = ", x);
            return;
        }
        default:
            Unsupported(ln, cat("the builtin ", BuiltinByKind(c->builtin).name, "()"));
    }
}

// print(a, b, ...): the whole line, newline included, is rendered on a data
// stack of the statement's before any of it is written, so that an argument
// that prints on its own cannot interleave with it, and it goes out as one
// write (§3.7), as the C backend's does (TempBuilder). The bytes are written
// at the stack's top through a cursor of the line's own: nothing else uses
// that stack while it is rendered, since a call in an argument is handed the
// indices above it. Each write is bounded by the end of the stack's region:
// a line that would run past it is a data stack overflow, as the C
// runtime's guard gap makes it. An argument that ends the flow (a return
// out of an inlined body, say) leaves the cursors declared and the rest of
// the line unreached, which FinishBody gives a use of its own.
inline void CodeGenGo::EmitPrint(Call *c) {
    if (Unreached()) return GenUnreached([&] { EmitPrint(c); });
    auto k = AllocStk();
    auto base = T(), cur = T(), end = T();
    Decl(base, "unsafe.Pointer", cat("gs.Stks[", k, "]"));
    Decl(cur, "unsafe.Pointer", base);
    Decl(end, "unsafe.Pointer", cat("gs.End(", k, ")"));
    for (auto a : c->ArgNodes()) RenderInto(cur, end, a, c);
    L(cur, " = gsPutStr(", cur, ", ", end, ", \"\\n\")");
    L("gsOut(", base, ", ", cur, ")");
}

// The text of argument a at the cursor `cur` (§3.7): a scalar's, or a string
// literal's bytes as they are.
inline void CodeGenGo::RenderInto(const string &cur, const string &end, Node *a, Call *c) {
    auto ctx = GoFmtContext(c, a);
    auto t = a->exprtype;
    // An overload the argument is rendered with, at any depth of it.
    if (ctx && !ctx->fmtspecs.empty()) Unsupported(a->line, "a format overload");
    if (auto s = Is<StrLit>(a)) {
        L(cur, " = gsPutStr(", cur, ", ", end, ", ", GoStr(s->val), ")");
        return;
    }
    if (!IsScalarT(t)) {
        if (t->kind == TY_REF && !t->ref->optional) t = t->ref->sub;
        GoT(t, a->line);
        auto l = GenLoc(a);
        if (Unreached()) return;
        RenderLoc(cur, end, l, t, false, a->line);
        return;
    }
    auto x = GenX(a);
    switch (t->kind) {
        case TY_INT:
            if (t->intstorage == IS_U64) L(cur, " = gsFmtU64(", cur, ", ", end, ", ", x, ")");
            else L(cur, " = gsFmtI64(", cur, ", ", end, ", ",
                   Coerce(x, t, ast.inttypes[IS_I64], a->line), ")");
            return;
        case TY_FLT:
            L(cur, " = gsFmt", t->fltstorage == FS_F32 ? "F32" : "F64", "(", cur, ", ", end, ", ", x,
              ")");
            return;
        default:
            L(cur, " = gsFmtBool(", cur, ", ", end, ", ", x, ")");
            return;
    }
}

}  // namespace goose
