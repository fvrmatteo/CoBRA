; RUN: opt -load-pass-plugin=%cobra_pass -passes='cobra-simplify<verified;boolean;subterms;affine;narrow-roots;bit-intrinsics;families=semilinear+bitwise;timeout=20000>' -S %s | FileCheck %s
; COBRA_PARAMS: subterms;affine;narrow-roots;bit-intrinsics;families=semilinear+bitwise;timeout=20000

; The intrinsics flags are written with, read as the operations they are
; (`bit-intrinsics`): a reversal of bits as the bits moved, the overflow of a
; signed subtraction as the sign of `(a ^ b) & (a ^ (a - b))`.

declare i8 @llvm.ctpop.i8(i8)
declare i11 @llvm.bitreverse.i11(i11)
declare { i64, i1 } @llvm.ssub.with.overflow.i64(i64, i64)

; A count of bits stays a leaf: it is a parity flag over a whole byte, and read
; as eight terms where the IR holds one instruction, the rewrites over it cost
; more than they save.
;
; CHECK-LABEL: @counts_of_complements(
; CHECK: call i8 @llvm.ctpop.i8(i8 %v)
; CHECK: call i8 @llvm.ctpop.i8(i8 %n)
; CHECK: ret i8
define i8 @counts_of_complements(i8 %v) {
  %n = xor i8 %v, -1
  %a = call i8 @llvm.ctpop.i8(i8 %v)
  %b = call i8 @llvm.ctpop.i8(i8 %n)
  %s = add i8 %a, %b
  ret i8 %s
}

; A flag word with bit 0 of `x` put in bit 10 the way InstCombine writes it, a
; reversal of the eleven-bit value holding only that bit, and the same bit taken
; out again: what is left is the other flag. Only the bits of the reversed value
; that can be set are read.
;
; CHECK-LABEL: @flag_bit_moved(
; CHECK-NOT: bitreverse
; CHECK: [[F:%[a-z0-9.]+]] = and i64 %y, 64
; CHECK-NEXT: ret i64 [[F]]
define i64 @flag_bit_moved(i64 %x, i64 %y) {
  %t = trunc i64 %x to i11
  %b = and i11 %t, 1
  %r = call i11 @llvm.bitreverse.i11(i11 %b)
  %z = zext i11 %r to i64
  %f = and i64 %y, 64
  %w = or disjoint i64 %z, %f
  %lo = shl i64 %x, 10
  %m = and i64 %lo, 1024
  %d = sub i64 %w, %m
  ret i64 %d
}

; `jl` after `cmp -5, v`: the sign of the difference differs from the overflow
; flag. No byte subtracted from -5 overflows, and every difference is negative,
; so the condition is constant.
;
; CHECK-LABEL: @overflow_of_a_narrow_difference(
; CHECK-NEXT: ret i8 1
define i8 @overflow_of_a_narrow_difference(i8 %v) {
  %z = zext i8 %v to i64
  %o = call { i64, i1 } @llvm.ssub.with.overflow.i64(i64 -5, i64 %z)
  %of = extractvalue { i64, i1 } %o, 1
  %d = extractvalue { i64, i1 } %o, 0
  %s = lshr i64 %d, 63
  %s8 = trunc i64 %s to i1
  %x = xor i1 %of, %s8
  %r = zext i1 %x to i8
  ret i8 %r
}
