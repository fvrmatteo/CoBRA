; RUN: opt -load-pass-plugin=%cobra_pass -passes='cobra-simplify<verified;boolean;affine;families=semilinear+bitwise;timeout=20000>' -S %s | FileCheck %s
; COBRA_PARAMS: affine;families=semilinear+bitwise;timeout=20000

; Without `narrow-roots` a byte root is read at eight bits, where the
; comparison of 64-bit values under it is a leaf: the flag stays as written.
;
; CHECK-LABEL: @flag_of_a_wide_comparison(
; CHECK: icmp ugt i64
; CHECK: [[F:%[a-z0-9.]+]] = zext i1 {{%[a-z0-9.]+}} to i8
; CHECK-NEXT: ret i8 [[F]]
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
