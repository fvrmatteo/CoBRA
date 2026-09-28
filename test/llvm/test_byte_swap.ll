; RUN: opt -load-pass-plugin=%cobra_pass -passes=cobra-simplify -S %s | FileCheck %s

; A byte swap is a fixed permutation of the value's bytes, and so a bitwise
; operation like any other once the width is known. Left as an opaque leaf it
; cuts the value it permutes off from everything read through it, which is where
; an identity written over the same value fails: here the low byte of the
; swapped word and the top byte of the original are the same byte, so their xor
; is zero and everything built on it cancels. With the swap as a leaf the two
; are unrelated variables and nothing cancels; modelled as the shifts and masks
; it expands to, the zero is plain.

; CHECK-LABEL: @swapped_low_byte(
; CHECK-NOT: llvm.bswap
; CHECK-NEXT: ret i32 %k
define i32 @swapped_low_byte(i32 %x, i32 %k) {
  %s = tail call i32 @llvm.bswap.i32(i32 %x)
  %lo = and i32 %s, 255
  %hi = lshr i32 %x, 24
  %hi2 = and i32 %hi, 255
  %zero = xor i32 %lo, %hi2
  %r = add i32 %k, %zero
  ret i32 %r
}

; The same for the widest swap, where the permutation is eight terms rather than
; four: byte 1 of the swapped word is byte 6 of the original.
; CHECK-LABEL: @swapped_second_byte(
; CHECK-NOT: llvm.bswap
; CHECK-NEXT: ret i64 %k
define i64 @swapped_second_byte(i64 %x, i64 %k) {
  %s = tail call i64 @llvm.bswap.i64(i64 %x)
  %a = lshr i64 %s, 8
  %a2 = and i64 %a, 255
  %b = lshr i64 %x, 48
  %b2 = and i64 %b, 255
  %zero = xor i64 %a2, %b2
  %r = add i64 %k, %zero
  ret i64 %r
}

declare i32 @llvm.bswap.i32(i32)
declare i64 @llvm.bswap.i64(i64)
