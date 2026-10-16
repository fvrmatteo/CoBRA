; RUN: opt -load-pass-plugin=%cobra_pass -passes='cobra-simplify<verified;families=semilinear;timeout=5000>' -S %s | FileCheck %s
; COBRA_PARAMS: families=semilinear;timeout=5000

; Two trees compared for equality whose difference is affine in the one value:
; `(h ^ c) + 1 - (-2 * (h & c))` is `h + c + 1`, so the comparison turns at
; `h == ~c`, which none of the tree's constants is (`c`, `1`, `-2` and their
; negations). The point is interpolated from the difference and proved.
;
; CHECK-LABEL: @two_trees(
; CHECK: [[R:%.*]] = icmp eq i64 %h, -2212647683481811901
; CHECK-NEXT: ret i1 [[R]]
define i1 @two_trees(i64 %h) {
  %x = xor i64 %h, 2212647683481811900
  %a = add i64 %x, 1
  %n = and i64 %h, 2212647683481811900
  %m = mul nsw i64 %n, -2
  %r = icmp eq i64 %a, %m
  ret i1 %r
}

; The control: a difference with an even slope, `2h + 2c`, is equal to `k` at two
; values of `h` or at none, and is no equality with one value.
;
; CHECK-LABEL: @even_slope(
; CHECK-NOT: icmp eq i64 %h,
; CHECK: ret i1
define i1 @even_slope(i64 %h) {
  %x = xor i64 %h, 2212647683481811900
  %n = and i64 %h, 2212647683481811900
  %m = shl i64 %n, 1
  %s = add i64 %x, %m
  %d = shl i64 %s, 1
  %r = icmp eq i64 %d, 4425295366963625800
  ret i1 %r
}
