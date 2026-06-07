; RUN: opt -load-pass-plugin=%cobra_pass -passes=cobra-simplify -S %s | FileCheck %s

; Test: a 3-node MBA that feeds a store should still simplify through the
; producer instruction rewrite. (x & y) ^ y + (x & y) = y, so the stored value
; should collapse to %x and the MBA tree should disappear.
; CHECK-LABEL: @test_store_simplify
; CHECK-NOT: and i64
; CHECK-NOT: xor i64
; CHECK-NOT: add i64
; CHECK: store i64 %x, ptr %p, align 8
; CHECK-NEXT: ret ptr %p
define ptr @test_store_simplify(i64 %x, ptr %p) {
entry:
  %old = load i64, ptr %p, align 8
  %and = and i64 %old, %x
  %xor = xor i64 %and, %x
  %add = add i64 %xor, %and
  store i64 %add, ptr %p, align 8
  ret ptr %p
}