; RUN: opt -load-pass-plugin=%cobra_pass -passes='cobra-simplify<verified;timeout=5000>' -S %s | FileCheck %s
; COBRA_PARAMS: timeout=5000

; Reading a condition as a root of its own is off unless it is asked for
; (`CobraPassOptions::read_conditions`): a comparison is one bit wide, and a
; function of nothing but tests would pay for a tree at every branch. The same
; expression as in `test_condition_root_verified_conditions.ll` is left alone
; here - the rotation, the sign extension and both comparisons.
;
; CHECK-LABEL: @rotate_of_a_condition(
; CHECK: sext i1
; CHECK: fshl
; CHECK: icmp ugt
define i64 @rotate_of_a_condition(i64 %base, i32 %va) {
entry:
  %v = zext i32 %va to i64
  %neg = sub nsw i64 0, %v
  %c = icmp ne i64 %base, %neg
  %s = sext i1 %c to i64
  %r = call i64 @llvm.fshl.i64(i64 %s, i64 %s, i64 1)
  %t = icmp ugt i64 %r, %v
  br i1 %t, label %found, label %missing

found:
  ret i64 %v

missing:
  ret i64 0
}

; What an integer value reads is rewritten with the option off as well: the
; reading that is off is the one that makes a condition a root, not the one that
; follows a condition into a tree.
;
; CHECK-LABEL: @an_integer_reads_it_too(
; CHECK: [[C:%.*]] = icmp ne i64 %base, %neg
; CHECK: [[W:%.*]] = zext i1 [[C]] to i64
; CHECK: ret i64 [[W]]
define i64 @an_integer_reads_it_too(i64 %base, i32 %va) {
entry:
  %v = zext i32 %va to i64
  %neg = sub nsw i64 0, %v
  %c = icmp ne i64 %base, %neg
  %s = sext i1 %c to i64
  %r = call i64 @llvm.fshl.i64(i64 %s, i64 %s, i64 1)
  %t = icmp ugt i64 %r, %v
  %kept = zext i1 %t to i64
  br i1 %t, label %found, label %missing

found:
  ret i64 %kept

missing:
  ret i64 0
}

declare i64 @llvm.fshl.i64(i64, i64, i64)
