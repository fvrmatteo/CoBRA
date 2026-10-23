; RUN: opt -load-pass-plugin=%cobra_pass -passes='cobra-simplify<verified;boolean;affine;narrow-roots;families=semilinear+bitwise;timeout=20000>' -S %s | FileCheck %s
; COBRA_PARAMS: affine;narrow-roots;families=semilinear+bitwise;timeout=20000

; Roots narrower than the values they read, read at the width of those values
; (`narrow-roots`), with the affine reading (`affine`) that answers them.

; A byte flag: whether a 64-bit sum is above a constant, the sum built from
; carries picked by testing the narrow `v` against zero. When `v` is zero the
; sum is 4660, and otherwise 4660 - 26; neither is above -11, so the flag is
; zero. At eight bits the comparison of 64-bit values is a leaf, and a free
; leaf is no constant.
;
; CHECK-LABEL: @flag_of_a_wide_comparison(
; CHECK-NEXT: ret i8 0
define i8 @flag_of_a_wide_comparison(i8 %v) {
  %zero = icmp eq i8 %v, 0
  %carry = select i1 %zero, i64 0, i64 -9223372036854775808
  %some = icmp ne i8 %v, 0
  %a = select i1 %some, i64 -13, i64 -12
  %s1 = add i64 %carry, %a
  %b = select i1 %some, i64 9223372036854775795, i64 12
  %s2 = add i64 %s1, %b
  %s3 = add i64 %s2, 4660
  %above = icmp ugt i64 %s3, -11
  %flag = zext i1 %above to i8
  ret i8 %flag
}

; A 16-bit sum written through halvings of a 64-bit extension: `a + r` as
; `a - (~r >> 1) + ((r + 2^17 - 1) >> 1)` over `r = zext v`, the halves taken back
; to sixteen bits, and the whole xored with a constant. At sixteen bits each
; halving is a leaf - the shift brings down a bit from above them - and the two
; are independent; at 64 bits they are halves of the same `r`. The answer reads
; only the low bits of what it reads, so it is built at sixteen bits.
;
; CHECK-LABEL: @narrow_sum_of_wide_halvings(
; CHECK-NOT: lshr
; CHECK: [[L:%[a-z0-9.]+]] = trunc i64 %a to i16
; CHECK-NEXT: [[V:%[a-z0-9.]+]] = trunc i32 %v to i16
; CHECK-NEXT: [[S:%[a-z0-9.]+]] = add i16 [[L]], [[V]]
; CHECK-NEXT: [[K:%[a-z0-9.]+]] = xor i16 [[S]], 23130
; CHECK-NEXT: ret i16 [[K]]
define i16 @narrow_sum_of_wide_halvings(i64 %a, i32 %v) {
  %low = trunc i64 %a to i16
  %r = zext i32 %v to i64
  %nr = xor i64 %r, -1
  %h1 = lshr i64 %nr, 1
  %t1 = trunc i64 %h1 to i16
  %d = sub i16 %low, %t1
  %up = add nuw nsw i64 %r, 131071
  %h2 = lshr i64 %up, 1
  %t2 = trunc i64 %h2 to i16
  %s = add i16 %d, %t2
  %k = xor i16 %s, 23130
  ret i16 %k
}
