; RUN: opt -load-pass-plugin=%cobra_pass -passes='cobra-simplify<verified;families=semilinear;timeout=5000>' -S %s | FileCheck %s
; COBRA_PARAMS: families=semilinear;timeout=5000

; A value compared with a constant through a tree that is true for exactly one
; value of it: an equality in disguise. `(x - k) | (k - x)` has a clear sign bit
; exactly when `x == k`.
;
; CHECK-LABEL: @sign_of_both_differences(
; CHECK-NEXT: [[R:%.*]] = icmp eq i64 %x, 4072225995569359896
; CHECK-NEXT: ret i1 [[R]]
define i1 @sign_of_both_differences(i64 %x) {
  %a = add i64 %x, -4072225995569359896
  %b = sub i64 4072225995569359896, %x
  %o = or i64 %b, %a
  %r = icmp sgt i64 %o, -1
  ret i1 %r
}

; The same test of `f - l` through a `smax`: the tree reads `f - l` twice, and
; held whole it is the one value tested.
;
; CHECK-LABEL: @through_smax(
; CHECK-NEXT: [[D:%.*]] = sub i64 %f, %l
; CHECK-NEXT: [[R:%.*]] = icmp ne i64 [[D]], 32
; CHECK-NEXT: ret i1 [[R]]
define i1 @through_smax(i64 %f, i64 %l) {
  %d = sub i64 %f, %l
  %a = add i64 %d, -32
  %b = sub i64 32, %d
  %o = or i64 %a, %b
  %s = and i64 %o, -9223372036854775808
  %m = call i64 @llvm.smax.i64(i64 %s, i64 -1)
  %r = icmp eq i64 %m, -1
  ret i1 %r
}

; The lowest set bit of `(x + k) & (-k - x)` is zero exactly when `x == -k`,
; tested through its decrement.
;
; CHECK-LABEL: @lowest_set_bit(
; CHECK-NEXT: [[R:%.*]] = icmp ne i64 %x, -2212647683481811901
; CHECK-NEXT: ret i1 [[R]]
define i1 @lowest_set_bit(i64 %x) {
  %a = add i64 %x, 2212647683481811901
  %b = sub i64 -2212647683481811901, %x
  %l = and i64 %a, %b
  %d = add i64 %l, -1
  %r = icmp sgt i64 %d, -1
  ret i1 %r
}

; `((2 * x) | c) ^ x` is a bijection of `x`: compared with `k` it holds at one value,
; which is none of the tree's constants and neither `k` nor `-k`, but `~k`.
;
; CHECK-LABEL: @complement_of_the_constant(
; CHECK-NEXT: [[R:%.*]] = icmp eq i64 %x, 8910050711687093894
; CHECK-NEXT: ret i1 [[R]]
define i1 @complement_of_the_constant(i64 %x) {
  %d = shl i64 %x, 1
  %o = or i64 %d, 626642650335363827
  %t = xor i64 %o, %x
  %r = icmp eq i64 %t, -8910050711687093895
  ret i1 %r
}

; Two values on a line, `(c - l) + f == k`, with `c - l` read by two comparisons: no
; `add` with a constant is left to fold in front of either, and the line `f - l ==
; k - c` goes through no constant of the tree. Both are read where the tree meets the
; constant, and the shared term is left to nobody.
;
; CHECK-LABEL: @line_through_a_shared_constant(
; CHECK-NOT: sub i64 -33
; CHECK: [[A:%.*]] = sub i64 %first, %l
; CHECK-NEXT: icmp eq i64 [[A]], 32
; CHECK: [[B:%.*]] = sub i64 %next, %l
; CHECK-NEXT: icmp eq i64 [[B]], 32
; CHECK: ret void
define void @line_through_a_shared_constant(i64 %l, i64 %first, i64 %next, ptr %out) {
  %s = sub i64 -33, %l
  %a = add i64 %first, %s
  %t1 = icmp eq i64 %a, -1
  %z1 = zext i1 %t1 to i8
  store i8 %z1, ptr %out
  %b = add i64 %next, %s
  %t2 = icmp eq i64 %b, -1
  %z2 = zext i1 %t2 to i8
  %p = getelementptr i8, ptr %out, i64 1
  store i8 %z2, ptr %p
  ret void
}

; The control: a comparison true for many values is no equality, and stays.
;
; CHECK-LABEL: @range_test(
; CHECK: icmp slt i64
; CHECK: ret i1
define i1 @range_test(i64 %x) {
  %a = add i64 %x, -100
  %b = xor i64 %a, 5
  %r = icmp slt i64 %b, 7
  ret i1 %r
}

declare i64 @llvm.smax.i64(i64, i64)
