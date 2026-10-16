; RUN: opt -load-pass-plugin=%cobra_pass -passes='cobra-simplify<verified;families=semilinear;timeout=20000>' -S %s | FileCheck %s
; COBRA_PARAMS: families=semilinear;timeout=20000

; A product by a constant written as the product of the magnitude, with the sign
; put back: `x <s 0 ? -(|x| * k) : |x| * k` is `x * k` for every `x`, `INT_MIN`
; included (`abs` without the poison flag returns it, and `-(INT_MIN * k)` wraps to
; `INT_MIN * k`). With `abs` a leaf the magnitude is a variable apart from the `x`
; its sign test reads and nothing relates them; read as the `select` over the sign
; test it is, the tree is the product.
;
; CHECK-LABEL: @sign_split_product(
; CHECK-NEXT: [[X:%.*]] = xor i64
; CHECK-NEXT: [[M:%.*]] = mul i64 1099511628211, [[X]]
; CHECK-NEXT: ret i64 [[M]]
define i64 @sign_split_product(i64 %h, i64 %c) {
  %x = xor i64 %h, %c
  %negative = icmp slt i64 %x, 0
  %magnitude = call i64 @llvm.abs.i64(i64 %x, i1 false)
  %product = mul i64 %magnitude, 1099511628211
  %negated = sub i64 0, %product
  %r = select i1 %negative, i64 %negated, i64 %product
  ret i64 %r
}

; The control: the sign put back is the sign of another value, so the tree is not
; the product of `x`, and no rewrite says it is.
;
; CHECK-LABEL: @other_sign(
; CHECK: call i64 @llvm.abs.i64
; CHECK: select i1
; CHECK: ret i64
define i64 @other_sign(i64 %h, i64 %c) {
  %x = xor i64 %h, %c
  %negative = icmp slt i64 %h, 0
  %magnitude = call i64 @llvm.abs.i64(i64 %x, i1 false)
  %product = mul i64 %magnitude, 1099511628211
  %negated = sub i64 0, %product
  %r = select i1 %negative, i64 %negated, i64 %product
  ret i64 %r
}

declare i64 @llvm.abs.i64(i64, i1)
