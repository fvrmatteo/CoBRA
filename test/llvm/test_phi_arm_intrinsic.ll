; RUN: opt -load-pass-plugin=%cobra_pass -passes='cobra-simplify' -S %s | FileCheck %s

; A phi is read through when its arms are instructions the walk reads, and a
; rotate is one: here both arms rotate the same value, so the phi is the rotate
; the root subtracts, and the root is zero. Asked only of the plain opcodes,
; the arms were calls and the phi a leaf.

; CHECK-LABEL: @phi_of_rotates(
; CHECK: m:
; CHECK-NEXT: ret i64 0
define i64 @phi_of_rotates(i1 %c, i64 %x) {
entry:
  br i1 %c, label %t, label %e
t:
  %r0 = call i64 @llvm.fshl.i64(i64 %x, i64 %x, i64 5)
  br label %m
e:
  %r1 = call i64 @llvm.fshl.i64(i64 %x, i64 %x, i64 5)
  br label %m
m:
  %p = phi i64 [ %r0, %t ], [ %r1, %e ]
  %q = call i64 @llvm.fshl.i64(i64 %x, i64 %x, i64 5)
  %xo = xor i64 %p, %q
  %an = and i64 %p, %q
  %mu = mul i64 %an, 2
  %sum = add i64 %xo, %mu
  %two = shl i64 %q, 1
  %r = sub i64 %sum, %two
  ret i64 %r
}

declare i64 @llvm.fshl.i64(i64, i64, i64)
