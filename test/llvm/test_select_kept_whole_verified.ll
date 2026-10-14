; RUN: opt -load-pass-plugin=%cobra_pass -passes='cobra-simplify<verified;timeout=20000>' -S %s | FileCheck %s
; COBRA_PARAMS: timeout=20000

; A `select` on a condition the tree does not read - here a comparison of a
; narrower value - stays a leaf, because it can be the answer:
; `2 * ((s - v) | v) - (v ^ (s - v))` is `s`. Read through, the tree held the
; select's arms and condition instead, no semilinear rewrite said "the select",
; and only a cut of the tree was left: `v + (s - v)`.
; CHECK-LABEL: @select_kept_whole(
; CHECK: %s = select i1 %c, i64 %x, i64 %y
; CHECK-NEXT: ret i64 %s
define i64 @select_kept_whole(i8 %f, i64 %x, i64 %y, i64 %v) {
  %c = icmp eq i8 %f, 0
  %s = select i1 %c, i64 %x, i64 %y
  %d = sub i64 %s, %v
  %o = or i64 %d, %v
  %t = shl i64 %o, 1
  %e = xor i64 %v, %d
  %r = sub i64 %t, %e
  ret i64 %r
}
