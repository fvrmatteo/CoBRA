; The last step of a hash taken one byte at a time, written two ways under a test
; of its sign: `-y` for `y = x * -k` the long way round, `((~y << 1) | 2) - (y ^ -2)`,
; and `x * k`. Both are `x * k`. Every root of the step collects its tree down to all
; six bytes, more leaves than `max-vars`, and only a root read with its tree cut short
; (`wide-roots`) holds the identity over the one value it is computed from. Its proof
; is a question about two products by dense constants, answered over `w = x * -k`.
; The `select` is then of two products a later InstCombine makes one value.
;
; RUN: opt -load-pass-plugin=%cobra_pass -passes='cobra-simplify<verified;wide-roots;max-vars=4;timeout=2000>' -S %s | FileCheck %s
; COBRA_PARAMS: wide-roots;max-vars=4;timeout=2000
;
; CHECK-LABEL: @last_step(
; CHECK: [[X:%.*]] = xor i64 %h4, %e5
; CHECK-NOT: shl
; CHECK-NOT: or i64
; CHECK: mul i64 {{.*}}1099511628211
; CHECK: select i1 %negative
; CHECK: ret i64
define i64 @last_step(ptr %bytes) {
  %p0 = getelementptr i8, ptr %bytes, i64 0
  %b0 = load i8, ptr %p0, align 1
  %e0 = sext i8 %b0 to i64
  %x0 = xor i64 -3750763034362895579, %e0
  %h0 = mul i64 %x0, 1099511628211
  %p1 = getelementptr i8, ptr %bytes, i64 1
  %b1 = load i8, ptr %p1, align 1
  %e1 = sext i8 %b1 to i64
  %x1 = xor i64 %h0, %e1
  %h1 = mul i64 %x1, 1099511628211
  %p2 = getelementptr i8, ptr %bytes, i64 2
  %b2 = load i8, ptr %p2, align 1
  %e2 = sext i8 %b2 to i64
  %x2 = xor i64 %h1, %e2
  %h2 = mul i64 %x2, 1099511628211
  %p3 = getelementptr i8, ptr %bytes, i64 3
  %b3 = load i8, ptr %p3, align 1
  %e3 = sext i8 %b3 to i64
  %x3 = xor i64 %h2, %e3
  %h3 = mul i64 %x3, 1099511628211
  %p4 = getelementptr i8, ptr %bytes, i64 4
  %b4 = load i8, ptr %p4, align 1
  %e4 = sext i8 %b4 to i64
  %x4 = xor i64 %h3, %e4
  %h4 = mul i64 %x4, 1099511628211
  %p5 = getelementptr i8, ptr %bytes, i64 5
  %b5 = load i8, ptr %p5, align 1
  %e5 = sext i8 %b5 to i64
  %x5 = xor i64 %h4, %e5
  %negative = icmp slt i64 %x5, 0
  %y = mul i64 %x5, -1099511628211
  %n = xor i64 %y, -1
  %s = shl i64 %n, 1
  %o = or i64 %s, 2
  %t = xor i64 %y, -2
  %long = sub i64 %o, %t
  %product = mul i64 %x5, 1099511628211
  %h5 = select i1 %negative, i64 %long, i64 %product
  ret i64 %h5
}
