; RUN: opt -load-pass-plugin=%cobra_pass -passes='cobra-simplify<verified;timeout=20000>' -S %s | FileCheck %s
; COBRA_PARAMS: timeout=20000

; A saturating addition or subtraction is the wrapped result or the bound it
; clamps to, chosen by its overflow test: a `select` over a comparison, which
; the model already writes. Left as an opaque leaf, a test spelled with one -
; `ssub.sat(a, b) == k` for "a - b is k and does not overflow", which is how a
; compiler's flag synthesis can spell an equality - is a variable of its own,
; apart from the operands it reads, and no identity that holds because of what
; it tests can be seen.
;
; The expansion puts comparisons in the tree, so these are verified tests.

; `(x >> 1) - (-x >> 1)` is `x` with the sign bit flipped unless `x` is zero,
; and adding the sign bit back when `x` is not zero gives `x`. Here `x` is
; `(d + 32) ^ n`, and its zero test is spelled over `d` and `n` instead: `n` is
; 32 above `d`, without overflowing, exactly when `x` is zero (the top half of
; `n` is known clear, so the difference cannot wrap onto 32). So the tree is
; `n` displaced by `b`.
; CHECK-LABEL: @carry_from_a_subtraction(
; CHECK-NOT: llvm.ssub.sat
; CHECK-NOT: lshr
; CHECK: [[SUM:%.*]] = add i64 %b, %n
; CHECK-NEXT: ret i64 [[SUM]]
define i64 @carry_from_a_subtraction(i64 %b, i32 %offset, i32 %narrow) {
  %o = zext i32 %offset to i64
  %d = add i64 %b, %o
  %a = add i64 %d, 32
  %n = zext i32 %narrow to i64
  %x = xor i64 %a, %n
  %negx = sub i64 0, %x
  %sat = call i64 @llvm.ssub.sat.i64(i64 %n, i64 %d)
  %zero = icmp eq i64 %sat, 32
  %low = lshr i64 %negx, 1
  %carry = select i1 %zero, i64 0, i64 -9223372036854775808
  %high = lshr i64 %x, 1
  %diff = sub nsw i64 %high, %low
  %corrected = add i64 %diff, %carry
  %y = xor i64 %corrected, %a
  %r = add i64 %y, %b
  ret i64 %r
}

; The same correction with the zero test spelled as a saturating sum: for
; `u = 32 - c`, `sadd.sat(u, u & ~64) == 64` holds exactly when `c` is zero, and
; the tree is `c + 32`.
; CHECK-LABEL: @carry_from_a_sum(
; CHECK-NOT: llvm.sadd.sat
; CHECK-NOT: lshr
; CHECK: [[SUM:%.*]] = add i64 %c, 32
; CHECK-NEXT: ret i64 [[SUM]]
define i64 @carry_from_a_sum(i64 %c) {
  %u = sub i64 32, %c
  %v = and i64 %u, -65
  %a = add i64 %c, 32
  %bit = and i64 %a, 32
  %w = sub i64 %bit, %a
  %sat = call i64 @llvm.sadd.sat.i64(i64 %u, i64 %v)
  %zero = icmp eq i64 %sat, 64
  %low = lshr i64 %w, 1
  %carry = select i1 %zero, i64 0, i64 -9223372036854775808
  %half = lshr i64 %a, 1
  %high = and i64 %half, 9223372036854775791
  %sum = add nuw i64 %high, %bit
  %diff = sub i64 %sum, %low
  %r = add i64 %diff, %carry
  ret i64 %r
}

; The unsigned forms: a saturating difference is the difference from the
; smaller operand, and a saturating sum is the complement of the saturating
; difference of the complement.
; CHECK-LABEL: @unsigned_forms(
; CHECK-NOT: llvm.usub.sat
; CHECK-NOT: llvm.uadd.sat
; CHECK-NEXT: ret i64 %a
define i64 @unsigned_forms(i64 %a, i64 %b) {
  %sub = call i64 @llvm.usub.sat.i64(i64 %a, i64 %b)
  %min = call i64 @llvm.umin.i64(i64 %a, i64 %b)
  %back = add i64 %sub, %min
  %na = xor i64 %a, -1
  %sum = call i64 @llvm.uadd.sat.i64(i64 %a, i64 %b)
  %mirror = call i64 @llvm.usub.sat.i64(i64 %na, i64 %b)
  %nmirror = xor i64 %mirror, -1
  %zero = xor i64 %sum, %nmirror
  %r = add i64 %back, %zero
  ret i64 %r
}

; A signed saturating difference of two values whose top halves are known
; clear cannot overflow, so it is the plain difference: what is added back
; leaves the first operand.
; CHECK-LABEL: @signed_difference_in_range(
; CHECK-NOT: llvm.ssub.sat
; CHECK: [[A:%.*]] = zext i32 %narrow_a to i64
; CHECK-NEXT: ret i64 [[A]]
define i64 @signed_difference_in_range(i32 %narrow_a, i32 %narrow_b) {
  %a = zext i32 %narrow_a to i64
  %b = zext i32 %narrow_b to i64
  %d = call i64 @llvm.ssub.sat.i64(i64 %a, i64 %b)
  %bx = xor i64 %b, 1311768467463790320
  %back = xor i64 %bx, 1311768467463790320
  %r = add i64 %d, %back
  ret i64 %r
}

declare i64 @llvm.ssub.sat.i64(i64, i64)
declare i64 @llvm.sadd.sat.i64(i64, i64)
declare i64 @llvm.usub.sat.i64(i64, i64)
declare i64 @llvm.uadd.sat.i64(i64, i64)
declare i64 @llvm.umin.i64(i64, i64)
