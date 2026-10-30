; A 128-bit product of two 64-bit values read through a truncation is written in
; 64-bit operations before anything is collected (`narrow-products`): its low
; word is the 64-bit product; its high part over an exact reciprocal is the
; quotient it computes - `(x * 99098522500536821) >> 88` is `x / 3123003270` on
; every 64-bit `x` - and any other high part is the high word built from 32-bit
; halves. A product read whole, by something other than a truncation, stays.
;
; RUN: opt -load-pass-plugin=%cobra_pass -passes='cobra-simplify<verified;narrow-products;timeout=2000>' -S %s | FileCheck %s
; COBRA_PARAMS: narrow-products;timeout=2000

; CHECK-LABEL: @low_word(
; CHECK-NOT: i128
; CHECK: mul i64 %a, %b
define i64 @low_word(i64 %a, i64 %b) {
  %wa = zext i64 %a to i128
  %wb = zext i64 %b to i128
  %p = mul i128 %wa, %wb
  %low = trunc i128 %p to i64
  ret i64 %low
}

; CHECK-LABEL: @quotient(
; CHECK-NOT: i128
; CHECK: udiv i64 %x, 3123003270
define i64 @quotient(i64 %x) {
  %wx = zext i64 %x to i128
  %p = mul nuw nsw i128 %wx, 99098522500536821
  %high = lshr i128 %p, 88
  %q = trunc nuw nsw i128 %high to i64
  ret i64 %q
}

; CHECK-LABEL: @high_word(
; CHECK-NOT: i128
; CHECK: ret i64
define i64 @high_word(i64 %a, i64 %b) {
  %wa = zext i64 %a to i128
  %wb = zext i64 %b to i128
  %p = mul i128 %wa, %wb
  %high = lshr i128 %p, 64
  %h = trunc i128 %high to i64
  ret i64 %h
}

; Not a reciprocal: the error of 2^64 / 3 rounded up is too large for 2^64 itself.
; CHECK-LABEL: @not_a_quotient(
; CHECK-NOT: udiv
; CHECK: ret i64
define i64 @not_a_quotient(i64 %x) {
  %wx = zext i64 %x to i128
  %p = mul i128 %wx, 6148914691236517206
  %high = lshr i128 %p, 64
  %h = trunc i128 %high to i64
  ret i64 %h
}

; CHECK-LABEL: @read_whole(
; CHECK: mul i128
define i1 @read_whole(i64 %a, i64 %b) {
  %wa = zext i64 %a to i128
  %wb = zext i64 %b to i128
  %p = mul i128 %wa, %wb
  %big = icmp ugt i128 %p, 18446744073709551615
  ret i1 %big
}
