; RUN: opt -load-pass-plugin=%cobra_pass -passes='cobra-simplify<verified;boolean;two-valued-selects;families=semilinear+bitwise;timeout=20000>' -S %s | FileCheck %s
; COBRA_PARAMS: two-valued-selects;families=semilinear+bitwise;timeout=20000

; A part of a rewrite that is one of two constants, picked by one bit of
; another value, is built as the `select` of the two on that bit
; (`two-valued-selects`): LLVM's value ranges read the two numbers of a
; `select`, and nothing of `194 * (t & 1)`.

; `x` plus 194 times the low bit of `t`, the bit written `(t | -2) + 2` and the
; sum through `(x ^ w) + 2 * (x & w)`. The search answers `x + 194 * (t & 1)`,
; whose second term is `0` or `194` by the low bit of `t`.
;
; CHECK-LABEL: @sum_with_a_two_valued_term(
; CHECK: [[B:%[a-z0-9.]+]] = trunc i64 %t to i1
; CHECK-NEXT: [[P:%[a-z0-9.]+]] = select i1 [[B]], i64 194, i64 0
; CHECK-NEXT: [[S:%[a-z0-9.]+]] = add i64 %x, [[P]]
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

; Two values one bit apart say all they have to say through their known bits:
; `x + 128 * (t & 1)` is built as the expression spells it.
;
; CHECK-LABEL: @one_bit_apart(
; CHECK-NOT: select
; CHECK: ret i64
define i64 @one_bit_apart(i64 %x, i64 %t) {
  %u = or i64 %t, -2
  %v = add i64 %u, 2
  %w = mul i64 %v, 128
  %a = xor i64 %x, %w
  %b = and i64 %x, %w
  %c = shl i64 %b, 1
  %r = add i64 %a, %c
  ret i64 %r
}
