// Goose compiler — the Go backend's values on data stacks (definitions of
// CodeGenGo members, codegen_go.h): resizable values (§5.1, §5.2) with their
// headers and fat references, frame objects (C.2), reusable pools (§5.4),
// and the construction of a value at a data stack's top (§4.2, §4.3), as the
// C backend builds them (codegen_construct.h, codegen_stmts.h,
// codegen_builtins.h).
//
// Representation (DESIGN.md §3.4):
// * A data stack is an index into the block's stacks, an int64 expression:
//   gs_sp + k for a function's own, a parameter's stack, a fat reference's
//   Stk, a destination gs_dst<i>, a global's dedicated gsGStk_<name>. Its
//   top is gs.Stks[stk]; growth is `gs.Stks[stk] = unsafe.Add(..., n)`, with
//   no capacity check: the region's guard gap catches a runaway stack, as C's
//   does.
// * A resizable value is a header (GsRhdr { Base, Len }) in the owning
//   frame, its elements from Base up to its stack's top; a frame object is
//   the Go struct of its fixed fields with its tail's header last
//   (FoTailHdr). A reference to one is a GsRref { Hdr, Stk }, and one with
//   reusable-pool provenance a GsPref, which adds the freelist's.
// * Every stack a construction takes, it takes where the C backend does
//   (AllocStk, SaveBase), and every exit restores the tops of the scopes it
//   leaves (EmitRestores), so both backends report and reserve the same
//   stacks.
#pragma once

