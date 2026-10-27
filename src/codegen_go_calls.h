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
    return ki.needsgs ? "gs" : GsArgPh(sp);
}

// The Go call of a specialization, its arguments evaluated: the data stack
// block and the first index free here where the callee needs them, the
// declared parameters, then the free variables. An argument read before a
// later one that runs statements is taken into a temporary first (§2).
inline string CodeGenGo::SpecCallX(Call *c, FnSpec *sp) {
    if (sp->sf->isextern) return ExternCallX(c, sp);
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
    auto call = cat(ki.name, "(", list, ")");
    if (!ki.hasrf) return call;
    // A long-distance return can come back through it: the call is a
    // statement of its own, checked after (EmitRfCheck), its results in
    // temporaries -- what this gives, "" for none.
    string names, marks;
    for (auto rt : sp->rets) {
        auto t = T();
        NoteType(t, GoT(rt, c->line));
        Append(names, names.empty() ? "" : ", ", t);
        Append(marks, marks.empty() ? "" : ",", t);
    }
    if (names.empty()) L(call);
    else L(names, " := ", call, DECLMARK, marks);
    EmitRfCheck(sp);
    return names;
}

// A call of an extern fn (§7.10): the C functions std binds that the Go
// runtime has an equivalent of, its arguments evaluated in order as for
// any call (CodeGen::EmitExternCall). Any other is not supported yet.
inline string CodeGenGo::ExternCallX(Call *c, FnSpec *sp) {
    auto &name = sp->sf->cname;
    auto an = CallArgNodes(c, sp->argtypes.size());
    auto known = sp->sf->dylib.empty() &&
                 (name == "gs_popcount" || name == "gs_clz" || name == "gs_ctz" || name == "gs_find_any" ||
                  name == "gs_find_pair" || name == "frexp" || name == "ldexp");
    if (!known) Unsupported(c->line, cat("a call to the extern fn ", sp->sf->name, " (C's ", name, ")"));
    vector<bool> later(an.size() + 1, false);
    for (size_t i = an.size(); i-- > 0;) later[i] = later[i + 1] || HasStmts(an[i]);
    vector<string> a;
    for (size_t i = 0; i < an.size(); i++) {
        auto pt = sp->argtypes[i];
        auto x = Coerce(GenX(an[i]), an[i]->exprtype, pt, an[i]->line);
        if (later[i + 1] && !IsGoConst(x)) x = Snapshot(pt, x);
        a.push_back(x);
    }
    // popcount, clz and ctz count 64 for zero, as Go's math/bits does.
    if (name == "gs_popcount" || name == "gs_clz" || name == "gs_ctz") {
        usesbits = true;
        auto fn = name == "gs_popcount" ? "OnesCount64" : name == "gs_clz" ? "LeadingZeros64" : "TrailingZeros64";
        return cat("int64(bits.", fn, "(", a[0], "))");
    }
    if (name == "gs_find_any") return cat("gsFindAny(", a[0], ", ", AddrOfRef(sp->argtypes[1], a[1]), ")");
    if (name == "gs_find_pair")
        return cat("gsFindPair(", a[0], ", ", AddrOfRef(sp->argtypes[1], a[1]), ", ", a[2], ", ",
                   AddrOfRef(sp->argtypes[3], a[3]), ")");
    if (name == "frexp") return cat("gsFrexp(", a[0], ", ", a[1], ")");
    return cat("math.Ldexp(", a[0], ", int(", a[1], "))");
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
    // A variable-array result taken as an element run, through the callee's
    // element-run twin (ername), or in its own layout with its prefix slid
    // out afterwards (slide); or landing in a slot of another length storage,
    // its prefix replaced after the call (reprefix).
    string ername, slidebase, reprefixbase, reprefixcnt;
    GoDst slide, reprefix;
    for (size_t i = 0; i < sp->rets.size(); i++) {
        auto rt = sp->rets[i];
        GoDst dd = alldst && i < alldst->size() ? (*alldst)[i] : (i == 0 ? d0 : GoDst {});
        if (i == 0 && dd.k == GD_STACK && !dd.lenlv.empty() && rt->kind == TY_ARRAY &&
            rt->arr->akind == A_VAR) {
            auto er = EnsureEr(sp);
            if (!er.empty()) {
                ername = er;
                counts[i] = dd.lenlv;
            } else {
                slide = dd;
                slidebase = T();
                Decl(slidebase, "unsafe.Pointer", Top(dd.s));
            }
            args.push_back(dd.s);
            continue;
        }
        if (i == 0 && NeedsReprefix(dd, rt)) {
            reprefix = dd;
            reprefixbase = T();
            Decl(reprefixbase, "unsafe.Pointer", Top(dd.s));
            auto er = EnsureEr(sp);
            if (!er.empty()) {
                // The run lands behind the destination's prefix: its bytes
                // claimed now, the count patched in afterwards.
                ername = er;
                reprefixcnt = T();
                Decl(reprefixcnt, "int64", "0");
                counts[i] = reprefixcnt;
                Bump(dd.s, cat(PrefixBytes(LenStore(dd.t->arr))));
            }
            args.push_back(dd.s);
            retex[i] = reprefixbase;
            continue;
        }
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
    auto call = cat(ername.empty() ? ki.name : ername, "(", list, ")");
    string names, marks;
    vector<pair<string, string>> assigns;
    for (size_t i = 0; i < sp->rets.size(); i++) {
        auto rt = sp->rets[i];
        // The twin returns its run's count.
        auto run = i == 0 && !ername.empty();
        if (IsBytesT(rt) && !IsResz(rt) && !run) continue;
        auto t = T();
        NoteType(t, run || (!IsFrameObj(rt) && IsResz(rt)) ? string("int64")
                    : IsFrameObj(rt) ? RzGoT(rt, c->line) : GoT(rt, c->line));
        Append(names, names.empty() ? "" : ", ", t);
        Append(marks, marks.empty() ? "" : ",", t);
        if (IsResz(rt) || run) assigns.push_back({ counts[i], t });
        else retex[i] = t;
    }
    if (names.empty()) L(call);
    else L(names, " := ", call, DECLMARK, marks);
    for (auto &[lv, t] : assigns) L(lv, " = ", t);
    // A long-distance return in flight leaves dummies, nothing to adjust.
    if (HasRf(sp)) EmitRfCheck(sp);
    if (!reprefixbase.empty()) EmitReprefix(sp, reprefix, reprefixbase, reprefixcnt);
    // The value form arrived as [len][elems]: its prefix slid out, the count
    // reported.
    if (!slidebase.empty()) EmitSlidePrefix(slidebase, LenStore(sp->rets[0]->arr), slide.s, slide.lenlv);
    return retex;
}

// Whether a variable-array result of type rt reaches d as a value slot of a
// T[] of another length storage, whose layout the callee cannot write
// (CodeGen::NeedsReprefix).
inline bool CodeGenGo::NeedsReprefix(const GoDst &d, TypeExpr *rt) {
    return d.k == GD_STACK && d.lenlv.empty() && d.t && d.t->kind == TY_ARRAY && d.t->arr->akind == A_VAR &&
           rt->kind == TY_ARRAY && rt->arr->akind == A_VAR && LenStore(d.t->arr) != LenStore(rt->arr);
}

// A variable array that arrived at `base` in value form ([len][elems]) where
// the receiver wanted an element run: the count to lenlv, the elements slid
// down over the prefix, whose bytes the stack's top gives back
// (CodeGen::EmitSlidePrefix).
inline void CodeGenGo::EmitSlidePrefix(const string &base, IntStorage ls, const string &stk,
                                       const string &lenlv) {
    auto ms = T();
    if (ls == IS_VARINT) {
        Decl(ms, "int64", cat("GsUlebSize(", base, ")"));
        L(lenlv, " = GsUlebRead(", base, ")");
    } else {
        Decl(ms, "int64", cat(IntSize(ls)));
        L(lenlv, " = int64(*(*", IntGoT(ls), ")(", base, "))");
    }
    auto n = T();
    Decl(n, "int64", cat("int64(uintptr(", Top(stk), ") - uintptr(", base, ")) - ", ms));
    L("copy(unsafe.Slice((*byte)(", base, "), ", n, "), unsafe.Slice((*byte)(unsafe.Add(", base, ", ", ms,
      ")), ", n, "))");
    Bump(stk, cat("-", ms));
}

// The elements of a variable-array call result at `base` -- a run behind
// prefix bytes reserved before the call, its count in cnt, or, where cnt is
// empty, behind the callee's own prefix -- given the destination's prefix
// (CodeGen::EmitReprefix).
inline void CodeGenGo::EmitReprefix(FnSpec *sp, const GoDst &dd, const string &base, const string &cnt) {
    auto ls = LenStore(dd.t->arr);
    if (!cnt.empty()) {
        EmitPrefixPatch(base, ls, dd.s, cnt, AddOff(base, PrefixBytes(ls)));
        return;
    }
    auto srcls = LenStore(sp->rets[0]->arr);
    auto count = T(), oms = T();
    if (srcls == IS_VARINT) {
        Decl(count, "int64", cat("GsUlebRead(", base, ")"));
        Decl(oms, "int64", cat("GsUlebSize(", base, ")"));
    } else {
        Decl(count, "int64", cat("int64(*(*", IntGoT(srcls), ")(", base, "))"));
        Decl(oms, "int64", cat(IntSize(srcls)));
    }
    EmitLenCheck(ls, count);
    auto ms = T();
    string pfx;
    if (ls == IS_VARINT) {
        pfx = T();
        Decl(pfx, "[10]byte");
        Decl(ms, "int64", cat("GsUlebWrite(unsafe.Pointer(&", pfx, "), uint64(", count, "))"));
    } else {
        Decl(ms, "int64", cat(IntSize(ls)));
    }
    auto n = T();
    Decl(n, "int64", cat("int64(uintptr(", Top(dd.s), ") - uintptr(", base, ")) - ", oms));
    L("copy(unsafe.Slice((*byte)(unsafe.Add(", base, ", ", ms, ")), ", n, "), unsafe.Slice((*byte)(unsafe.Add(",
      base, ", ", oms, ")), ", n, "))");
    if (ls == IS_VARINT) L("copy(unsafe.Slice((*byte)(", base, "), ", ms, "), ", pfx, "[:", ms, "])");
    else L("*(*", IntGoT(ls), ")(", base, ") = ", IntGoT(ls), "(", count, ")");
    Bump(dd.s, cat(ms, " - ", oms));
}

// A call's results as EmitSpecCall gives them, whatever is called: a
// builtin, a spliced function value, a case-function dispatch or a
// specialization (CodeGen::EmitCall).
inline vector<string> CodeGenGo::GenCallVals(Call *c, const GoDst &d0, vector<GoDst> *alldst) {
    if (c->builtin >= 0) return GenRzBuiltin(c, d0);
    if (c->fvbody) {
        auto rv = GenFvCall(c, d0);
        if (rv.empty()) return {};
        return { rv };
    }
    if (!c->dispatch.empty()) {
        for (auto rt : c->dispatch[0]->rets)
            if (IsBytesT(rt)) Unsupported(c->line, "a case-function dispatch with a non-fixed result");
        return GenDispatch(c);
    }
    if (!c->spec) Fail(c->line, "internal: unresolved call");
    if (c->spec->sf->isextern && !PlainSig(c->spec)) Unsupported(c->line, "a call to an extern fn");
    if (PlainSig(c->spec)) {
        auto sp = c->spec;
        auto call = SpecCallX(c, sp);
        vector<string> res;
        if (sp->rets.empty()) {
            if (!call.empty()) L(call);
            return res;
        }
        string names, marks;
        for (size_t i = 0; i < sp->rets.size(); i++) {
            auto t = T();
            NoteType(t, GoT(sp->rets[i], c->line));
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
    // A result that is not fixed-size (or the one a reference result points
    // at) read as a fixed type -- a slice of it, a static-capacity limited
    // array -- from where the call left it.
    auto st = IsPlainRef(rt) ? rt->ref->sub : rt;
    if (IsBytesT(st) && IsFix(et) && et->kind != TY_REF) return LoadLoc(CallResLoc(c, r0), et, c->line);
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
// A variable-mode one is its image: a case taking its payload by reference
// gets the payload's address in it; one taking it by value a copy, taken
// before the later arguments run -- of the whole image, on a stack of its
// own, where a by-value case takes a variable-size payload, else of each
// fixed-size payload a by-value case takes (CodeGen::EmitDispatch).
inline vector<string> CodeGenGo::GenDispatch(Call *c) {
    auto ln = c->line;
    auto sp0 = c->dispatch[0];
    auto an = CallArgNodes(c, sp0->params.size());
    auto pos = c->dispatcharg;
    auto sn = an[pos];
    auto enumtype = sn->exprtype;
    auto isref = enumtype->kind == TY_REF;
    if (isref) enumtype = enumtype->ref->sub;
    if (enumtype->kind != TY_ENUM)
        Unsupported(ln, cat("a case-function dispatch over ", TypeText(enumtype)));
    auto varmode = enumtype->enu->varmode;
    auto ei = EIOf(enumtype);
    auto &ti = TI(enumtype, ln);
    auto nv = ei->en->variants.size();
    auto ts = TagSize(ei->en);
    GoLoc e;
    string p, copied, tagx;
    vector<string> snap(nv);
    vector<string> args(an.size());
    for (size_t i = 0; i < an.size(); i++) {
        if ((int)i == pos && varmode) {
            p = T();
            if (isref) {
                Decl(p, "unsafe.Pointer", GenX(sn));
            } else {
                auto l = GenLoc(sn);
                while (l.t->kind == TY_REF) DerefLoc(l);
                Decl(p, "unsafe.Pointer", l.addr);
            }
            tagx = cat("*(*", IntGoT(TagStore(ei->en)), ")(", p, ")");
            auto wholecopy = false;
            for (size_t vi = 0; vi < nv; vi++)
                wholecopy = wholecopy || (c->dispatch[vi]->argtypes[pos]->kind != TY_REF &&
                                          IsBytesT(VariantType(enumtype, (int)vi)));
            if (wholecopy) {
                string stk;
                copied = BytesTemp(stk);
                auto sz = T();
                Decl(sz, "int64", SizeX(enumtype, p));
                L("copy(unsafe.Slice((*byte)(", Top(stk), "), ", sz, "), unsafe.Slice((*byte)(", p, "), ", sz, "))");
                Bump(stk, sz);
                continue;
            }
            for (size_t vi = 0; vi < nv; vi++) {
                if (c->dispatch[vi]->argtypes[pos]->kind == TY_REF || EmptyLayout(ei->en->variants[vi].fields))
                    continue;
                auto vt = VariantType(enumtype, (int)vi);
                snap[vi] = T();
                Decl(snap[vi], LocalT(vt, ln));
                L("if ", tagx, " == ", vi, " {");
                ind++;
                L(snap[vi], " = *(*", GoT(vt, ln), ")(unsafe.Add(", p, ", ", ts, "))");
                ind--;
                LS("}");
            }
            continue;
        }
        if ((int)i == pos) {
            // A copy of the value, through a reference too, taken before the
            // later arguments run: they may change it, and the dispatch is on
            // the value it had (CodeGen::EmitDispatch).
            e = AtVar(Snapshot(enumtype, CoerceVal(GenX(sn), sn->exprtype, enumtype, ln)), enumtype);
            continue;
        }
        auto pt = sp0->argtypes[i];
        // By the convention an ordinary call passes them (EmitArg).
        if (IsPoolParam(sp0, i)) {
            args[i] = GenPrefVal(an[i]);
            continue;
        }
        if (IsBytesT(pt)) Unsupported(an[i]->line, "a case-function dispatch with a non-fixed argument");
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
        if (varmode) Decl(tag, IntGoT(TagStore(ei->en)), tagx);
        else Decl(tag, ti.rep == GR_TAG ? ti.name : IntGoT(TagStore(ei->en)), TagOf(e, ln));
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
            string varg;
            if (varmode) {
                auto payload = cat("unsafe.Add(", pt->kind == TY_REF ? p : copied, ", ", ts, ")");
                if (pt->kind == TY_REF) varg = IsBytesT(vt) ? payload : RefFromAddr(pt, payload);
                else if (IsBytesT(vt)) varg = payload;   // The copy's: the case's own value.
                else if (EmptyLayout(ei->en->variants[vi].fields)) varg = ZeroOf(vt, ln);
                else if (!snap[vi].empty()) varg = snap[vi];
                else varg = cat("*(*", GoT(vt, ln), ")(", payload, ")");
            } else {
                if (pt->kind == TY_REF) Fail(ln, "internal: a fixed-mode payload dispatched by reference");
                varg = ti.voffs.empty() || ti.voffs[vi] < 0 ? ZeroOf(vt, ln) : PayloadLoc(e, (int)vi, ln).lv;
            }
            auto pv = pt->kind == TY_REF || IsBytesT(pt) ? varg : Coerce(varg, vt, pt, ln);
            auto list = StackArgs(sp, ln);
            for (size_t i = 0; i < an.size(); i++)
                Append(list, list.empty() ? "" : ", ", (int)i == pos ? pv : args[i]);
            for (auto fv : ki.freevars) Append(list, list.empty() ? "" : ", ", FreeVarArg(c->FreeVarArg(fv)));
            auto call = cat(ki.name, "(", list, ")");
            if (res.empty()) {
                L(call);
            } else {
                string names;
                for (size_t i = 0; i < res.size(); i++) Append(names, i ? ", " : "", res[i]);
                L(names, " = ", call);
            }
            if (ki.hasrf) EmitRfCheck(sp);
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
    if (sp->sf->isextern && !PlainSig(sp)) Unsupported(c->line, "a call to an extern fn");
    if (sp->rets.empty()) Fail(c->line, "internal: a call without results as a value");
    if (!PlainSig(sp)) {
        auto rets = EmitSpecCall(c, sp, GoDst {}, nullptr);
        // A non-fixed result is read as the fixed type its receiver takes
        // (a slice of it, a limited array), from where the call built it.
        if (IsBytesT(sp->rets[0])) return LoadLoc(CallResLoc(c, rets[0]), c->exprtype, c->line);
        return CallVal0(c, rets[0], c->exprtype);
    }
    auto call = SpecCallX(c, sp);
    auto t = T();
    if (sp->rets.size() > 1) {
        // A call in an ordinary value context yields its first result.
        NoteType(t, GoT(sp->rets[0], c->line));
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
    if (sp->sf->isextern && !PlainSig(sp)) Unsupported(c->line, "a call to an extern fn");
    if (!PlainSig(sp)) {
        auto rets = EmitSpecCall(c, sp, d, nullptr);
        if (d.k == GD_DISCARD || rets.empty() || Unreached()) return;
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
    // A call's results may go unused, as an expression statement; a call
    // checked for a long-distance return has run already.
    if (d.k == GD_DISCARD || sp->rets.empty()) {
        if (!HasRf(sp)) L(call);
        return;
    }
    auto want = d.t ? d.t : c->exprtype;
    string r = call;
    if (sp->rets.size() > 1) {
        r = T();
        NoteType(r, GoT(sp->rets[0], c->line));
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
    if (c->fvbody || (c->builtin < 0 && !c->spec) || (c->builtin >= 0 && c->builtin != B_FROM_BYTES))
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
        NoteType(n, GoT(sp->rets[i], c->line));
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
// Calling a function value: the checked body instance spliced inline
// (§7.6), its parameters ordinary locals of this function
// (CodeGen::EmitFvCall). The call was checked against its destination and
// the body's value without one, so the two can be different array or slice
// types: an array where a slice is expected (§3.10), any array or slice
// constructing an array of another kind (§4.2). The body then builds its
// value as its own type in a temporary, which is sliced whole or copied as
// the type the destination names, the call's where it names none: the
// call's value is a copy (§4.1), so a slice of it never points into a
// variable the body names. The value, or "" where it was built at a stack
// destination (or there is none).
inline string CodeGenGo::GenFvCall(Call *c, const GoDst &d0) {
    auto et = c->fvbody->exprtype;
    auto wantsval = !IsVoidT(et);
    if (Unreached()) {
        string r;
        GenUnreached([&] { r = GenFvCall(c, d0); });
        return r;
    }
    auto want = d0.t ? d0.t : c->exprtype;
    auto adapt = wantsval && (want->kind == TY_ARRAY || want->kind == TY_SLICE) && !TEq(want, et) &&
                 (IsFix(want) || d0.k == GD_STACK);
    string rv, stk;
    GoDst d = d0;
    if (wantsval && !IsBytesT(et) && (adapt || d0.k != GD_LVALUE)) {
        rv = T();
        Decl(rv, LocalT(et, c->line));
        d = GoDst { GD_LVALUE, rv, et };
    } else if (wantsval && IsResz(et) && (adapt || d0.k != GD_STACK)) {
        rv = RzTemp(et, stk);
        d = GoDst { GD_STACK, stk, et, RzLenLv(et, rv) };
    } else if (wantsval && IsBytesT(et) && (adapt || d0.k != GD_STACK)) {
        rv = BytesTemp(stk);
        d = GoDst { GD_STACK, stk, et };
    } else if (wantsval && d0.k == GD_LVALUE) {
        rv = d0.s;
    }
    PushSc(SC_PLAIN);
    L("{");
    ind++;
    for (size_t i = 0; i < c->fvparams.size(); i++)
        BindLocal(c->fvparams[i], i < c->args.size() ? c->args[i] : nullptr);
    for (auto st : c->fvbody->stmts) GenStmt(st);
    if (auto tail = c->fvbody->tail) {
        if (!wantsval) GenAny(tail, GoDst {});
        else if (d.k == GD_STACK) GenConstruct(tail, d.s, d.t ? d.t : c->exprtype, d.lenlv);
        else GenAny(tail, GoDst { GD_LVALUE, d.s, d.t ? d.t : c->exprtype });
    }
    PopSc();
    ind--;
    LS("}");
    if (!wantsval || Unreached()) return "";
    if (adapt) {
        GoLoc lv;
        if (IsResz(et)) lv = RzTempLoc(et, rv, stk);
        else if (IsBytesT(et)) lv = BytesLoc(rv, et, GoLoc {});
        else lv = AtVar(rv, et);
        if (d0.k == GD_STACK) {
            if (IsFix(want)) EmitValStore(d0.s, want, LoadLoc(lv, want, c->line));
            else ConstructFromLoc(lv, want, d0.s, d0.lenlv, c->line);
            return "";
        }
        rv = LoadLoc(lv, want, c->line);
        if (d0.k == GD_LVALUE) {
            L(d0.s, " = ", rv);
            rv = d0.s;
        }
    }
    // A value built on the stack destination is complete there, as the
    // type the destination names: nothing is left for the construction
    // around the call to store (ConstructCall).
    return rv;
}

// ------------------------------------------------------------------
// Builtins.

// The call whose format overloads (§3.7) the call's argument is rendered
// with (CodeGen::FmtContext).
static Call *GoFmtContext(Call *c, Node *arg) {
    if (!c || c->fmtcontexts.empty()) return c;
    auto args = c->ArgNodes();
    // format's first argument is its builder, which has no context.
    size_t start = c->builtin == B_FORMAT ? 1 : 0;
    for (size_t i = start; i < args.size(); i++)
        if (args[i] == arg && i - start < c->fmtcontexts.size()) return c->fmtcontexts[i - start];
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
                GenInto(c->defaultinit, ElemAt(av, i, ln), t->arr->sub);
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
            if (IsBytesT(rt)) {
                // A value held where it lies, read as the fixed slot's type
                // (the C backend's LoadLoc at the call's type); a slice
                // destination views a copy built on a temporary, not the
                // source.
                GoLoc l;
                if (want && want->kind == TY_SLICE) {
                    auto save = c->exprtype;
                    c->exprtype = rt;
                    l = GenLoc(c);
                    c->exprtype = save;
                } else {
                    l = GenLoc(an[0]);
                }
                auto x = LoadLoc(l, want, ln);
                if (d.k == GD_DISCARD || Unreached()) return;
                if (x != d.s) L(d.s, " = ", x);
                return;
            }
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
        case B_TO_BYTES: {
            // Appending to a builder, or a fresh image read as the slot's
            // type (a slice, say): built on a temporary of its own.
            auto res = EmitToBytes(an, GoDst {}, ln);
            if (d.k == GD_DISCARD || res.empty() || Unreached()) return;
            auto want = d.t ? d.t : c->exprtype;
            auto x = LoadLoc(RzTempLoc(GrowU8(), res[0], ""), want, ln);
            if (x != d.s) L(d.s, " = ", x);
            return;
        }
        case B_BYTES_OF: {
            auto res = EmitBytesOf(c, an, ln);
            if (d.k == GD_DISCARD || Unreached()) return;
            auto want = d.t ? d.t : c->exprtype;
            auto x = CoerceVal(res[0], c->rettypes[0], want, ln);
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
    RenderTarget rt;
    rt.stk = k;
    rt.base = base;
    auto saved = rtarget;
    rtarget = &rt;
    for (auto a : c->ArgNodes()) RenderInto(cur, end, a, c);
    rtarget = saved;
    L(cur, " = gsPutStr(", cur, ", ", end, ", \"\\n\")");
    L("gsOut(", base, ", ", cur, ")");
    // An overload grew the line as a builder: the stack's top goes back.
    if (rt.used) L(Top(k), " = ", base);
}

// The place an argument is rendered from (CodeGen::RenderedLoc): a call's
// or an inlined body's that gives a plain reference is the reference, held
// in a local, so that an overload taking the value by reference is handed
// the storage it points at, not a copy.
inline CodeGenGo::GoLoc CodeGenGo::RenderedLoc(Node *a) {
    auto c = Is<Call>(a);
    auto ib = Is<InlineBlock>(a);
    TypeExpr *rt = nullptr;
    if (c && !c->rettypes.empty()) rt = c->rettypes[0];
    else if (ib && ib->spec && ib->spec->rets.size() == 1) rt = ib->spec->rets[0];
    if (!rt || !IsPlainRef(rt) || IsVarintT(rt->ref->sub)) return GenLoc(a);
    auto v = T();
    Decl(v, LocalT(rt, a->line));
    GenAny(a, GoDst { GD_LVALUE, v, rt });
    return AtVar(v, rt);
}

// The overload a rendering calls for a value of type t, if any.
inline FnSpec *CodeGenGo::FmtSpecFor(Call *c, TypeExpr *t) {
    if (!c) return nullptr;
    for (auto &fs : c->fmtspecs)
        if (TEq(fs.first, t)) return fs.second;
    return nullptr;
}

// A user format overload called on the value at l (CodeGen::EmitUserFormat),
// handed the builder the text is rendered into: the cursor's text joins it
// first, and the cursor goes on after what it holds when the overload
// returns -- which may have cleared it, as an overload may.
inline void CodeGenGo::EmitUserFormat(const string &cur, const GoLoc &l, FnSpec *sp, Line ln) {
    if (!rtarget) Fail(ln, "internal: a format overload with no builder to hand it");
    if (!sinfo.count(sp)) Fail(ln, cat("internal: no Go function for ", sp->sf->name));
    auto &ki = sinfo[sp];
    if (IsPoolParam(sp, 0) || IsPoolParam(sp, 1) || !ki.freevars.empty())
        Unsupported(ln, "a format overload taking a pool or capturing variables");
    auto &rt = *rtarget;
    PushSc(SC_PLAIN);
    L(Top(rt.stk), " = ", cur);
    GoLoc b;
    if (rt.b.hdr.empty()) {
        // print's line, from where it starts.
        auto h = T();
        Decl(h, "GsRhdr", cat("GsRhdr{", rt.base, ", int64(uintptr(", cur, ") - uintptr(", rt.base, "))}"));
        b.t = GrowU8();
        b.hdr = h;
        b.stk = rt.stk;
    } else {
        L(rt.b.lenlv, " += int64(uintptr(", cur, ") - uintptr(", rt.start, "))");
        b = rt.b;
    }
    auto pt = sp->argtypes[1];
    string arg;
    if (pt->kind == TY_REF && IsResz(pt->ref->sub)) {
        arg = RefOfRz(l, ln);
    } else if (pt->kind == TY_REF && IsBytesT(pt->ref->sub)) {
        arg = l.addr;
    } else if (pt->kind == TY_REF && IsVarintT(l.t)) {
        // An i64 read out of varint storage, which holds no i64: the
        // overload is handed a temporary holding the value (CodeGen::RefArg).
        auto x = T();
        Decl(x, GoT(pt->ref->sub, ln), LoadLoc(l, pt->ref->sub, ln));
        arg = cat("&", x);
    } else if (pt->kind == TY_REF) {
        arg = RefOfLoc(l);
    } else {
        arg = LoadLoc(l, pt, ln);
    }
    if (!IsGoName(arg) && !IsGoConst(arg) && arg != "nil") arg = Snapshot(pt, arg);
    auto list = StackArgs(sp, ln);
    Append(list, list.empty() ? "" : ", ", RefOfRz(b, ln), ", ", arg);
    L(ki.name, "(", list, ")");
    if (ki.hasrf) EmitRfCheck(sp);
    if (rt.b.hdr.empty()) {
        L(cur, " = ", Top(rt.stk));
    } else {
        L(rt.start, " = ", Top(rt.stk));
        L(cur, " = ", rt.start);
    }
    rt.used = true;
    PopSc();
}

// The text of argument a at the cursor `cur` (§3.7): a scalar's, or a string
// literal's bytes as they are.
inline void CodeGenGo::RenderInto(const string &cur, const string &end, Node *a, Call *c) {
    auto ctx = GoFmtContext(c, a);
    auto t = a->exprtype;
    // The overloads the argument is rendered with, at any depth of it.
    struct Ctx {
        Call *&r, *saved;
        ~Ctx() { r = saved; }
    } keep { rctx, rctx };
    rctx = ctx;
    auto vt = t->kind == TY_REF && !t->ref->optional ? t->ref->sub : t;
    if (auto s = Is<StrLit>(a); s && !FmtSpecFor(ctx, t)) {
        L(cur, " = gsPutStr(", cur, ", ", end, ", ", GoStr(s->val), ")");
        return;
    }
    if (!IsScalarT(t) || FmtSpecFor(ctx, t)) {
        GoT(vt, a->line);
        auto l = RenderedLoc(a);
        if (Unreached()) return;
        RenderLoc(cur, end, l, vt, false, a->line);
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
