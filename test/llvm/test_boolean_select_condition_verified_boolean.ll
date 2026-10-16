; RUN: opt -load-pass-plugin=%cobra_pass -passes='cobra-simplify<verified;boolean;families=semilinear;timeout=20000>' -S %s | FileCheck %s
; COBRA_PARAMS: families=semilinear;timeout=20000

; Conditions that stand on a `select`, or are one.
;
; `@sign_select`: the high part is a `select` of the sign test `o <s 0`, the low bit
; the sign of `o`; the two are one condition, and the funnel shift is its sign
; extension - built from the comparison the function holds.
;
; `@selected_operand`: the value compared is itself chosen by a `select`. The
; condition is put back over it as read, `B ^ ((A ^ B) & -c)`, which is built as the
; `select` it is - spelled out in xors and masks it would cost more than the funnel
; shift it replaces.
;
; In `@sign_select` the comparison left is then one of a tree over `h` with a
; constant, which the equality reading takes over `m = h*P`: `m != -K`.
;
; CHECK-LABEL: @sign_select(
; CHECK-NOT: @llvm.fshl
; CHECK: icmp ne i64 %m, -2212647683481811901
;
; CHECK-LABEL: @selected_operand(
; CHECK-NOT: @llvm.fshl
; CHECK: icmp {{eq|ne}} i64 %d, 0
; CHECK: ret i1
define i1 @sign_select(i64 noundef %h) {
  %m = mul i64 %h, 1099511628211
  %a = add i64 %m, 2212647683481811901
  %b = sub i64 -2212647683481811901, %m
  %o = or i64 %a, %b
  %lt = icmp slt i64 %o, 0
  %s = select i1 %lt, i64 9223372036854775807, i64 0
  %f = call i64 @llvm.fshl.i64(i64 %s, i64 %o, i64 1)
  %c = icmp eq i64 %f, -1
  ret i1 %c
}

define i1 @selected_operand(i64 noundef %p, i64 noundef %q, i64 noundef %h) {
  %m = mul i64 %h, 1099511628211
  %neg = sub i64 0, %m
  %lt = icmp slt i64 %p, %q
  %y = select i1 %lt, i64 %neg, i64 %m
  %d = add i64 %y, -24041939960651397
  %e = sub i64 24041939960651397, %y
  %o = or i64 %d, %e
  %c = icmp ne i64 %d, 0
  %s = sext i1 %c to i64
  %f = call i64 @llvm.fshl.i64(i64 %s, i64 %o, i64 1)
  %r = icmp eq i64 %f, -1
  ret i1 %r
}

declare i64 @llvm.fshl.i64(i64, i64, i64)
