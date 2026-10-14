; RUN: opt -load-pass-plugin=%cobra_pass -passes='cobra-simplify' -S %s | FileCheck %s

; A chain of byte `getelementptr`s is read as the sum of its indices at the
; width the target computes addresses in - here 32 bits - and rebuilt as one
; `getelementptr` of the simplified sum.

target datalayout = "e-p:32:32"

; (x ^ y) + 2 * (x & y), split across two steps of the chain, is x + y.
; CHECK-LABEL: @index_width_32
; CHECK: %[[SUM:.*]] = add i32
; CHECK: getelementptr i8, ptr %base, i32 %[[SUM]]
; CHECK-NOT: xor
define ptr @index_width_32(ptr %base, i32 %x, i32 %y) {
  %xo = xor i32 %x, %y
  %an = and i32 %x, %y
  %tw = shl i32 %an, 1
  %p1 = getelementptr i8, ptr %base, i32 %xo
  %p2 = getelementptr i8, ptr %p1, i32 %tw
  ret ptr %p2
}

; A narrower index is sign-extended to the index width first, which the sum
; does not say: the chain is left as it is.
; CHECK-LABEL: @index_extended
; CHECK: getelementptr i8, ptr %base, i16 %xo
; CHECK: getelementptr i8, ptr %p1, i16 %tw
define ptr @index_extended(ptr %base, i16 %x, i16 %y) {
  %xo = xor i16 %x, %y
  %an = and i16 %x, %y
  %tw = shl i16 %an, 1
  %p1 = getelementptr i8, ptr %base, i16 %xo
  %p2 = getelementptr i8, ptr %p1, i16 %tw
  ret ptr %p2
}

; A wider index is truncated to the index width, which the sum survives.
; CHECK-LABEL: @index_truncated
; CHECK: %[[WIDE:.*]] = add i64
; CHECK: getelementptr i8, ptr %base, i64 %[[WIDE]]
define ptr @index_truncated(ptr %base, i64 %x, i64 %y) {
  %xo = xor i64 %x, %y
  %an = and i64 %x, %y
  %tw = shl i64 %an, 1
  %p1 = getelementptr i8, ptr %base, i64 %xo
  %p2 = getelementptr i8, ptr %p1, i64 %tw
  ret ptr %p2
}
