// Goose compiler — the Go backend's aggregate values (definitions of
// CodeGenGo members, codegen_go.h): struct, variant and array literals,
// string literals (§3.7), slices (§3.10), fields and properties, equality
// (§4.5), elementwise arithmetic (§6.1), loops over arrays and slices
// (§6.5), matches over ADTs (§8.1), the array builtins (§3.3) and the
// structural text of print (§3.7), as the C backend builds them
// (codegen_construct.h, codegen_values.h, codegen_builtins.h,
// codegen_render.h).
#pragma once

namespace goose {

// ------------------------------------------------------------------
// Values.

// The value every byte of which is zero, which is the default of every
// fixed-size type (§4.2): Go's zero value of its representation.
inline string CodeGenGo::ZeroOf(TypeExpr *t, Line ln) {
    switch (t->kind) {
        case TY_INT: return GoIntLit(0, false, t, ln);
        case TY_FLT: return GoFltLit(0, t, ln);
        case TY_BOOL: return "false";
        case TY_REF:
            if (t->ref->lenstorage >= 0) return cat(RelGoT(t), "(0)");
            // A fat reference is a struct, null with no header.
            return GoT(t, ln) == "GsRref" ? "GsRref{}" : "nil";
        default: break;
    }
    auto &ti = TI(t, ln);
    if (ti.rep == GR_TAG) return cat(ti.name, "(0)");
    return cat(ti.name, "{}");
}

static int64_t GoLitCount(ArrayLit *al) {
    if (!al->fillval) return (int64_t)al->elems.size();
    return ((IntLit *)al->fillcount)->val;
}

// Whether a node's value is all zero bytes, which a fresh Go value has.
static bool ZeroLeaf(Node *n) {
    if (auto i = Is<IntLit>(n)) return i->val == 0;
    if (auto f = Is<FltLit>(n)) return f->val == 0 && !std::signbit(f->val);
    if (auto b = Is<BoolLit>(n)) return !b->val;
    return Is<NullLit>(n) != nullptr;
}

// A literal built at dst, field by field or element by element, in
// declaration order (§4.2). `fresh`: dst holds a fresh zero value, whose
// omitted optional fields are null already.
inline void CodeGenGo::LitInto(Node *n, const GoLoc &dst, bool fresh) {
    if (auto al = Is<ArrayLit>(n)) ArrayLitInto(al, dst, fresh);
    else StructLitInto((StructLit *)n, dst, fresh);
}

inline void CodeGenGo::StructLitInto(StructLit *sl, const GoLoc &dst, bool fresh) {
    auto et = dst.t;
    auto ln = sl->line;
    auto fieldset = [&](const GoLoc &base, const vector<Field> &fields,
                        const vector<TypeExpr *> &ftypes, const Layout &lo, int64_t baseoff) {
        for (size_t i = 0; i < fields.size(); i++) {
            if (fields[i].ispad) continue;
            if (Unreached()) return;
            auto init = sl->InitFor((int)i);
            auto ft = ftypes[i];
            auto f = FieldLocAt(base, (int)i, ln);
            if (!init) {
                if (!fresh) St(f, ZeroOf(ft, ln));
                continue;
            }
            if (IsRelRef(ft)) {
                if (Is<SelfRef>(init)) EmitRelSelf(f, baseoff + lo.offs[i], ln);
                else GenRelStore(f, init, RelSlotApart(ft, et, baseoff + lo.offs[i]));
                continue;
            }
            if ((Is<StructLit>(init) || Is<ArrayLit>(init)) && HasRelRef(ft)) {
                LitInto(init, f, fresh);
                continue;
            }
            GenInto(init, f, ft);
        }
    };
    if (et->kind == TY_STRUCT) {
        auto si = SI(et);
        fieldset(dst, si->st->fields, si->ftypes, StructLayout(si), 0);
        return;
    }
    if (et->kind == TY_VARIANT) {
        auto ei = EIVar(et);
        auto vi = ei->en->VariantIndex(et->var->variant);
        fieldset(dst, ei->en->variants[vi].fields, ei->vftypes[vi], VariantLayout(ei, vi), 0);
        return;
    }
    if (et->kind != TY_ENUM || !sl->variant) Fail(ln, "internal: a literal of no struct type");
    auto ei = EIOf(et);
    auto vi = ei->en->VariantIndex(sl->variant);
    L(TagOf(dst, ln), " = ", vi);
    if (TI(et, ln).voffs[vi] < 0) return;
    fieldset(PayloadLoc(dst, vi, ln), ei->en->variants[vi].fields, ei->vftypes[vi],
             VariantLayout(ei, vi), TagSize(ei->en));
}

inline void CodeGenGo::ArrayLitInto(ArrayLit *al, const GoLoc &dst, bool fresh) {
    auto et = dst.t;
    auto ln = al->line;
    if (et->kind != TY_ARRAY || (et->arr->akind != A_FIXED && et->arr->akind != A_LIMITED))
        Unsupported(ln, cat("an array literal of type ", TypeText(et)));
    auto elem = et->arr->sub;
    auto count = GoLitCount(al);
    auto v = ArrayView(dst, ln);
    if (et->arr->akind == A_LIMITED) L(v.lenlv, " = ", count);
    auto emit = [&](Node *e, const GoLoc &at) {
        if (IsRelRef(elem)) GenRelStore(at, e, false);
        else if ((Is<StructLit>(e) || Is<ArrayLit>(e)) && HasRelRef(elem)) LitInto(e, at, fresh);
        else GenInto(e, at, elem);
    };
    if (al->fillval) {
        // The value is evaluated once, also for no elements, and repeated
        // (§4.2); a relative link keeps its target and is encoded per slot.
        vector<Node *> added;
        FreezeFill(al->fillval, elem, added);
        if (Unreached()) {
            for (auto n : added) fillvalues.erase(n);
            return;
        }
        auto zero = fresh && ZeroLeaf(al->fillval) && !IsRelRef(elem);
        if (count > 0 && !zero) {
            auto i = T();
            L("for ", i, " := int64(0); ", i, " < ", count, "; ", i, "++ {");
            ind++;
            PushSc(SC_PLAIN);
            emit(al->fillval, ElemAt(v, i, ln));
            PopSc();
            ind--;
            LS("}");
        }
        for (auto n : added) fillvalues.erase(n);
        return;
    }
    for (size_t i = 0; i < al->elems.size(); i++) emit(al->elems[i], ElemAt(v, cat(i), ln));
}

// A fill's value taken once, before its repetitions (CodeGen::FreezeFill): a
// literal holding relative references by its leaves, each a plain reference
// whose offset every slot encodes from where it is.
inline void CodeGenGo::FreezeFill(Node *n, TypeExpr *t, vector<Node *> &added) {
    if (!n || Is<SelfRef>(n) || fillvalues.count(n)) return;
    if (HasRelRefAny(t)) {
        if (auto sl = Is<StructLit>(n)) {
            const vector<TypeExpr *> *ftypes;
            if (t->kind == TY_STRUCT) ftypes = &SI(t)->ftypes;
            else {
                auto ei = t->kind == TY_VARIANT ? EIVar(t) : EIOf(t);
                ftypes = &ei->vftypes[ei->en->VariantIndex(t->kind == TY_VARIANT ? t->var->variant
                                                                                 : sl->variant)];
            }
            for (size_t i = 0; i < sl->inits.size(); i++)
                FreezeFill(sl->inits[i].val, (*ftypes)[sl->fieldindices[i]], added);
            return;
        }
        if (auto al = Is<ArrayLit>(n); al && t->kind == TY_ARRAY) {
            if (al->fillval) FreezeFill(al->fillval, t->arr->sub, added);
            else for (auto e : al->elems) FreezeFill(e, t->arr->sub, added);
            return;
        }
    }
    if (IsVarintT(t)) t = ast.inttypes[IS_I64];
    if (IsRelRef(t)) t = ast.RefTo(t->ref->sub, n->line, t->ref->optional);
    if (Is<NullLit>(n)) return;
    // A value that is not fixed-size is held on a temporary stack of its own
    // (CodeGen::FreezeFill).
    if (IsResz(t)) {
        string stk;
        auto h = RzTemp(t, stk);
        GenAny(n, GoDst { GD_STACK, stk, t, RzLenLv(t, h) });
        fillvalues.emplace(n, RzTempLoc(t, h, stk));
        added.push_back(n);
        return;
    }
    if (IsBytesT(t)) {
        GoLoc l;
        l.t = t;
        l.addr = BytesTemp(l.stk);
        GenConstruct(n, l.stk, t);
        fillvalues.emplace(n, l);
        added.push_back(n);
        return;
    }
    auto x = CoerceVal(GenX(n), n->exprtype, t, n->line);
    auto v = T();
    Decl(v, LocalT(t, n->line), x);
    fillvalues.emplace(n, AtVar(v, t));
    added.push_back(n);
}

// A literal's value in a temporary of its own type, which the destination
// copies (a literal holding relative references is built where it stays
// instead: LeafAny, BindLocal).
inline string CodeGenGo::GenLitVal(Node *n) {
    auto t = n->exprtype;
    if (t->kind == TY_SLICE) {
        // A literal in a slice's place: a temporary fixed array, sliced whole
        // (§3.10).
        auto al = Is<ArrayLit>(n);
        if (!al) Fail(n->line, "internal: a struct literal of a slice type");
        auto at = ast.ArrayOf(t->sub, A_FIXED, n->line, GoLitCount(al));
        auto save = al->exprtype;
        al->exprtype = at;
        auto a = GenLitVal(al);
        al->exprtype = save;
        return LoadLoc(AtVar(a, at), t, n->line);
    }
    // A large value's temporary is on a data stack, as C's is, zeroed first.
    auto v = FixedLocal(t, T(), "", false, n->line);
    LitInto(n, AtLocal(v, t), true);
    return v;
}

// A string literal (§3.7): a slice of its bytes in the program's static data,
// read-only, or those bytes copied into a fixed or limited u8 array.
inline string CodeGenGo::GenStrLit(StrLit *s) {
    auto t = s->exprtype;
    if (t->kind == TY_SLICE) return cat("GsStr(", GoStr(s->val), ")");
    if (t->kind != TY_ARRAY || (t->arr->akind != A_FIXED && t->arr->akind != A_LIMITED))
        Unsupported(s->line, cat("a string literal of type ", TypeText(t)));
    auto v = T();
    Decl(v, LocalT(t, s->line));
    StrLitInto(s, AtVar(v, t));
    return v;
}

// A string literal's bytes into the fixed or limited u8 array at dst.
inline void CodeGenGo::StrLitInto(StrLit *s, const GoLoc &dst) {
    auto view = ArrayView(dst, s->line);
    if (dst.t->kind == TY_ARRAY && dst.t->arr->akind == A_LIMITED) L(view.lenlv, " = ", s->val.size());
    if (!s->val.empty())
        L("copy(unsafe.Slice((*byte)(", view.data, "), ", s->val.size(), "), ", GoStr(s->val), ")");
}

// a[lo..hi] (§3.10): the bounds checked against the length, read once
// with the elements, unless BCE proved them in range.
inline string CodeGenGo::GenSlice(SliceExpr *se) {
    auto l = GenLoc(se->obj);
    while (l.t->kind == TY_REF) DerefLoc(l);
    auto v = ArrayView(l, se->line);
    auto data = T(), n = T();
    Decl(data, "unsafe.Pointer", v.data);
    Decl(n, "int64", v.len);
    auto bound = [&](Node *b, bool fromend, const string &dflt) {
        if (!b) return dflt;
        auto x = IndexValue(b);
        return fromend ? cat("(", n, " - ", x, ")") : x;
    };
    auto lo = T(), hi = T();
    Decl(lo, "int64", bound(se->lo, se->lo_from_end, "0"));
    Decl(hi, "int64", bound(se->hi, se->hi_from_end, n));
    if (!se->nobc) {
        L("if ", lo, " < 0 || ", hi, " < ", lo, " || ", hi, " > ", n, " {");
        ind++;
        L("GsAbort(GsESlice, ", LocArgs(se->line), ")");
        ind--;
        LS("}");
    }
    auto s = cat("GsSub(", data, ", ", lo, ", ", hi, ", ", n, ", ", v.esize, ")");
    if (IsStaticLimited(se->exprtype)) {
        auto st = ast.SliceOf(v.elem, se->line);
        auto sv = T();
        Decl(sv, "GsSlice", s);
        return AdaptToLimited(AtVar(sv, st), se->exprtype, se->line);
    }
    return s;
}

// A field, a property (.len, .cap) or a payload-less variant (§3.5).
inline string CodeGenGo::GenDot(Dot *d) {
    if (d->variantconst) {
        auto et = d->exprtype;
        auto vi = d->einst->en->VariantIndex(d->variantconst);
        if (et->kind == TY_VARIANT) return ZeroOf(et, d->line);
        if (et->kind != TY_ENUM) Fail(d->line, "internal: a variant constant of no ADT type");
        auto &ti = TI(et, d->line);
        if (ti.rep == GR_TAG) return cat(ti.name, "(", vi, ")");
        auto v = T();
        Decl(v, ti.name);
        L(TagOf(AtVar(v, et), d->line), " = ", vi);
        return v;
    }
    if (d->member >= 0) {
        auto l = GenLoc(d->obj);
        while (l.t->kind == TY_REF) DerefLoc(l);
        auto i64 = ast.inttypes[IS_I64];
        if (l.t->kind == TY_ARRAY && d->member == B_CAP) {
            // A runtime capacity is read where the array stores it.
            if (ArrSize(l.t->arr) < 0) return Coerce(ArrayView(l, d->line).cap, i64, d->exprtype, d->line);
            return Coerce(GoIntLit(ArrSize(l.t->arr), false, i64, d->line), i64, d->exprtype, d->line);
        }
        auto v = ArrayView(l, d->line);
        auto len = v.fixed >= 0 ? GoIntLit(v.fixed, false, i64, d->line) : v.len;
        return Coerce(len, i64, d->exprtype, d->line);
    }
    return LoadLoc(GenLoc(d), d->exprtype, d->line);
}

// ------------------------------------------------------------------
// Equality (§4.5) of two direct operands: a slice compares its length,
// then its elements; anything else structurally (EqX), its references and
// slices as themselves.

inline string CodeGenGo::GenEquality(Binary *b) {
    auto lt = b->left->exprtype;
    if (lt && IsPlainRef(lt)) lt = lt->ref->sub;
    if (lt && IsBytesT(lt) && !IsResz(lt)) {
        // Bytes values compare through their images (EqFnBytes), the left
        // one's address taken before the right operand runs.
        auto la = GenLoc(b->left);
        while (la.t->kind == TY_REF) DerefLoc(la);
        auto lp = T();
        Decl(lp, "unsafe.Pointer", la.addr);
        auto ra = GenLoc(b->right);
        while (ra.t->kind == TY_REF) DerefLoc(ra);
        auto eq = cat(EqFnBytes(lt, b->line), "(", lp, ", ", ra.addr, ")");
        return b->op == T_EQ ? eq : cat("(!", eq, ")");
    }
    if (lt && IsResz(lt)) {
        // Resizable arrays compare as element ranges through their views,
        // the left one read before the right operand runs.
        if (lt->kind != TY_ARRAY) Unsupported(b->line, "comparing resizable structs");
        auto la = GenLoc(b->left);
        while (la.t->kind == TY_REF) DerefLoc(la);
        auto lv = ArrayView(la, b->line);
        auto ld = T(), ln = T();
        Decl(ld, "unsafe.Pointer", lv.data);
        Decl(ln, "int64", lv.len);
        auto ra = GenLoc(b->right);
        while (ra.t->kind == TY_REF) DerefLoc(ra);
        auto rv = ArrayView(ra, b->line);
        auto eq = RangeEq(lv.elem, ld, ln, rv.data, rv.fixed >= 0 ? cat(rv.fixed) : rv.len, b->line);
        return b->op == T_EQ ? eq : cat("(!", eq, ")");
    }
    auto later = HasStmts(b->right);
    auto l = CoerceVal(GenX(b->left), b->left->exprtype, lt, b->line);
    if (later || !IsGoName(l) || IsGoConst(l)) l = Snapshot(lt, l);
    auto r = CoerceVal(GenX(b->right), b->right->exprtype, lt, b->line);
    if (lt->kind == TY_SLICE || !IsGoName(r) || IsGoConst(r)) r = Snapshot(lt, r);
    string eq;
    if (lt->kind == TY_SLICE) {
        auto ev = ArrayView(AtVar(l, lt), b->line), rv = ArrayView(AtVar(r, lt), b->line);
        eq = RangeEq(lt->sub, ev.data, ev.len, rv.data, rv.len, b->line);
    } else {
        eq = EqX(lt, AtVar(l, lt), AtVar(r, lt), b->line);
    }
    return b->op == T_EQ ? eq : cat("(!", eq, ")");
}

// Two runs of elements equal, length first (CodeGen::GenRangeEq): byte by
// byte where the bytes of equal elements are equal, element by element
// otherwise.
inline string CodeGenGo::RangeEq(TypeExpr *elem, const string &ad, const string &an,
                                 const string &bd, const string &bn, Line ln) {
    auto &ti = TI(elem, ln);
    auto t = T();
    Decl(t, "bool", cat(an, " == ", bn));
    L("if ", t, " {");
    ind++;
    if (!IsFix(elem)) {
        // Variable-size elements are walked, a cursor on each side.
        auto pa = T(), pb = T(), i = T();
        Decl(pa, "unsafe.Pointer", ad);
        Decl(pb, "unsafe.Pointer", bd);
        L("for ", i, " := int64(0); ", t, " && ", i, " < ", an, "; ", i, "++ {");
        ind++;
        L(t, " = ", EqFnBytes(elem, ln), "(", pa, ", ", pb, ")");
        L(pa, " = unsafe.Add(", pa, ", ", SizeFn(elem), "(", pa, "))");
        L(pb, " = unsafe.Add(", pb, ", ", SizeFn(elem), "(", pb, "))");
        ind--;
        LS("}");
    } else if ((IsScalarT(elem) && elem->kind != TY_FLT) || (ti.exact && BitwiseEq(elem) && !ti.ptrs)) {
        L(t, " = GsMemEq(", ad, ", ", bd, ", ", an, "*", ti.size, ")");
    } else {
        auto i = T();
        L("for ", i, " := int64(0); ", t, " && ", i, " < ", an, "; ", i, "++ {");
        ind++;
        GoView va, vb;
        va.data = ad;
        vb.data = bd;
        va.elem = vb.elem = elem;
        va.esize = vb.esize = ti.size;
        L(t, " = ", EqX(elem, ElemAt(va, i, ln), ElemAt(vb, i, ln), ln));
        ind--;
        LS("}");
    }
    ind--;
    LS("}");
    return t;
}

// ------------------------------------------------------------------
// Elementwise arithmetic on structs and fixed arrays of numbers (§6.1):
// member by member, each member of the result from the same member of each
// operand, so that the destination may be one of them. A scalar operand
// meets every member.

inline void CodeGenGo::ElemwiseInto(TypeExpr *t, TType op, Line ln, const string &l, bool lscalar,
                                    const string &r, bool rscalar, const GoLoc &dst) {
    function<void(TypeExpr *, const GoLoc &, const GoLoc &, const GoLoc &)> rec =
        [&](TypeExpr *tt, const GoLoc &d, const GoLoc &a, const GoLoc &b) {
            switch (tt->kind) {
                case TY_INT: case TY_FLT: {
                    auto x = lscalar ? l : a.lv, y = rscalar ? r : b.lv;
                    L(d.lv, " = ", ArithX(op, tt, x, y, ln));
                    return;
                }
                case TY_STRUCT: {
                    auto si = SI(tt);
                    for (size_t i = 0; i < si->st->fields.size(); i++) {
                        if (si->st->fields[i].ispad) continue;
                        rec(si->ftypes[i], FieldLocAt(d, (int)i, ln),
                            lscalar ? a : FieldLocAt(a, (int)i, ln),
                            rscalar ? b : FieldLocAt(b, (int)i, ln));
                    }
                    return;
                }
                case TY_ARRAY: {
                    auto i = T();
                    L("for ", i, " := int64(0); ", i, " < ", ArrSize(tt->arr), "; ", i, "++ {");
                    ind++;
                    auto dv = ArrayView(d, ln);
                    auto av = lscalar ? GoView {} : ArrayView(a, ln);
                    auto bv = rscalar ? GoView {} : ArrayView(b, ln);
                    rec(tt->arr->sub, ElemAt(dv, i, ln), lscalar ? a : ElemAt(av, i, ln),
                        rscalar ? b : ElemAt(bv, i, ln));
                    ind--;
                    LS("}");
                    return;
                }
                default: Fail(ln, cat("internal: elementwise arithmetic on ", TypeText(tt)));
            }
        };
    rec(t, dst, AtLocal(l, t), AtLocal(r, t));
}

inline void CodeGenGo::ElemwiseNegInto(TypeExpr *t, Line ln, const string &x, const GoLoc &dst) {
    function<void(TypeExpr *, const GoLoc &, const GoLoc &)> rec = [&](TypeExpr *tt, const GoLoc &d,
                                                                      const GoLoc &a) {
        switch (tt->kind) {
            case TY_INT: {
                if (debug && !IsUnsigned(tt->intstorage)) {
                    auto name = string(IntStorageName(tt->intstorage));
                    name[0] = (char)toupper(name[0]);
                    L(d.lv, " = GsNeg", name, "(", a.lv, ", ", LocArgs(ln), ")");
                } else {
                    L(d.lv, " = -", a.lv);
                }
                return;
            }
            case TY_FLT: L(d.lv, " = -", a.lv); return;
            case TY_STRUCT: {
                auto si = SI(tt);
                for (size_t i = 0; i < si->st->fields.size(); i++)
                    if (!si->st->fields[i].ispad)
                        rec(si->ftypes[i], FieldLocAt(d, (int)i, ln), FieldLocAt(a, (int)i, ln));
                return;
            }
            case TY_ARRAY: {
                auto i = T();
                L("for ", i, " := int64(0); ", i, " < ", ArrSize(tt->arr), "; ", i, "++ {");
                ind++;
                rec(tt->arr->sub, ElemAt(ArrayView(d, ln), i, ln), ElemAt(ArrayView(a, ln), i, ln));
                ind--;
                LS("}");
                return;
            }
            default: Fail(ln, cat("internal: elementwise negation of ", TypeText(tt)));
        }
    };
    rec(t, dst, AtLocal(x, t));
}

// The operands of an elementwise operation, each in a variable: a scalar
// can meet any member of the destination, which may be an operand, and the
// left one is read before the right one runs (§2).
inline void CodeGenGo::ElemwiseOperands(Binary *b, string &l, bool &ls, string &r, bool &rs) {
    auto et = b->exprtype;
    auto lt = b->left->exprtype, rt = b->right->exprtype;
    if (IsPlainRef(lt)) lt = lt->ref->sub;
    if (IsPlainRef(rt)) rt = rt->ref->sub;
    ls = lt->kind != et->kind;
    rs = rt->kind != et->kind;
    l = Snapshot(lt, CoerceVal(GenX(b->left), b->left->exprtype, lt, b->line));
    r = Snapshot(rt, CoerceVal(GenX(b->right), b->right->exprtype, rt, b->line));
}

// ------------------------------------------------------------------
// for x in a (§6.5): over a fixed or limited array or a slice, by value or,
// with &x, by reference. The length is read again each iteration, since
// pushing into a limited array while iterating it is legal, and the
// elements are reached through the place they lie in, which a body that
// re-points a slice moves (CodeGen's ForLoop::CgStmt); BCE's fixedlen
// reads the length once.

inline void CodeGenGo::GenForArray(ForLoop *f) {
    auto l = GenLoc(f->iter);
    while (l.t->kind == TY_REF) DerefLoc(l);
    auto v = ArrayView(l, f->line);
    auto len = v.fixed >= 0 ? cat(v.fixed) : v.len;
    if (f->fixedlen && v.fixed < 0) len = Snapshot(ast.inttypes[IS_I64], len);
    if (Unreached()) return GenUnreached([&] { GenForArray(f); });
    auto et = f->vdef->type;
    GoT(et, f->line);
    auto held = v.elem->kind == TY_REF;
    auto typedix = f->idxdef && !SameGoT(f->idxdef->type, ast.inttypes[IS_I64]);
    // A body that re-points a slice on the path, or rebinds a reference on
    // it, moves the walk to the elements the path leads to now, at the next
    // index, as the length read again does (ForLoop::CgStmt).
    auto moves = !f->fixedlen && (l.t->kind == TY_SLICE || l.viaref);
    // Variable-size elements are walked, a cursor advancing past each one in
    // the loop's post statement, so that a continue moves it on too; such an
    // element binds by reference only.
    string cur, at;
    if (!IsFix(v.elem)) {
        cur = T();
        Decl(cur, "unsafe.Pointer", v.data);
        if (moves) {
            at = T();
            Decl(at, "unsafe.Pointer", cur);
        }
    }
    OpenLabeled(SC_LOOP);
    // The index is the loop's own counter, and its binder a copy declared in
    // the body, as every local is (Decl): one whose address the body takes
    // then lives in the function's frame (FinishBody), where a header's
    // would be on Go's moving stack.
    auto gi = T();
    LabelHere();
    if (!cur.empty())
        L("for ", gi, " := int64(0); ", gi, " < ", len, "; ", gi, ", ", cur, " = ", gi, "+1, unsafe.Add(", cur,
          ", ", SizeFn(v.elem), "(", cur, ")) {");
    else
        L("for ", gi, " := int64(0); ", gi, " < ", len, "; ", gi, "++ {");
    ind++;
    if (f->idxdef) {
        auto it = f->idxdef->type;
        Decl(LocalName(f->idxdef), LocalT(it, f->line),
             typedix ? Coerce(gi, ast.inttypes[IS_I64], it, f->line) : gi);
    }
    if (!cur.empty()) {
        if (moves) {
            // Where the elements moved, the cursor walks from their start
            // again.
            auto e = T();
            Decl(e, "unsafe.Pointer", ArrayView(l, f->line).data);
            L("if ", e, " != ", at, " {");
            ind++;
            L(at, " = ", e);
            L(cur, " = ", e);
            auto k = T();
            L("for ", k, " := int64(0); ", k, " < ", gi, "; ", k, "++ {");
            ind++;
            L(cur, " = unsafe.Add(", cur, ", ", SizeFn(v.elem), "(", cur, "))");
            ind--;
            LS("}");
            ind--;
            LS("}");
        }
        auto e = BytesLoc(cur, v.elem, GoLoc {});
        e.viaref = l.viaref || held;
        auto iv = LocalName(f->vdef);
        if (held) Decl(iv, LocalT(et, f->line), LoadLoc(e, et, f->line));
        else if (!f->vdef->copybind) Decl(iv, "unsafe.Pointer", cur);
        else Fail(f->line, "internal: a variable-size element bound by value");
        GenLoopBody(f->body);
        ind--;
        LS("}");
        auto s = PopLabeled();
        PlaceLabel(s);
        dead = false;
        return;
    }
    // The start of the elements behind a varint length prefix is a
    // statement's, which runs again where they can move.
    auto e = ElemAt(moves ? ArrayView(l, f->line) : v, gi, f->line);
    e.viaref = l.viaref || held;
    auto iv = LocalName(f->vdef);
    if (!f->vdef->copybind && !held) Decl(iv, LocalT(et, f->line), RefOfLoc(e));
    else Decl(iv, LocalT(et, f->line), LoadLoc(e, et, f->line));
    GenLoopBody(f->body);
    ind--;
    LS("}");
    auto s = PopLabeled();
    PlaceLabel(s);
    dead = false;
}

// ------------------------------------------------------------------
// match over a fixed-mode ADT, or a reference to one (§8.1): a chain of
// ifs on its tag, read once, in arm order. A binder is a copy of its
// variant's payload (fixed mode binds by value only). The variants cover
// every tag; a debug build checks the tag where no arm matches it, as the
// C runtime does (GS_E_TAG).

inline void CodeGenGo::GenEnumMatch(MatchExpr *m, TypeExpr *enumtype, bool isref, const GoDst &d) {
    auto ln = m->line;
    auto ei = EIOf(enumtype);
    auto &ti = TI(enumtype, ln);
    auto varmode = IsBytesT(enumtype);
    GoLoc e;
    if (varmode) {
        // A variable-mode ADT is its image: its tag, then the payload,
        // which a binder takes by reference or copies (§8.1).
        auto l = GenLoc(m->scrutinee);
        while (l.t->kind == TY_REF) DerefLoc(l);
        auto p = T();
        Decl(p, "unsafe.Pointer", l.addr);
        e = BytesLoc(p, enumtype, GoLoc {});
    } else if (isref) {
        e = AtRef(GenPure(m->scrutinee), m->scrutinee->exprtype);
    } else {
        auto sv = CoerceVal(GenX(m->scrutinee), m->scrutinee->exprtype, enumtype, ln);
        if (!IsGoName(sv)) sv = Snapshot(enumtype, sv);
        e = AtVar(sv, enumtype);
    }
    if (Unreached()) return GenUnreached([&] { GenEnumMatch(m, enumtype, isref, d); });
    auto tag = T();
    Decl(tag, ti.rep == GR_TAG ? ti.name : IntGoT(TagStore(ei->en)), TagOf(e, ln));
    auto first = true, alldead = true, wild = false;
    for (size_t ai = 0; ai < m->arms.size(); ai++) {
        auto &arm = m->arms[ai];
        string open;
        auto last = ai + 1 == m->arms.size();
        if (arm.pat.kind == P_WILDCARD || (last && !debug)) {
            open = first ? "{" : "} else {";
            wild = true;
        } else {
            string c;
            for (auto v : arm.variants) {
                if (!c.empty()) c += " || ";
                Append(c, tag, " == ", ei->en->VariantIndex(v));
            }
            open = cat(first ? "" : "} else ", "if ", c, " {");
        }
        if (first) L(open);
        else LS(open);
        dead = false;
        first = false;
        ind++;
        PushSc(SC_PLAIN);
        if (arm.binder && varmode) {
            auto vi = ei->en->VariantIndex(arm.variants[0]);
            auto vt = VariantType(enumtype, vi);
            auto pl = PayloadLoc(e, vi, ln);
            if (arm.pat.byref) {
                auto bn = LocalName(arm.binder);
                if (IsBytesT(vt)) Decl(bn, "unsafe.Pointer", pl.addr);
                else Decl(bn, cat("*", GoT(vt, ln)), cat("(*", GoT(vt, ln), ")(", AddrOf(pl), ")"));
            } else if (IsBytesT(vt)) {
                // A copy of a variable payload, on a stack of its own.
                auto bn = LocalName(arm.binder);
                auto stk = AllocStk(true);
                Decl(bn, "unsafe.Pointer", Top(stk));
                SaveBase(true, stk, bn);
                vstk[arm.binder] = stk;
                auto sz = T();
                Decl(sz, "int64", SizeX(vt, pl.addr));
                L("copy(unsafe.Slice((*byte)(", Top(stk), "), ", sz, "), unsafe.Slice((*byte)(", pl.addr, "), ",
                  sz, "))");
                Bump(stk, sz);
            } else if (EmptyLayout(arm.variants[0]->fields)) {
                Decl(LocalName(arm.binder), LocalT(vt, ln));
            } else {
                Decl(LocalName(arm.binder), LocalT(vt, ln), pl.lv);
            }
        } else if (arm.binder) {
            auto vi = ei->en->VariantIndex(arm.variants[0]);
            auto vt = VariantType(enumtype, vi);
            auto bn = LocalName(arm.binder);
            if (ti.voffs.empty() || ti.voffs[vi] < 0) Decl(bn, LocalT(vt, ln));
            else Decl(bn, LocalT(vt, ln), PayloadLoc(e, vi, ln).lv);
        }
        GenAny(arm.body, d);
        PopSc();
        ind--;
        alldead = alldead && dead;
        if (wild) break;
    }
    if (first) return;
    if (!wild) {
        // A debug build's check of a tag no variant has (GS_UNREACHABLE).
        LS("} else {");
        ind++;
        LS("panic(GsTagError(", LocArgs(ln), "))");
        ind--;
        wild = true;
    }
    LS("}");
    dead = alldead && wild;
}

// ------------------------------------------------------------------
// The array builtins on static-capacity limited arrays (§3.3, §5.3), and
// index_of on any array.

// A member operation's receiver, dereferenced; one reached through a
// reference that its arguments may rebind is resolved first.
inline CodeGenGo::GoLoc CodeGenGo::RecvLoc(Node *n) {
    auto l = GenLoc(n);
    while (l.t->kind == TY_REF) DerefLoc(l);
    if (l.viaref) l = PinLoc(l);
    return l;
}

inline void CodeGenGo::CheckLimited(const GoLoc &l, Line ln) {
    if (l.t->kind != TY_ARRAY || l.t->arr->akind != A_LIMITED)
        Unsupported(ln, cat("this operation on ", TypeText(l.t)));
}

// A pushed element's value, evaluated before its slot is claimed (§2): a
// relative reference's target as the plain reference, nothing for an
// element holding relative references, which is built in its slot.
inline string CodeGenGo::ElemValue(Node *n, TypeExpr *elem) {
    if (IsRelRef(elem)) {
        if (Is<NullLit>(n)) return "nil";
        auto pt = ast.RefTo(elem->ref->sub, n->line, elem->ref->optional);
        return Snapshot(pt, CoerceVal(GenX(n), n->exprtype, pt, n->line));
    }
    // A variable-size element is built in its slot, as one holding relative
    // references is.
    if (!IsFix(elem) || HasRelRef(elem)) return "";
    return Snapshot(elem, CoerceVal(GenX(n), n->exprtype, elem, n->line));
}

inline void CodeGenGo::StoreElem(Node *n, const GoLoc &slot, const string &ev) {
    if (IsRelRef(slot.t)) {
        if (ev == "nil") L(slot.lv, " = 0");
        else EmitRelStore(slot, ev, NonNullRef(n), false, n->line);
    } else if (ev.empty()) {
        if (Is<StructLit>(n) || Is<ArrayLit>(n)) LitInto(n, slot, false);
        else GenInto(n, slot);
    } else {
        St(slot, ev);
    }
}

// What push returns for the element just added: a reference to it, or the
// reference an element that is one holds (there are no references to
// references, §3.8).
inline string CodeGenGo::NewElemResult(const GoLoc &slot, Line ln) {
    if (IsBytesT(slot.t) && slot.t->kind != TY_REF) return slot.addr;
    if (slot.t->kind != TY_REF) return RefOfLoc(slot);
    return Snapshot(ast.RefTo(slot.t->ref->sub, ln, slot.t->ref->optional),
                    LoadLoc(slot, slot.t, ln));
}

inline string CodeGenGo::GenArrayBuiltin(Call *c) {
    auto an = c->ArgNodes();
    auto ln = c->line;
    auto i64 = ast.inttypes[IS_I64];
    // A resizable receiver grows and shrinks at its stack's top
    // (codegen_go_rz.h).
    auto recv = an[0]->exprtype;
    while (recv && recv->kind == TY_REF) recv = recv->ref->sub;
    if (c->builtin == B_RANGE_INDEX) {
        // The index of the slice's first element in the array (CodeGen's):
        // 0 for an empty one, which may come from anywhere. One the checker
        // did not prove the array's own (Call::poolcheck) must start a whole
        // number of elements into it and end inside its length, or abort,
        // with GS_E_ARRAYSLICE or the message given; and where elements have
        // no bytes, every slice starts at 0 and must fit the length.
        auto l = recv && IsResz(recv) ? RzRecvLoc(an[0]) : RecvLoc(an[0]);
        auto v = ArrayView(l, ln);
        auto st = ast.SliceOf(v.elem, ln);
        auto sv = Snapshot(st, CoerceVal(GenX(an[1]), an[1]->exprtype, st, ln));
        // Elements of no bytes in C's layout are each at 0 (FixedSize), what
        // the Go array of them spaces out.
        auto esz = FixedSize(v.elem) ? v.esize : 0;
        auto fail = FailCall("GsEArraySlice", an.size() > 2 ? an[2] : nullptr, ln);
        auto len = v.fixed >= 0 ? cat(v.fixed) : v.len;
        auto i = T();
        Decl(i, "int64", "0");
        if (!esz) {
            L("if uint64(", sv, ".Len) > uint64(", len, ") {");
            ind++;
            L(fail);
            ind--;
            LS("}");
        } else if (!c->poolcheck) {
            L("if ", sv, ".Len != 0 {");
            ind++;
            L(i, " = int64(uintptr(", sv, ".Data)-uintptr(", v.data, ")) / ", esz);
            ind--;
            LS("}");
        } else {
            L("if ", sv, ".Len != 0 {");
            ind++;
            auto off = T();
            Decl(off, "uint64", cat("uint64(uintptr(", sv, ".Data) - uintptr(", v.data, "))"));
            L("if ", off, "%", esz, " != 0 || ", off, "/", esz, " >= uint64(", len, ") || uint64(", sv, ".Len) > uint64(",
              len, ")-", off, "/", esz, " {");
            ind++;
            L(fail);
            ind--;
            LS("}");
            L(i, " = int64(", off, " / ", esz, ")");
            ind--;
            LS("}");
        }
        return i;
    }
    if (recv && IsResz(recv) && c->builtin != B_LEN && c->builtin != B_CAP) return GenRzArrayBuiltin(c);
    switch ((BuiltinKind)c->builtin) {
        case B_LEN: case B_CAP: {
            auto l = GenLoc(an[0]);
            while (l.t->kind == TY_REF) DerefLoc(l);
            if (c->builtin == B_CAP) {
                CheckLimited(l, ln);
                if (ArrSize(l.t->arr) < 0) return ArrayView(l, ln).cap;
                return GoIntLit(ArrSize(l.t->arr), false, i64, ln);
            }
            auto v = ArrayView(l, ln);
            return v.fixed >= 0 ? GoIntLit(v.fixed, false, i64, ln) : v.len;
        }
        case B_INDEX_OF: {
            // The checker proved the reference is an element of this very
            // array (§3.3): the distance is a whole number of elements.
            auto l = RecvLoc(an[0]);
            auto v = ArrayView(l, ln);
            auto r = CoerceVal(GenX(an[1]), an[1]->exprtype,
                               ast.RefTo(v.elem, ln, false), ln);
            return cat("(int64(uintptr(unsafe.Pointer(", r, ")) - uintptr(", v.data, ")) / ", v.esize,
                       ")");
        }
        case B_PUSH: {
            auto l = RecvLoc(an[0]);
            CheckLimited(l, ln);
            auto v = ArrayView(l, ln);
            auto ev = ElemValue(an[1], v.elem);
            auto n = T();
            Decl(n, "int64", v.len);
            L("if ", n, " >= ", v.cap, " {");
            ind++;
            L("GsAbort(GsECapacity, ", LocArgs(ln), ")");
            ind--;
            LS("}");
            auto slot = PinLoc(ElemAt(v, n, ln));
            StoreElem(an[1], slot, ev);
            L(v.lenlv, " = ", IntGoT(v.lenst), "(", n, " + 1)");
            return NewElemResult(slot, ln);
        }
        case B_APPEND: {
            auto l = RecvLoc(an[0]);
            CheckLimited(l, ln);
            auto v = ArrayView(l, ln);
            auto src = an[1];
            if (auto cc = Is<Call>(src); cc && cc->builtin == B_COPY) src = cc->FirstArg();
            if (auto al = Is<ArrayLit>(src); al && HasRelRef(v.elem)) {
                // Built where its elements stay, each link measured from its
                // own slot (§4.3).
                auto count = GoLitCount(al);
                auto ol = T();
                Decl(ol, "int64", v.len);
                L("if ", ol, " + ", count, " > ", v.cap, " {");
                ind++;
                L("GsAbort(GsECapacity, ", LocArgs(ln), ")");
                ind--;
                LS("}");
                auto rt = ast.ArrayOf(v.elem, A_FIXED, ln, count);
                auto at = AtAddr(ElemAddr(v, ol), rt);
                ArrayLitInto(al, at, false);
                L(v.lenlv, " = ", IntGoT(v.lenst), "(", ol, " + ", count, ")");
                return "";
            }
            auto sl = GenLoc(src);
            while (sl.t->kind == TY_REF) DerefLoc(sl);
            auto sv = ArrayView(sl, ln);
            auto sd = T(), nn = T();
            Decl(sd, "unsafe.Pointer", sv.data);
            Decl(nn, "int64", sv.len);
            auto ol = T();
            Decl(ol, "int64", v.len);
            L("if ", ol, " + ", nn, " > ", v.cap, " {");
            ind++;
            L("GsAbort(GsECapacity, ", LocArgs(ln), ")");
            ind--;
            LS("}");
            CopyElemBytes(ElemAddr(v, ol), sd, nn, v.esize);
            L(v.lenlv, " = ", IntGoT(v.lenst), "(", ol, " + ", nn, ")");
            return "";
        }
        case B_POP: {
            auto l = RecvLoc(an[0]);
            CheckLimited(l, ln);
            auto v = ArrayView(l, ln);
            auto nl = T();
            Decl(nl, "int64", cat(v.len, " - 1"));
            // An array BCE proved not empty here has no check (Call::nobc).
            if (!c->nobc) {
                L("if ", nl, " < 0 {");
                ind++;
                L("GsAbort(GsEPop, ", LocArgs(ln), ")");
                ind--;
                LS("}");
            }
            L(v.lenlv, " = ", IntGoT(v.lenst), "(", nl, ")");
            // Read from the slot it leaves, which nothing has written over
            // yet: a relative reference's offset is measured from there.
            auto slot = ElemAt(v, nl, ln);
            auto rt = c->rettypes.empty() ? v.elem : c->rettypes[0];
            if (IsRelRef(v.elem)) return Snapshot(rt, LoadLoc(slot, rt, ln));
            return Snapshot(v.elem, Ld(slot));
        }
        case B_POP_N: {
            auto l = RecvLoc(an[0]);
            CheckLimited(l, ln);
            auto v = ArrayView(l, ln);
            auto nn = Snapshot(i64, Coerce(GenX(an[1]), an[1]->exprtype, i64, ln));
            auto ol = T();
            Decl(ol, "int64", v.len);
            L("if uint64(", nn, ") > uint64(", ol, ") {");
            ind++;
            L("GsAbort(GsEPopN, ", LocArgs(ln), ")");
            ind--;
            LS("}");
            L(v.lenlv, " = ", IntGoT(v.lenst), "(", ol, " - ", nn, ")");
            return "";
        }
        case B_CLEAR: {
            auto l = RecvLoc(an[0]);
            CheckLimited(l, ln);
            L(ArrayView(l, ln).lenlv, " = 0");
            return "";
        }
        case B_RESIZE: {
            auto l = RecvLoc(an[0]);
            CheckLimited(l, ln);
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
            L(v.lenlv, " = ", IntGoT(v.lenst), "(", nn, ")");
            ind--;
            LS("} else if ", nn, " > ", ol, " {");
            ind++;
            if (fv.empty()) {
                L("GsAbort(GsEResizeFill, ", LocArgs(ln), ")");
            } else {
                L("if ", nn, " > ", v.cap, " {");
                ind++;
                L("GsAbort(GsECapacity, ", LocArgs(ln), ")");
                ind--;
                LS("}");
                auto i = T();
                L("for ", i, " := ", ol, "; ", i, " < ", nn, "; ", i, "++ {");
                ind++;
                auto slot = ElemAt(v, i, ln);
                if (rel) {
                    if (fv == "nil") L(slot.lv, " = 0");
                    else EmitRelStore(slot, fv, NonNullRef(an[2]), false, ln);
                } else {
                    St(slot, fv);
                }
                ind--;
                LS("}");
                L(v.lenlv, " = ", IntGoT(v.lenst), "(", nn, ")");
            }
            ind--;
            LS("}");
            return "";
        }
        default: break;
    }
    Unsupported(ln, cat("the builtin ", BuiltinByKind(c->builtin).name, "()"));
}

// The address of element i of a view.
inline string CodeGenGo::ElemAddr(const GoView &v, const string &i) {
    auto ix = IsGoName(i) ? i : cat("(", i, ")");
    return cat("unsafe.Add(", v.data, ", ", v.esize == 1 ? ix : cat(ix, "*", v.esize), ")");
}

// ------------------------------------------------------------------
// The text of a value that is no scalar (§3.7), rendered at the print's
// cursor: an array or a slice as `[a, b]` (a u8 one's bytes as they are, or
// quoted inside an aggregate), a struct or a variant as its positional
// literal, an ADT as its variant's, a reference as its pointee, a null as
// `null`. A type that reaches itself through references renders its levels
// below the first through a function of its own (RenderFn), which takes and
// gives the cursor.

inline void CodeGenGo::RenderLoc(const string &cur, const string &end, GoLoc l, TypeExpr *t,
                                 bool nested, Line ln) {
    if (l.t->kind == TY_REF && t->kind != TY_REF) DerefLoc(l);
    if (auto sp = FmtSpecFor(rctx, t)) {
        EmitUserFormat(cur, l, sp, ln);
        return;
    }
    auto lit = [&](const string &s) { L(cur, " = GsPutStr(", cur, ", ", end, ", ", GoStr(s), ")"); };
    auto nominal = t->kind == TY_STRUCT || t->kind == TY_VARIANT || t->kind == TY_ENUM;
    auto overloads = rctx && !rctx->fmtspecs.empty();
    // An overload rendering one part of a value may rebind a reference on
    // the way to it: the other parts are read from where the value lay when
    // its rendering began, which the checker holds meanwhile.
    if (l.viaref && overloads && nominal) {
        if (IsResz(t)) {
            l = PinRz(l);
        } else if (IsBytesT(t)) {
            auto p = T();
            Decl(p, "unsafe.Pointer", l.addr);
            l.addr = p;
            l.viaref = false;
        } else {
            l = PinLoc(l);
        }
    }
    if (nominal) {
        for (auto r : rendering)
            if (TEq(r, t)) {
                // The value as its render function takes it: a frame object
                // by its Go struct (viewed as one where it lies as bytes), a
                // resizable bytes value by its image and count, any other
                // bytes value by its image.
                string arg;
                if (IsFrameObj(t)) {
                    if (IsFoBytesLoc(l)) l = FoView(l);
                    arg = PtrOf(l);
                } else if (IsResz(t)) {
                    if (l.lenlv.empty()) Fail(ln, "internal: a resizable value rendered without its count");
                    arg = cat("GsRhdr{Base: ", l.addr, ", Len: ", l.lenlv, "}");
                } else {
                    arg = IsBytesT(t) ? l.addr : PtrOf(l);
                }
                L(cur, " = ", RenderFn(t, ln), "(", cur, ", ", end, ", ", arg, ")");
                return;
            }
        rendering.push_back(t);
    }
    switch (t->kind) {
        case TY_INT: case TY_FLT: case TY_BOOL:
            RenderScalar(cur, end, LoadLoc(l, t, ln), t, ln);
            break;
        case TY_REF: {
            auto x = LoadLoc(l, l.t, ln);
            auto sub = t->ref->sub;
            // The pointee where the reference points: a resizable's through
            // its header, a bytes value at its address.
            auto at = [&](const string &p) {
                if (IsResz(sub)) return FatRefLoc(p, sub);
                if (IsBytesT(sub)) return BytesLoc(p, sub, GoLoc {});
                return RefIsAddr(t) ? AtAddr(p, sub) : AtTyped(p, sub);
            };
            if (t->ref->optional) {
                auto p = Snapshot(ast.RefTo(sub, ln, true), x);
                L("if ", p, IsResz(sub) ? ".Hdr" : "", " == nil {");
                ind++;
                lit("null");
                ind--;
                LS("} else {");
                ind++;
                RenderLoc(cur, end, at(p), sub, nested, ln);
                ind--;
                LS("}");
                break;
            }
            RenderLoc(cur, end, at(Snapshot(ast.RefTo(sub, ln, false), x)), sub, nested, ln);
            break;
        }
        case TY_ARRAY: case TY_SLICE: {
            auto v = ArrayView(l, ln);
            auto elem = v.elem;
            auto d = T(), n = T();
            Decl(d, "unsafe.Pointer", v.data);
            Decl(n, "int64", v.fixed >= 0 ? cat(v.fixed) : v.len);
            if (IsU8(elem)) {
                if (nested) L(cur, " = GsPutQuoted(", cur, ", ", end, ", ", d, ", ", n, ")");
                else L(cur, " = GsPutBytes(", cur, ", ", end, ", unsafe.Slice((*byte)(", d, "), ", n, "))");
                break;
            }
            lit("[");
            auto i = T();
            // Variable-size elements are walked with a cursor of their own.
            string p;
            if (!IsFix(elem)) {
                p = T();
                Decl(p, "unsafe.Pointer", d);
            }
            L("for ", i, " := int64(0); ", i, " < ", n, "; ", i, "++ {");
            ind++;
            PushSc(SC_PLAIN);
            L("if ", i, " > 0 {");
            ind++;
            lit(", ");
            ind--;
            LS("}");
            GoLoc el;
            if (!p.empty()) {
                el = BytesLoc(p, elem, GoLoc {});
            } else {
                GoView ev;
                ev.data = d;
                ev.elem = elem;
                ev.esize = v.esize;
                el = ElemAt(ev, i, ln);
            }
            el.viaref = elem->kind == TY_REF;
            RenderLoc(cur, end, el, elem, true, ln);
            if (!p.empty()) L(p, " = unsafe.Add(", p, ", ", SizeX(elem, p), ")");
            PopSc();
            ind--;
            LS("}");
            lit("]");
            break;
        }
        case TY_STRUCT: {
            auto si = SI(t);
            string name;
            t->Dump(name);
            lit(cat(name, " { "));
            auto firstf = true;
            for (auto i = 0; i < (int)si->st->fields.size(); i++) {
                if (si->st->fields[i].ispad) continue;
                if (!firstf) lit(", ");
                firstf = false;
                RenderLoc(cur, end, FieldLocAt(l, i, ln), si->ftypes[i], true, ln);
            }
            lit(" }");
            break;
        }
        case TY_VARIANT: RenderVariant(cur, end, l, t, ln); break;
        case TY_ENUM: {
            auto ei = EIOf(t);
            auto &ti = TI(t, ln);
            auto bytes = IsBytesT(t);
            auto tag = T();
            Decl(tag, ti.rep == GR_TAG ? ti.name : IntGoT(TagStore(ei->en)), TagOf(l, ln));
            for (size_t vi = 0; vi < ei->en->variants.size(); vi++) {
                if (vi) LS("} else if ", tag, " == ", vi, " {");
                else L("if ", tag, " == ", vi, " {");
                ind++;
                PushSc(SC_PLAIN);
                auto vt = VariantType(t, (int)vi);
                GoLoc pl;
                if (bytes) pl = PayloadLoc(l, (int)vi, ln);
                else if (ti.voffs.empty() || ti.voffs[vi] < 0) pl = AtVar(ZeroOf(vt, ln), vt);
                else pl = PayloadLoc(l, (int)vi, ln);
                // An overload rendering one part of a fixed-mode value may
                // overwrite it with another variant: the parts are rendered
                // from a copy (CodeGen::RenderLoc).
                if (!bytes && overloads && !(ti.voffs.empty() || ti.voffs[vi] < 0))
                    pl = AtVar(Snapshot(vt, Ld(pl)), vt);
                RenderVariant(cur, end, pl, vt, ln);
                PopSc();
                ind--;
            }
            LS("}");
            break;
        }
        default: Fail(ln, cat("internal: no text for a value of type ", TypeText(t)));
    }
    if (nominal) rendering.pop_back();
}

inline void CodeGenGo::RenderVariant(const string &cur, const string &end, const GoLoc &l,
                                     TypeExpr *t, Line ln) {
    auto ei = EIVar(t);
    auto vi = ei->en->VariantIndex(t->var->variant);
    auto &v = ei->en->variants[vi];
    auto lit = [&](const string &s) { L(cur, " = GsPutStr(", cur, ", ", end, ", ", GoStr(s), ")"); };
    if (v.fields.empty()) {
        lit(cat(ei->en->name, ".", v.name));
        return;
    }
    lit(cat(v.name, " { "));
    auto firstf = true;
    for (auto i = 0; i < (int)v.fields.size(); i++) {
        if (v.fields[i].ispad) continue;
        if (!firstf) lit(", ");
        firstf = false;
        RenderLoc(cur, end, FieldLocAt(l, i, ln), ei->vftypes[vi][i], true, ln);
    }
    lit(" }");
}

inline void CodeGenGo::RenderScalar(const string &cur, const string &end, const string &x,
                                    TypeExpr *t, Line ln) {
    switch (t->kind) {
        case TY_INT:
            if (t->intstorage == IS_U64) L(cur, " = GsFmtU64(", cur, ", ", end, ", ", x, ")");
            else L(cur, " = GsFmtI64(", cur, ", ", end, ", ", Coerce(x, t, ast.inttypes[IS_I64], ln), ")");
            return;
        case TY_FLT:
            L(cur, " = GsFmt", t->fltstorage == FS_F32 ? "F32" : "F64", "(", cur, ", ", end, ", ", x, ")");
            return;
        default:
            L(cur, " = GsFmtBool(", cur, ", ", end, ", ", x, ")");
            return;
    }
}

// The function rendering the levels of a value of type t below the first,
// from a pointer to it; its body is emitted once every function is
// (EmitRenderFns).
inline string CodeGenGo::RenderFn(TypeExpr *t, Line ln) {
    auto m = Mangle(t);
    if (auto it = renderfns.find(m); it != renderfns.end()) return it->second;
    auto name = Unique(cat("gsRender_", m));
    renderfns[m] = name;
    renderqueue.push_back({ t, ln, name });
    return name;
}

inline void CodeGenGo::EmitRenderFns() {
    for (size_t i = 0; i < renderqueue.size(); i++) {
        auto r = renderqueue[i];
        auto save = SaveFnState();
        ResetFnState();
        curspec = nullptr;
        curinfo = nullptr;
        spexpr = "0";
        PushSc(SC_FN);
        auto fo = IsFrameObj(r.t);
        auto rzb = IsResz(r.t) && !fo;
        auto bytes = IsBytesT(r.t) && !fo && !rzb;
        GoLoc v;
        string vt;
        if (rzb) {
            v.t = r.t;
            v.addr = "v.Base";
            v.lenlv = "v.Len";
            vt = "GsRhdr";
        } else if (bytes) {
            v = BytesLoc("v", r.t, GoLoc {});
            vt = "unsafe.Pointer";
        } else {
            v = AtTyped("v", r.t);
            if (fo) v.hdr = v.lv;
            vt = cat("*", GoT(r.t, r.ln));
        }
        RenderLoc("cur", "end", v, r.t, true, r.ln);
        PopSc();
        auto b = FinishBody(body);
        RestoreFnState(save);
        Append(code, "// The text of a ", TypeText(r.t), " below the first level (§3.7).\n", "func ",
               r.name, "(cur, end unsafe.Pointer, v ", vt, ") unsafe.Pointer {\n", b, "\treturn cur\n}\n\n");
    }
}

// The emission state of the function being emitted, which a function
// generated on the side (an equality, a render function) sets aside.
inline CodeGenGo::FnState CodeGenGo::SaveFnState() {
    FnState s;
    s.body = std::move(body);
    s.ind = ind;
    s.tmpn = tmpn;
    s.stknext = stknext;
    s.stkmax = stkmax;
    s.spexpr = spexpr;
    s.fnused = std::move(fnused);
    s.fnnextsuffix = std::move(fnnextsuffix);
    s.vnames = std::move(vnames);
    s.fvptr = std::move(fvptr);
    s.refvars = std::move(refvars);
    s.refplaces = std::move(refplaces);
    s.scopes = std::move(scopes);
    s.dead = dead;
    s.dry = dry;
    s.curspec = curspec;
    s.curinfo = curinfo;
    s.rendering = std::move(rendering);
    s.rtarget = rtarget;
    s.rctx = rctx;
    rtarget = nullptr;
    rctx = nullptr;
    s.vstk = std::move(vstk);
    s.vpool = std::move(vpool);
    s.nrvo = std::move(nrvo);
    s.openat = std::move(openat);
    s.dsttop0 = std::move(dsttop0);
    s.dstcounts = std::move(dstcounts);
    s.ltypes = std::move(ltypes);
    s.ptypes = std::move(ptypes);
    s.fdstsaves = std::move(fdstsaves);
    s.curzeros = std::move(curzeros);
    s.curgorets = std::move(curgorets);
    fdstsaves.clear();
    curzeros.clear();
    curgorets.clear();
    ltypes.clear();
    ptypes.clear();
    vstk.clear();
    vpool.clear();
    nrvo.clear();
    openat.clear();
    dsttop0.clear();
    dstcounts.clear();
    body.clear();
    fnused.clear();
    fnnextsuffix.clear();
    vnames.clear();
    fvptr.clear();
    refvars.clear();
    refplaces.clear();
    scopes.clear();
    rendering.clear();
    ind = 1;
    dead = false;
    dry = 0;
    return s;
}

inline void CodeGenGo::RestoreFnState(FnState &s) {
    body = std::move(s.body);
    ind = s.ind;
    tmpn = s.tmpn;
    stknext = s.stknext;
    stkmax = s.stkmax;
    spexpr = s.spexpr;
    fnused = std::move(s.fnused);
    fnnextsuffix = std::move(s.fnnextsuffix);
    vnames = std::move(s.vnames);
    fvptr = std::move(s.fvptr);
    refvars = std::move(s.refvars);
    refplaces = std::move(s.refplaces);
    scopes = std::move(s.scopes);
    dead = s.dead;
    dry = s.dry;
    curspec = s.curspec;
    curinfo = s.curinfo;
    rendering = std::move(s.rendering);
    rtarget = s.rtarget;
    rctx = s.rctx;
    vstk = std::move(s.vstk);
    vpool = std::move(s.vpool);
    nrvo = std::move(s.nrvo);
    openat = std::move(s.openat);
    dsttop0 = std::move(s.dsttop0);
    dstcounts = std::move(s.dstcounts);
    ltypes = std::move(s.ltypes);
    ptypes = std::move(s.ptypes);
    fdstsaves = std::move(s.fdstsaves);
    curzeros = std::move(s.curzeros);
    curgorets = std::move(s.curgorets);
}

}  // namespace goose
