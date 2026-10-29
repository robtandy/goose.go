// Goose compiler — the Go backend's places (definitions of CodeGenGo members,
// codegen_go.h): a variable, a field, an element or a pointee as a Go
// lvalue of its type's Go representation (codegen_go_types.h), and what a
// path reads as, as the C backend's locations do (Loc, codegen_values.h):
// a reference's pointee, a relative reference's target (§3.9), an array as
// a slice of itself (§3.10), a variant as its ADT (§3.5), and an array or a
// slice copied into a static-capacity limited array (§4.2).
#pragma once

namespace goose {

// ------------------------------------------------------------------
// Addresses.

// unsafe.Add(p, off), folding the offsets of one into another.
inline string CodeGenGo::AddOff(const string &p, int64_t off) {
    if (!off) return p;
    // unsafe.Add(q, n) + off is unsafe.Add(q, n + off), where n is a number.
    if (p.compare(0, 11, "unsafe.Add(") == 0 && p.back() == ')') {
        auto inner = p.substr(11, p.size() - 12);
        auto parts = GoSplitTop(inner, ", ");
        int64_t n;
        bool uns;
        if (parts.size() == 2 && GoConstInt(parts[1], n, uns) && !uns)
            return n + off ? cat("unsafe.Add(", parts[0], ", ", n + off, ")") : parts[0];
    }
    return cat("unsafe.Add(", p, ", ", off, ")");
}

// The value of type t at address p, an unsafe.Pointer: an lvalue of its Go
// type, but for a type holding Go pointers, which p may hold unaligned
// (GoLoc::unal).
inline CodeGenGo::GoLoc CodeGenGo::AtAddr(const string &p, TypeExpr *t) {
    GoLoc l;
    l.t = t;
    l.addr = p;
    if (GoHasPtrs(t)) {
        GoT(t, Line {});
        l.unal = true;
    } else {
        l.lv = cat("(*(*", GoT(t, Line {}), ")(", p, "))");
    }
    return l;
}

// The value of type t that the Go pointer x points at.
inline CodeGenGo::GoLoc CodeGenGo::AtTyped(const string &x, TypeExpr *t) {
    GoLoc l;
    l.t = t;
    l.ptr = x;
    l.lv = cat("(*", x, ")");
    return l;
}

// A Go variable (or any other addressable Go lvalue) of type t.
inline CodeGenGo::GoLoc CodeGenGo::AtVar(const string &x, TypeExpr *t) {
    GoLoc l;
    l.t = t;
    l.lv = x;
    return l;
}

// The same, where x may be a variable the body declares (Decl), which is a
// Go local of the function's own (GoLoc::golocal).
inline CodeGenGo::GoLoc CodeGenGo::AtLocal(const string &x, TypeExpr *t) {
    auto l = AtVar(x, t);
    l.golocal = IsGoName(x) && ltypes.count(x);
    return l;
}

inline string CodeGenGo::AddrOf(const GoLoc &l) {
    if (!l.addr.empty()) return l.addr;
    if (!l.ptr.empty()) return cat("unsafe.Pointer(", l.ptr, ")");
    return cat("unsafe.Pointer(&", l.lv, ")");
}

inline string CodeGenGo::PtrOf(const GoLoc &l) {
    if (!l.ptr.empty()) return l.ptr;
    if (l.unal) Fail(l.t->line, cat("internal: a typed pointer to a ", TypeText(l.t), " that may lie unaligned"));
    if (!l.addr.empty()) return cat("(*", GoT(l.t, Line {}), ")(", l.addr, ")");
    return cat("&", l.lv);
}

// A reference to the place l: its address where the reference is one
// (RefIsAddr, a bytes value's), a typed Go pointer otherwise.
inline string CodeGenGo::RefOfLoc(const GoLoc &l) {
    if (IsBytesT(l.t) && !IsResz(l.t)) return l.addr;
    if (GoHasPtrs(l.t)) return AddrOf(l);
    return PtrOf(l);
}

inline CodeGenGo::GoLoc CodeGenGo::AtRef(const string &x, TypeExpr *rt) {
    auto sub = rt->ref->sub;
    if (IsResz(sub)) return FatRefLoc(x, sub);
    if (IsBytesT(sub)) return BytesLoc(x, sub, GoLoc {});
    return RefIsAddr(rt) ? AtAddr(x, sub) : AtTyped(x, sub);
}

// A place resolved to the storage it names now: what code run before its
// later uses -- the right side of an assignment, the arguments after a
// receiver -- cannot move by rebinding a reference or changing an index.
inline CodeGenGo::GoLoc CodeGenGo::PinLoc(const GoLoc &l) {
    // A Go local's own storage is where it is: its indices are snapshots,
    // and every array it is indexed in is a fixed one. Only a bounds check
    // in it runs now, as the pin's would, and again, passing, where the
    // place is used.
    if (l.golocal && !l.lv.empty() && !l.unal) {
        if (l.lv.find("GsIdx(") != string::npos) L("_ = ", l.lv);
        return l;
    }
    auto p = T();
    if (l.unal) {
        Decl(p, "unsafe.Pointer", l.addr);
        return AtAddr(p, l.t);
    }
    Decl(p, cat("*", GoT(l.t, Line {})), PtrOf(l));
    auto r = AtTyped(p, l.t);
    return r;
}

inline string CodeGenGo::Ld(const GoLoc &l) {
    if (!l.unal) return l.lv;
    return cat(LdStFn(l.t, false), "(", l.addr, ")");
}

inline void CodeGenGo::St(const GoLoc &l, const string &x) {
    if (!l.unal) {
        L(l.lv, " = ", x);
        return;
    }
    L(LdStFn(l.t, true), "(", l.addr, ", ", x, ")");
}

// The helpers that read and write a value of type t at an address that may
// be unaligned: a copy of its bytes through a variable of its Go type,
// which Go compiles to a few loads and stores.
inline string CodeGenGo::LdStFn(TypeExpr *t, bool store) {
    auto &ti = TI(t, t->line);
    auto it = ldstfns.find(ti.name);
    if (it == ldstfns.end()) {
        auto m = Mangle(t);
        auto ld = Unique(cat("gsLd_", m)), st = Unique(cat("gsSt_", m));
        it = ldstfns.emplace(ti.name, pair<string, string> { ld, st }).first;
        Append(ldstcode, "// A ", ti.name, " where it may lie unaligned, byte for byte.\nfunc ", ld,
               "(p unsafe.Pointer) (v ", ti.name, ") {\n\t*(*[", ti.size,
               "]byte)(unsafe.Pointer(&v)) = *(*[", ti.size, "]byte)(p)\n\treturn\n}\n\nfunc ", st,
               "(p unsafe.Pointer, v ", ti.name, ") {\n\t*(*[", ti.size, "]byte)(p) = *(*[", ti.size,
               "]byte)(unsafe.Pointer(&v))\n}\n\n");
    }
    return store ? it->second.second : it->second.first;
}

inline void CodeGenGo::CopyElemBytes(const string &dst, const string &src, const string &n,
                                     int64_t esize) {
    auto nn = IsGoName(n) || IsGoConst(n) ? n : cat("(", n, ")");
    auto nb = esize == 1 ? nn : cat(nn, "*", esize);
    L("copy(unsafe.Slice((*byte)(", dst, "), ", nb, "), unsafe.Slice((*byte)(", src, "), ", nb, "))");
}

inline void CodeGenGo::ClearElemBytes(const string &dst, const string &n, int64_t esize) {
    auto nn = IsGoName(n) || IsGoConst(n) ? n : cat("(", n, ")");
    L("clear(unsafe.Slice((*byte)(", dst, "), ", esize == 1 ? nn : cat(nn, "*", esize), "))");
}

inline void CodeGenGo::GenInto(Node *n, const GoLoc &l, TypeExpr *t) {
    if (!t) t = l.t;
    if (!l.unal) {
        GenAny(n, GoDst { GD_LVALUE, l.lv, t });
        return;
    }
    auto v = T();
    Decl(v, LocalT(t, n->line));
    GenAny(n, GoDst { GD_LVALUE, v, t });
    if (Unreached()) return;
    St(l, v);
}

// ------------------------------------------------------------------
// Parts of values.

inline CodeGenGo::GoLoc CodeGenGo::FieldLocAt(const GoLoc &base, int fi, Line ln) {
    const vector<Field> *fields;
    const vector<TypeExpr *> *ftypes;
    TypeExpr *ft;
    if (base.t->kind == TY_STRUCT) {
        auto si = SI(base.t);
        fields = &si->st->fields;
        ftypes = &si->ftypes;
        ft = si->ftypes[fi];
    } else if (base.t->kind == TY_VARIANT) {
        auto ei = EIVar(base.t);
        auto vi = ei->en->VariantIndex(base.t->var->variant);
        fields = &ei->en->variants[vi].fields;
        ftypes = &ei->vftypes[vi];
        ft = ei->vftypes[vi][fi];
    } else {
        Fail(ln, cat("internal: a field of a value of type ", TypeText(base.t)));
    }
    // A field of a frame object laid out as bytes (C.2).
    if (IsFrameObj(base.t) && IsFoBytesLoc(base)) return FoFieldLoc(base, fi, ln);
    if (IsBytesT(base.t) && !IsFrameObj(base.t)) {
        // A field of a bytes value: at its offset in the image, past a
        // cursor over the variable-size fields before it (FieldPtr). The
        // resizable tail of a value that is no frame object has no header
        // of its own: its count is the owner's (C.2).
        auto r = BytesLoc(FieldPtr(base.addr, *fields, *ftypes, fi), ft, base);
        if (!IsResz(ft)) r.lenlv.clear();
        r.hdr.clear();
        return r;
    }
    auto &ti = TI(base.t, ln);
    GoLoc r;
    if (ti.rep == GR_NATIVE) {
        r = AtVar(cat(base.lv, ".", Sanitize((*fields)[fi].name)), ft);
        r.golocal = base.golocal;
    } else {
        r = AtAddr(AddOff(AddrOf(base), ti.offs[fi]), ft);
    }
    r.viaref = base.viaref || ft->kind == TY_REF;
    // A part of a frame object lives on its stack: its tail is a header (or
    // a nested frame object) of its own (C.2).
    r.stk = base.stk;
    r.fl = base.fl;
    r.flstk = base.flstk;
    if (IsResz(ft)) {
        r.hdr = r.lv;
        if (!IsFrameObj(ft)) {
            r.addr = cat(r.lv, ".Base");
            r.lenlv = cat(r.lv, ".Len");
            r.lv.clear();
            r.ptr.clear();
        }
    }
    return r;
}

// An ADT's tag, as an lvalue of its representation's integer type.
inline string CodeGenGo::TagOf(const GoLoc &e, Line ln) {
    auto ei = EIOf(e.t);
    // A variable-mode ADT's tag leads its image.
    if (IsBytesT(e.t)) return cat("(*(*", IntGoT(TagStore(ei->en)), ")(", e.addr, "))");
    auto &ti = TI(e.t, ln);
    if (ti.rep == GR_TAG) return e.lv;
    if (TagStore(ei->en) == IS_U8) return cat(e.lv, "[0]");
    return cat("(*(*uint16)(", AddrOf(e), "))");
}

// The payload of variant vi, which the ADT at e holds.
inline CodeGenGo::GoLoc CodeGenGo::PayloadLoc(const GoLoc &e, int vi, Line ln) {
    auto ei = EIOf(e.t);
    auto vt = VariantType(e.t, vi);
    if (IsBytesT(e.t)) return BytesLoc(AddOff(e.addr, TagSize(ei->en)), vt, e);
    auto &ti = TI(e.t, ln);
    if (ti.voffs[vi] < 0) Fail(ln, "internal: the payload of a variant without one");
    auto r = AtAddr(AddOff(AddrOf(e), ti.voffs[vi]), vt);
    r.viaref = e.viaref;
    return r;
}

// The elements of an array or a slice at l: where they start, how many
// there are, the lvalue of a limited array's length, and the Go array
// itself where the value is one.
inline CodeGenGo::GoView CodeGenGo::ArrayView(const GoLoc &l, Line ln) {
    GoView v;
    auto t = l.t;
    if (t->kind == TY_SLICE) {
        v.elem = t->sub;
        auto x = Ld(l);
        v.data = cat(x, ".Data");
        v.len = cat(x, ".Len");
    } else if (t->kind == TY_ARRAY && t->arr->akind == A_FIXED) {
        v.elem = t->arr->sub;
        v.arr = l.lv;
        v.local = l.golocal && !l.lv.empty();
        v.data = AddrOf(l);
        v.len = cat(ArrSize(t->arr));
        v.fixed = ArrSize(t->arr);
    } else if (IsStaticLimited(t)) {
        v.elem = t->arr->sub;
        auto &ti = TI(t, ln);
        if (ti.rep == GR_NATIVE) {
            v.arr = cat(l.lv, ".e");
            v.data = cat("unsafe.Pointer(&", l.lv, ".e)");
            v.lenlv = cat(l.lv, ".len");
        } else {
            v.data = AddOff(AddrOf(l), ti.elemoff);
            auto ls = LenStore(t->arr);
            v.lenlv = ls == IS_U8 ? cat(l.lv, "[0]")
                                  : cat("(*(*", IntGoT(ls), ")(", AddrOf(l), "))");
        }
        v.len = cat("int64(", v.lenlv, ")");
        v.cap = cat(ArrSize(t->arr));
        v.lenst = LenStore(t->arr);
    } else if (t->kind == TY_ARRAY && t->arr->akind == A_VAR) {
        // A variable array's image (§3.3): its length prefix, then its
        // elements, which a varint prefix puts behind a size of its own.
        v.elem = t->arr->sub;
        auto ls = LenStore(t->arr);
        if (ls == IS_VARINT) {
            auto p = T();
            Decl(p, "unsafe.Pointer", cat("unsafe.Add(", l.addr, ", GsUlebSize(", l.addr, "))"));
            v.data = p;
            v.len = cat("GsUlebRead(", l.addr, ")");
        } else {
            v.data = AddOff(l.addr, IntSize(ls));
            v.len = cat("int64(*(*", IntGoT(ls), ")(", l.addr, "))");
        }
    } else if (t->kind == TY_ARRAY && t->arr->akind == A_LIMITED) {
        // A runtime capacity: [cap u32][len u32][cap slots].
        v.elem = t->arr->sub;
        v.data = AddOff(l.addr, 8);
        v.lenlv = cat("(*(*uint32)(", AddOff(l.addr, 4), "))");
        v.len = cat("int64(", v.lenlv, ")");
        v.cap = cat("int64(*(*uint32)(", l.addr, "))");
        v.lenst = IS_U32;
    } else if (t->kind == TY_ARRAY && (t->arr->akind == A_GROW || t->arr->akind == A_GROWSHRINK)) {
        // A resizable's elements from its header's base; its count is the
        // header's (C.2).
        v.elem = t->arr->sub;
        v.data = l.addr;
        v.len = l.lenlv;
        v.lenlv = l.lenlv;
        v.lenst = IS_I64;
        if (l.lenlv.empty()) Fail(ln, "internal: a resizable array without its header");
    } else {
        Fail(ln, cat("internal: the elements of a value of type ", TypeText(t)));
    }
    // Variable-size elements are walked, never indexed.
    v.esize = IsFix(v.elem) ? TI(v.elem, ln).size : -1;
    return v;
}

// Element i of a view, i an int64 already checked against its length.
inline CodeGenGo::GoLoc CodeGenGo::ElemAt(const GoView &v, const string &i, Line ln) {
    int64_t k;
    bool uns;
    auto isconst = GoConstInt(i, k, uns);
    if (!v.arr.empty() && isconst && !uns && k >= 0) {
        auto r = AtVar(cat(v.arr, "[", k, "]"), v.elem);
        r.golocal = v.local;
        return r;
    }
    // An element of a Go local's own fixed array: Go's indexing, which takes
    // no address of the variable, so that it stays a Go value rather than a
    // slot of the function's frame (GoLoc::golocal). The index is in range,
    // checked (GsIdx) or proven so, and Go's own check passes too.
    if (v.local && !isconst) {
        auto r = AtVar(cat(v.arr, "[", i, "]"), v.elem);
        r.golocal = true;
        return r;
    }
    if (isconst && !uns) return AtAddr(AddOff(v.data, k * v.esize), v.elem);
    auto ix = IsGoName(i) ? i : cat("(", i, ")");
    auto off = v.esize == 1 ? ix : cat(ix, "*", v.esize);
    (void)ln;
    return AtAddr(cat("unsafe.Add(", v.data, ", ", off, ")"), v.elem);
}

// An index's value as the int64 its check and its address take, read now
// (CodeGen's GenPure): the place it addresses is spelled where it is used,
// which may be after code that changes the variable the index names.
inline string CodeGenGo::IndexValue(Node *idx) {
    auto x = GenX(idx);
    auto it = idx->exprtype;
    if (it && it->kind == TY_INT && it->ix) it = ast.inttypes[it->intstorage];
    int64_t v;
    bool uns;
    // A u64 constant above i64.max is its bits, a negative int64, which the
    // check reports as GS_IDX's (int64_t) cast does.
    if (GoConstInt(x, v, uns)) return cat(v);
    auto i64 = ast.inttypes[IS_I64];
    return Snapshot(i64, Coerce(x, it, i64, idx->line));
}

// Element idx of the array or slice at l, bounds-checked unless BCE proved
// it in range or it is a constant within a fixed array's length (§10.5).
// The check sits in the address itself (GsIdx), so it runs where the
// element is reached, as GS_IDX does: on the side of a && or || only where
// that side runs.
inline CodeGenGo::GoLoc CodeGenGo::IndexLoc(GoLoc l, Node *idxnode, Line ln, bool nobc) {
    while (l.t->kind == TY_REF) DerefLoc(l);
    auto v = ArrayView(l, ln);
    // An index that runs statements may change the array: the elements and
    // the length are read before it. A Go local's own fixed array stays
    // where it is, and so does its length.
    if (HasStmts(idxnode) && !v.local) {
        auto d = T(), n = T();
        Decl(d, "unsafe.Pointer", v.data);
        Decl(n, "int64", v.len);
        v.data = d;
        v.len = n;
        v.arr.clear();
    }
    auto idx = IndexValue(idxnode);
    int64_t k;
    bool uns;
    auto inrange = v.fixed >= 0 && GoConstInt(idx, k, uns) && !uns && k >= 0 && k < v.fixed;
    if (!nobc && !inrange) idx = cat("GsIdx(", idx, ", ", v.len, ", ", LocArgs(ln), ")");
    auto r = ElemAt(v, idx, ln);
    r.viaref = l.viaref || v.elem->kind == TY_REF;
    return r;
}

// ------------------------------------------------------------------
// Paths.

inline CodeGenGo::GoLoc CodeGenGo::VarLoc(const VarDef *vd, Line ln) {
    if (vd->reusable && !vpool.count(vd) && !gpools.count(vd)) {
        // A captured pool arrives as a GsPref.
        auto l = FatRefLoc(VarRef(vd, ln), vd->type);
        auto x = VarRef(vd, ln);
        l.fl = cat("(*", x, ".FreeHdr)");
        l.flstk = cat(x, ".FreeStk");
        return l;
    }
    if (IsResz(vd->type)) {
        auto it = vnames.find(vd);
        auto name = it != vnames.end() ? it->second : VarRef(vd, ln);
        return RzVarLoc(vd, name);
    }
    if (IsBytesT(vd->type)) {
        GoLoc l;
        l.t = vd->type;
        l.addr = VarRef(vd, ln);
        if (auto it = vstk.find(vd); it != vstk.end()) l.stk = it->second;
        else if (auto git = gstks.find(vd); git != gstks.end()) l.stk = git->second;
        return l;
    }
    auto l = AtVar(VarRef(vd, ln), vd->type);
    // A local or a parameter, but for a free variable, which arrives as a
    // pointer to the caller's, and a large value, a pointer into a data
    // stack: a Go variable of the function's own.
    l.golocal = vnames.count(vd) && !fvptr.count(vd);
    if (PrefVar(vd)) {
        l.ispref = true;
        l.fl = cat("(*", l.lv, ".FreeHdr)");
        l.flstk = cat(l.lv, ".FreeStk");
    }
    // A reference held in a `var` can be rebound, and what is reached
    // through it moves with it.
    l.viaref = vd->isvar && vd->type->kind == TY_REF;
    return l;
}

// Steps from a place holding a reference to the reference's pointee: a
// plain reference is a Go pointer, a relative one the target its offset
// encodes (§3.9). An optional one has been narrowed where this is reached.
inline void CodeGenGo::DerefLoc(GoLoc &l) {
    if (l.t->kind != TY_REF) Fail(Line {}, "internal: a dereference of no reference");
    auto sub = l.t->ref->sub;
    auto viaref = l.viaref;
    if (!IsRelRef(l.t) && IsResz(sub)) {
        auto ispref = l.ispref;
        auto x = Ld(l);
        l = FatRefLoc(x, sub);
        if (ispref) {
            l.fl = cat("(*", x, ".FreeHdr)");
            l.flstk = cat(x, ".FreeStk");
        }
        l.viaref = viaref;
        return;
    }
    if (!IsRelRef(l.t) && IsBytesT(sub)) {
        auto x = Ld(l);
        l = GoLoc {};
        l.t = sub;
        l.addr = x;
        l.viaref = viaref;
        return;
    }
    if (IsRelRef(l.t)) {
        auto p = T();
        Decl(p, "unsafe.Pointer", RelLoad(l));
        l = IsBytesT(sub) ? BytesLoc(p, sub, GoLoc {}) : AtAddr(p, sub);
    } else if (RefIsAddr(l.t)) {
        l = AtAddr(Ld(l), sub);
    } else {
        l = AtTyped(Ld(l), sub);
    }
    l.viaref = viaref;
}

// The address a relative reference slot encodes: its own address plus the
// offset it holds, null where an optional one holds 0 (§3.9).
inline string CodeGenGo::RelLoad(const GoLoc &slot) {
    auto off = cat("int64(", slot.lv, ")");
    // A varint width's offset is decoded from the slot's own bytes (§3.6).
    if (slot.t->ref->lenstorage == IS_VARINT)
        off = cat(slot.t->ref->pool ? "GsUlebRead(" : "GsZigRead(", slot.addr, ")");
    if (auto pool = slot.t->ref->pool) {
        // From the pool's base, biased by one so that 0 stays null.
        auto base = cat(gnames.at(pool), ".Base");
        if (!slot.t->ref->optional) return cat("unsafe.Add(", base, ", ", off, "-1)");
        return cat("GsPoolLoad(", base, ", ", off, ")");
    }
    if (!slot.t->ref->optional) return cat("unsafe.Add(", AddrOf(slot), ", ", off, ")");
    return cat("GsRelLoad(", AddrOf(slot), ", ", off, ")");
}

inline CodeGenGo::GoLoc CodeGenGo::GenLoc(Node *n) {
    if (auto it = fillvalues.find(n); it != fillvalues.end()) return it->second;
    if (auto id = Is<Ident>(n); id && id->vdef) {
        GoT(id->vdef->type, n->line);
        return VarLoc(id->vdef, n->line);
    }
    if (auto d = Is<Dot>(n); d && d->IsField()) {
        auto l = GenLoc(d->obj);
        while (l.t->kind == TY_REF) DerefLoc(l);
        return FieldLocAt(l, d->fieldidx, n->line);
    }
    if (auto ix = Is<Index>(n)) {
        auto l = GenLoc(ix->obj);
        return IndexLoc(l, ix->idx, ix->line, ix->nobc);
    }
    // `&path` addressed as a place is the path.
    if (auto u = Is<Unary>(n); u && u->op == T_BITAND &&
        (Is<Ident>(u->child) || Is<Dot>(u->child) || Is<Index>(u->child)) &&
        u->child->exprtype && u->child->exprtype->kind != TY_REF)
        return GenLoc(u->child);
    // Anything else: a temporary holding its value, on a stack of its own
    // where it is no fixed-size value (GenRzTmp, GenPtr).
    auto t = n->exprtype;
    if (IsResz(t)) return GenRzTmp(n);
    if (auto s = Is<StrLit>(n); s && t->kind == TY_ARRAY && t->arr->akind == A_VAR) {
        // A `u8[]` literal is an image in static data: its length prefix,
        // then its bytes (CodeGen::GenStrBytes).
        GoLoc l;
        l.t = t;
        l.addr = StrImage(s->val, LenStore(t->arr));
        return l;
    }
    if (IsBytesT(t)) {
        GoLoc l;
        l.t = t;
        l.addr = BytesTemp(l.stk);
        GenAny(n, GoDst { GD_STACK, l.stk, t });
        return l;
    }
    if (IsRelRef(t)) t = ast.RefTo(t->ref->sub, n->line, t->ref->optional);
    auto x = GenX(n);
    if (IsGoName(x) && !IsGoConst(x) && (Is<Call>(n) || IsCtl(n) || Is<StructLit>(n) ||
                                         Is<ArrayLit>(n) || Is<StrLit>(n) || Is<SliceExpr>(n)))
        return AtLocal(x, t);   // A temporary of the node's own already.
    auto v = T();
    Decl(v, LocalT(t, n->line), x);
    return AtLocal(v, t);
}

// ------------------------------------------------------------------
// What a place reads as.

// The value of the place l as type et: a reference's pointee where et is
// no reference, a relative reference's target as the plain reference, an
// array as a slice of itself, a variant as its ADT, an array or a slice
// as a static-capacity limited array.
inline string CodeGenGo::LoadLoc(GoLoc l, TypeExpr *et, Line ln) {
    if (!et || et->kind == TY_VOID) et = l.t;
    if (IsResz(l.t) && et->kind == TY_REF) return RefOfRz(l, ln);
    if (l.t->kind != TY_REF && IsBytesT(l.t) && !IsResz(l.t) && et->kind == TY_REF) return l.addr;
    if (l.t->kind == TY_REF && et->kind != TY_REF) {
        DerefLoc(l);
        return LoadLoc(l, et, ln);
    }
    if (IsRelRef(l.t)) {
        auto sub = l.t->ref->sub;
        if (IsBytesT(sub) || GoHasPtrs(sub)) return RelLoad(l);
        return cat("(*", GoT(sub, ln), ")(", RelLoad(l), ")");
    }
    if (et->kind == TY_SLICE && l.t->kind == TY_ARRAY) {
        // A whole array where a slice of it is wanted (§3.10).
        auto v = ArrayView(l, ln);
        return cat("GsSlice{", v.data, ", ", v.len, "}");
    }
    if (IsStaticLimited(et) &&
        (l.t->kind == TY_SLICE || (l.t->kind == TY_ARRAY && !TEq(l.t, et))))
        return AdaptToLimited(l, et, ln);
    // A varint reads as the i64 it decodes to; a bytes value is its
    // address, or a fixed-mode value read out of it.
    if (IsVarintT(l.t)) {
        auto x = cat("GsZigRead(", l.addr, ")");
        return IsScalarT(et) ? Coerce(x, ast.inttypes[IS_I64], et, ln) : x;
    }
    if (IsBytesT(l.t) && !IsResz(l.t)) {
        if (IsFix(et) && et->kind == TY_ENUM) return AdaptBytesToEnum(l, et, ln);
        if (IsFix(et) && !TEq(l.t, et))
            Unsupported(ln, cat("reading a ", TypeText(l.t), " as a ", TypeText(et)));
        return l.addr;
    }
    if (et->kind == TY_ENUM && l.t->kind == TY_VARIANT) return AdaptToEnum(l, et, ln);
    // A scalar landing in a varint slot is the i64 the slot encodes (§3.6),
    // whatever integer type it is read at.
    if (IsScalarT(l.t) && (IsScalarT(et) || IsVarintT(et))) return Coerce(l.lv, l.t, et, ln);
    return Ld(l);
}

// A variant or a variable-mode ADT, as a bytes value, read as the fixed-mode
// ADT et (CodeGen::AdaptToFixed): the tag, and the payload of the variant
// it holds.
inline string CodeGenGo::AdaptBytesToEnum(const GoLoc &l, TypeExpr *et, Line ln) {
    auto ei = EIOf(et);
    auto tv = T();
    Decl(tv, LocalT(et, ln));
    auto e = AtVar(tv, et);
    auto &ti = TI(et, ln);
    if (l.t->kind == TY_VARIANT) {
        auto vi = ei->en->VariantIndex(l.t->var->variant);
        L(TagOf(e, ln), " = ", vi);
        if (ti.voffs[vi] >= 0)
            L(PayloadLoc(e, vi, ln).lv, " = *(*", GoT(VariantType(et, vi), ln), ")(", l.addr, ")");
        return tv;
    }
    auto tag = T();
    Decl(tag, IntGoT(TagStore(ei->en)), cat("*(*", IntGoT(TagStore(ei->en)), ")(", l.addr, ")"));
    L(TagOf(e, ln), " = ", ti.rep == GR_TAG ? cat(ti.name, "(", tag, ")") : tag);
    for (size_t vi = 0; vi < ei->en->variants.size(); vi++) {
        if (ti.voffs.empty() || ti.voffs[vi] < 0) continue;
        L("if ", tag, " == ", vi, " {");
        ind++;
        L(PayloadLoc(e, (int)vi, ln).lv, " = *(*", GoT(VariantType(et, (int)vi), ln), ")(",
          AddOff(l.addr, TagSize(ei->en)), ")");
        ind--;
        LS("}");
    }
    return tv;
}

// A variant as its ADT (§3.5): the variant's tag, its value the payload.
inline string CodeGenGo::AdaptToEnum(const GoLoc &l, TypeExpr *et, Line ln) {
    auto ei = EIOf(et);
    auto vi = ei->en->VariantIndex(l.t->var->variant);
    auto tv = T();
    Decl(tv, LocalT(et, ln));
    auto e = AtVar(tv, et);
    L(TagOf(e, ln), " = ", vi);
    if (TI(et, ln).voffs[vi] >= 0) L(PayloadLoc(e, vi, ln).lv, " = ", l.lv);
    return tv;
}

// The elements of the array or slice at l copied into a new static-capacity
// limited array of type et, under its capacity check (§4.2).
inline string CodeGenGo::AdaptToLimited(const GoLoc &l, TypeExpr *et, Line ln) {
    auto tv = T();
    Decl(tv, LocalT(et, ln));
    CopyIntoLimited(l, et, ln, AtVar(tv, et));
    return tv;
}

// The elements at l into the limited array dst: the count and the elements
// are read before anything is written, and they move as memmove moves them,
// so the source may lie in dst's own slots.
inline void CodeGenGo::CopyIntoLimited(const GoLoc &l, TypeExpr *et, Line ln, const GoLoc &dst) {
    auto v = ArrayView(l, ln);
    auto n = T();
    Decl(n, "int64", v.len);
    L("if ", n, " > ", ArrSize(et->arr), " {");
    ind++;
    L("GsAbort(GsECapacity, ", LocArgs(ln), ")");
    ind--;
    LS("}");
    auto dv = ArrayView(dst, ln);
    // Byte for byte: elements holding Go pointers may lie unaligned.
    auto nb = n;
    if (v.esize != 1) {
        nb = T();
        Decl(nb, "int64", cat(n, "*", v.esize));
    }
    L("copy(unsafe.Slice((*byte)(", dv.data, "), ", nb, "), unsafe.Slice((*byte)(", v.data, "), ", nb, "))");
    L(dv.lenlv, " = ", IntGoT(dv.lenst), "(", n, ")");
}

// &lvalue (§3.8): the reference to it, or the reference a place holding one
// holds (a relative one decoded).
inline string CodeGenGo::GenRefVal(Node *child, Line ln) {
    auto l = GenLoc(child);
    if (l.t->kind == TY_REF) return LoadLoc(l, l.t, ln);
    // A reference to a resizable value is its header's address and its
    // stack (C.2), one to a bytes value the value's address.
    if (IsResz(l.t)) return RefOfRz(l, ln);
    return RefOfLoc(l);
}

// x, a value of type `from`, as a value of type `to` (Coerce for the rest of
// the types): a reference that decays to its pointee, or is handed on as
// an optional one, an array as a slice, a variant as its ADT.
inline string CodeGenGo::CoerceVal(const string &x, TypeExpr *from, TypeExpr *to, Line ln) {
    if (!to || !from || to->kind == TY_VOID || from == to) return x;
    // A varint's value is an i64 (§3.6), which Coerce converts to and from.
    if ((IsScalarT(from) || IsVarintT(from)) && (IsScalarT(to) || IsVarintT(to)))
        return Coerce(x, from, to, ln);
    if (IsRelRef(to)) to = ast.RefTo(to->ref->sub, ln, to->ref->optional);
    if (from->kind == TY_REF && to->kind == TY_REF) return x;
    if (from->kind == TY_REF) {
        // The pointee where the reference points: a resizable's through its
        // header, a bytes value at its address.
        auto sub = from->ref->sub;
        if (IsResz(sub)) return LoadLoc(FatRefLoc(x, sub), to, ln);
        if (IsBytesT(sub)) return LoadLoc(BytesLoc(x, sub, GoLoc {}), to, ln);
        return LoadLoc(RefIsAddr(from) ? AtAddr(x, sub) : AtTyped(x, sub), to, ln);
    }
    if (TEq(from, to)) return x;
    auto needsplace = (to->kind == TY_SLICE && from->kind == TY_ARRAY) ||
                      (to->kind == TY_ENUM && from->kind == TY_VARIANT) ||
                      (IsStaticLimited(to) && (from->kind == TY_SLICE || from->kind == TY_ARRAY));
    if (needsplace) {
        auto v = x;
        if (!IsGoName(v)) {
            v = T();
            Decl(v, LocalT(from, ln), x);
        }
        return LoadLoc(AtVar(v, from), to, ln);
    }
    if (GoT(from, ln) == GoT(to, ln)) return x;
    Fail(ln, cat("internal: no conversion from ", TypeText(from), " to ", TypeText(to)));
}

// ------------------------------------------------------------------
// Relative references (§3.9): a slot holds the distance from its own
// address to its target, as a signed integer of its width.

// The range check a store into a slot of rt's width needs: the offset must
// fit the width, which it always does where the root it spans is no wider
// than the width's signed range -- a fixed-size root of at most
// `relrootmax` bytes, and a data stack's reservation -- as the C backend
// decides with GS_STACK_RESERVE, which is a constant here.
inline void CodeGenGo::EmitRelRangeCheck(TypeExpr *rt, const string &off, Line ln) {
    auto bits = IntSize((IntStorage)rt->ref->lenstorage) * 8;
    if (bits >= 64) return;
    auto half = (int64_t)1 << (bits - 1);
    if (relrootmax <= half && (int64_t)opts.stacks.reserve <= half) return;
    L("if ", off, " < -", half, " || ", off, " >= ", half, " {");
    ind++;
    L("GsAbort(GsEReloff, ", LocArgs(ln), ")");
    ind--;
    LS("}");
}

// The plain reference rv stored into the relative slot `slot`. A null
// target of an optional slot stores 0; a non-null one at the slot's own
// address would read back as null, and aborts, unless the slot is a field
// no target can start at (`apart`).
inline void CodeGenGo::EmitRelStore(const GoLoc &slot, const string &rv, bool nonnull, bool apart,
                                    Line ln) {
    auto rt = slot.t;
    if (auto pool = rt->ref->pool) {
        // The `in pool` form: the target's distance from the pool's base,
        // plus one; null is 0. The width spans 2^N - 1 bytes of a pool, and
        // a data stack's reservation bounds the distance (EmitRelRangeCheck).
        auto base = cat(gnames.at(pool), ".Base");
        auto p = T();
        Decl(p, "unsafe.Pointer", cat("unsafe.Pointer(", rv, ")"));
        auto off = T();
        if (rt->ref->optional && !nonnull) Decl(off, "uint64", cat("GsPoolOff(", p, ", ", base, ")"));
        else Decl(off, "uint64", cat("uint64(uintptr(", p, ") - uintptr(", base, ")) + 1"));
        auto bits = IntSize((IntStorage)rt->ref->lenstorage) * 8;
        if (bits < 64 && opts.stacks.reserve >= (1ull << bits)) {
            L("if ", off, " > ", (1ull << bits) - 1, " {");
            ind++;
            L("GsAbort(GsEReloff, ", LocArgs(ln), ")");
            ind--;
            LS("}");
        }
        L(slot.lv, " = ", RelGoT(rt), "(", off, ")");
        return;
    }
    auto fa = T();
    Decl(fa, "unsafe.Pointer", AddrOf(slot));
    auto p = T();
    Decl(p, "unsafe.Pointer", cat("unsafe.Pointer(", rv, ")"));
    auto off = T();
    auto optional = rt->ref->optional;
    if (optional && !nonnull) Decl(off, "int64", cat("GsRelOff(", p, ", ", fa, ")"));
    else Decl(off, "int64", cat("int64(uintptr(", p, ") - uintptr(", fa, "))"));
    if (optional && !apart) {
        L("if ", nonnull ? "" : cat(p, " != nil && "), off, " == 0 {");
        ind++;
        L("GsAbort(GsERelNull, ", LocArgs(ln), ")");
        ind--;
        LS("}");
    }
    EmitRelRangeCheck(rt, off, ln);
    L("*(*", RelGoT(rt), ")(", fa, ") = ", RelGoT(rt), "(", off, ")");
}

// `self` in a literal's field at byte `fieldoff` of the value it builds:
// minus that offset, wherever the value lies.
inline void CodeGenGo::EmitRelSelf(const GoLoc &slot, int64_t fieldoff, Line ln) {
    auto rt = slot.t;
    if (auto pool = rt->ref->pool) {
        // The value's own position in its pool (§3.9), which it is built in.
        auto base = cat(gnames.at(pool), ".Base");
        auto p = T();
        Decl(p, "unsafe.Pointer", cat("unsafe.Add(", AddrOf(slot), ", ", -fieldoff, ")"));
        EmitRelStore(slot, p, true, true, ln);
        (void)base;
        return;
    }
    auto bits = IntSize((IntStorage)rt->ref->lenstorage) * 8;
    if (bits < 64 && fieldoff > ((int64_t)1 << (bits - 1)))
        Fail(ln, cat("self at byte offset ", fieldoff, " does not fit a ",
                     IntStorageName(rt->ref->lenstorage), "-width relative reference"));
    L(slot.lv, " = ", -fieldoff);
}

// Whether a non-optional plain reference is all n can give (NonNullRefValue).
inline bool CodeGenGo::NonNullRef(Node *v) {
    auto nonnull = [](TypeExpr *t) { return t && t->kind == TY_REF && !t->ref->optional; };
    if (!v) return false;
    if (nonnull(v->exprtype)) return true;
    if (auto c = Is<Call>(v)) return !c->rettypes.empty() && nonnull(c->rettypes[0]);
    if (auto b = Is<Block>(v)) return NonNullRef(b->tail);
    if (auto i = Is<IfExpr>(v)) return i->elseb && NonNullRef(i->thenb) && NonNullRef(i->elseb);
    return false;
}

// Whether no target can lie at the address of a self-relative slot that is
// field `fi` of a fixed struct of type `holder` (CodeGen::RelSlotApart).
inline bool CodeGenGo::RelSlotApart(TypeExpr *rt, TypeExpr *holder, int64_t off) {
    return off > 0 && holder && holder->kind == TY_STRUCT && IsFix(holder) &&
           rt->ref->sub->kind != TY_REF;
}

// The node's value stored into the relative slot.
inline void CodeGenGo::GenRelStore(const GoLoc &slot, Node *val, bool apart) {
    if (Is<NullLit>(val)) {
        L(slot.lv, " = 0");
        return;
    }
    auto pt = ast.RefTo(slot.t->ref->sub, val->line, slot.t->ref->optional);
    auto rv = CoerceVal(GenX(val), val->exprtype, pt, val->line);
    EmitRelStore(slot, rv, NonNullRef(val), apart, val->line);
}

// The largest fixed-size value holding relative references: the widest any
// root of a self-relative reference can span outside a data stack.
inline void CodeGenGo::ComputeRelRootMax() {
    for (auto vd : ast.vardefs) {
        if (!vd->type || !IsFix(vd->type) || !HasRelRef(vd->type)) continue;
        relrootmax = std::max(relrootmax, FixedSize(vd->type));
    }
}

}  // namespace goose
