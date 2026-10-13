; RUN: opt -load-pass-plugin=%cobra_pass -passes='cobra-simplify' -S %s | FileCheck %s

; Without a solver a tree holding a comparison is not rewritten: the
; comparison is true on one input in 2^64, and the probes a rewrite is fitted
; to never see it. The tree here has no expression - a shift by a variable
; amount is collected but not expressible - so the comparison has to be
; recognised in the tree itself. Fitted to probes, the root folds to `%x`,
; which is wrong wherever `3 * %x` is the constant.

; CHECK-LABEL: @cmp_no_expr(
; CHECK: icmp eq i64 %t, 305419896
; CHECK-NOT: ret i64 %x
; CHECK: ret i64
define i64 @cmp_no_expr(i64 %x, i64 %a, i64 %s) {
  %w = shl i64 %a, %s
  %t = mul i64 %x, 3
  %c = icmp eq i64 %t, 305419896
  %z = zext i1 %c to i64
  %xo = xor i64 %x, %w
  %an = and i64 %x, %w
  %mu = mul i64 %an, 2
  %sum = add i64 %xo, %mu
  %d = sub i64 %sum, %w
  %r = add i64 %d, %z
  ret i64 %r
}
