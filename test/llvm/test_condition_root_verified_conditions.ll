; RUN: opt -load-pass-plugin=%cobra_pass -passes='cobra-simplify<verified;conditions;timeout=5000>' -S %s | FileCheck %s
; COBRA_PARAMS: timeout=5000

; A condition nothing but a branch reads is a root of its own
; (`CobraPassOptions::read_conditions`), read at the width of what it compares
; and on the one bit the branch looks at.

; The shape an obfuscator leaves where it tests an address against a field it
; derived from it: the answer is sign-extended, rotated by one, and compared
; against a value whose top half is known zero. A rotation of a word whose bits
; are all the same bit is that word, and `-1` is above every 32-bit value, so the
; whole chain is the first comparison - which nothing could see while the only
; reader was a branch.
;
; CHECK-LABEL: @rotate_of_a_condition(
; CHECK-NOT: fshl
; CHECK-NOT: sext
; CHECK: [[C:%.*]] = icmp ne i64 %base, %neg
; CHECK-NEXT: br i1 [[C]],
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

; The ordinary reading is unchanged by the condition above it: the difference of
; a disjunction and a conjunction is the exclusive or, and the comparison that
; reads it is left as it is.
;
; CHECK-LABEL: @xor_under_a_comparison(
; CHECK: xor i64 %a, %b
; CHECK: icmp ult
define i64 @xor_under_a_comparison(i64 %a, i64 %b) {
entry:
  %or = or i64 %a, %b
  %and = and i64 %a, %b
  %x = sub i64 %or, %and
  %t = icmp ult i64 %x, 1
  br i1 %t, label %same, label %other

same:
  ret i64 1

other:
  ret i64 0
}

; A condition an integer value reads as well is not rooted at: that value is a
; tree of its own and reads the condition as part of it, which is the larger
; question and the better root. The reader is rewritten, and what the branch
; reads stays until a later run meets it with the reader gone.
;
; CHECK-LABEL: @an_integer_reads_it_too(
; CHECK: [[C:%.*]] = icmp ne i64 %base, %neg
; CHECK: fshl
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

; The control: a comparison of two values and nothing else. There is no tree
; under it to read, and it is left exactly as it is.
;
; CHECK-LABEL: @nothing_under_it(
; CHECK: [[T:%.*]] = icmp eq i64 %a, %b
; CHECK-NEXT: br i1 [[T]],
define i64 @nothing_under_it(i64 %a, i64 %b) {
entry:
  %t = icmp eq i64 %a, %b
  br i1 %t, label %same, label %other

same:
  ret i64 1

other:
  ret i64 0
}

declare i64 @llvm.fshl.i64(i64, i64, i64)
