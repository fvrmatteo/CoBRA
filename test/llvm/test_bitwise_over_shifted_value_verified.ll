; COBRA_PARAMS: families=semilinear;timeout=10000;max-vars=10

; A bitwise operation with a constant over a value shifted up is the shifted
; bitwise value, with the constant's low bits beneath it.
;
; `InstCombine` moves a mask or a flip outward past a doubling: `2 * (x ^ c)`
; is printed `(x << 1) ^ (c << 1)`. The tree below is that shape -
; `((x << 1) & -4) ^ -84`, plus `(x ^ 41) + 3` - and it is `-(x ^ 40)`. Read
; with `x << 1` as a product under a bitwise operation, it was no semilinear
; sum at all and stayed six instructions; read as `2 * ((x & -2) ^ 86)`, it is
; one.
;
; The control shifts the two sides of the `xor` by different distances, which
; no single shift takes out.

; CHECK-LABEL: @shifted_mask_and_flip(
; CHECK-NOT:   shl i8
; CHECK-NOT:   and i8
; CHECK:       xor i8
; CHECK-NOT:   shl i8
; CHECK-NOT:   and i8
; CHECK:       ret i8
define i8 @shifted_mask_and_flip(i8 %x) {
  %s = shl i8 %x, 1
  %m = and i8 %s, -4
  %k = xor i8 %m, -84
  %y = xor i8 %x, 41
  %a = add i8 %y, 3
  %r = add i8 %a, %k
  ret i8 %r
}

; CHECK-LABEL: @different_shifts(
; CHECK:       shl i8 %x, 1
; CHECK:       shl i8 %x, 2
; CHECK:       ret i8
define i8 @different_shifts(i8 %x) {
  %s1 = shl i8 %x, 1
  %s2 = shl i8 %x, 2
  %k = xor i8 %s1, %s2
  %y = xor i8 %x, 41
  %a = add i8 %y, 3
  %r = add i8 %a, %k
  ret i8 %r
}
