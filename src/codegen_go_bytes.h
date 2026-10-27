// Goose compiler — the Go backend's variable-size values (definitions of
// CodeGenGo members, codegen_go.h): bytes values -- variable arrays (T[],
// T[uN], T[varint]), runtime-capacity limited arrays (T[..]), varints,
// structs with variable-size fields and variable-mode ADTs -- as the
// self-describing byte images the C backend lays out (implementation.md
// §6.1, §6.9): the generated size walkers (gsSize_<T>), the cursor that
// reaches a field behind variable-size ones (FieldPtr), their construction
// at a stack's top, their equality, and serialization (to_bytes, bytes_of,
// from_bytes with its verifier, docs/design/serialization.md).
//
// A bytes value is an unsafe.Pointer to its image's start on a data stack
// (DESIGN.md §3.3). A fixed-size part of an image holds the Go
// representation of its type, as the stride of a resizable's elements does:
// for a flat type, which is what an image that is serialized holds, that is
// its C layout byte for byte (codegen_go_types.h).
#pragma once

namespace goose {

// ------------------------------------------------------------------
// Sizes.

// The bytes a fixed-size value takes in an image: its Go representation's.
inline int64_t CodeGenGo::ImgSize(TypeExpr *t) { return TI(t, t->line).size; }

// The byte size of the value of type t at p, an unsafe.Pointer.
inline string CodeGenGo::SizeX(TypeExpr *t, const string &p) {
    if (IsFix(t)) return cat(ImgSize(t));
    return cat(SizeFn(t), "(", p, ")");
}

// gsSize_<T>(p): the byte size of a value of type t at p, a walk over its
// image (CodeGen::SizeFn).
inline string CodeGenGo::SizeFn(TypeExpr *t) {
    auto m = Mangle(t);
    if (auto it = sizefns.find(m); it != sizefns.end()) return it->second;
    auto name = Unique(cat("gsSize_", m));
    sizefns[m] = name;
    string b;
    int tmp = 0;
    EmitSizeWalk(b, t, "p", "q", tmp, 1);
    Append(bcode, "// The byte size of a ", TypeText(t), " at p.\nfunc ", name,
           "(p unsafe.Pointer) int64 {\n\tvar q int64\n", b, "\treturn q\n}\n\n");
    return name;
}

// Statements advancing the cursor q (bytes from p) over one value of type t,
// indented `in` tabs; tmp numbers the names they declare.
inline void CodeGenGo::EmitSizeWalk(string &b, TypeExpr *t, const string &p, const string &q, int &tmp,
                                    int in) {
    string I((size_t)in, '\t');
    auto at = cat("unsafe.Add(", p, ", ", q, ")");
    if (IsFix(t)) {
        Append(b, I, q, " += ", ImgSize(t), "\n");
        return;
    }
    switch (t->kind) {
        case TY_INT: case TY_REF:   // A varint, or a varint-width relative reference.
            Append(b, I, q, " += GsUlebSize(", at, ")\n");
            return;
        case TY_ARRAY: {
            auto &a = *t->arr;
            if (a.akind == A_LIMITED) {
                // A runtime capacity: [cap u32][len u32][cap slots].
                Append(b, I, q, " += 8 + int64(*(*uint32)(", at, "))*", ImgSize(a.sub), "\n");
                return;
            }
            if (a.akind != A_VAR) Fail(t->line, "internal: a resizable inside a bytes value");
            auto n = cat("n", tmp++);
            auto ls = LenStore(t->arr);
            if (ls == IS_VARINT)
                Append(b, I, n, " := GsUlebRead(", at, ")\n", I, q, " += GsUlebSize(", at, ")\n");
            else
                Append(b, I, n, " := int64(*(*", IntGoT(ls), ")(", at, "))\n", I, q, " += ",
                       IntSize(ls), "\n");
            if (IsFix(a.sub)) {
                Append(b, I, q, " += ", n, "*", ImgSize(a.sub), "\n");
            } else {
                auto i = cat("i", tmp++);
                Append(b, I, "for ", i, " := int64(0); ", i, " < ", n, "; ", i, "++ {\n");
                EmitSizeWalk(b, a.sub, p, q, tmp, in + 1);
                Append(b, I, "}\n");
            }
            return;
        }
        case TY_STRUCT: case TY_VARIANT:
            for (auto &run : FieldRuns(t))
                for (size_t i = 0; i < run.fields->size(); i++) {
                    auto &f = (*run.fields)[i];
                    // A bytes layout has no bare-pad alignment; explicit pads count.
                    if (f.ispad) {
                        if (f.padsize > 0) Append(b, I, q, " += ", f.padsize, "\n");
                        continue;
                    }
                    EmitSizeWalk(b, (*run.ftypes)[i], p, q, tmp, in);
                }
            return;
        case TY_ENUM: {
            auto ei = EIOf(t);
            auto ts = TagSize(ei->en);
            Append(b, I, "switch *(*", IntGoT(TagStore(ei->en)), ")(", at, ") {\n");
            for (size_t vi = 0; vi < ei->en->variants.size(); vi++) {
                Append(b, I, "case ", vi, ":\n", I, "\t", q, " += ", ts, "\n");
                EmitSizeWalk(b, VariantType(t, (int)vi), p, q, tmp, in + 1);
            }
            Append(b, I, "}\n");
            return;
        }
        default: Fail(t->line, cat("internal: no size walk for ", TypeText(t)));
    }
}

inline int64_t CodeGenGo::PrefixBytes(IntStorage ls) { return ls == IS_VARINT ? 1 : IntSize(ls); }

// The image of a string literal as a variable array of length storage ls,
// in the program's static data: read-only, as the literal is (§9.5).
inline string CodeGenGo::StrImage(const string &v, IntStorage ls) {
    string img;
    auto n = (uint64_t)v.size();
    if (ls == IS_VARINT) {
        do {
            uint8_t b = n & 0x7f;
            n >>= 7;
            img += (char)(n ? b | 0x80 : b);
        } while (n);
    } else {
        for (int64_t i = 0; i < IntSize(ls); i++) img += (char)(uint8_t)(n >> (8 * i));
    }
    img += v;
    return cat("unsafe.Pointer(unsafe.StringData(", GoStr(img), "))");
}

// The fewest bytes any value of this type can occupy (with `zero`, the
// all-zero value's, whose arrays are empty and whose ADTs at variant 0):
// what bounds the element count a length field may claim (CodeGen::MinBytes).
inline int64_t CodeGenGo::MinBytes(TypeExpr *t, bool zero) {
    if (IsFix(t)) return FixedSize(t);
    switch (t->kind) {
        case TY_INT: case TY_REF: return 1;
        case TY_ARRAY:
            switch (t->arr->akind) {
                case A_VAR: return PrefixBytes(LenStore(t->arr));
                case A_LIMITED: return 8;
                default: return 0;
            }
        case TY_STRUCT: case TY_VARIANT: {
            int64_t n = 0;
            for (auto &run : FieldRuns(t))
                for (size_t i = 0; i < run.fields->size(); i++) {
                    auto &f = (*run.fields)[i];
                    if (f.ispad) {
                        if (f.padsize > 0) n += f.padsize;
                        continue;
                    }
                    n += MinBytes((*run.ftypes)[i], zero);
                }
            return n;
        }
        case TY_ENUM: {
            auto ei = EIOf(t);
            if (zero) return TagSize(ei->en) + MinBytes(VariantType(t, 0), true);
            int64_t m = INT64_MAX;
            for (size_t vi = 0; vi < ei->en->variants.size(); vi++)
                m = std::min(m, MinBytes(VariantType(t, (int)vi)));
            return TagSize(ei->en) + m;
        }
        default: return 0;
    }
}

// ------------------------------------------------------------------
// Places in images.

// The address of field `fieldidx` of the bytes value at `base` (fields and
// ftypes of a struct or a variant): its offset, past a cursor that walks
// the variable-size fields before it (CodeGen::FieldPtr).
inline string CodeGenGo::FieldPtr(const string &base, const vector<Field> &fields,
                                  const vector<TypeExpr *> &ftypes, int fieldidx) {
    int64_t off = 0;
    string cur;
    for (auto i = 0; i < fieldidx; i++) {
        auto &f = fields[i];
        if (f.ispad) {
            off += f.padsize > 0 ? f.padsize : 0;
            continue;
        }
        if (IsFix(ftypes[i])) {
            off += ImgSize(ftypes[i]);
            continue;
        }
        if (cur.empty()) {
            cur = T();
            Decl(cur, "unsafe.Pointer", AddOff(base, off));
        } else if (off) {
            L(cur, " = unsafe.Add(", cur, ", ", off, ")");
        }
        off = 0;
        L(cur, " = unsafe.Add(", cur, ", ", SizeX(ftypes[i], cur), ")");
    }
    return AddOff(cur.empty() ? base : cur, off);
}

// The value of type t at p, inside the bytes value `from` (whose stack and
// count ride along): a fixed one a Go lvalue there, a bytes one its address.
inline CodeGenGo::GoLoc CodeGenGo::BytesLoc(const string &p, TypeExpr *t, const GoLoc &from) {
    GoLoc l;
    if (IsFix(t) && !IsVarintT(t)) {
        l = AtAddr(p, t);
    } else {
        l.t = t;
        l.addr = p;
    }
    l.stk = from.stk;
    l.lenlv = from.lenlv;
    l.fl = from.fl;
    l.flstk = from.flstk;
    l.viaref = from.viaref || t->kind == TY_REF;
    return l;
}

// ------------------------------------------------------------------
// Construction at a stack's top.

// A count that goes into a length field must fit its storage.
inline void CodeGenGo::EmitLenCheck(IntStorage ls, const string &n) {
    if (ls == IS_VARINT || IntSize(ls) == 8) return;
    L("GsLenCheck(", n, ", ", (1ull << (IntSize(ls) * 8)) - 1, ")");
}

inline void CodeGenGo::EmitLenStore(const string &stk, IntStorage ls, const string &n) {
    EmitLenCheck(ls, n);
    if (ls == IS_VARINT) {
        Bump(stk, cat("GsUlebWrite(", Top(stk), ", uint64(", n, "))"));
        return;
    }
    L("*(*", IntGoT(ls), ")(", Top(stk), ") = ", IntGoT(ls), "(", n, ")");
    Bump(stk, cat(IntSize(ls)));
}

// A varint field or element (§3.6): the i64 value x, zigzag-encoded.
inline void CodeGenGo::EmitVarintStore(const string &stk, const string &x) {
    Bump(stk, cat("GsZigWrite(", Top(stk), ", ", x, ")"));
}

// The count of `count` elements written into the prefix bytes reserved at
// `pref`, in front of the elements from `elems` up to the stack's top: in
// place for a fixed width; a varint moves the elements up only where the
// count outgrew its one reserved byte (CodeGen::EmitPrefixPatch).
inline void CodeGenGo::EmitPrefixPatch(const string &pref, IntStorage ls, const string &stk,
                                       const string &count, const string &elems) {
    if (ls != IS_VARINT) {
        EmitLenCheck(ls, count);
        L("*(*", IntGoT(ls), ")(", pref, ") = ", IntGoT(ls), "(", count, ")");
        return;
    }
    L("if ", count, " < 128 {");
    ind++;
    L("*(*byte)(", pref, ") = byte(", count, ")");
    ind--;
    LS("} else {");
    ind++;
    auto tmp = T(), ms = T();
    Decl(tmp, "[10]byte");
    Decl(ms, "int64", cat("GsUlebWrite(unsafe.Pointer(&", tmp, "), uint64(", count, "))"));
    auto n = T();
    Decl(n, "int64", cat("int64(uintptr(", Top(stk), ") - uintptr(", elems, "))"));
    L("copy(unsafe.Slice((*byte)(unsafe.Add(", elems, ", ", ms, "-1)), ", n, "), unsafe.Slice((*byte)(",
      elems, "), ", n, "))");
    L("copy(unsafe.Slice((*byte)(", pref, "), ", ms, "), ", tmp, "[:", ms, "])");
    Bump(stk, cat(ms, "-1"));
    ind--;
    LS("}");
}

// The plain reference rv as a relative reference of type rt at stk's top:
// a fixed width encoded in its slot there, a varint one zigzag-encoded (the
// `in pool` form unsigned) at its own width (CodeGen::EmitRelStore).
inline void CodeGenGo::EmitRelStoreStk(const string &stk, TypeExpr *rt, const string &rv, bool nonnull,
                                       Line ln) {
    if (rt->ref->lenstorage != IS_VARINT) {
        auto fa = T();
        Decl(fa, "unsafe.Pointer", Top(stk));
        EmitRelStore(AtAddr(fa, rt), rv, nonnull, false, ln);
        Bump(stk, cat(IntSize((IntStorage)rt->ref->lenstorage)));
        return;
    }
    auto fa = T();
    Decl(fa, "unsafe.Pointer", Top(stk));
    auto p = T();
    Decl(p, "unsafe.Pointer", cat("unsafe.Pointer(", rv, ")"));
    auto off = T();
    if (auto pool = rt->ref->pool) {
        auto base = cat(gnames.at(pool), ".Base");
        if (rt->ref->optional && !nonnull) Decl(off, "uint64", cat("GsPoolOff(", p, ", ", base, ")"));
        else Decl(off, "uint64", cat("uint64(uintptr(", p, ") - uintptr(", base, ")) + 1"));
        Bump(stk, cat("GsUlebWrite(", fa, ", ", off, ")"));
        return;
    }
    if (rt->ref->optional && !nonnull) Decl(off, "int64", cat("GsRelOff(", p, ", ", fa, ")"));
    else Decl(off, "int64", cat("int64(uintptr(", p, ") - uintptr(", fa, "))"));
    if (rt->ref->optional) {
        L("if ", nonnull ? "" : cat(p, " != nil && "), off, " == 0 {");
        ind++;
        L("GsAbort(GsERelNull, ", LocArgs(ln), ")");
        ind--;
        LS("}");
    }
    Bump(stk, cat("GsZigWrite(", fa, ", ", off, ")"));
}

// A struct, variant or variable-mode ADT literal at a bytes destination:
// the fields in layout order, an omitted optional one null
// (CodeGen::GenStructLit). A `self` field points at the value this
// literal builds, whose start is taken first.
inline void CodeGenGo::GenStructLitStk(StructLit *sl, const string &stk, const string &lenlv) {
    auto et = sl->exprtype;
    string selfbase;
    for (auto &fi : sl->inits)
        if (Is<SelfRef>(fi.val)) {
            selfbase = T();
            Decl(selfbase, "unsafe.Pointer", Top(stk));
            break;
        }
    if (et->kind == TY_ENUM) {
        if (!et->enu->varmode || !sl->variant) Fail(sl->line, "internal: a fixed-mode ADT literal on a stack");
        auto ei = EIOf(et);
        auto vi = ei->en->VariantIndex(sl->variant);
        EmitValStoreTag(stk, TagStore(ei->en), cat(vi));
        // A resizable-class ADT: a variant without a resizable tail leaves
        // the receiving count zero.
        if (!lenlv.empty() && IsFix(VariantType(et, vi))) L(lenlv, " = 0");
        GenFieldInits(sl, ei->en->variants[vi].fields, ei->vftypes[vi], stk, lenlv, selfbase);
        return;
    }
    if (et->kind == TY_VARIANT) {
        auto ei = EIVar(et);
        auto vi = ei->en->VariantIndex(et->var->variant);
        GenFieldInits(sl, ei->en->variants[vi].fields, ei->vftypes[vi], stk, lenlv, selfbase);
        return;
    }
    if (et->kind != TY_STRUCT) Fail(sl->line, "internal: a literal of no struct type");
    if (IsFrameObj(et)) {
        if (!selfbase.empty()) Unsupported(sl->line, "self in a frame object");
        GenFrameObjLit(sl, stk, lenlv);
        return;
    }
    auto si = SI(et);
    GenFieldInits(sl, si->st->fields, si->ftypes, stk, lenlv, selfbase);
}

inline void CodeGenGo::EmitValStoreTag(const string &stk, IntStorage ts, const string &x) {
    L("*(*", IntGoT(ts), ")(", Top(stk), ") = ", IntGoT(ts), "(", x, ")");
    Bump(stk, cat(IntSize(ts)));
}

inline void CodeGenGo::GenFieldInits(StructLit *sl, const vector<Field> &fields,
                                     const vector<TypeExpr *> &ftypes, const string &stk,
                                     const string &lenlv, const string &selfbase) {
    for (size_t i = 0; i < fields.size(); i++) {
        if (Unreached()) return;
        if (fields[i].ispad) {
            auto n = fields[i].padsize > 0 ? fields[i].padsize : 0;
            if (n) {
                L("clear(unsafe.Slice((*byte)(", Top(stk), "), ", n, "))");
                Bump(stk, cat(n));
            }
            continue;
        }
        auto init = sl->InitFor((int)i);
        auto ft = ftypes[i];
        if (IsRelRef(ft)) {
            if (!init || Is<NullLit>(init)) {
                if (ft->ref->lenstorage == IS_VARINT) {
                    L("*(*byte)(", Top(stk), ") = 0");
                    Bump(stk, "1");
                } else {
                    auto w = IntSize((IntStorage)ft->ref->lenstorage);
                    L("clear(unsafe.Slice((*byte)(", Top(stk), "), ", w, "))");
                    Bump(stk, cat(w));
                }
                continue;
            }
            string rv;
            if (Is<SelfRef>(init)) {
                rv = selfbase;
            } else {
                auto pt = ast.RefTo(ft->ref->sub, init->line, ft->ref->optional);
                rv = Snapshot(pt, CoerceVal(GenX(init), init->exprtype, pt, init->line));
            }
            EmitRelStoreStk(stk, ft, rv, Is<SelfRef>(init) || NonNullRef(init), sl->line);
            continue;
        }
        if (!init) {
            // An omitted optional: null.
            auto n = IsFix(ft) ? ImgSize(ft) : 1;
            L("clear(unsafe.Slice((*byte)(", Top(stk), "), ", n, "))");
            Bump(stk, cat(n));
            continue;
        }
        // The resizable tail (if any) takes the enclosing header's count;
        // every other field constructs plainly.
        if (IsResz(ft) && IsFrameObj(ft)) GenFoAsBytes(init, stk, ft, lenlv);
        else GenConstruct(init, stk, ft, IsResz(ft) ? lenlv : "");
    }
}

// A variable-mode ADT built at stk's top from a variant or a fixed-mode ADT
// at l: its tag, then the payload's bytes (CodeGen::GenVarEnumFromLoc).
inline void CodeGenGo::GenVarEnumFromLoc(GoLoc l, TypeExpr *et, const string &stk) {
    auto ei = EIOf(et);
    auto ts = TagStore(ei->en);
    if (l.t->kind == TY_VARIANT) {
        auto vi = ei->en->VariantIndex(l.t->var->variant);
        EmitValStoreTag(stk, ts, cat(vi));
        if (!IsFix(l.t)) {
            auto sz = T();
            Decl(sz, "int64", SizeX(l.t, l.addr));
            L("copy(unsafe.Slice((*byte)(", Top(stk), "), ", sz, "), unsafe.Slice((*byte)(", l.addr, "), ", sz, "))");
            Bump(stk, sz);
        } else if (VariantLayout(ei, vi).size) {
            EmitValStore(stk, l.t, l.lv);
        }
        return;
    }
    if (l.t->kind != TY_ENUM || l.t->enu->varmode)
        Fail(Line {}, cat("internal: a variable-mode ADT from a ", TypeText(l.t)));
    // A fixed-mode ADT: its tag, then the payload of the variant it holds.
    auto sv = T();
    Decl(sv, GoT(l.t, Line {}), l.lv);
    auto e = AtVar(sv, l.t);
    auto tag = T();
    Decl(tag, "int64", cat("int64(", TagOf(e, Line {}), ")"));
    EmitValStoreTag(stk, ts, tag);
    auto &ti = TI(l.t, Line {});
    for (size_t vi = 0; vi < ei->en->variants.size(); vi++) {
        if (ti.voffs.empty() || ti.voffs[vi] < 0) continue;
        L("if ", tag, " == ", vi, " {");
        ind++;
        auto vt = VariantType(l.t, (int)vi);
        EmitValStore(stk, vt, PayloadLoc(e, (int)vi, Line {}).lv);
        ind--;
        LS("}");
    }
}

// The elements of the variable array or runtime-capacity limited array `at`
// counted at stk's top: its length prefix, or its capacity and length (the
// capacity chosen as the length, v1), or the receiving count of an element
// run.
inline void CodeGenGo::EmitBytesCount(const string &stk, TypeExpr *at, const string &lenlv,
                                      const string &n, bool check) {
    if (at->arr->akind == A_VAR) {
        if (!lenlv.empty()) L(lenlv, " = ", n);
        else EmitLenStore(stk, LenStore(at->arr), n);
        return;
    }
    // A runtime capacity: chosen as the initial length.
    if (check) EmitLenCheck(IS_U32, n);
    L("*(*uint32)(", Top(stk), ") = uint32(", n, ")");
    L("*(*uint32)(unsafe.Add(", Top(stk), ", 4)) = uint32(", n, ")");
    Bump(stk, "8");
}

// ------------------------------------------------------------------
// Equality (§4.5) of two bytes values, at addresses a and b: byte for byte
// where equal values have equal bytes (canonical varints, no pads, floats,
// fixed-mode ADTs or limited arrays), a walk with a cursor pair otherwise
// (CodeGen::EmitEqBytes).

inline string CodeGenGo::EqFnBytes(TypeExpr *t, Line ln) {
    auto m = Mangle(t);
    if (auto it = eqfns.find(m); it != eqfns.end()) return it->second;
    auto name = Unique(cat("gsEq_", m));
    eqfns[m] = name;
    function<bool(TypeExpr *)> bitwise = [&](TypeExpr *x) -> bool {
        if (IsFix(x)) return BitwiseEq(x) && TI(x, ln).exact;
        switch (x->kind) {
            case TY_INT: case TY_REF: return true;
            case TY_ARRAY: return x->arr->akind != A_LIMITED && bitwise(x->arr->sub);
            case TY_STRUCT: case TY_VARIANT: case TY_ENUM:
                for (auto &run : FieldRuns(x))
                    for (size_t i = 0; i < run.fields->size(); i++)
                        if ((*run.fields)[i].ispad || !bitwise((*run.ftypes)[i])) return false;
                return true;
            default: return false;
        }
    };
    string b;
    if (bitwise(t)) {
        Append(b, "\tna, nb := ", SizeX(t, "a"), ", ", SizeX(t, "b"), "\n",
               "\treturn na == nb && GsMemEq(a, b, na)\n");
    } else {
        int tmp = 0;
        Append(b, "\tvar qa, qb int64\n");
        EmitEqWalk(b, t, tmp, 1, ln);
        Append(b, "\t_, _ = qa, qb\n\treturn true\n");
    }
    Append(eqcode, "// == on ", TypeText(t), " (§4.5).\nfunc ", name, "(a, b unsafe.Pointer) bool {\n", b,
           "}\n\n");
    return name;
}

// Compares one value at a+qa and b+qb, advancing both cursors; returns
// false from the function on a mismatch.
inline void CodeGenGo::EmitEqWalk(string &b, TypeExpr *t, int &tmp, int in, Line ln) {
    string I((size_t)in, '\t');
    auto pa = string("unsafe.Add(a, qa)"), pb = string("unsafe.Add(b, qb)");
    if (IsFix(t)) {
        auto &ti = TI(t, ln);
        if (BitwiseEq(t) && ti.exact) {
            Append(b, I, "if !GsMemEq(", pa, ", ", pb, ", ", ti.size, ") {\n", I, "\treturn false\n", I, "}\n");
        } else {
            // The comparison of the Go values the bytes hold.
            auto save = SaveFnState();
            auto x = EqX(t, AtAddr(pa, t), AtAddr(pb, t), ln);
            RestoreFnState(save);
            Append(b, I, "if !", x, " {\n", I, "\treturn false\n", I, "}\n");
        }
        Append(b, I, "qa += ", ti.size, "\n", I, "qb += ", ti.size, "\n");
        return;
    }
    switch (t->kind) {
        case TY_INT: case TY_REF: {   // A varint: canonical bytes.
            auto n = cat("n", tmp++);
            Append(b, I, n, " := GsUlebSize(", pa, ")\n", I, "if GsUlebSize(", pb, ") != ", n,
                   " || !GsMemEq(", pa, ", ", pb, ", ", n, ") {\n", I, "\treturn false\n", I, "}\n", I,
                   "qa += ", n, "\n", I, "qb += ", n, "\n");
            return;
        }
        case TY_ARRAY: {
            auto &a = *t->arr;
            auto na = cat("na", tmp), nb = cat("nb", tmp), ca = cat("ca", tmp), cb = cat("cb", tmp);
            tmp++;
            auto adv = [&](const string &p, const string &q, const string &n, const string &c) {
                auto at = cat("unsafe.Add(", p, ", ", q, ")");
                if (a.akind == A_VAR) {
                    auto ls = LenStore(t->arr);
                    if (ls == IS_VARINT)
                        Append(b, I, n, " := GsUlebRead(", at, ")\n", I, q, " += GsUlebSize(", at, ")\n");
                    else
                        Append(b, I, n, " := int64(*(*", IntGoT(ls), ")(", at, "))\n", I, q, " += ",
                               IntSize(ls), "\n");
                } else {
                    Append(b, I, c, " := int64(*(*uint32)(", at, "))\n", I, n,
                           " := int64(*(*uint32)(unsafe.Add(", at, ", 4)))\n", I, q, " += 8\n");
                }
            };
            adv("a", "qa", na, ca);
            adv("b", "qb", nb, cb);
            Append(b, I, "if ", na, " != ", nb, " {\n", I, "\treturn false\n", I, "}\n");
            auto iv = cat("i", tmp++);
            Append(b, I, "for ", iv, " := int64(0); ", iv, " < ", na, "; ", iv, "++ {\n");
            EmitEqWalk(b, a.sub, tmp, in + 1, ln);
            Append(b, I, "}\n");
            if (a.akind == A_LIMITED) {
                auto es = ImgSize(a.sub);
                Append(b, I, "qa += (", ca, " - ", na, ")*", es, "\n", I, "qb += (", cb, " - ", nb, ")*", es, "\n");
            }
            return;
        }
        case TY_STRUCT: case TY_VARIANT:
            for (auto &run : FieldRuns(t))
                for (size_t i = 0; i < run.fields->size(); i++) {
                    auto &f = (*run.fields)[i];
                    if (!f.ispad) EmitEqWalk(b, (*run.ftypes)[i], tmp, in, ln);
                    else if (f.padsize > 0)
                        Append(b, I, "qa += ", f.padsize, "\n", I, "qb += ", f.padsize, "\n");
                }
            return;
        case TY_ENUM: {
            auto ei = EIOf(t);
            auto ts = TagSize(ei->en);
            auto tt = IntGoT(TagStore(ei->en));
            auto tg = cat("tg", tmp++);
            Append(b, I, tg, " := *(*", tt, ")(", pa, ")\n", I, "if ", tg, " != *(*", tt, ")(", pb,
                   ") {\n", I, "\treturn false\n", I, "}\n", I, "qa += ", ts, "\n", I, "qb += ", ts, "\n",
                   I, "switch ", tg, " {\n");
            for (size_t vi = 0; vi < ei->en->variants.size(); vi++) {
                Append(b, I, "case ", vi, ":\n");
                EmitEqWalk(b, VariantType(t, (int)vi), tmp, in + 1, ln);
            }
            Append(b, I, "}\n");
            return;
        }
        default: Fail(ln, cat("internal: no equality walk for ", TypeText(t)));
    }
}

// ------------------------------------------------------------------
// Serialization (docs/design/serialization.md, implementation.md §6.9):
// to_bytes(a) is ULEB128(payload bytes) then a's element region; bytes_of(a)
// views the region; from_bytes<A>(b) verifies an image and copies it into
// a fresh A, or yields an empty one and false.

// Whether a value of this type holds a relative reference at any depth,
// variable-size parts included (CodeGen::HasRelRefAny).
inline bool CodeGenGo::HasRelRefAny(TypeExpr *t) {
    return HasRelRefOf(t, true, [&](TypeExpr *x) { return FieldRuns(x); });
}

// Whether the verifier has anything to check in a value of this type, or
// can simply step over its bytes (CodeGen::NeedsVerifyWalk).
inline bool CodeGenGo::NeedsVerifyWalk(TypeExpr *t) {
    if (!IsFix(t)) return true;
    switch (t->kind) {
        case TY_BOOL: return true;
        case TY_INT: return t->ix != nullptr;
        case TY_REF: return t->ref->lenstorage >= 0;
        case TY_ENUM: return true;
        case TY_ARRAY: return t->arr->akind == A_LIMITED || NeedsVerifyWalk(t->arr->sub);
        case TY_STRUCT: case TY_VARIANT:
            return AnyFieldOf(FieldRuns(t), [&](TypeExpr *ft) { return NeedsVerifyWalk(ft); });
        default: return false;
    }
}

// gsVerify_<T>(p, n, bm): the element count of the n-byte image at p, or -1
// where those bytes are not one (CodeGen::VerifyFn). Fixed elements start at
// multiples of their size, so their links are checked in the framing pass;
// variable ones need the framing pass to set their starts in bm before a
// second pass checks a link against them. Nothing past the image is formed
// into an address.
inline string CodeGenGo::VerifyFn(TypeExpr *elem) {
    auto m = Mangle(elem);
    if (auto it = verifyfns.find(m); it != verifyfns.end()) return it->second;
    auto name = Unique(cat("gsVerify_", m));
    verifyfns[m] = name;
    string b;
    int tmp = 0;
    if (MinBytes(elem) <= 0) {
        // Values of no bytes: only the empty image is an array of them.
        Append(b, "\t_, _ = p, bm\n\tif n == 0 {\n\t\treturn 0\n\t}\n\treturn -1\n");
    } else {
        auto fixed = IsFix(elem);
        auto rel = HasRelRefAny(elem);
        Append(b, "\tvar q, cnt int64\n\t_ = bm\n\tfor q < n {\n");
        if (!fixed && rel) Append(b, "\t\tgsBmSet(bm, q)\n");
        EmitVerifyWalk(b, elem, elem, "q", fixed && rel, tmp, 2);
        Append(b, "\t\tcnt++\n\t}\n\tif q != n {\n\t\treturn -1\n\t}\n");
        if (!fixed && rel) {
            Append(b, "\tq = 0\n\tfor q < n {\n");
            EmitVerifyWalk(b, elem, elem, "q", true, tmp, 2);
            Append(b, "\t}\n");
        }
        Append(b, "\treturn cnt\n");
    }
    Append(bcode, "// The verifier of an image of ", TypeText(elem), " elements (serialization.md §5).\n",
           "func ", name, "(p unsafe.Pointer, n int64, bm unsafe.Pointer) int64 {\n", b, "}\n\n");
    return name;
}

// One self-relative reference at byte q holding off: its target must be the
// start of an element of the image, and, where the reference is typed at a
// variant of the element ADT, one carrying that tag. Offset 0 is null, and
// only an optional reference may hold it (CodeGen::EmitVerifyLink).
inline void CodeGenGo::EmitVerifyLink(string &b, TypeExpr *rt, TypeExpr *elem, const string &q,
                                      const string &off, int &tmp, int in) {
    string I((size_t)in, '\t');
    auto v = cat("v", tmp++);
    auto pt = rt->ref->sub;
    auto variant = pt->kind == TY_VARIANT && elem->kind == TY_ENUM;
    int64_t skip = 0, vi = 0;
    IntStorage ts = IS_U8;
    if (variant) {
        auto ei = EIVar(pt);
        vi = ei->en->VariantIndex(pt->var->variant);
        ts = TagStore(ei->en);
        skip = IntSize(ts);
    }
    Append(b, I, rt->ref->optional ? cat("if ", off, " != 0 {\n") : string("{\n"));
    // A variant reference field is already past the element tag, so the base
    // stays nonnegative; the untrusted offset is bounded before it is added.
    Append(b, I, "\t", v, " := ", q, skip ? cat(" - ", skip) : string(), "\n");
    Append(b, I, "\tif ", off, " < -", v, " || ", off, " >= n-", v, " {\n", I, "\t\treturn -1\n", I, "\t}\n");
    Append(b, I, "\t", v, " += ", off, "\n");
    if (IsFix(elem))
        Append(b, I, "\tif ", v, "%", std::max<int64_t>(FixedSize(elem), 1), " != 0 {\n", I,
               "\t\treturn -1\n", I, "\t}\n");
    else
        Append(b, I, "\tif !gsBmGet(bm, ", v, ") {\n", I, "\t\treturn -1\n", I, "\t}\n");
    if (variant)
        Append(b, I, "\tif n-", v, " < ", skip, " || *(*", IntGoT(ts), ")(unsafe.Add(p, ", v, ")) != ", vi,
               " {\n", I, "\t\treturn -1\n", I, "\t}\n");
    Append(b, I, "}\n");
}

// The checks and cursor advance for a run of fields. `lo` is the packed
// layout of a fixed struct or variant, whose offsets may include alignment
// a sequential walk would miss; a bytes layout has none and passes null.
inline void CodeGenGo::EmitVerifyFields(string &b, const vector<Field> &fields,
                                        const vector<TypeExpr *> &ftypes, const Layout *lo,
                                        int64_t total, TypeExpr *elem, const string &q, bool links,
                                        int &tmp, int in) {
    string I((size_t)in, '\t');
    string start;
    if (lo) {
        start = cat("s", tmp++);
        Append(b, I, start, " := ", q, "\n", I, "if n-", q, " < ", total, " {\n", I, "\treturn -1\n", I,
               "}\n");
    }
    for (size_t i = 0; i < fields.size(); i++) {
        auto &f = fields[i];
        if (f.ispad) {
            if (!lo && f.padsize > 0) Append(b, I, q, " += ", f.padsize, "\n");
            continue;
        }
        if (lo && !NeedsVerifyWalk(ftypes[i])) continue;
        if (lo) Append(b, I, q, " = ", start, " + ", lo->offs[i], "\n");
        EmitVerifyWalk(b, ftypes[i], elem, q, links, tmp, in);
    }
    if (lo) Append(b, I, q, " = ", start, " + ", total, "\n");
}

// The checks and cursor advance for one value of type t at byte q of the
// image (CodeGen::EmitVerifyWalk); a failure returns -1.
inline void CodeGenGo::EmitVerifyWalk(string &b, TypeExpr *t, TypeExpr *elem, const string &q,
                                      bool links, int &tmp, int in) {
    string I((size_t)in, '\t');
    auto at = cat("unsafe.Add(p, ", q, ")");
    auto need = [&](const string &bytes) {
        Append(b, I, "if n-", q, " < ", bytes, " {\n", I, "\treturn -1\n", I, "}\n");
    };
    if (!NeedsVerifyWalk(t)) {
        auto sz = FixedSize(t);
        if (sz > 0) {
            need(cat(sz));
            Append(b, I, q, " += ", sz, "\n");
        }
        return;
    }
    switch (t->kind) {
        case TY_BOOL:
            need("1");
            Append(b, I, "if *(*byte)(", at, ") > 1 {\n", I, "\treturn -1\n", I, "}\n", I, q, "++\n");
            return;
        case TY_INT: {
            if (t->ix) Unsupported(t->line, cat("serializing the index type ", TypeText(t)));
            // A varint field: only its length matters to the walk.
            auto k = cat("k", tmp++);
            Append(b, I, "if _, ", k, " := gsUlebCheck(p, ", q, ", n); ", k, " == 0 {\n", I,
                   "\treturn -1\n", I, "} else {\n", I, "\t", q, " += ", k, "\n", I, "}\n");
            return;
        }
        case TY_REF: {
            // The framing pass only has to get past the offset; the link pass
            // needs its value.
            auto w = (IntStorage)t->ref->lenstorage;
            auto v = cat("o", tmp++);
            Append(b, I, "{\n");
            if (w == IS_VARINT) {
                if (links) Append(b, I, "\t", v, ", ", v, "k := gsZigCheck(p, ", q, ", n)\n");
                else Append(b, I, "\t_, ", v, "k := gsUlebCheck(p, ", q, ", n)\n");
                Append(b, I, "\tif ", v, "k == 0 {\n", I, "\t\treturn -1\n", I, "\t}\n");
            } else {
                Append(b, I, "\tif n-", q, " < ", IntSize(w), " {\n", I, "\t\treturn -1\n", I, "\t}\n");
                if (links) Append(b, I, "\t", v, " := int64(*(*", RelGoT(t), ")(", at, "))\n");
            }
            if (links) {
                string inner;
                EmitVerifyLink(inner, t, elem, q, v, tmp, in + 1);
                b += inner;
            }
            Append(b, I, "\t", q, " += ", w == IS_VARINT ? cat(v, "k") : cat(IntSize(w)), "\n");
            Append(b, I, "}\n");
            return;
        }
        case TY_ARRAY: {
            auto &a = *t->arr;
            switch (a.akind) {
                case A_FIXED: {
                    auto i = cat("i", tmp++);
                    Append(b, I, "for ", i, " := 0; ", i, " < ", ArrSize(t->arr), "; ", i, "++ {\n");
                    EmitVerifyWalk(b, a.sub, elem, q, links, tmp, in + 1);
                    Append(b, I, "}\n");
                    return;
                }
                case A_VAR: {
                    auto ls = LenStore(t->arr);
                    auto v = cat("c", tmp++);
                    Append(b, I, "{\n", I, "\tvar ", v, " int64\n");
                    if (ls == IS_VARINT) {
                        Append(b, I, "\t", v, "u, ", v, "k := gsUlebCheck(p, ", q, ", n)\n", I, "\tif ", v,
                               "k == 0 || ", v, "u > uint64(math.MaxInt64) {\n", I, "\t\treturn -1\n", I,
                               "\t}\n", I, "\t", v, " = int64(", v, "u)\n", I, "\t", q, " += ", v, "k\n");
                    } else {
                        Append(b, I, "\tif n-", q, " < ", IntSize(ls), " {\n", I, "\t\treturn -1\n", I,
                               "\t}\n", I, "\t", v, " = int64(*(*", IntGoT(ls), ")(", at, "))\n", I, "\t", q,
                               " += ", IntSize(ls), "\n");
                    }
                    Append(b, I, "\tif ", v, " < 0 || ", v, " > (n-", q, ")/",
                           std::max<int64_t>(MinBytes(a.sub), 1), " {\n", I, "\t\treturn -1\n", I, "\t}\n");
                    if (!NeedsVerifyWalk(a.sub)) {
                        Append(b, I, "\t", q, " += ", v, "*", FixedSize(a.sub), "\n");
                    } else {
                        auto i = cat("i", tmp++);
                        Append(b, I, "\tfor ", i, " := int64(0); ", i, " < ", v, "; ", i, "++ {\n");
                        EmitVerifyWalk(b, a.sub, elem, q, links, tmp, in + 2);
                        Append(b, I, "\t}\n");
                    }
                    Append(b, I, "}\n");
                    return;
                }
                case A_LIMITED: {
                    // Slots past the length were never written (§5.3): only
                    // the live ones are walked, the whole capacity skipped.
                    auto v = cat("l", tmp++);
                    auto esz = std::max<int64_t>(FixedSize(a.sub), 1);
                    auto stat = IsFix(t);
                    Append(b, I, "{\n", I, "\t", v, "s := ", q, "\n", I, "\tvar ", v, "n int64\n");
                    if (stat) {
                        auto ls = LenStore(t->arr);
                        Append(b, I, "\tif n-", q, " < ", FixedSize(t), " {\n", I, "\t\treturn -1\n", I,
                               "\t}\n", I, "\t", v, "n = int64(*(*", IntGoT(ls), ")(", at, "))\n", I, "\tif ",
                               v, "n < 0 || ", v, "n > ", ArrSize(t->arr), " {\n", I, "\t\treturn -1\n", I,
                               "\t}\n", I, "\t", q, " += ", IntSize(ls), "\n");
                    } else {
                        Append(b, I, "\tif n-", q, " < 8 {\n", I, "\t\treturn -1\n", I, "\t}\n", I, "\t", v,
                               "c := int64(*(*uint32)(", at, "))\n", I, "\t", v,
                               "n = int64(*(*uint32)(unsafe.Add(", at, ", 4)))\n", I, "\tif ", v, "n > ", v,
                               "c || ", v, "c > (n-", q, "-8)/", esz, " {\n", I, "\t\treturn -1\n", I, "\t}\n",
                               I, "\t", q, " += 8\n");
                    }
                    if (NeedsVerifyWalk(a.sub)) {
                        auto i = cat("i", tmp++);
                        Append(b, I, "\tfor ", i, " := int64(0); ", i, " < ", v, "n; ", i, "++ {\n");
                        EmitVerifyWalk(b, a.sub, elem, q, links, tmp, in + 2);
                        Append(b, I, "\t}\n");
                    }
                    if (stat) Append(b, I, "\t", q, " = ", v, "s + ", FixedSize(t), "\n");
                    else Append(b, I, "\t", q, " = ", v, "s + 8 + ", v, "c*", FixedSize(a.sub), "\n");
                    Append(b, I, "}\n");
                    return;
                }
                default: Fail(t->line, "internal: a resizable inside an image");
            }
        }
        case TY_STRUCT: {
            auto si = SI(t);
            auto fix = IsFix(t);
            EmitVerifyFields(b, si->st->fields, si->ftypes, fix ? &StructLayout(si) : nullptr,
                             fix ? FixedSize(t) : 0, elem, q, links, tmp, in);
            return;
        }
        case TY_VARIANT: {
            auto ei = EIVar(t);
            auto vi = ei->en->VariantIndex(t->var->variant);
            auto fix = IsFix(t);
            EmitVerifyFields(b, ei->en->variants[vi].fields, ei->vftypes[vi],
                             fix ? &VariantLayout(ei, vi) : nullptr, fix ? FixedSize(t) : 0, elem, q, links,
                             tmp, in);
            return;
        }
        case TY_ENUM: {
            // The tag is range-checked in both modes.
            auto ei = EIOf(t);
            auto ts = TagStore(ei->en);
            auto nv = (int64_t)ei->en->variants.size();
            auto fix = !t->enu->varmode;
            auto v = cat("e", tmp++);
            Append(b, I, "{\n");
            if (fix) Append(b, I, "\t", v, "s := ", q, "\n");
            Append(b, I, "\tif n-", q, " < ", fix ? FixedSize(t) : IntSize(ts), " {\n", I, "\t\treturn -1\n", I,
                   "\t}\n", I, "\t", v, "t := int64(*(*", IntGoT(ts), ")(", at, "))\n", I, "\tif uint64(", v,
                   "t) >= ", nv, " {\n", I, "\t\treturn -1\n", I, "\t}\n", I, "\t", q, " += ", IntSize(ts), "\n",
                   I, "\tswitch ", v, "t {\n");
            for (int64_t i = 0; i < nv; i++) {
                auto vt = VariantType(t, (int)i);
                Append(b, I, "\tcase ", i, ":\n");
                if (!fix || NeedsVerifyWalk(vt)) EmitVerifyWalk(b, vt, elem, q, links, tmp, in + 2);
            }
            Append(b, I, "\t}\n");
            if (fix) Append(b, I, "\t", q, " = ", v, "s + ", FixedSize(t), "\n");
            Append(b, I, "}\n");
            return;
        }
        default: Fail(t->line, cat("internal: no verifier walk for ", TypeText(t)));
    }
}

// The element region of a to_bytes or bytes_of receiver: its address and
// its byte count, which for variable-size elements is a walk
// (CodeGen::PayloadOf).
inline void CodeGenGo::PayloadOf(Node *n, string &src, string &sz) {
    auto nt = n->exprtype;
    auto rt = nt->kind == TY_REF ? nt->ref->sub : nt;
    auto elem = rt->kind == TY_SLICE ? rt->sub : rt->arr->sub;
    SrcElems se;
    if (rt->kind == TY_ARRAY || nt->kind == TY_REF) {
        GoLoc l;
        auto st = rt;
        while (st->kind == TY_REF) st = st->ref->sub;
        if (IsResz(st)) l = RzRecvLoc(n);
        else l = RecvLoc(n);
        auto v = ArrayView(l, n->line);
        auto cnt = T();
        Decl(cnt, "int64", v.fixed >= 0 ? cat(v.fixed) : v.len);
        se.elems = v.data;
        se.n = cnt;
    } else {
        se = GenSrcElems(n);
    }
    src = T();
    Decl(src, "unsafe.Pointer", se.elems);
    sz = T();
    if (IsFix(elem)) {
        Decl(sz, "int64", cat(se.n, "*", FixedSize(elem)));
    } else {
        auto i = T();
        Decl(sz, "int64", "0");
        L("for ", i, " := int64(0); ", i, " < ", se.n, "; ", i, "++ {");
        ind++;
        L(sz, " += ", SizeFn(elem), "(unsafe.Add(", src, ", ", sz, "))");
        ind--;
        LS("}");
    }
}

// The destination a resizable or variable builtin result is built at
// (CodeGen::OpenRzDest): the caller's header, a value slot that takes a
// length prefix in front of the elements, or a fresh temporary. `elems` is
// where the elements start.
inline CodeGenGo::RzDest CodeGenGo::OpenRzDest(TypeExpr *t, const GoDst &d0, Line ln, const char *what) {
    RzDest rd;
    auto resz = IsResz(t);
    rd.stk = d0.s;
    rd.lenlv = d0.lenlv;
    if (d0.k != GD_STACK) {
        if (resz) {
            rd.hdr = RzTemp(t, rd.stk);
            rd.lenlv = RzLenLv(t, rd.hdr);
        } else {
            rd.hdr = BytesTemp(rd.stk);
        }
    }
    if (rd.lenlv.empty()) {
        // A value slot: the count goes into a length prefix reserved in
        // front of the elements, in the slot's own length storage where the
        // destination has one, and the result's otherwise.
        auto at = d0.t && d0.t->kind == TY_ARRAY && d0.t->arr->akind == A_VAR ? d0.t : resz ? nullptr : t;
        if (!at) Fail(ln, cat(what, "() needs a resizable or variable-array destination"));
        rd.ls = LenStore(at->arr);
        rd.pref = T();
        Decl(rd.pref, "unsafe.Pointer", Top(rd.stk));
        Bump(rd.stk, cat(PrefixBytes(rd.ls)));
    }
    rd.elems = T();
    Decl(rd.elems, "unsafe.Pointer", Top(rd.stk));
    return rd;
}

inline void CodeGenGo::CloseRzDest(RzDest &rd, const string &count) {
    if (!rd.pref.empty()) EmitPrefixPatch(rd.pref, rd.ls, rd.stk, count, rd.elems);
    else L(rd.lenlv, " = ", count);
}

inline void CodeGenGo::EmitLeCheck(Line ln) {
    L("if !gsIsLE() {");
    ind++;
    L("GsAbort(GsEEndian, ", LocArgs(ln), ")");
    ind--;
    LS("}");
}

// bytes_of(a): the element region as a u8[:], no copy.
inline vector<string> CodeGenGo::EmitBytesOf(Call *c, vector<Node *> &an, Line ln) {
    (void)c;
    EmitLeCheck(ln);
    string src, sz;
    PayloadOf(an[0], src, sz);
    auto s = T();
    Decl(s, "GsSlice", cat("GsSlice{", src, ", ", sz, "}"));
    return { s };
}

// to_bytes(a): the image -- a varint byte count, then the element region --
// as a fresh u8[>..], or appended to a builder the caller owns.
inline vector<string> CodeGenGo::EmitToBytes(vector<Node *> &an, const GoDst &d0, Line ln) {
    EmitLeCheck(ln);
    string src, sz;
    PayloadOf(an[0], src, sz);
    auto pfx = T(), pn = T();
    Decl(pfx, "[10]byte");
    Decl(pn, "int64", cat("GsUlebWrite(unsafe.Pointer(&", pfx, "), uint64(", sz, "))"));
    if (an.size() > 1) {
        auto ot = an[1]->exprtype;
        while (ot->kind == TY_REF) ot = ot->ref->sub;
        auto out = IsResz(ot) ? RzRecvLoc(an[1]) : RecvLoc(an[1]);
        AppendBytes(out, cat("unsafe.Pointer(&", pfx, ")"), pn, ln);
        AppendBytes(out, src, sz, ln);
        return {};
    }
    auto rd = OpenRzDest(GrowU8(), d0, ln, "to_bytes");
    L("copy(unsafe.Slice((*byte)(", Top(rd.stk), "), ", pn, "), ", pfx, "[:", pn, "])");
    Bump(rd.stk, pn);
    L("copy(unsafe.Slice((*byte)(", Top(rd.stk), "), ", sz, "), unsafe.Slice((*byte)(", src, "), ", sz, "))");
    Bump(rd.stk, sz);
    auto tot = T();
    Decl(tot, "int64", cat(pn, " + ", sz));
    CloseRzDest(rd, tot);
    return rd.hdr.empty() ? vector<string> {} : vector<string> { rd.hdr };
}

// from_bytes<T[...]>(b): the framing prefix, then the verifier over the
// payload, then a copy of it into the result's element region. A rejected
// image leaves the array empty, and the bool says which happened.
inline vector<string> CodeGenGo::EmitFromBytes(Call *c, vector<Node *> &an, const GoDst &d0, Line ln) {
    EmitLeCheck(ln);
    auto t = c->rettypes[0];
    auto elem = t->arr->sub;
    auto st = ast.SliceOf(ast.inttypes[IS_U8], ln);
    auto sl = Snapshot(st, CoerceVal(GenX(an[0]), an[0]->exprtype, st, ln));
    auto vf = VerifyFn(elem);
    // The prefix has to account for exactly the rest of the slice.
    auto pay = T(), pn = T(), ok = T(), uv = T(), kv = T();
    Decl(pay, "unsafe.Pointer", cat(sl, ".Data"));
    Decl(pn, "int64", "0");
    Decl(ok, "bool", "false");
    L(uv, ", ", kv, " := gsUlebCheck(", sl, ".Data, 0, ", sl, ".Len)", DECLMARK, uv, ",", kv);
    L("if ", kv, " != 0 && ", uv, " <= uint64(math.MaxInt64) && int64(", uv, ") == ", sl, ".Len-", kv, " {");
    ind++;
    L(pay, " = unsafe.Add(", pay, ", ", kv, ")");
    L(pn, " = int64(", uv, ")");
    L(ok, " = true");
    ind--;
    LS("}");
    string bm = "nil";
    if (!IsFix(elem) && HasRelRefAny(elem)) {
        // One bit per payload byte, where a variable element starts, on a
        // stack of its own: an image needing more of it than one reserves
        // is one this cannot verify, which is false rather than an abort.
        string bstk;
        bm = BytesTemp(bstk);
        auto bn = T();
        Decl(bn, "int64", cat("(", pn, " + 7) / 8"));
        L("if ", bn, " > gsStackReserve {");
        ind++;
        L(bn, " = 0");
        L(ok, " = false");
        ind--;
        LS("}");
        L("clear(unsafe.Slice((*byte)(", bm, "), ", bn, "))");
        Bump(bstk, bn);
    }
    auto cnt = T();
    Decl(cnt, "int64", "-1");
    L("if ", ok, " {");
    ind++;
    L(cnt, " = ", vf, "(", pay, ", ", pn, ", ", bm, ")");
    ind--;
    LS("}");
    L(ok, " = ", cnt, " >= 0");
    auto rd = OpenRzDest(t, d0, ln, "from_bytes");
    L("if ", ok, " {");
    ind++;
    L("copy(unsafe.Slice((*byte)(", Top(rd.stk), "), ", pn, "), unsafe.Slice((*byte)(", pay, "), ", pn, "))");
    Bump(rd.stk, pn);
    ind--;
    LS("} else {");
    ind++;
    L(cnt, " = 0");
    ind--;
    LS("}");
    CloseRzDest(rd, cnt);
    return { rd.hdr, ok };
}

}  // namespace goose
