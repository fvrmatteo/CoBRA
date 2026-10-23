; COBRA_PARAMS: families=semilinear;timeout=10000;max-vars=10

; A class of bits is read modulo what it can hold.
;
; `2 * ((x & -2) ^ 86) + (x ^ 41) + 3` is `-(x ^ 40)` at every bit. Split into
; the classes on which its terms read the same function of `x`, the top bit is
; a class of its own, and its slice is `3 * x` where the others read `x` or
; `-x`. On the top bit alone `3`, `1` and `-1` are the same contribution, so the
; class is a translation of the others; compared at the full width it was not,
; no class was the reference for all of them, and the sum stayed six
; instructions.

; CHECK-LABEL: @top_bit_class(
; CHECK-NOT:   and i8
; CHECK-NOT:   mul i8
; CHECK:       xor i8
; CHECK-NOT:   and i8
; CHECK-NOT:   mul i8
; CHECK:       ret i8
define i8 @top_bit_class(i8 %x) {
  %m = and i8 %x, -2
  %k = xor i8 %m, 86
  %d = mul i8 %k, 2
  %y = xor i8 %x, 41
  %a = add i8 %y, 3
  %r = add i8 %a, %d
  ret i8 %r
}
