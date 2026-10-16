; RUN: opt -load-pass-plugin=%cobra_pass -passes='cobra-simplify<verified;families=semilinear;timeout=5000>' -S %s | FileCheck %s
; COBRA_PARAMS: families=semilinear;timeout=5000

; `sext(~b)` is `~sext(b)`: an extension copies a bit into the bits above it,
; and a bitwise operation with a constant does the same to every bit. The step
; below is `(h ^ sext(b)) * k`, one of its operands spelled `h ^ sext(~b)`. Read
; with each extension as a leaf - which it has to be, the byte under them is
; refused at full width - the two extensions are two variables, independent to
; the search, and nothing holds for every pair of them. Written over the
; extension the function already computes, `sext(~b)` is `sext(b) ^ -1`, and the
; step is linear in `h` and `sext(b)`.
;
; CHECK-LABEL: @complemented_byte_step(
; CHECK-NEXT: [[C:%.*]] = sext i8 %b to i64
; CHECK-NEXT: [[X:%.*]] = xor i64
; CHECK-NEXT: [[M:%.*]] = mul i64 [[X]], 1099511628211
; CHECK-NEXT: ret i64 [[M]]
define i64 @complemented_byte_step(i64 %h, i8 %b) {
  %c = sext i8 %b to i64
  %1 = xor i64 %h, %c
  %2 = sub i64 0, %1
  %eq = icmp eq i64 %h, %c
  %nb = xor i8 %b, -1
  %nc = sext i8 %nb to i64
  %n = xor i64 %h, %nc
  %neg = icmp slt i64 %1, 0
  %3 = or i64 %n, 1
  %sh = lshr i64 %n, 1
  %4 = lshr i64 %2, 1
  %5 = select i1 %eq, i64 -9223372036854775808, i64 0
  %6 = add i64 %4, %3
  %7 = sub i64 %6, %sh
  %8 = add i64 %7, %5
  %9 = sub i64 %1, %8
  %10 = select i1 %neg, i64 0, i64 %9
  %11 = or i64 %10, %7
  %sl = shl i64 %11, 1
  %12 = xor i64 %10, %8
  %13 = sub i64 %sl, %12
  %t = shl i64 %11, 1
  %d = sub i64 %12, %t
  %s = select i1 %neg, i64 %d, i64 %13
  %r = mul i64 %s, 1099511628211
  ret i64 %r
}

; The control: no extension of the byte itself is computed, so nothing is
; rewritten over one.
;
; CHECK-LABEL: @no_other_extension(
; CHECK: xor i8 %b, -1
; CHECK: sext i8
define i64 @no_other_extension(i64 %h, i8 %b) {
  %nb = xor i8 %b, -1
  %nc = sext i8 %nb to i64
  %l = and i64 %h, %nc
  ret i64 %l
}
