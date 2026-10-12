; RUN: opt -load-pass-plugin=%cobra_pass -passes='cobra-simplify<verified-boolean>' -S %s | FileCheck %s
; COBRA_ENV: COBRA_FAMILIES=semilinear COBRA_TIMEOUT_MS=20000

; A condition and the negation of another spelling of it. The `select` picks 0 when
; `v == 5` and the largest signed value otherwise, so shifted up by one it is 0 or
; all ones but the lowest bit; the low bit is the sign of `-zext(v - 5)`, which is
; `v != 5`. The funnel shift is therefore 0 when `v == 5` and -1 otherwise:
; `zext(v == 5) - 1`.
;
; The sign is merged with the comparison as its negation, on the solver's proof.
;
; CHECK-LABEL: @negated_condition(
; CHECK-NOT: @llvm.fshl
; CHECK: icmp eq i64 %{{.*}}, 5
; CHECK: zext i1
; CHECK: add i64 -1,
define i64 @negated_condition(i8 noundef %v) {
  %c = icmp eq i8 %v, 5
  %a = select i1 %c, i64 0, i64 9223372036854775807
  %d = add i8 %v, -5
  %z = zext i8 %d to i64
  %b = sub nsw i64 0, %z
  %f = call i64 @llvm.fshl.i64(i64 %a, i64 %b, i64 1)
  ret i64 %f
}

declare i64 @llvm.fshl.i64(i64, i64, i64)
