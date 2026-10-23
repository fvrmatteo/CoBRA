; RUN: opt -load-pass-plugin=%cobra_pass -passes='cobra-simplify<verified;boolean;families=semilinear+bitwise;timeout=20000>' -S %s | FileCheck %s
; COBRA_PARAMS: families=semilinear+bitwise;timeout=20000

; Without `affine` the root is read as before: the sum under the `&` is an
; atom of its own, and the xor of `x - y - w` with a constant is not found.
;
; CHECK-LABEL: @xor_encoded_sum(
; CHECK: and i64
; CHECK-NOT: xor i64 {{%[a-z0-9.]+}}, 7640891576956012807
; CHECK: ret i64
define i64 @xor_encoded_sum(i64 %x, i64 %y, i64 %w) {
  %ny = xor i64 %y, -1
  %s = sub i64 %ny, %w
  %d = add i64 %x, %s
  %a = add i64 %d, 7640891576956012808
  %t = shl i64 %d, 1
  %u = add i64 %t, 2
  %m = and i64 %u, -3164960919797526002
  %r = sub i64 %a, %m
  ret i64 %r
}
