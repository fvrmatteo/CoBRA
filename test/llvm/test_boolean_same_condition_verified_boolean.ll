; RUN: opt -load-pass-plugin=%cobra_pass -passes='cobra-simplify<verified;boolean;families=semilinear;timeout=20000>' -S %s | FileCheck %s
; COBRA_PARAMS: families=semilinear;timeout=20000

; Two spellings of one condition. `c` compares a 16-bit value with 90; the sign of
; `-((w + 65446) & 0xFFFF)` - the negated difference `w - 90` at 16 bits - is set
; exactly when that difference is not zero, which is `c` again. The funnel shift
; puts the first in the high bits and the second in the low one, so it is the sign
; extension of `c`, and `f + 1` is `w == 90`.
;
; The comparison is narrower than the tree, so the ordinary collection keeps it a
; leaf; the boolean cut takes it in, so that the solver can be asked whether the
; sign is the same condition - and only its proof makes the two one variable.
;
; CHECK-LABEL: @same_condition(
; CHECK-NOT: @llvm.fshl
; CHECK: icmp eq i64 %{{.*}}, 90
; CHECK: zext i1
; CHECK: ret i64
define i64 @same_condition(i16 noundef %w) {
  %c = icmp ne i16 %w, 90
  %s = sext i1 %c to i64
  %z = zext i16 %w to i64
  %d = add nuw nsw i64 %z, 65446
  %m = and i64 %d, 65535
  %b = sub nsw i64 0, %m
  %f = call i64 @llvm.fshl.i64(i64 %s, i64 %b, i64 1)
  %r = add i64 %f, 1
  ret i64 %r
}

declare i64 @llvm.fshl.i64(i64, i64, i64)
