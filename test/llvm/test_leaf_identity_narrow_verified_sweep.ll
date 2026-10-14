; RUN: opt -load-pass-plugin=%cobra_pass -passes='cobra-simplify<verified;sweep;families=semilinear;timeout=10000>' -S %s | FileCheck %s
; COBRA_PARAMS: families=semilinear;timeout=10000

; A tree that computes one of its own inputs, where that input is narrower than
; the tree.
;
; With `x = a ^ b`, the value
;   m = ((x >> 1) + ((a == b) ? 0 : 1 << 63)) - ((-x) >> 1)
; is `x` at every width: the two logical shifts of `x` and of `-x` each drop the
; bit the other keeps, and the sign bit the comparison contributes closes the
; case where `x` is odd. So `m ^ a` is `b` again, and an obfuscator writes a
; value it has just read this way.
;
; No technique of the search reads such a tree. The answer is not a combination
; of the leaves, it *is* one of them, and the comparison standing in for the
; carry is true on one input in 2^64, so no probe sees both of its sides and
; nothing fitted to the probes is sound. Only a proof can accept it.
;
; The width is what makes this a case of its own: `b` is read from an `i32`, so
; every reading of the tree masks that leaf to 32 bits. A candidate phrased as
; the bare variable is a different function, and the full-width probe that
; decides a leaf identity rejects it - which hid this family entirely until the
; candidate was phrased the way the tree reads the leaf (`LeafExpr`).

; CHECK-LABEL: @rebuilt_narrow_leaf(
; CHECK: %[[B:.*]] = zext i32 %brva to i64
; CHECK: ret i64 %[[B]]
define i64 @rebuilt_narrow_leaf(i64 %base, i32 %dirrva, i32 %brva) {
  %zdir = zext i32 %dirrva to i64
  %dir = add i64 %base, %zdir
  %a = add i64 %dir, 32
  %b = zext i32 %brva to i64
  %x = xor i64 %a, %b
  %negx = sub i64 0, %x
  %lo = lshr i64 %negx, 1
  %eq = icmp eq i64 %a, %b
  %hi = select i1 %eq, i64 0, i64 -9223372036854775808
  %shr = lshr i64 %x, 1
  %diff = sub i64 %hi, %lo
  %m = add i64 %diff, %shr
  %rebuilt = xor i64 %m, %a
  ret i64 %rebuilt
}

; The same identity with every leaf at the tree's own width, which is the shape
; that already folded: it is here so that a change to the leaf's reading cannot
; fix one and break the other.
; CHECK-LABEL: @rebuilt_wide_leaf(
; CHECK: ret i64 %b
define i64 @rebuilt_wide_leaf(i64 %a, i64 %b) {
  %x = xor i64 %a, %b
  %negx = sub i64 0, %x
  %lo = lshr i64 %negx, 1
  %eq = icmp eq i64 %a, %b
  %hi = select i1 %eq, i64 0, i64 -9223372036854775808
  %shr = lshr i64 %x, 1
  %diff = sub i64 %hi, %lo
  %m = add i64 %diff, %shr
  %rebuilt = xor i64 %m, %a
  ret i64 %rebuilt
}