namespace goose {

// ------------------------------------------------------------------
// Types and headers.

inline string CodeGenGo::RzGoT(TypeExpr *t, Line ln) {
    if (IsFrameObj(t)) return GoT(t, ln);
    return "GsRhdr";
}

// The innermost tail's GsRhdr lvalue within frame object `obj`.
inline string CodeGenGo::FoTailHdr(TypeExpr *t, const string &obj) {
    auto si = SI(t);
    auto ti = LastRealField(si->st->fields);
    auto s = cat(obj, ".", Sanitize(si->st->fields[ti].name));
    auto ft = si->ftypes[ti];
    return IsFrameObj(ft) ? FoTailHdr(ft, s) : s;
}

inline TypeExpr *CodeGenGo::FoTailArr(TypeExpr *t) {
    auto si = SI(t);
    auto ft = si->ftypes[LastRealField(si->st->fields)];
    return IsFrameObj(ft) ? FoTailArr(ft) : ft;
}

inline void CodeGenGo::Bump(const string &stk, const string &n) {
    L(Top(stk), " = unsafe.Add(", Top(stk), ", ", n, ")");
}

inline TypeExpr *CodeGenGo::GrowU8() {
    if (!growu8) growu8 = ast.ArrayOf(ast.inttypes[IS_U8], A_GROW, Line {});
    return growu8;
}

// ------------------------------------------------------------------
// Locals and temporaries.

// Declares the local `d` of a resizable or bytes type -- a resizable's
// header or frame object, a reusable pool's freelist beside it, a bytes
// value's base pointer -- at the top of a data stack of its own, or of its
// destination's for a named result, and returns where its value is built
// (CodeGen::DeclStackLocal).
inline GoDst CodeGenGo::DeclStackLocal(VarDef *d, bool forlocal) {
    auto name = LocalName(d);
    auto t = d->type;
    auto resz = IsResz(t);
    string stk;
    auto nit = nrvo.find(d);
    if (nit != nrvo.end()) {
        auto &nd = nit->second;
        stk = nd.stk;
        if (resz) nd.hdr = name;
        // A return of another value finds it in front (ExitStart).
        if (!nd.inlined) openat[stk]++;
    } else {
        stk = AllocStk(forlocal);
    }
    vstk[d] = stk;
    // A named result lives at the destination, which outlives us: no
    // watermark to restore there.
    auto own = nit == nrvo.end();
    if (!resz) {
        Decl(name, "unsafe.Pointer", Top(stk));
        if (own) SaveBase(forlocal, stk, name);
        return GoDst { GD_STACK, stk, t };
    }
    if (IsFrameObj(t)) {
        Decl(name, RzGoT(t, d->line));
        if (own) SaveBase(forlocal, stk, cat(FoTailHdr(t, name), ".Base"));
        return GoDst { GD_STACK, stk, t, name };
    }
    Decl(name, "GsRhdr", cat("GsRhdr{", Top(stk), ", 0}"));
    if (own) SaveBase(forlocal, stk, cat(name, ".Base"));
    if (d->reusable) {
        // Its freelist: free slot indices, or a slice pool's tree of spans,
        // on a stack of their own.
        auto flstk = AllocStk(forlocal);
        auto fln = Unique2(cat(name, "_fl"));
        Decl(fln, "GsRhdr", cat("GsRhdr{", Top(flstk), ", 0}"));
        SaveBase(forlocal, flstk, cat(fln, ".Base"));
        vpool[d] = { fln, flstk };
    }
    return GoDst { GD_STACK, stk, t, cat(name, ".Len") };
}

// A fresh statement-scoped stack for a bytes-class temporary, returned in
// `stk`; the value's base (also the watermark to restore) is the result.
inline string CodeGenGo::BytesTemp(string &stk) {
    stk = AllocStk(false);
    auto base = T();
    Decl(base, "unsafe.Pointer", Top(stk));
    SaveBase(false, stk, base);
    return base;
}

// A fresh header -- or frame object -- for a resizable value of type t,
// named by the result, on a fresh statement-scoped stack returned in
// `stk`; the elements are built there next.
inline string CodeGenGo::RzTemp(TypeExpr *t, string &stk) {
    stk = AllocStk(false);
    auto h = T();
    if (IsFrameObj(t)) {
        Decl(h, RzGoT(t, t->line));
        SaveBase(false, stk, cat(FoTailHdr(t, h), ".Base"));
    } else {
        Decl(h, "GsRhdr", cat("GsRhdr{", Top(stk), ", 0}"));
        SaveBase(false, stk, cat(h, ".Base"));
    }
    return h;
}

// What a construction of type t behind header h writes its count to: the
// whole frame object, or the header's count.
inline string CodeGenGo::RzLenLv(TypeExpr *t, const string &h) {
    return IsFrameObj(t) ? h : cat(h, ".Len");
}

// The value behind such a header as a location.
inline CodeGenGo::GoLoc CodeGenGo::RzTempLoc(TypeExpr *t, const string &h, const string &stk) {
    GoLoc l;
    l.t = t;
    l.stk = stk;
    l.hdr = h;
    if (IsFrameObj(t)) {
        l.lv = h;
    } else {
        l.addr = cat(h, ".Base");
        l.lenlv = cat(h, ".Len");
    }
    return l;
}

// A resizable-valued temporary (a call's or a construct's result): its
// elements on a fresh statement-scoped stack, its header in a temporary.
inline CodeGenGo::GoLoc CodeGenGo::GenRzTmp(Node *n) {
    auto t = n->exprtype;
    string stk;
    auto h = RzTemp(t, stk);
    GenAny(n, GoDst { GD_STACK, stk, t, RzLenLv(t, h) });
    return RzTempLoc(t, h, stk);
}

// The pointee of a fat reference `x` (a GsRref, or a GsPref) as a location.
inline CodeGenGo::GoLoc CodeGenGo::FatRefLoc(const string &x, TypeExpr *sub) {
    GoLoc l;
    l.t = sub;
    l.stk = cat(x, ".Stk");
    if (IsFrameObj(sub)) {
        l.lv = cat("(*(*", RzGoT(sub, Line {}), ")(unsafe.Pointer(", x, ".Hdr)))");
        l.hdr = l.lv;
    } else {
        l.addr = cat(x, ".Hdr.Base");
        l.lenlv = cat(x, ".Hdr.Len");
        l.hdr = cat("(*", x, ".Hdr)");
    }
    return l;
}

// A resizable variable, local or global, named `name` (a pointer to it
// where it is a free variable), at its header in the frame.
inline CodeGenGo::GoLoc CodeGenGo::RzVarLoc(const VarDef *vd, const string &name) {
    GoLoc l;
    l.t = vd->type;
    auto hdr = fvptr.count(vd) ? cat("(*", name, ")") : name;
    l.hdr = hdr;
    if (auto it = vstk.find(vd); it != vstk.end()) l.stk = it->second;
    else if (auto git = gstks.find(vd); git != gstks.end()) l.stk = git->second;
    if (vd->reusable) {
        auto pit = vpool.find(vd);
        auto git = gpools.find(vd);
        if (pit != vpool.end() || git != gpools.end()) {
            auto &p = pit != vpool.end() ? pit->second : git->second;
            l.fl = fvptr.count(vd) && pit != vpool.end() ? cat("(*", p.first, ")") : p.first;
            l.flstk = p.second;
        }
    }
    if (IsFrameObj(vd->type)) {
        l.lv = hdr;
        return l;
    }
    l.addr = cat(hdr, ".Base");
    l.lenlv = cat(hdr, ".Len");
    return l;
}

// A fat reference to the resizable at l: its header's address and its
// stack. A resizable with no header of its own (the tail of a value that
// is no frame object) cannot be referred to (C.2).
inline string CodeGenGo::RefOfRz(const GoLoc &l, Line ln) {
    if (l.hdr.empty() || l.stk.empty())
        Fail(ln, "a reference to a resizable value nested in a variable-size prefix or an ADT "
                 "payload is unsupported; reference the owning variable");
    auto h = l.hdr;
    string addr;
    if (h.size() > 3 && h.compare(0, 2, "(*") == 0 && h.back() == ')' && IsGoName(h.substr(2, h.size() - 3)))
        addr = h.substr(2, h.size() - 3);
    else
        addr = cat("&", h);
    if (IsFrameObj(l.t)) addr = cat("(*GsRhdr)(unsafe.Pointer(", addr, "))");
    return cat("GsRref{", addr, ", ", l.stk, "}");
}

// A GsPref value from a &pool expression or another pool reference, or from
// a construct choosing among pools, which each branch builds into one
// temporary (CodeGen::GenPrefVal).
inline string CodeGenGo::GenPrefVal(Node *n) {
    if (IsCtl(n)) {
        auto t = T();
        Decl(t, "GsPref");
        GenAny(n, GoDst { GD_LVALUE, t, n->exprtype, "", true });
        return t;
    }
    if (auto u = Is<Unary>(n); u && u->op == T_BITAND) {
        if (auto id = Is<Ident>(u->child); id && id->vdef && id->vdef->reusable) {
            auto vd = id->vdef;
            auto pit = vpool.find(vd);
            auto git = gpools.find(vd);
            // A captured pool arrives as a GsPref.
            if (pit == vpool.end() && git == gpools.end()) return VarRef(vd, n->line);
            auto l = RzVarLoc(vd, VarRef(vd, n->line));
            auto t = T();
            auto addr = [](const string &h) {
                if (h.size() > 3 && h.compare(0, 2, "(*") == 0 && h.back() == ')')
                    return h.substr(2, h.size() - 3);
                return cat("&", h);
            };
            Decl(t, "GsPref", cat("GsPref{", addr(l.hdr), ", ", l.stk, ", ", addr(l.fl), ", ",
                                  l.flstk, "}"));
            return t;
        }
        auto l = GenLoc(u->child);
        if (l.t->kind != TY_REF || !l.ispref) Fail(n->line, "cannot form a reusable pool reference here");
        return l.lv;
    }
    if (auto id = Is<Ident>(n); id && id->vdef && PrefVar(id->vdef)) return VarRef(id->vdef, n->line);
    Fail(n->line, "cannot form a reusable pool reference from this expression");
}

// ------------------------------------------------------------------
// Construction (§4.2, §4.3): a value written front to back at a stack's
// top, which it bumps.

inline void CodeGenGo::EmitValStore(const string &stk, TypeExpr *t, const string &x) {
    auto &ti = TI(t, t->line);
    if (ti.size == 0) {
        if (!IsGoName(x) && !IsGoConst(x)) L("_ = ", x);
        return;
    }
    L("*(*", ti.name, ")(", Top(stk), ") = ", x);
    Bump(stk, cat(ti.size));
}

// Copies `n` elements of the fixed type `elem` from `src`, an
// unsafe.Pointer, to the stack's top, as Go copies them: element by
// element where they hold pointers, which the collector must see written.
inline void CodeGenGo::EmitCopyElems(const string &stk, TypeExpr *elem, const string &src,
                                     const string &n) {
    if (!IsFix(elem)) Unsupported(elem->line, cat("an array of variable-size elements (", TypeText(elem), ")"));
    auto &ti = TI(elem, elem->line);
    auto nn = IsGoName(n) || IsGoConst(n) ? n : cat("(", n, ")");
    L("copy(unsafe.Slice((*", ti.name, ")(", Top(stk), "), ", nn, "), unsafe.Slice((*", ti.name,
      ")(", src, "), ", nn, "))");
    Bump(stk, ti.size == 1 ? nn : cat(nn, "*", ti.size));
}

// The count of an array of type at, n elements long, built at stk's top:
// into the receiving count lenlv (a resizable's).
inline void CodeGenGo::EmitArrayCount(const string &stk, TypeExpr *at, const string &lenlv,
                                      const string &n) {
    (void)stk;
    if (at->arr->akind == A_GROW || at->arr->akind == A_GROWSHRINK) {
        if (lenlv.empty()) Fail(at->line, "internal: a resizable built without its header");
        L(lenlv, " = ", n);
        return;
    }
    Unsupported(at->line, cat("a value of type ", TypeText(at)));
}

// Element count and elements of an array- or slice-valued source node, for
// construction and append: a string literal's bytes, a slice, any array
// where it is stored (through references too).
inline CodeGenGo::SrcElems CodeGenGo::GenSrcElems(Node *n) {
    SrcElems r;
    if (auto s = Is<StrLit>(n)) {
        auto v = T();
        Decl(v, "GsSlice", cat("GsStr(", GoStr(s->val), ")"));
        r.elems = cat(v, ".Data");
        r.n = cat(v, ".Len");
        return r;
    }
    auto t = n->exprtype;
    if (t->kind == TY_SLICE) {
        auto x = Snapshot(t, GenX(n));
        r.elems = cat(x, ".Data");
        r.n = cat(x, ".Len");
        return r;
    }
    auto l = GenLoc(n);
    while (l.t->kind == TY_REF) DerefLoc(l);
    if (l.t->kind == TY_SLICE) {
        auto x = l.lv;
        if (!IsGoName(x)) x = Snapshot(l.t, x);
        r.elems = cat(x, ".Data");
        r.n = cat(x, ".Len");
        return r;
    }
    auto v = ArrayView(l, n->line);
    auto nn = T();
    Decl(nn, "int64", v.fixed >= 0 ? cat(v.fixed) : v.len);
    r.elems = v.data;
    r.n = nn;
    return r;
}

// Constructs n's value at stk's top (CodeGen::GenConstruct). For a
// resizable value, lenlv names the receiving header's count: the elements
// are written there and the count assigned (§7.3's metadata outside the
// data); a frame object's is the whole object.
inline void CodeGenGo::GenConstruct(Node *n, const string &stk, TypeExpr *want,
                                    const string &lenlv) {
    if (Unreached()) return GenUnreached([&] { GenConstruct(n, stk, want, lenlv); });
    if (auto it = fillvalues.find(n); it != fillvalues.end()) {
        auto target = want ? want : it->second.t;
        if (IsBytesT(target)) ConstructFromLoc(it->second, target, stk, lenlv, n->line);
        else EmitValStore(stk, target, LoadLoc(it->second, target, n->line));
        return;
    }
    // An inlined body's named result reaching the destination it was bound
    // to (OpenIbNrvo): the elements are in place, and only the count is
    // left.
    if (auto nd = NrvoBoundAt(n, stk, lenlv)) {
        EmitNrvoFinish(*nd);
        return;
    }
    auto et = n->exprtype;
    // An array that is not fixed-size landing in a slot of another such array
    // type takes the slot's layout (§4.2).
    if (want && et && want->kind == TY_ARRAY && IsBytesT(want) && et->kind == TY_ARRAY &&
        IsBytesT(et) && !TEq(want, et) && TEq(want->arr->sub, et->arr->sub))
        et = want;
    if (want && et && IsStaticLimited(want) && (et->kind == TY_ARRAY || et->kind == TY_SLICE) &&
        !TEq(et, want))
        et = want;
    if (want && et && IsPlainRef(et) && want->kind != TY_REF && want->kind != TY_VOID) {
        // A spliced reference in a decayed slot: the pointee, constructed as
        // the slot's type from where it lies.
        if (IsBytesT(want)) ConstructFromLoc(GenLoc(n), want, stk, lenlv, n->line);
        else EmitValStore(stk, want, CoerceVal(GenX(n), et, want, n->line));
        return;
    }
    if (IsCtl(n)) {
        GenAny(n, GoDst { GD_STACK, stk, want, lenlv });
        return;
    }
    // What this places at stk while its parts are built sits in front of any
    // exit taken inside one of them (ExitStart).
    OpenAt open(*this, stk);
    if (auto c = Is<Call>(n); c && c->builtin == B_COPY) {
        // The source's storage, adapted into the destination.
        auto target = want ? want : et;
        auto l = GenLoc(c->FirstArg());
        if (IsBytesT(target)) ConstructFromLoc(l, target, stk, lenlv, n->line);
        else EmitValStore(stk, target, LoadLoc(l, target, n->line));
        return;
    }
    // A fixed-size array or slice landing in a slot of an array type that is
    // not: taken as its own type first, which checks a limited array's
    // capacity as the callee's return would, then constructed as the slot's.
    if (want && et && IsFix(et) && (et->kind == TY_ARRAY || et->kind == TY_SLICE) &&
        want->kind == TY_ARRAY && IsBytesT(want)) {
        auto v = Snapshot(et, CoerceVal(GenX(n), n->exprtype, et, n->line));
        ConstructFromLoc(AtVar(v, et), want, stk, lenlv, n->line);
        return;
    }
    if (auto c = Is<Call>(n)) {
        ConstructCall(c, et, stk, want, lenlv);
        return;
    }
    if (want && IsRelRef(want) && et->kind == TY_REF)
        Unsupported(n->line, "a relative reference built on a data stack");
    if (!IsBytesT(et)) {
        if ((Is<StructLit>(n) || Is<ArrayLit>(n)) && HasRelRef(et)) {
            // Built where it stays, so that its offsets are measured from
            // there (§3.9).
            auto &ti = TI(et, n->line);
            auto p = T();
            Decl(p, "unsafe.Pointer", Top(stk));
            Bump(stk, cat(ti.size));
            LitInto(n, AtAddr(p, et), false);
            return;
        }
        EmitValStore(stk, et, CoerceVal(GenX(n), n->exprtype, et, n->line));
        return;
    }
    if (auto s = Is<StrLit>(n)) {
        // A string literal building a u8 array: its count, then its bytes.
        EmitArrayCount(stk, et, lenlv, cat(s->val.size()));
        if (!s->val.empty()) {
            L("copy(unsafe.Slice((*byte)(", Top(stk), "), ", s->val.size(), "), ", GoStr(s->val), ")");
            Bump(stk, cat(s->val.size()));
        }
        return;
    }
    if (auto al = Is<ArrayLit>(n)) {
        if (!al->capexpr || TEq(et, al->exprtype)) {
            GenArrayLitAt(al, stk, lenlv, et);
            return;
        }
    }
    if (Is<StructLit>(n)) Unsupported(n->line, cat("a literal of type ", TypeText(et)));
    if (auto se = Is<SliceExpr>(n); se && et->kind == TY_ARRAY) {
        // A slice expression constructing an array copies the range.
        auto sv = Snapshot(ast.SliceOf(et->arr->sub, n->line), GenSlice(se));
        GenArrayFromLoc(AtVar(sv, ast.SliceOf(et->arr->sub, n->line)), et, stk, n->line, lenlv);
        return;
    }
    // Every other node denotes an existing value: the location, copied or
    // adapted.
    ConstructFromLoc(GenLoc(n), et, stk, lenlv, n->line);
}

// Constructs the value at an existing location as the non-fixed et at stk's
// top (CodeGen::ConstructFromLoc).
inline void CodeGenGo::ConstructFromLoc(GoLoc l, TypeExpr *et, const string &stk,
                                        const string &lenlv, Line ln) {
    while (l.t->kind == TY_REF && et->kind != TY_REF) DerefLoc(l);
    if (IsResz(et)) {
        if (l.t->kind == TY_SLICE && et->kind == TY_ARRAY) {
            GenArrayFromLoc(l, et, stk, ln, lenlv);
            return;
        }
        if (l.t->kind == TY_ARRAY || TEq(l.t, et)) {
            if (TEq(l.t, et) && et->kind != TY_ARRAY) EmitRzCopy(l, et, stk, lenlv, ln);
            else GenArrayFromLoc(l, et, stk, ln, lenlv);
            return;
        }
        Unsupported(ln, cat("constructing a ", TypeText(et), " from a ", TypeText(l.t)));
    }
    Unsupported(ln, cat("a value of type ", TypeText(et)));
}

// A call's first result constructed as et at stk's top (CodeGen::ConstructCall).
inline void CodeGenGo::ConstructCall(Call *c, TypeExpr *et, const string &stk, TypeExpr *want,
                                     const string &lenlv) {
    auto rt0 = c->rettypes.empty() ? nullptr : c->rettypes[0];
    // A bytes-class result feeding a fixed-class slot, or a slot of an array
    // kind it cannot be built in, is built on a temporary of its own and
    // copied into the slot below.
    auto builds = [&](TypeExpr *rt) {
        if (rt->kind != TY_ARRAY || et->kind != TY_ARRAY || TEq(rt, et)) return true;
        auto var = [](TypeExpr *t) { return t->arr->akind == A_VAR; };
        if (!lenlv.empty()) return IsResz(rt) || var(rt);
        if (IsResz(rt)) return c->builtin < 0 || var(et);
        return var(rt) && var(et);
    };
    auto own = rt0 && IsBytesT(rt0) && (!IsBytesT(et) || !builds(rt0));
    auto rets = GenCallVals(c, own ? GoDst {} : GoDst { GD_STACK, stk, want, lenlv });
    if (rets.empty() || IsVoidT(et) || Unreached()) return;
    // A resizable result with no receiving header was built behind a
    // temporary one: copied into the slot as the slot's array kind.
    if (rt0 && IsResz(rt0) && rt0->kind == TY_ARRAY && lenlv.empty() && et->kind == TY_ARRAY &&
        !rets[0].empty()) {
        GenArrayFromLoc(RzTempLoc(rt0, rets[0], ""), et, stk, c->line, lenlv);
        return;
    }
    auto fixarr = rt0 && IsFix(rt0) && (rt0->kind == TY_ARRAY || rt0->kind == TY_SLICE);
    if (rt0 && IsBytesT(et) && (IsPlainRef(rt0) || fixarr || own)) {
        ConstructFromLoc(CallResLoc(c, rets[0]), et, stk, lenlv, c->line);
        return;
    }
    if (!IsBytesT(et)) EmitValStore(stk, et, CallVal0(c, rets[0], et));
}

// The elements of the array or slice at l, as the non-fixed array type et
// at stk's top: the count, then the elements.
inline void CodeGenGo::GenArrayFromLoc(GoLoc l, TypeExpr *et, const string &stk, Line ln,
                                       const string &lenlv) {
    while (l.t->kind == TY_REF) DerefLoc(l);
    auto v = ArrayView(l, ln);
    auto nn = T();
    Decl(nn, "int64", v.fixed >= 0 ? cat(v.fixed) : v.len);
    auto d = T();
    Decl(d, "unsafe.Pointer", v.data);
    EmitArrayCount(stk, et, lenlv, nn);
    EmitCopyElems(stk, et->arr->sub, d, nn);
}

// Copies an existing resizable value that is no array to the stack top: a
// frame object's fields, then its innermost tail's elements behind a fresh
// base; the count (the object) goes to the receiving header.
inline void CodeGenGo::EmitRzCopy(GoLoc l, TypeExpr *et, const string &stk, const string &lenlv,
                                  Line ln) {
    if (lenlv.empty()) Fail(ln, "internal: a resizable copied without its header");
    if (!IsFrameObj(et) || l.lv.empty())
        Unsupported(ln, cat("copying a value of type ", TypeText(et)));
    auto src = T();
    Decl(src, RzGoT(et, ln), l.lv);
    L(lenlv, " = ", src);
    auto dh = FoTailHdr(et, lenlv), sh = FoTailHdr(et, src);
    L(dh, ".Base = ", Top(stk));
    EmitCopyElems(stk, FoTailArr(et)->arr->sub, cat(sh, ".Base"), cat(sh, ".Len"));
}

// An array literal at stk's top, as `as` where that is given (GenConstruct's
// retarget), as its own type otherwise (CodeGen::GenArrayLit).
inline void CodeGenGo::GenArrayLitAt(ArrayLit *al, const string &stk, const string &lenlv,
                                     TypeExpr *as) {
    auto et = as ? as : al->exprtype;
    if (et->kind != TY_ARRAY) Fail(al->line, "internal: an array literal of no array type");
    auto elem = et->arr->sub;
    if (al->capexpr) Unsupported(al->line, cat("a `[..cap]` literal of type ", TypeText(et)));
    auto count = GoLitCount(al);
    EmitArrayCount(stk, et, lenlv, cat(count));
    if (al->fillval) {
        vector<Node *> added;
        FreezeFill(al->fillval, elem, added);
        if (!Unreached()) {
            if (IsBytesT(elem) || HasRelRef(elem)) {
                auto iv = T();
                L("for ", iv, " := int64(0); ", iv, " < ", count, "; ", iv, "++ {");
                ind++;
                PushSc(SC_PLAIN);
                GenConstruct(al->fillval, stk, elem);
                PopSc();
                ind--;
                LS("}");
            } else {
                auto fv = Snapshot(elem, CoerceVal(GenX(al->fillval), al->fillval->exprtype, elem,
                                                   al->line));
                auto iv = T();
                L("for ", iv, " := int64(0); ", iv, " < ", count, "; ", iv, "++ {");
                ind++;
                EmitValStore(stk, elem, fv);
                ind--;
                LS("}");
            }
        }
        for (auto n : added) fillvalues.erase(n);
        return;
    }
    for (auto e : al->elems) GenConstruct(e, stk, elem);
}


// ------------------------------------------------------------------
// The array builtins on resizable receivers (§3.3, §5.1, §5.2): growth
// writes at the stack's top and bumps it and the count, with no capacity
// check; a shrink moves the top back down (CodeGen::EmitBuiltin).

// A member operation's resizable receiver, dereferenced, with its stack: one
// reached through a reference its arguments may rebind is resolved first,
// to its header's address and its stack (CodeGen::RecvLoc, PinLoc).
inline CodeGenGo::GoLoc CodeGenGo::RzRecvLoc(Node *n) {
    auto l = GenLoc(n);
    auto viaref = l.viaref;
    if (l.t->kind == TY_REF && viaref && !l.ispref) {
        // The reference itself, read now.
        auto r = Snapshot(l.t, l.lv);
        l = AtVar(r, l.t);
        viaref = false;
    }
    while (l.t->kind == TY_REF) DerefLoc(l);
    if (!viaref) return l;
    return PinRz(l);
}

// A resizable location resolved to the header and the stack it names now.
inline CodeGenGo::GoLoc CodeGenGo::PinRz(GoLoc l) {
    if (l.hdr.empty()) {
        if (!l.addr.empty() && l.lenlv.empty()) {
            auto p = T();
            Decl(p, "unsafe.Pointer", l.addr);
            l.addr = p;
        }
        l.viaref = false;
        return l;
    }
    auto h = T();
    auto ht = IsFrameObj(l.t) ? RzGoT(l.t, l.t->line) : string("GsRhdr");
    string addr = l.hdr;
    if (addr.size() > 3 && addr.compare(0, 2, "(*") == 0 && addr.back() == ')') addr = addr.substr(2, addr.size() - 3);
    else addr = cat("&", addr);
    Decl(h, cat("*", ht), addr);
    l.hdr = cat("(*", h, ")");
    if (IsFrameObj(l.t)) {
        l.lv = l.hdr;
    } else {
        l.addr = cat(h, ".Base");
        l.lenlv = cat(h, ".Len");
    }
    if (!l.stk.empty() && !IsGoName(l.stk) && !IsGoConst(l.stk)) {
        auto s = T();
        Decl(s, "int64", l.stk);
        l.stk = s;
    }
    l.viaref = false;
    return l;
}

inline string CodeGenGo::GenRzArrayBuiltin(Call *c) {
    auto an = c->ArgNodes();
    auto ln = c->line;
    auto i64 = ast.inttypes[IS_I64];
    switch ((BuiltinKind)c->builtin) {
        case B_PUSH: {
            auto r = EmitPush(an, ln);
            return r.empty() ? "" : r[0];
        }
        case B_APPEND: EmitAppend(an, ln); return "";
        case B_INDEX_OF: {
            // The checker proved the reference is an element of this very
            // array (§3.3): the distance is a whole number of elements.
            auto l = RzRecvLoc(an[0]);
            auto v = ArrayView(l, ln);
            auto r = CoerceVal(GenX(an[1]), an[1]->exprtype, ast.RefTo(v.elem, ln, false), ln);
            return cat("(int64(uintptr(unsafe.Pointer(", r, ")) - uintptr(", v.data, ")) / ", v.esize, ")");
        }
        case B_POP: {
            auto l = RzRecvLoc(an[0]);
            auto v = ArrayView(l, ln);
            auto elem = v.elem;
            auto nl = T();
            Decl(nl, "int64", cat(v.len, " - 1"));
            L("if ", nl, " < 0 {");
            ind++;
            L("GsAbort(GsEPop, ", LocArgs(ln), ")");
            ind--;
            LS("}");
            L(v.lenlv, " = ", nl);
            // The array tops its stack: its elements end at the top.
            Bump(l.stk, cat(-v.esize));
            auto slot = AtAddr(Top(l.stk), elem);
            auto rt = c->rettypes.empty() ? elem : c->rettypes[0];
            if (IsRelRef(elem)) {
                auto p = T();
                Decl(p, "unsafe.Pointer", Top(l.stk));
                return Snapshot(rt, LoadLoc(AtAddr(p, elem), rt, ln));
            }
            return Snapshot(elem, slot.lv);
        }
        case B_POP_N: {
            auto l = RzRecvLoc(an[0]);
            auto v = ArrayView(l, ln);
            auto nn = Snapshot(i64, Coerce(GenX(an[1]), an[1]->exprtype, i64, ln));
            // The length is read after the count, which may have changed it.
            auto ol = T();
            Decl(ol, "int64", v.len);
            L("if uint64(", nn, ") > uint64(", ol, ") {");
            ind++;
            L("GsAbort(GsEPopN, ", LocArgs(ln), ")");
            ind--;
            LS("}");
            L(v.lenlv, " = ", ol, " - ", nn);
            Bump(l.stk, cat("-", nn, v.esize == 1 ? "" : cat("*", v.esize)));
            return "";
        }
        case B_CLEAR: {
            auto l = RzRecvLoc(an[0]);
            auto v = ArrayView(l, ln);
            L(Top(l.stk), " = ", v.data);
            L(v.lenlv, " = 0");
            return "";
        }
        case B_RESIZE: {
            auto l = RzRecvLoc(an[0]);
            auto v = ArrayView(l, ln);
            auto elem = v.elem;
            auto rel = IsRelRef(elem);
            auto nn = Snapshot(i64, Coerce(GenX(an[1]), an[1]->exprtype, i64, ln));
            string fv;
            if (an.size() > 2) {
                if (rel) {
                    auto pt = ast.RefTo(elem->ref->sub, ln, elem->ref->optional);
                    fv = Is<NullLit>(an[2]) ? "nil"
                                            : Snapshot(pt, CoerceVal(GenX(an[2]), an[2]->exprtype, pt, ln));
                } else {
                    fv = Snapshot(elem, CoerceVal(GenX(an[2]), an[2]->exprtype, elem, ln));
                }
            }
            auto ol = T();
            Decl(ol, "int64", v.len);
            L("if ", nn, " < 0 {");
            ind++;
            L("GsAbort(GsEResizeNeg, ", LocArgs(ln), ")");
            ind--;
            LS("}");
            L("if ", nn, " < ", ol, " {");
            ind++;
            L(v.lenlv, " = ", nn);
            L(Top(l.stk), " = unsafe.Add(", v.data, ", ", nn, v.esize == 1 ? "" : cat("*", v.esize), ")");
            ind--;
            LS("} else if ", nn, " > ", ol, " {");
            ind++;
            if (fv.empty()) {
                L("GsAbort(GsEResizeFill, ", LocArgs(ln), ")");
            } else {
                // The new slots are filled through a cursor of their own, and
                // the top is written once, after the fill.
                auto cur = T();
                Decl(cur, "unsafe.Pointer", Top(l.stk));
                auto iv = T();
                L("for ", iv, " := ", ol, "; ", iv, " < ", nn, "; ", iv, "++ {");
                ind++;
                auto slot = AtAddr(cur, elem);
                if (rel) {
                    if (fv == "nil") L(slot.lv, " = 0");
                    else EmitRelStore(slot, fv, NonNullRef(an[2]), false, ln);
                } else {
                    L(slot.lv, " = ", fv);
                }
                L(cur, " = unsafe.Add(", cur, ", ", v.esize, ")");
                ind--;
                LS("}");
                L(Top(l.stk), " = ", cur);
                L(v.lenlv, " = ", nn);
            }
            ind--;
            LS("}");
            return "";
        }
        default: break;
    }
    Unsupported(ln, cat("the builtin ", BuiltinByKind(c->builtin).name, "() on a resizable array"));
}

// push (CodeGen::EmitPush): the receiver, then the argument, then the
// element is added (§2). A fixed-size element is evaluated before its slot
// is claimed (ElemValue); one holding relative references is built in the
// slot, which the checker keeps the array from growing into meanwhile.
inline vector<string> CodeGenGo::EmitPush(vector<Node *> &an, Line ln) {
    auto l = RzRecvLoc(an[0]);
    auto v = ArrayView(l, ln);
    auto elem = v.elem;
    if (!IsFix(elem)) Unsupported(ln, cat("a push of a variable-size element (", TypeText(elem), ")"));
    auto ev = ElemValue(an[1], elem);
    if (Unreached()) return { "nil" };
    auto e = T();
    Decl(e, "unsafe.Pointer", Top(l.stk));
    auto slot = AtAddr(e, elem);
    if (IsRelRef(elem)) {
        if (ev == "nil") L(slot.lv, " = 0");
        else EmitRelStore(slot, ev, NonNullRef(an[1]), false, ln);
        Bump(l.stk, cat(v.esize));
    } else if (ev.empty()) {
        GenConstruct(an[1], l.stk, elem);
    } else {
        EmitValStore(l.stk, elem, ev);
    }
    L(v.lenlv, "++");
    return { NewElemResult(slot, ln) };
}

// append (CodeGen::EmitAppend): a fresh call's or construct's resizable
// result built straight at the receiver's top, a literal's elements built
// there, anything else's elements copied there.
inline void CodeGenGo::EmitAppend(vector<Node *> &an, Line ln) {
    auto l = RzRecvLoc(an[0]);
    auto v = ArrayView(l, ln);
    auto elem = v.elem;
    auto src = an[1];
    if (auto c = Is<Call>(src); c && c->builtin == B_COPY) src = c->FirstArg();
    auto st = src->exprtype;
    auto asrun = IsResz(st) || (st->kind == TY_ARRAY && st->arr->akind == A_VAR);
    auto fresh = Is<Call>(src) || IsCtl(src);
    if (fresh && asrun) {
        auto nn = T();
        Decl(nn, "int64", "0");
        GenAny(src, GoDst { GD_STACK, l.stk, st, nn });
        L(v.lenlv, " += ", nn);
        return;
    }
    if (auto al = Is<ArrayLit>(src); al && (IsBytesT(elem) || HasRelRef(elem))) {
        if (IsBytesT(elem)) {
            auto nn = T();
            Decl(nn, "int64", "0");
            GenArrayLitAt(al, l.stk, nn, nullptr);
            L(v.lenlv, " += ", nn);
            return;
        }
        // The run is a fixed array of the literal's type laid over the
        // slots at the top, built there.
        auto count = GoLitCount(al);
        auto rt = ast.ArrayOf(elem, A_FIXED, ln, count);
        auto p = T();
        Decl(p, "unsafe.Pointer", Top(l.stk));
        ArrayLitInto(al, AtAddr(p, rt), false);
        Bump(l.stk, cat(count * v.esize));
        L(v.lenlv, " += ", count);
        return;
    }
    auto se = GenSrcElems(src);
    auto nn = T();
    Decl(nn, "int64", se.n);
    EmitCopyElems(l.stk, elem, se.elems, nn);
    L(v.lenlv, " += ", nn);
}

// ------------------------------------------------------------------
// Builtins with results on data stacks.

inline vector<string> CodeGenGo::GenRzBuiltin(Call *c, const GoDst &d0) {
    auto an = c->ArgNodes();
    switch ((BuiltinKind)c->builtin) {
        case B_STR: return EmitStr(c, an, d0, c->line);
        default: break;
    }
    auto t = c->exprtype;
    if (IsVoidT(t) || d0.k == GD_DISCARD) {
        GenBuiltin(c, GoDst {});
        return { "" };
    }
    if (IsBytesT(t)) Unsupported(c->line, cat("the builtin ", BuiltinByKind(c->builtin).name,
                                             "() with a result of type ", TypeText(t)));
    auto tv = T();
    Decl(tv, LocalT(t, c->line));
    GenBuiltin(c, GoDst { GD_LVALUE, tv, t });
    return { tv };
}

// A u8[>..] builder on a statement stack (CodeGen::TempBuilder).
inline CodeGenGo::GoLoc CodeGenGo::TempBuilder() {
    string stk;
    auto h = RzTemp(GrowU8(), stk);
    return RzTempLoc(GrowU8(), h, stk);
}

// The n bytes at src appended to the growable u8 array at l (AppendBytes):
// at a resizable's top, or into a limited array's free slots under its
// capacity check.
inline void CodeGenGo::AppendBytes(const GoLoc &l, const string &src, const string &n, Line ln) {
    auto v = ArrayView(l, ln);
    if (l.t->arr->akind == A_LIMITED) {
        auto ol = T();
        Decl(ol, "int64", v.len);
        L("if ", ol, " + ", n, " > ", v.cap, " {");
        ind++;
        L("GsAbort(GsECapacity, ", LocArgs(ln), ")");
        ind--;
        LS("}");
        L("copy(unsafe.Slice((*byte)(unsafe.Add(", v.data, ", ", ol, ")), ", n, "), unsafe.Slice((*byte)(",
          src, "), ", n, "))");
        L(v.lenlv, " = ", IntGoT(v.lenst), "(", ol, " + ", n, ")");
        return;
    }
    L("copy(unsafe.Slice((*byte)(", Top(l.stk), "), ", n, "), unsafe.Slice((*byte)(", src, "), ", n, "))");
    Bump(l.stk, n);
    L(v.lenlv, " += ", n);
}

// The text of `a` rendered at the top of the builder b, whose count it adds
// to: through a cursor, as print renders its line (RenderInto), bounded by
// the end of the builder's region.
inline void CodeGenGo::RenderToBuilder(const GoLoc &b, Node *a, Call *c) {
    auto cur = T(), end = T(), start = T();
    Decl(start, "unsafe.Pointer", Top(b.stk));
    Decl(cur, "unsafe.Pointer", start);
    Decl(end, "unsafe.Pointer", cat("gs.End(", b.stk, ")"));
    RenderInto(cur, end, a, c);
    if (Unreached()) return;
    L(Top(b.stk), " = ", cur);
    L(b.lenlv, " += int64(uintptr(", cur, ") - uintptr(", start, "))");
}

// The scalar, bool and u8-array texts that need no builder of their own
// (CodeGen::SimpleText).
inline bool CodeGenGo::SimpleText(Call *c, TypeExpr *t) {
    if (c)
        for (auto &fs : c->fmtspecs)
            if (TEq(fs.first, t)) return false;
    if (t->kind == TY_INT || t->kind == TY_FLT || t->kind == TY_BOOL) return true;
    if (t->kind == TY_ARRAY) return IsU8(t->arr->sub);
    if (t->kind == TY_SLICE) return IsU8(t->sub);
    return false;
}

// Appends the text of `a` to the growable u8 array at l (CodeGen::
// EmitFormatInto): rendered straight at a resizable's top, or into a
// builder of its own first for a limited array, so that its capacity check
// comes before anything lands in it.
inline void CodeGenGo::EmitFormatInto(GoLoc l, Node *a, Line ln, Call *c) {
    if (l.t->arr->akind != A_LIMITED) {
        RenderToBuilder(l, a, c);
        return;
    }
    auto b = TempBuilder();
    RenderToBuilder(b, a, c);
    if (Unreached()) return;
    AppendBytes(l, cat(b.hdr, ".Base"), cat(b.hdr, ".Len"), ln);
}

// str(a, b, ...): the arguments' text as a fresh u8[>..] at the
// destination: the elements at its top, the count to the receiving header,
// or a temporary on a statement stack where there is no destination of its
// own (CodeGen::EmitStr).
inline vector<string> CodeGenGo::EmitStr(Call *c, vector<Node *> &an, const GoDst &d0, Line ln) {
    string stk = d0.s, lenlv = d0.lenlv, hdr;
    if (d0.k != GD_STACK) {
        hdr = RzTemp(GrowU8(), stk);
        lenlv = cat(hdr, ".Len");
    } else if (lenlv.empty()) {
        Unsupported(ln, "str() building a variable-size value");
    }
    auto cnt = T();
    Decl(cnt, "int64", "0");
    GoLoc b;
    b.t = GrowU8();
    b.stk = stk;
    b.lenlv = cnt;
    for (auto a : an) {
        if (Unreached()) break;
        // A value with no simple text renders into a builder of its own
        // first, whose bytes are then copied (RenderToTemp), as C's does.
        if (!SimpleText(GoFmtContext(c, a), a->exprtype)) {
            auto tb = TempBuilder();
            RenderToBuilder(tb, a, c);
            if (Unreached()) break;
            AppendBytes(b, cat(tb.hdr, ".Base"), cat(tb.hdr, ".Len"), ln);
            continue;
        }
        RenderToBuilder(b, a, c);
    }
    L(lenlv, " = ", cnt);
    if (!hdr.empty()) return { hdr };
    return {};
}


// ------------------------------------------------------------------
// Named results (§7.3) and exits that deliver a value at a stack's top.

// Guaranteed NRVO: the top-level local the returns hand back in one
// non-fixed return position is allocated at that destination (a local of
// the result's own type: C's other case, a resizable returned as a variable
// array behind a reserved prefix, is a bytes-class result this version
// does not take), and a return of any other value builds it behind the
// local and moves it down (CodeGen::DetectNrvo).
inline void CodeGenGo::DetectNrvo(FnSpec *sp) {
    nrvo.clear();
    for (size_t j = 0; j < sp->rets.size(); j++) {
        if (!IsBytesT(sp->rets[j])) continue;
        auto cand = NamedResultOf(sp->body, sp->sf, sp->rets.size(), j);
        if (!cand || nrvo.count(cand)) continue;
        auto rt = sp->rets[j];
        auto ct = cand->type;
        auto rzarr = IsResz(ct) && ct->kind == TY_ARRAY && rt->kind == TY_ARRAY &&
                     rt->arr->akind == A_VAR && TEq(ct->arr->sub, rt->arr->sub);
        if (rzarr) Unsupported(sp->sf->line, cat("a resizable local returned as a ", TypeText(rt)));
        if (!TEq(ct, rt)) continue;
        NrvoDest nd;
        nd.stk = cat("gs_dst", j);
        if (IsResz(rt)) {
            nd.lenlv = dstcounts[j];
            nd.fo = IsFrameObj(ct);
        }
        nrvo[cand] = nd;
    }
}

// An inlined call in a construction context binds its block's named result
// to the destination, rather than to a stack of its own (CodeGen::
// OpenIbNrvo); returns the bound local, or null.
inline const VarDef *CodeGenGo::OpenIbNrvo(InlineBlock *ib, const GoDst &d) {
    if (d.k != GD_STACK || !ib->spec || ib->spec->rets.size() != 1) return nullptr;
    auto vd = NamedResultOf(ib->body, ib->sf, 1, 0);
    if (!vd || !IsBytesT(vd->type) || nrvo.count(vd)) return nullptr;
    auto ct = vd->type;
    NrvoDest nd;
    nd.stk = d.s;
    nd.inlined = true;
    if (!IsResz(ct)) {
        if (!d.lenlv.empty() || !d.t || !TEq(ct, d.t)) return nullptr;
        nrvo[vd] = nd;
        return vd;
    }
    if (d.lenlv.empty() || !d.t) return nullptr;
    auto elems = ct->kind == TY_ARRAY && d.t->kind == TY_ARRAY &&
                 (IsResz(d.t) || d.t->arr->akind == A_VAR) && TEq(ct->arr->sub, d.t->arr->sub);
    if (!TEq(ct, d.t) && !elems) return nullptr;
    nd.lenlv = d.lenlv;
    nd.fo = IsFrameObj(ct);
    nrvo[vd] = nd;
    return vd;
}

// An inlined body's named result reaching the destination it was bound to
// is in place already: building it there writes only its count.
inline const CodeGenGo::NrvoDest *CodeGenGo::NrvoBoundAt(Node *val, const string &stk,
                                                         const string &lenlv) {
    auto id = Is<Ident>(val);
    if (!id || !id->vdef) return nullptr;
    auto it = nrvo.find(id->vdef);
    if (it == nrvo.end() || !it->second.inlined || it->second.stk != stk ||
        it->second.lenlv != lenlv)
        return nullptr;
    return &it->second;
}

inline void CodeGenGo::EmitNrvoFinish(const NrvoDest &nd) {
    if (nd.hdr.empty() || nd.lenlv.empty()) return;   // A bytes value is complete.
    if (nd.fo) L(nd.lenlv, " = ", nd.hdr);
    else L(nd.lenlv, " = ", nd.hdr, ".Len");
}

// Scope si receives exit values at d: what is open on its stack now, and the
// point its top on entry gets declared at if an exit needs it.
inline void CodeGenGo::EnterDst(int si, const GoDst &d) {
    auto &s = scopes[si];
    s.dst = d;
    if (d.k != GD_STACK) return;
    s.open0 = openat[d.s];
    s.topat = body.size();
    s.topind = ind;
}

// Declares `name` as stk's top at body offset `at`, where its scope begins,
// and shifts the offsets recorded after it (labels, other tops). In code
// that is generated to be dropped, nothing is declared.
inline void CodeGenGo::DeclareTop0(size_t at, int indent, const string &name, const string &stk) {
    string line((size_t)indent, '\t');
    Append(line, "var ", name, " unsafe.Pointer = ", Top(stk), DECLMARK, name, "\n");
    if (at > body.size()) at = body.size();
    body.insert(at, line);
    // A label recorded here belongs in front of the statement that follows,
    // which the declaration now precedes.
    for (auto &s : scopes) {
        if (s.topat > at) s.topat += line.size();
        if (s.labelat >= at) s.labelat += line.size();
    }
}

inline string CodeGenGo::ScopeTop0(int si) {
    auto &s = scopes[si];
    // Code generated to be dropped declares nothing: the text it would go
    // in is not the text kept.
    if (dry && s.top0.empty()) return T();
    if (s.top0.empty()) {
        s.top0 = T();
        DeclareTop0(s.topat, s.topind, s.top0, s.dst.s);
    }
    return s.top0;
}

inline string CodeGenGo::DstTop0(size_t i) {
    if (dsttop0.size() <= i) dsttop0.resize(i + 1);
    if (dry && dsttop0[i].empty()) return T();
    if (dsttop0[i].empty()) {
        dsttop0[i] = T();
        DeclareTop0(0, 1, dsttop0[i], cat("gs_dst", i));
    }
    return dsttop0[i];
}

// Where an exit's value will start on stk, taken before it is built where a
// construction opened there since the exit's scope was entered may have
// placed something in front of it; "" where none can have.
inline string CodeGenGo::ExitStart(Node *val, const string &stk, const string &lenlv, int open0) {
    if (openat[stk] <= open0 || NrvoBoundAt(val, stk, lenlv)) return "";
    auto start = T();
    Decl(start, "unsafe.Pointer", Top(stk));
    return start;
}

// The value built from `start` up to stk's top moves down to top0; a frame
// object's tail header, which lenlv receives, follows its elements.
inline void CodeGenGo::LandValue(const string &stk, const string &top0, const string &start,
                                 TypeExpr *t, const string &lenlv) {
    if (Unreached()) return;
    L("if ", start, " != ", top0, " {");
    ind++;
    auto n = T();
    Decl(n, "int64", cat("int64(uintptr(", Top(stk), ") - uintptr(", start, "))"));
    L("copy(unsafe.Slice((*byte)(", top0, "), ", n, "), unsafe.Slice((*byte)(", start, "), ", n, "))");
    L(Top(stk), " = unsafe.Add(", top0, ", ", n, ")");
    if (t && IsFrameObj(t)) {
        auto th = FoTailHdr(t, lenlv);
        L(th, ".Base = unsafe.Add(", top0, ", uintptr(", th, ".Base) - uintptr(", start, "))");
    }
    ind--;
    LS("}");
}

}  // namespace goose
