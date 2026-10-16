; RUN: opt -load-pass-plugin=%cobra_pass -passes='cobra-simplify<verified;families=semilinear;timeout=5000>' -S %s | FileCheck %s
; COBRA_PARAMS: families=semilinear;timeout=5000

; A product split by the sign of what it multiplies: the magnitude, multiplied,
; negated back where the value was negative. Both arms are `x * k`, but only
; with the condition said: the false arm reads `x & INT_MAX`, which is `x` where
; `x` is not negative and nothing the true arm relates to otherwise. Read with
; the condition a free variable, the tree has no rewrite; with the condition put
; back, the proof relates products by a 64-bit constant and does not finish.
; Each arm read under its condition is linear: `-(-x * k)` and `x * k`.
;
; CHECK-LABEL: @sign_split_product(
; CHECK-NEXT: [[S:%.*]] = sext i8 %c to i64
; CHECK-NEXT: [[X:%.*]] = xor i64 %h, [[S]]
; CHECK-NEXT: [[M:%.*]] = mul i64 1099511628211, [[X]]
; CHECK-NEXT: ret i64 [[M]]
define i64 @sign_split_product(i64 noundef %h, i8 noundef %c) {
  %s = sext i8 %c to i64
  %x = xor i64 %h, %s
  %n = sub i64 0, %x
  %cmp = icmp slt i64 %x, 0
  %and = and i64 %x, 9223372036854775807
  %t = select i1 %cmp, i64 %n, i64 %and
  %mul = mul i64 %t, 1099511628211
  %neg = sub i64 0, %mul
  %r = select i1 %cmp, i64 %neg, i64 %mul
  ret i64 %r
}

; The same with the sign tested on another value that has the same sign bit:
; `h ^ sext(ashr(c, 7))` and `h ^ sext(c)` both have the sign of `h` exclusive
; or that of `c`. The false arm puts the sign back by hand from the tested
; value (`(x & INT_MAX) | (y & INT_MIN)`), which is `x` there too.
;
; CHECK-LABEL: @sign_tested_on_another_value(
; CHECK-NEXT: [[S:%.*]] = sext i8 %c to i64
; CHECK-NEXT: [[X:%.*]] = xor i64 %h, [[S]]
; CHECK-NEXT: [[M:%.*]] = mul i64 1099511628211, [[X]]
; CHECK-NEXT: ret i64 [[M]]
define i64 @sign_tested_on_another_value(i64 noundef %h, i8 noundef %c) {
  %b = ashr i8 %c, 7
  %e = sext i8 %b to i64
  %s = sext i8 %c to i64
  %y = xor i64 %h, %e
  %x = xor i64 %h, %s
  %low = and i64 %x, 9223372036854775807
  %cmp = icmp slt i64 %y, 0
  %high = and i64 %y, -9223372036854775808
  %back = or disjoint i64 %low, %high
  %n = sub i64 0, %x
  %t = select i1 %cmp, i64 %n, i64 %back
  %mul = mul i64 %t, 1099511628211
  %neg = sub i64 0, %mul
  %r = select i1 %cmp, i64 %neg, i64 %mul
  ret i64 %r
}

; The control: `h` may be `undef`, and then the value the condition tests and
; the one the arm masks need not agree on its sign. Nothing is read under the
; condition.
;
; CHECK-LABEL: @maybe_undef(
; CHECK: and i64 %x, 9223372036854775807
; CHECK: select i1 %cmp
define i64 @maybe_undef(i64 %h, i8 noundef %c) {
  %s = sext i8 %c to i64
  %x = xor i64 %h, %s
  %n = sub i64 0, %x
  %cmp = icmp slt i64 %x, 0
  %and = and i64 %x, 9223372036854775807
  %t = select i1 %cmp, i64 %n, i64 %and
  %mul = mul i64 %t, 1099511628211
  %neg = sub i64 0, %mul
  %r = select i1 %cmp, i64 %neg, i64 %mul
  ret i64 %r
}

; The control: the condition tests the sign of a value the arm does not mask,
; and says nothing of `x`.
;
; CHECK-LABEL: @another_sign(
; CHECK: and i64 %x, 9223372036854775807
; CHECK: select i1 %cmp
define i64 @another_sign(i64 noundef %h, i64 noundef %g) {
  %x = xor i64 %h, 12345
  %cmp = icmp slt i64 %g, 0
  %and = and i64 %x, 9223372036854775807
  %n = sub i64 0, %x
  %r = select i1 %cmp, i64 %n, i64 %and
  ret i64 %r
}
