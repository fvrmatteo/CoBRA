; RUN: opt -load-pass-plugin=%cobra_pass -passes='cobra-simplify<verified;boolean;subterms;affine;narrow-roots;two-valued-selects;shared-values;families=semilinear+bitwise;timeout=20000>' -S %s | FileCheck %s
; COBRA_PARAMS: subterms;affine;narrow-roots;two-valued-selects;shared-values;families=semilinear+bitwise;timeout=20000

; More fixtures of other files with every intermediate also handed to an opaque
; call (`shared-values`), the ones whose proofs take longest.

; A step whose products are also read by calls
; (`test_hoist_constant_factor_verified.ll`): the common factor is taken out of
; the root's sum all the same, and the root is the product of `h ^ c`.
;
; CHECK-LABEL: @multiplied_step_shared(
; CHECK: [[X:%[0-9]+]] = xor i64 %h, %c
; CHECK: [[R:%[a-z0-9.]+]] = mul i64 [[X]], 1099511628211
; CHECK-NEXT: ret i64 [[R]]
define i64 @multiplied_step_shared(i64 %h, i64 %c) {
  %1 = xor i64 %h, %c
  call void @cobra.opaque.i64(i64 %1)
  %2 = sub i64 0, %1
  call void @cobra.opaque.i64(i64 %2)
  %eq = icmp eq i64 %h, %c
  call void @cobra.opaque.i1(i1 %eq)
  %n = xor i64 %1, -1
  call void @cobra.opaque.i64(i64 %n)
  %neg = icmp slt i64 %1, 0
  call void @cobra.opaque.i1(i1 %neg)
  %3 = or i64 %n, 1
  call void @cobra.opaque.i64(i64 %3)
  %sh = lshr i64 %n, 1
  call void @cobra.opaque.i64(i64 %sh)
  %4 = lshr i64 %2, 1
  call void @cobra.opaque.i64(i64 %4)
  %5 = select i1 %eq, i64 -9223372036854775808, i64 0
  call void @cobra.opaque.i64(i64 %5)
  %6 = add i64 %4, %3
  call void @cobra.opaque.i64(i64 %6)
  %7 = sub i64 %6, %sh
  call void @cobra.opaque.i64(i64 %7)
  %8 = add i64 %7, %5
  call void @cobra.opaque.i64(i64 %8)
  %9 = sub i64 %1, %8
  call void @cobra.opaque.i64(i64 %9)
  %10 = select i1 %neg, i64 0, i64 %9
  call void @cobra.opaque.i64(i64 %10)
  %11 = or i64 %10, %7
  call void @cobra.opaque.i64(i64 %11)
  %sl = shl i64 %11, 1
  call void @cobra.opaque.i64(i64 %sl)
  %12 = xor i64 %10, %8
  call void @cobra.opaque.i64(i64 %12)
  %13 = sub i64 %sl, %12
  call void @cobra.opaque.i64(i64 %13)
  %14 = mul i64 %13, 1099511628211
  call void @cobra.opaque.i64(i64 %14)
  %m1 = mul i64 %12, 1099511628211
  call void @cobra.opaque.i64(i64 %m1)
  %m2 = mul i64 %11, -2199023256422
  call void @cobra.opaque.i64(i64 %m2)
  %a = add i64 %m1, %m2
  call void @cobra.opaque.i64(i64 %a)
  %r = select i1 %neg, i64 %a, i64 %14
  ret i64 %r
}

;
; CHECK-LABEL: @fnv32_step_split_on_a_sign_shared(
; CHECK: [[R:%[a-z0-9.]+]] = and i64 %m, 4294967295
; CHECK-NEXT: ret i64 [[R]]
define i64 @fnv32_step_split_on_a_sign_shared(i64 noundef %h, i8 noundef %b) {
  %c = sext i8 %b to i64
  call void @cobra.opaque.i64(i64 %c)
  %and = and i64 %h, %c
  call void @cobra.opaque.i64(i64 %and)
  %or = or i64 %h, %c
  call void @cobra.opaque.i64(i64 %or)
  %x = xor i64 %h, %c
  call void @cobra.opaque.i64(i64 %x)
  %t1 = trunc nuw i64 %and to i32
  call void @cobra.opaque.i32(i32 %t1)
  %t2 = trunc i64 %or to i32
  call void @cobra.opaque.i32(i32 %t2)
  %cond = icmp sgt i32 %t1, %t2
  call void @cobra.opaque.i1(i1 %cond)
  %a1 = mul nsw i64 %x, -16777619
  call void @cobra.opaque.i64(i64 %a1)
  %a2 = and i64 %a1, 4278189676
  call void @cobra.opaque.i64(i64 %a2)
  %a3 = and i64 %a1, 4294967295
  call void @cobra.opaque.i64(i64 %a3)
  %a4 = lshr i64 %a3, 1
  call void @cobra.opaque.i64(i64 %a4)
  %a5 = xor i64 %a4, 8388809
  call void @cobra.opaque.i64(i64 %a5)
  %a6 = add nsw i64 %a3, -16777619
  call void @cobra.opaque.i64(i64 %a6)
  %a7 = lshr i64 %a6, 1
  call void @cobra.opaque.i64(i64 %a7)
  %a8 = sub nsw i64 %a5, %a7
  call void @cobra.opaque.i64(i64 %a8)
  %a9 = xor i64 %a2, %a8
  call void @cobra.opaque.i64(i64 %a9)
  %a10 = xor i64 %a9, 4278189676
  call void @cobra.opaque.i64(i64 %a10)
  %a11 = add i64 %a10, 1
  call void @cobra.opaque.i64(i64 %a11)
  %m = mul nsw i64 %x, 16777619
  call void @cobra.opaque.i64(i64 %m)
  %v = select i1 %cond, i64 %a11, i64 %m
  call void @cobra.opaque.i64(i64 %v)
  %r = and i64 %v, 4294967295
  ret i64 %r
}

;
; CHECK-LABEL: @complemented_byte_step_shared(
; CHECK: [[R:%[a-z0-9.]+]] = mul i64 {{%[a-z0-9.]+}}, 1099511628211
; CHECK-NEXT: ret i64 [[R]]
define i64 @complemented_byte_step_shared(i64 %h, i8 %b) {
  %c = sext i8 %b to i64
  call void @cobra.opaque.i64(i64 %c)
  %1 = xor i64 %h, %c
  call void @cobra.opaque.i64(i64 %1)
  %2 = sub i64 0, %1
  call void @cobra.opaque.i64(i64 %2)
  %eq = icmp eq i64 %h, %c
  call void @cobra.opaque.i1(i1 %eq)
  %nb = xor i8 %b, -1
  call void @cobra.opaque.i8(i8 %nb)
  %nc = sext i8 %nb to i64
  call void @cobra.opaque.i64(i64 %nc)
  %n = xor i64 %h, %nc
  call void @cobra.opaque.i64(i64 %n)
  %neg = icmp slt i64 %1, 0
  call void @cobra.opaque.i1(i1 %neg)
  %3 = or i64 %n, 1
  call void @cobra.opaque.i64(i64 %3)
  %sh = lshr i64 %n, 1
  call void @cobra.opaque.i64(i64 %sh)
  %4 = lshr i64 %2, 1
  call void @cobra.opaque.i64(i64 %4)
  %5 = select i1 %eq, i64 -9223372036854775808, i64 0
  call void @cobra.opaque.i64(i64 %5)
  %6 = add i64 %4, %3
  call void @cobra.opaque.i64(i64 %6)
  %7 = sub i64 %6, %sh
  call void @cobra.opaque.i64(i64 %7)
  %8 = add i64 %7, %5
  call void @cobra.opaque.i64(i64 %8)
  %9 = sub i64 %1, %8
  call void @cobra.opaque.i64(i64 %9)
  %10 = select i1 %neg, i64 0, i64 %9
  call void @cobra.opaque.i64(i64 %10)
  %11 = or i64 %10, %7
  call void @cobra.opaque.i64(i64 %11)
  %sl = shl i64 %11, 1
  call void @cobra.opaque.i64(i64 %sl)
  %12 = xor i64 %10, %8
  call void @cobra.opaque.i64(i64 %12)
  %13 = sub i64 %sl, %12
  call void @cobra.opaque.i64(i64 %13)
  %t = shl i64 %11, 1
  call void @cobra.opaque.i64(i64 %t)
  %d = sub i64 %12, %t
  call void @cobra.opaque.i64(i64 %d)
  %s = select i1 %neg, i64 %d, i64 %13
  call void @cobra.opaque.i64(i64 %s)
  %r = mul i64 %s, 1099511628211
  ret i64 %r
}

declare void @cobra.opaque.i1(i1)
declare void @cobra.opaque.i32(i32)
declare void @cobra.opaque.i64(i64)
declare void @cobra.opaque.i8(i8)
