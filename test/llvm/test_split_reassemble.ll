; RUN: opt -load-pass-plugin=%cobra_pass -passes=cobra-simplify -S %s | FileCheck %s

; A word taken apart into narrower halves, each hidden under its own constant,
; and put back together. Every piece is narrower than the root, so the detector
; has to hold each at its own width (a trunc is a mask, a zext the identity on
; a value held that way), and the `add` under the xor has to stay a variable to
; the bitwise reading above it, which is what the boundary cut is for. The
; halves are then shifted atoms under coefficients that realign them, and the
; partition solve reads them back into one xor.

; CHECK-LABEL: @ptr_next(
; CHECK: %[[U:.*]] = add i64 %v, 4
; CHECK-NEXT: %[[R:.*]] = xor i64 6083334420050919884, %[[U]]
; CHECK-NEXT: ret i64 %[[R]]
define i64 @ptr_next(i64 %v) {
  %u = add i64 %v, 4
  %s = xor i64 %u, -3316038060006119497
  %t0 = trunc i64 %s to i16
  %h0 = xor i16 %t0, 9851
  %s1 = lshr i64 %s, 16
  %t1 = trunc i64 %s1 to i16
  %h1 = xor i16 %t1, -29022
  %s2 = lshr i64 %s, 32
  %t2 = trunc i64 %s2 to i16
  %h2 = xor i16 %t2, 18584
  %s3 = lshr i64 %s, 48
  %t3 = trunc nuw i64 %s3 to i16
  %h3 = xor i16 %t3, -31337
  %z3 = zext i16 %h3 to i64
  %w3 = shl nuw i64 %z3, 48
  %z2 = zext i16 %h2 to i64
  %w2 = shl nuw nsw i64 %z2, 32
  %m32 = or disjoint i64 %w2, %w3
  %z1 = zext i16 %h1 to i64
  %w1 = shl nuw nsw i64 %z1, 16
  %m321 = or disjoint i64 %m32, %w1
  %z0 = zext i16 %h0 to i64
  %r = or disjoint i64 %m321, %z0
  ret i64 %r
}

; CHECK-LABEL: @acc_split(
; CHECK: %[[R:.*]] = xor i32 -204210176, %acc
; CHECK-NEXT: ret i32 %[[R]]
define i32 @acc_split(i32 %acc) {
  %hi = lshr i32 %acc, 16
  %th = trunc nuw i32 %hi to i16
  %xh = xor i16 %th, -3116
  %lo = and i32 %acc, 65535
  %zh = zext i16 %xh to i32
  %sh = shl nuw i32 %zh, 16
  %r = or disjoint i32 %sh, %lo
  ret i32 %r
}

; CHECK-LABEL: @acc_next(
; CHECK: %[[R:.*]] = xor i32 -1710754204, %a
; CHECK-NEXT: ret i32 %[[R]]
define i32 @acc_next(i32 %a) {
  %x = xor i32 %a, 1775482468
  %lo = trunc i32 %a to i16
  %hl = xor i16 %lo, -2460
  %hi = lshr i32 %x, 16
  %th = trunc nuw i32 %hi to i16
  %hh = xor i16 %th, -3116
  %zh = zext i16 %hh to i32
  %sh = shl nuw i32 %zh, 16
  %zl = zext i16 %hl to i32
  %r = or disjoint i32 %sh, %zl
  ret i32 %r
}

; A narrow sum under a widening: the i16 add wraps at sixteen bits, and the
; model has to keep that. Nothing here folds, and nothing may be rewritten
; into the wrong width.
; CHECK-LABEL: @narrow_sum(
; CHECK: add i16
; CHECK: zext i16
define i64 @narrow_sum(i16 %a, i16 %b) {
  %s = add i16 %a, %b
  %z = zext i16 %s to i64
  %r = xor i64 %z, 65536
  ret i64 %r
}
