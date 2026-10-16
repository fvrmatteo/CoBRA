; RUN: opt -load-pass-plugin=%cobra_pass -passes='cobra-simplify<verified;families=semilinear;timeout=5000>' -S %s | FileCheck %s
; COBRA_PARAMS: families=semilinear;timeout=5000

; The same step with the constant multiplied in on every path: `k * a + (-2k) * b`
; on one arm of the select, `k * c` on the other, which is
; `k * (s ? a - 2b : c)`. The search finds `k * (h ^ c)` either way, but the proof
; of the tree as written relates three products by a 64-bit constant and does not
; finish; with the common factor taken out first the select under it is a root
; with nothing to multiply, proved at once, and the product is applied to it.
;
; CHECK-LABEL: @multiplied_step(
; CHECK-NEXT: [[X:%.*]] = xor i64
; CHECK-NEXT: [[M:%.*]] = mul i64 [[X]], 1099511628211
; CHECK-NEXT: ret i64 [[M]]
define i64 @multiplied_step(i64 %h, i64 %c) {
  %1 = xor i64 %h, %c
  %2 = sub i64 0, %1
  %eq = icmp eq i64 %h, %c
  %n = xor i64 %1, -1
  %neg = icmp slt i64 %1, 0
  %3 = or i64 %n, 1
  %sh = lshr i64 %n, 1
  %4 = lshr i64 %2, 1
  %5 = select i1 %eq, i64 -9223372036854775808, i64 0
  %6 = add i64 %4, %3
  %7 = sub i64 %6, %sh
  %8 = add i64 %7, %5
  %9 = sub i64 %1, %8
  %10 = select i1 %neg, i64 0, i64 %9
  %11 = or i64 %10, %7
  %sl = shl i64 %11, 1
  %12 = xor i64 %10, %8
  %13 = sub i64 %sl, %12
  %14 = mul i64 %13, 1099511628211
  %m1 = mul i64 %12, 1099511628211
  %m2 = mul i64 %11, -2199023256422
  %a = add i64 %m1, %m2
  %r = select i1 %neg, i64 %a, i64 %14
  ret i64 %r
}

; The control: products whose multipliers share no factor stay as they are.
;
; CHECK-LABEL: @coprime(
; CHECK: mul i64 %x, 1099511628211
; CHECK: mul i64 %y, 16777619
; CHECK: ret i64
define i64 @coprime(i64 %x, i64 %y) {
  %a = mul i64 %x, 1099511628211
  %b = mul i64 %y, 16777619
  %r = add i64 %a, %b
  ret i64 %r
}
