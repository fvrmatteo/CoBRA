; RUN: opt -load-pass-plugin=%cobra_pass -passes='cobra-simplify<verified-boolean>' -S %s | FileCheck %s
; COBRA_ENV: COBRA_FAMILIES=semilinear COBRA_TIMEOUT_MS=20000

; `d != 0` and the sign of `d | (K - y)` are one condition, with `d = y - K` and
; `y` a `select` of a hash and its negation; the funnel shift is its sign
; extension. Put back over `y`, the condition reads the hash's leaf `h` through
; both arms of the `select`. A `select` stops the poison of the arm it does not
; take, so a leaf under its arms is frozen before a rewrite reads it - but `h` is
; under both arms, poison in whichever is taken, and so poison in the `select` and
; in the root: it needs no freeze. Frozen, it would match nothing the function
; holds, every value above it would be built again, and the rewrite would cost
; more than the funnel shift; read as it is, the rewrite is the comparison of `d`
; the function already computes.
;
; CHECK-LABEL: @select_both_arms(
; CHECK-NOT: freeze
; CHECK-NOT: @llvm.fshl
; CHECK: %d = add i64 %y, -24041939960651397
; CHECK: icmp eq i64 %d, 0
define i1 @select_both_arms(i64 %p, i64 %q, i64 %h) {
  %m = mul i64 %h, 1099511628211
  %neg = sub i64 0, %m
  %lt = icmp slt i64 %p, %q
  %y = select i1 %lt, i64 %neg, i64 %m
  %d = add i64 %y, -24041939960651397
  %c = icmp ne i64 %d, 0
  %s = sext i1 %c to i64
  %e = sub i64 24041939960651397, %y
  %o = or i64 %d, %e
  %f = call i64 @llvm.fshl.i64(i64 %s, i64 %o, i64 1)
  %r = icmp eq i64 %f, -1
  ret i1 %r
}

declare i64 @llvm.fshl.i64(i64, i64, i64)
