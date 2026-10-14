; RUN: opt -load-pass-plugin=%cobra_pass -passes='cobra-simplify<verified>' -S %s | FileCheck %s

; A shift by a variable amount has no expression - the model shifts by
; constants - so it is a leaf of the tree, like any other value the model does
; not read. Collected into the tree it left the tree with no expression at all:
; with the solver on, nothing to prove a rewrite against, so the whole root was
; declined and recorded as examined.

; CHECK-LABEL: @variable_shl(
; CHECK: %w = shl i64 %a, %s
; CHECK-NEXT: %[[R:.*]] = add i64 %x, %w
; CHECK-NEXT: ret i64 %[[R]]
define i64 @variable_shl(i64 %x, i64 %a, i64 %s) {
  %w = shl i64 %a, %s
  %xo = xor i64 %x, %w
  %an = and i64 %x, %w
  %mu = mul i64 %an, 2
  %r = add i64 %xo, %mu
  ret i64 %r
}

; CHECK-LABEL: @variable_lshr(
; CHECK: %w = lshr i64 %a, %s
; CHECK-NEXT: %[[R:.*]] = add i64 %x, %w
; CHECK-NEXT: ret i64 %[[R]]
define i64 @variable_lshr(i64 %x, i64 %a, i64 %s) {
  %w = lshr i64 %a, %s
  %xo = xor i64 %x, %w
  %an = and i64 %x, %w
  %mu = mul i64 %an, 2
  %r = add i64 %xo, %mu
  ret i64 %r
}
