; An addition written as `(p ^ z) + 2 * (p & z)` over a product of two arguments,
; `p = x * y`. A linear reading cannot hold the product, but it holds the sum above
; it: the product is read as one more leaf, and the root becomes `p + z`. (The whole
; root used to be declined for the product its tree reached.)
;
; RUN: opt -load-pass-plugin=%cobra_pass -passes='cobra-simplify<verified>' -S %s | FileCheck %s
; COBRA_PARAMS: verified
;
; CHECK-LABEL: @sum_over_product(
; CHECK: [[P:%.*]] = mul i64 %x, %y
; CHECK-NOT: = xor
; CHECK-NOT: = and
; CHECK: add i64
; CHECK: ret i64
define i64 @sum_over_product(i64 %x, i64 %y, i64 %z) {
  %p = mul i64 %x, %y
  %a = xor i64 %p, %z
  %b = and i64 %p, %z
  %c = shl i64 %b, 1
  %r = add i64 %a, %c
  ret i64 %r
}
