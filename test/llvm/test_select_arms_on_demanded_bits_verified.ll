; RUN: opt -load-pass-plugin=%cobra_pass -passes='cobra-simplify<verified;families=semilinear;timeout=5000>' -S %s | FileCheck %s
; COBRA_PARAMS: families=semilinear;timeout=5000

; A `select` whose only reader keeps its low half, over two arms that agree in
; the low half: a 32-bit step computed once as `w = x * P` and once as the
; negation of `x * -P`. Neither arm is the other as a 64-bit word, so no reading
; of the arms makes them one value; on the bits read they are, and the `select`
; is the arm that takes less with it.
;
; CHECK-LABEL: @masked_arms(
; CHECK-NOT: select
; CHECK: [[W:%.*]] = mul i64 %x, 16777619
; CHECK-NEXT: [[R:%.*]] = and i64 [[W]], 4294967295
; CHECK-NEXT: ret i64 [[R]]
define i64 @masked_arms(i64 noundef %x, i1 %c) {
  %w = mul i64 %x, 16777619
  %n = mul i64 %x, -16777619
  %u = and i64 %n, 4294967295
  %t = sub i64 4294967296, %u
  %s = select i1 %c, i64 %t, i64 %w
  %r = and i64 %s, 4294967295
  ret i64 %r
}

; The arm kept may be read where the condition chose the other, so it loses the
; flags that would make it poison there.
;
; CHECK-LABEL: @flags_of_the_arm_kept(
; CHECK-NOT: select
; CHECK: mul i64 %x, 16777619
; CHECK-NOT: mul nsw
; CHECK: ret i64
define i64 @flags_of_the_arm_kept(i64 noundef %x, i1 %c) {
  %w = mul nsw i64 %x, 16777619
  %n = mul i64 %x, -16777619
  %u = and i64 %n, 4294967295
  %t = sub i64 4294967296, %u
  %s = select i1 %c, i64 %t, i64 %w
  %r = and i64 %s, 4294967295
  ret i64 %r
}

; The control: every bit of the `select` is read, and the arms differ in the
; high half.
;
; CHECK-LABEL: @every_bit_read(
; CHECK: select
; CHECK: ret i64
define i64 @every_bit_read(i64 noundef %x, i1 %c) {
  %w = mul i64 %x, 16777619
  %n = mul i64 %x, -16777619
  %u = and i64 %n, 4294967295
  %t = sub i64 4294967296, %u
  %s = select i1 %c, i64 %t, i64 %w
  ret i64 %s
}

; The control: the arms differ in the bits read.
;
; CHECK-LABEL: @arms_differ(
; CHECK: select
; CHECK: ret i64
define i64 @arms_differ(i64 noundef %x, i1 %c) {
  %w = mul i64 %x, 16777619
  %t = add i64 %w, 1
  %s = select i1 %c, i64 %t, i64 %w
  %r = and i64 %s, 4294967295
  ret i64 %r
}

; The control: a leaf that may be `undef` is read by each arm on its own.
;
; CHECK-LABEL: @undefined_leaf(
; CHECK: select
; CHECK: ret i64
define i64 @undefined_leaf(i64 %x, i1 %c) {
  %w = mul i64 %x, 16777619
  %n = mul i64 %x, -16777619
  %u = and i64 %n, 4294967295
  %t = sub i64 4294967296, %u
  %s = select i1 %c, i64 %t, i64 %w
  %r = and i64 %s, 4294967295
  ret i64 %r
}

; An FNV-1a-32 step as an obfuscated program computes it: held in 64 bits,
; split on a sign test (bit 31 of `h ^ c`), with the true arm a
; borrow-aware halving of the negated product that agrees with the product in
; the low half only. The arms read `x` through two products, `x * -P` and
; `x * P`; over `w = x * P` the question holds none.
;
; CHECK-LABEL: @fnv32_step_split_on_a_sign(
; CHECK-NOT: select
; CHECK: [[M:%.*]] = mul i64 %x, 16777619
; CHECK-NEXT: [[R:%.*]] = and i64 [[M]], 4294967295
; CHECK-NEXT: ret i64 [[R]]
define i64 @fnv32_step_split_on_a_sign(i64 noundef %h, i8 noundef %b) {
  %c = sext i8 %b to i64
  %and = and i64 %h, %c
  %or = or i64 %h, %c
  %x = xor i64 %h, %c
  %t1 = trunc nuw i64 %and to i32
  %t2 = trunc i64 %or to i32
  %cond = icmp sgt i32 %t1, %t2
  %a1 = mul nsw i64 %x, -16777619
  %a2 = and i64 %a1, 4278189676
  %a3 = and i64 %a1, 4294967295
  %a4 = lshr i64 %a3, 1
  %a5 = xor i64 %a4, 8388809
  %a6 = add nsw i64 %a3, -16777619
  %a7 = lshr i64 %a6, 1
  %a8 = sub nsw i64 %a5, %a7
  %a9 = xor i64 %a2, %a8
  %a10 = xor i64 %a9, 4278189676
  %a11 = add i64 %a10, 1
  %m = mul nsw i64 %x, 16777619
  %v = select i1 %cond, i64 %a11, i64 %m
  %r = and i64 %v, 4294967295
  ret i64 %r
}
