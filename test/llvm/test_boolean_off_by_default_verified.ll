; RUN: opt -load-pass-plugin=%cobra_pass -passes='cobra-simplify<verified>' -S %s | FileCheck %s
; COBRA_ENV: COBRA_FAMILIES=semilinear

; The boolean reading is off unless asked for: with the solver and without it, the
; tree `test_boolean_sign_splat_funnel_verified_boolean.ll` folds stays as written.
;
; CHECK-LABEL: @sign_splat(
; CHECK: @llvm.fshl.i64
define i1 @sign_splat(i64 noundef %y, ptr %p) {
  %n = sub i64 0, %y
  %o = or i64 %y, %n
  %s = ashr i64 %o, 63
  %f = call i64 @llvm.fshl.i64(i64 %s, i64 %o, i64 1)
  %r = add i64 %f, 1
  store i64 %r, ptr %p
  %c = icmp eq i64 %f, -1
  ret i1 %c
}

declare i64 @llvm.fshl.i64(i64, i64, i64)
