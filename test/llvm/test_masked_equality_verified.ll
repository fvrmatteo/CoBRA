; RUN: opt -load-pass-plugin=%cobra_pass -passes='cobra-simplify<verified;families=semilinear;timeout=5000>' -S %s | FileCheck %s
; COBRA_PARAMS: families=semilinear;timeout=5000

; `x >s (x & (x - 1))` is false exactly for 0 and 2^63: the lowest set bit cleared
; leaves a smaller value unless there is none, or only the sign bit. That is a test
; of every bit but the sign, `(x & 0x7fff...) != 0`, which no single point the
; comparison turns at says.
;
; CHECK-LABEL: @all_but_the_sign(
; CHECK: [[B:%.*]] = and i64 %x, 9223372036854775807
; CHECK-NEXT: [[R:%.*]] = icmp ne i64 [[B]], 0
; CHECK-NEXT: ret i1 [[R]]
define i1 @all_but_the_sign(i64 %x) {
  %d = add i64 %x, -1
  %l = and i64 %x, %d
  %r = icmp sgt i64 %x, %l
  ret i1 %r
}

; The low byte tested as the top of a shift: `t >u t >> 1` holds exactly when `t` is
; not zero, and `t = x << 56` is zero exactly when the low byte is: `(x & 255) != 0`.
;
; CHECK-LABEL: @low_byte(
; CHECK: [[B:%.*]] = and i64 %x, 255
; CHECK-NEXT: [[R:%.*]] = icmp ne i64 [[B]], 0
; CHECK-NEXT: ret i1 [[R]]
define i1 @low_byte(i64 %x) {
  %t = shl i64 %x, 56
  %h = lshr i64 %t, 1
  %r = icmp ugt i64 %t, %h
  ret i1 %r
}

; The control: an order of two trees that is no test of some bits stays an order.
;
; CHECK-LABEL: @order(
; CHECK-NOT: and i64 %x,
; CHECK: icmp
; CHECK: ret i1
define i1 @order(i64 %x) {
  %a = mul i64 %x, 3
  %b = add i64 %x, 5
  %r = icmp sgt i64 %a, %b
  ret i1 %r
}

; The same test of all bits but the sign, `(x << 1) - 2 == -2`, where the shift is
; also stored: the shift stays whatever the comparison becomes, so a mask in place of
; the subtraction InstCombine folds anyway would be one instruction more, and the
; comparison stays as it is.
;
; CHECK-LABEL: @shared_shift(
; CHECK: [[S:%.*]] = shl i64 %x, 1
; CHECK-NEXT: store i64 [[S]], ptr %out
; CHECK-NOT: and i64 %x,
; CHECK: ret i1
define i1 @shared_shift(i64 %x, ptr %out) {
  %s = shl i64 %x, 1
  store i64 %s, ptr %out
  %t = add i64 %s, -2
  %r = icmp eq i64 %t, -2
  ret i1 %r
}
