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
