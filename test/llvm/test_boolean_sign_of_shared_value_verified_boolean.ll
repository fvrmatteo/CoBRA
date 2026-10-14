; RUN: opt -load-pass-plugin=%cobra_pass -passes='cobra-simplify<verified;boolean;families=semilinear;timeout=20000>' -S %s | FileCheck %s
; COBRA_PARAMS: families=semilinear;timeout=20000

; `fshl(ashr(o, 63), o, 1)` is `ashr(o, 63)` whatever `o` is: the high part is the
; sign of `o` spread over the word, and the bit shifted in below it is that sign
; again. Read over the whole tree the sign is a condition on everything `o` is
; computed from, and the rewrite has to put all of it back - here with `x` and `y`
; each read twice, and neither known not to be `undef`, so frozen first - at a
; cost the funnel shift never had. Read on the cut that keeps `o` whole (it is read
; twice), the sign is a condition on `o` alone and the rewrite is the shift the
; function already holds. So the reading of conditions is tried on every reading
; of a root the search turned down, not only on the boolean cut.
;
; CHECK-LABEL: @sign_of_shared_value(
; CHECK-NOT: @llvm.fshl
; CHECK-NOT: freeze
; CHECK: %s = ashr i64 %o, 63
; CHECK-NEXT: icmp eq i64 %s, -1
define i1 @sign_of_shared_value(i64 %x, i64 %y) {
  %h1 = xor i64 %x, %y
  %h2 = add i64 %h1, %x
  %y8 = shl i64 %y, 3
  %h3 = xor i64 %h2, %y8
  %m = mul i64 %h3, 1099511628211
  %a = add i64 %m, 4937148438292785925
  %b = sub i64 -4937148438292785925, %m
  %o = or i64 %a, %b
  %s = ashr i64 %o, 63
  %f = call i64 @llvm.fshl.i64(i64 %s, i64 %o, i64 1)
  %c = icmp eq i64 %f, -1
  ret i1 %c
}

declare i64 @llvm.fshl.i64(i64, i64, i64)
