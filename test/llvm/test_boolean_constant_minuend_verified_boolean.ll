; RUN: opt -load-pass-plugin=%cobra_pass -passes='cobra-simplify<verified-boolean>' -S %s | FileCheck %s
; COBRA_ENV: COBRA_FAMILIES=semilinear COBRA_TIMEOUT_MS=20000

; A hash compared with a constant through the sign of `d | -d`: `d = h*P + K` and
; `-d` is written `-K - h*P`. The funnel shift is the spread sign itself, so the
; comparison of it with -1 is `d != 0`. The rewrite reads that sign as the function
; already computes it, which needs `-K + -(h*P)` built as the `sub` it is: built as
; a negation and an add it matches no instruction, the whole cone is spelled again
; and the rewrite costs more than the funnel shift it replaces.
;
; CHECK-LABEL: @hash_compare(
; CHECK-NOT: @llvm.fshl
; CHECK: %s = ashr i64 %o, 63
; CHECK: icmp eq i64 %s, -1
define i1 @hash_compare(i64 noundef %h) {
  %m = mul i64 %h, 1099511628211
  %a = add i64 %m, 4937148438292785925
  %b = sub i64 -4937148438292785925, %m
  %o = or i64 %a, %b
  %s = ashr i64 %o, 63
  %f = call i64 @llvm.fshl.i64(i64 %s, i64 %o, i64 1)
  %c = icmp eq i64 %f, -1
  ret i1 %c
}

declare i64 @llvm.fshl.i64(i64, i64, i64)
