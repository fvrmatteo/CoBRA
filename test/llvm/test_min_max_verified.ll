; RUN: opt -load-pass-plugin=%cobra_pass -passes='cobra-simplify<verified>' -S %s | FileCheck %s

; A two-operand minimum or maximum is the `select` over a comparison it stands
; for, and the model already writes a `select` in existing operators
; (`select(c, A, B) == B ^ ((A ^ B) & -c)`). Following one costs nothing new;
; left as an opaque leaf it cuts the two values it chooses between off from
; everything read through it.
;
; The expansion puts a comparison in the tree, which the bitwise and semilinear
; models treat as opaque, so a rewrite over one is only taken under a proof -
; the same reason a narrow `select` is tested under the verified pipeline.

; Both arms are the same value, so the choice cannot matter and the difference
; the comparison masks is zero whichever way it goes.
; CHECK-LABEL: @max_of_itself(
; CHECK-NOT: llvm.umax
; CHECK-NEXT: ret i64 %k
define i64 @max_of_itself(i64 %a, i64 %b, i64 %k) {
  %x = add i64 %a, %b
  %m = tail call i64 @llvm.umax.i64(i64 %x, i64 %x)
  %zero = xor i64 %m, %x
  %r = add i64 %k, %zero
  ret i64 %r
}

; And the signed form, read through a mask: the value chosen between two equal
; arms is the arm, so the whole of it is known however wide the reader.
; CHECK-LABEL: @min_of_itself(
; CHECK-NOT: llvm.smin
; CHECK-NEXT: ret i64 %k
define i64 @min_of_itself(i64 %a, i64 %k) {
  %x = xor i64 %a, 1442695040888963407
  %m = tail call i64 @llvm.smin.i64(i64 %x, i64 %x)
  %zero = xor i64 %m, %x
  %r = add i64 %k, %zero
  ret i64 %r
}

declare i64 @llvm.umax.i64(i64, i64)
declare i64 @llvm.smin.i64(i64, i64)
