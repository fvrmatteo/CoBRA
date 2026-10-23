; RUN: opt -load-pass-plugin=%cobra_pass -passes='cobra-simplify<verified;boolean;families=semilinear+bitwise;timeout=20000>' -S %s | FileCheck %s
; COBRA_PARAMS: families=semilinear+bitwise;timeout=20000

; Without `two-valued-selects` a two-valued term is built as the expression
; spells it.
;
; CHECK-LABEL: @sum_with_a_two_valued_term(
; CHECK-NOT: select
; CHECK: [[A:%[a-z0-9.]+]] = and i64 %t, 1
; CHECK-NEXT: [[M:%[a-z0-9.]+]] = mul i64 194, [[A]]
; CHECK-NEXT: [[S:%[a-z0-9.]+]] = add i64 %x, [[M]]
; CHECK-NEXT: ret i64 [[S]]
define i64 @sum_with_a_two_valued_term(i64 %x, i64 %t) {
  %u = or i64 %t, -2
  %v = add i64 %u, 2
  %w = mul i64 %v, 194
  %a = xor i64 %x, %w
  %b = and i64 %x, %w
  %c = shl i64 %b, 1
  %r = add i64 %a, %c
  ret i64 %r
}
