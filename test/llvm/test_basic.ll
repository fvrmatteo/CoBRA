; RUN: opt -load-pass-plugin=%cobra_pass -passes='cobra-simplify' -S %s | FileCheck %s

; Test: (x ^ y) + 2 * (x & y) should simplify to x + y
; CHECK-LABEL: @test_xor_and
; CHECK: %cobra.add = add i64 %x, %y
; CHECK-NEXT: ret i64 %cobra.add
define i64 @test_xor_and(i64 %x, i64 %y) {
entry:
  %xor = xor i64 %x, %y
  %and = and i64 %x, %y
  %mul = mul i64 %and, 2
  %add = add i64 %xor, %mul
  ret i64 %add
}
