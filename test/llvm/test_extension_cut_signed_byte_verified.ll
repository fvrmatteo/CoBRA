; RUN: opt -load-pass-plugin=%cobra_pass -passes='cobra-simplify<verified;families=semilinear;timeout=5000>' -S %s | FileCheck %s
; COBRA_PARAMS: families=semilinear;timeout=5000

; One step of a hash over signed bytes, `(h ^ sext(b)) * k`, its product written
; once and the xor before it spelled with halving identities, a borrow put back
; on `h == c`, and a select on the sign of the xor. The tree is linear in `h` and
; `c = sext(b)`. Read through the extension it is a function of the byte, held
; zero-extended, and the signature - taken on inputs that are 0 or 1, where the
; extension is the byte itself - proposes `k * (h ^ b)`, which is refused at full
; width. Read with the extension as the leaf it is `k * (h ^ c)`.
;
; CHECK-LABEL: @signed_byte_step(
; CHECK-NEXT: [[C:%.*]] = sext i8 %b to i64
; CHECK-NEXT: [[X:%.*]] = xor i64
; CHECK-NEXT: [[M:%.*]] = mul i64 [[X]], 1099511628211
; CHECK-NEXT: ret i64 [[M]]
define i64 @signed_byte_step(i64 %h, i8 %b) {
  %c = sext i8 %b to i64
  %1 = xor i64 %h, %c
  %2 = sub i64 0, %1
  %eq = icmp eq i64 %h, %c
  %n = xor i64 %1, -1
  %neg = icmp slt i64 %1, 0
  %3 = or i64 %n, 1
  %sh = lshr i64 %n, 1
  %4 = lshr i64 %2, 1
  %5 = select i1 %eq, i64 -9223372036854775808, i64 0
  %6 = add i64 %4, %3
  %7 = sub i64 %6, %sh
  %8 = add i64 %7, %5
  %9 = sub i64 %1, %8
  %10 = select i1 %neg, i64 0, i64 %9
  %11 = or i64 %10, %7
  %sl = shl i64 %11, 1
  %12 = xor i64 %10, %8
  %13 = sub i64 %sl, %12
  %t = shl i64 %11, 1
  %d = sub i64 %12, %t
  %s = select i1 %neg, i64 %d, i64 %13
  %r = mul i64 %s, 1099511628211
  ret i64 %r
}
