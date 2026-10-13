; RUN: opt -load-pass-plugin=%cobra_pass -passes='cobra-simplify' -S %s | FileCheck %s

; A leaf is read under what is known of its bits, and a mask on it that only
; says what those bits already say is left out of the rewrite. Known bits hold
; of a value only while it is not poison, though, and `urem %p, 16` is poison
; with `%p`. The tree reads it through a `freeze`, so the rewrite freezes it
; too - and a frozen poison is any value at all, with no bit known. The mask
; stays: `x + freeze(l)` could be any value, where the tree is at most `x + 15`.

; CHECK-LABEL: @frozen_masked_leaf(
; CHECK: %[[F:.*]] = freeze i64 %l
; CHECK: and i64 %[[F]], 15
; CHECK: ret i64
define i64 @frozen_masked_leaf(i64 %p, i64 %x) {
  %l = urem i64 %p, 16
  %f = freeze i64 %l
  %m = and i64 %f, 15
  %a = xor i64 %m, %x
  %b = and i64 %m, %x
  %b2 = mul i64 %b, 2
  %r = add i64 %a, %b2
  ret i64 %r
}

; Without the `freeze` the leaf's poison is the root's, and the mask its known
; bits imply is left out.
; CHECK-LABEL: @masked_leaf(
; CHECK-NOT: and
; CHECK: %[[R:.*]] = add i64 %x, %l
; CHECK-NEXT: ret i64 %[[R]]
define i64 @masked_leaf(i64 %p, i64 %x) {
  %l = urem i64 %p, 16
  %m = and i64 %l, 15
  %a = xor i64 %m, %x
  %b = and i64 %m, %x
  %b2 = mul i64 %b, 2
  %r = add i64 %a, %b2
  ret i64 %r
}
