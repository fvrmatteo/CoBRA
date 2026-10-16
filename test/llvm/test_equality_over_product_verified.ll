; RUN: opt -load-pass-plugin=%cobra_pass -passes='cobra-simplify<verified;families=semilinear;timeout=5000>' -S %s | FileCheck %s
; COBRA_PARAMS: families=semilinear;timeout=5000

; A hash `m = x * P` compared with a constant through `m + k` and `x * -P - k`,
; which is InstCombine's form of `-(m + k)`: the sign of `(m + k) | -(m + k)` is
; clear exactly when `m == -k`. The tree is over `x`, where the test turns at
; `x == -k * P^-1`, and the question holds two products; read over `w = x * P`
; it holds none, and the comparison is written over `m`.
;
; CHECK-LABEL: @product_and_its_negation(
; CHECK: [[M:%.*]] = mul i64 %x, 1099511628211
; CHECK-NEXT: [[R:%.*]] = icmp eq i64 [[M]], -2212647683481811901
; CHECK-NEXT: ret i1 [[R]]
define i1 @product_and_its_negation(i64 %x) {
  %m = mul i64 %x, 1099511628211
  %a = add i64 %m, 2212647683481811901
  %n = mul i64 %x, -1099511628211
  %b = add i64 %n, -2212647683481811901
  %o = or i64 %a, %b
  %r = icmp sgt i64 %o, -1
  ret i1 %r
}

; The lowest set bit of `(m + k) & -(m + k)` is zero exactly when `m == -k`,
; tested through its decrement; `m` carries a flag the rewrite cannot keep.
;
; CHECK-LABEL: @lowest_set_bit_of_a_product(
; CHECK: [[M:%.*]] = mul i64 %x, 1099511628211
; CHECK-NEXT: [[R:%.*]] = icmp ne i64 [[M]], -2212647683481811901
; CHECK-NEXT: ret i1 [[R]]
define i1 @lowest_set_bit_of_a_product(i64 %x) {
  %m = mul nsw i64 %x, 1099511628211
  %a = add i64 %m, 2212647683481811901
  %n = mul i64 %x, -1099511628211
  %b = add i64 %n, -2212647683481811901
  %l = and i64 %a, %b
  %d = add i64 %l, -1
  %r = icmp sgt i64 %d, -1
  ret i1 %r
}

; With only `x * -P` in the tree, the change of variable is over that product,
; and the comparison is written over it.
;
; CHECK-LABEL: @negated_product_only(
; CHECK: [[N:%.*]] = mul i64 %x, -1099511628211
; CHECK-NEXT: [[R:%.*]] = icmp eq i64 [[N]], 2212647683481811901
; CHECK-NEXT: ret i1 [[R]]
define i1 @negated_product_only(i64 %x) {
  %n = mul i64 %x, -1099511628211
  %b = sub i64 2212647683481811901, %n
  %c = add i64 %n, -2212647683481811901
  %o = or i64 %b, %c
  %r = icmp sgt i64 %o, -1
  ret i1 %r
}

; A product compared with a range is no equality with one value: `m ^ -m` is below
; 1000 only where it is zero, for `m` 0 or 2^63 - and for an odd factor that is `x` 0
; or 2^63, every bit but the sign clear: a test of some of `x`'s bits.
;
; CHECK-LABEL: @product_range(
; CHECK: [[B:%.*]] = and i64 %x, 9223372036854775807
; CHECK-NEXT: [[R:%.*]] = icmp eq i64 [[B]], 0
; CHECK-NEXT: ret i1 [[R]]
define i1 @product_range(i64 %x) {
  %m = mul i64 %x, 1099511628211
  %n = mul i64 %x, -1099511628211
  %a = xor i64 %m, %n
  %r = icmp ult i64 %a, 1000
  ret i1 %r
}

; The control: a product in a range of a thousand values is no test of its bits, and
; stays.
;
; CHECK-LABEL: @product_in_a_range(
; CHECK: icmp ult i64
; CHECK: ret i1
define i1 @product_in_a_range(i64 %x) {
  %m = mul i64 %x, 1099511628211
  %a = add i64 %m, 7
  %r = icmp ult i64 %a, 1000
  ret i1 %r
}

; Two trees compared with each other are their exclusive or compared with zero:
; `(y & -y) == -(y & (y - 1))` holds exactly when `y == 0`, here `y = m + k`.
;
; CHECK-LABEL: @lowest_bit_against_the_rest(
; CHECK: [[M:%.*]] = mul i64 %x, 1099511628211
; CHECK-NEXT: [[R:%.*]] = icmp eq i64 [[M]], -2212647683481811901
; CHECK-NEXT: ret i1 [[R]]
define i1 @lowest_bit_against_the_rest(i64 %x) {
  %m = mul i64 %x, 1099511628211
  %y1 = add i64 %m, 2212647683481811900
  %y = add i64 %m, 2212647683481811901
  %n = sub i64 -2212647683481811901, %m
  %low = and i64 %y, %n
  %rest = and i64 %y, %y1
  %neg = sub i64 0, %rest
  %r = icmp eq i64 %low, %neg
  ret i1 %r
}

; Two trees equal for many values are no equality with one value: `x & 5` and `x & 3`
; agree exactly when bits 1 and 2 are clear - a test of those bits.
;
; CHECK-LABEL: @trees_equal_often(
; CHECK: [[B:%.*]] = and i64 %x, 6
; CHECK-NEXT: [[R:%.*]] = icmp eq i64 [[B]], 0
; CHECK-NEXT: ret i1 [[R]]
define i1 @trees_equal_often(i64 %x) {
  %a = and i64 %x, 5
  %b = and i64 %x, 3
  %r = icmp eq i64 %a, %b
  ret i1 %r
}

; The control: two trees that agree on a set no test of bits describes stay a
; comparison of the two.
;
; CHECK-LABEL: @trees_equal_on_no_pattern(
; CHECK: icmp eq i64
; CHECK: ret i1
define i1 @trees_equal_on_no_pattern(i64 %x) {
  %a = mul i64 %x, 3
  %b = add i64 %x, 1000
  %c = lshr i64 %a, 4
  %d = lshr i64 %b, 4
  %r = icmp eq i64 %c, %d
  ret i1 %r
}

; Two values tested for one difference through two differences computed apart:
; `(f - l - 32) | ((l + 32) - f)` has a clear sign bit exactly when
; `f - l == 32`, and the tree reads nothing twice.
;
; CHECK-LABEL: @difference_of_two_values(
; CHECK: [[D:%.*]] = sub i64 %f, %l
; CHECK-NEXT: [[R:%.*]] = icmp eq i64 [[D]], 32
; CHECK-NEXT: ret i1 [[R]]
define i1 @difference_of_two_values(i64 %f, i64 %l) {
  %d = sub i64 %f, %l
  %a = add i64 %d, -32
  %e = add i64 %l, 32
  %b = sub i64 %e, %f
  %o = or i64 %b, %a
  %r = icmp sgt i64 %o, -1
  ret i1 %r
}

; The control: two values compared through a tree that holds on no line.
;
; CHECK-LABEL: @no_line(
; CHECK: icmp
; CHECK-NOT: icmp eq i64 {{.*}}, 32
; CHECK: ret i1
define i1 @no_line(i64 %f, i64 %l) {
  %a = and i64 %f, %l
  %r = icmp ugt i64 %a, 32
  ret i1 %r
}
