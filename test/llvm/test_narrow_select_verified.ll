; RUN: opt -load-pass-plugin=%cobra_pass -passes='cobra-simplify<verified>' -S %s | FileCheck %s

; A `select` narrower than the tree it sits in. The top sixteen bits of %r are
; hidden under a constant, run through `((A & B) ^ B) + (A & B)` with `A & B`
; spelt as the carry-aware average less half of `A ^ B`, and put back over the
; low forty-eight. The carry of the sixteen-bit sum is not computed at sixteen
; bits: it is a comparison of the whole word, picked into a sixteen-bit value
; by the `select`. Left as a leaf that `select` is a free variable, and with a
; free carry the identity is false; held like any other narrow value - its arms
; are in range, and it hands one of them on untouched - the tree has one
; variable and is the identity on it.

; CHECK-LABEL: @carry_from_the_whole_word(
; CHECK-NEXT: ret i64 %r
define i64 @carry_from_the_whole_word(i64 %r) {
  %moved = add i64 %r, 5452505456
  %ah = lshr i64 %moved, 48
  %at = trunc i64 %ah to i16
  %a = xor i16 %at, 19862
  %bh = lshr i64 %r, 48
  %bt = trunc i64 %bh to i16
  %b = xor i16 %bt, 19862
  %sum = add i16 %a, %b
  %sumh = lshr i16 %sum, 1
  %wraps = icmp slt i64 %r, -5452505456
  %top = select i1 %wraps, i16 -32768, i16 0
  %avg = or i16 %sumh, %top
  %x = xor i16 %at, %bt
  %half = lshr i16 %x, 1
  %both = sub i16 %avg, %half
  %t = xor i16 %both, %b
  %high16 = add i16 %t, %both
  %high = zext i16 %high16 to i64
  %placed = shl i64 %high, 48
  %low = and i64 %r, 281474976710655
  %joined = or i64 %placed, %low
  %out = xor i64 %joined, 5590655987427049472
  ret i64 %out
}
