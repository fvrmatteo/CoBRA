; RUN: opt -load-pass-plugin=%cobra_pass -passes='cobra-simplify<verified-sweep>' -S %s | FileCheck %s
; COBRA_ENV: COBRA_TIMEOUT_MS=20000

; An arithmetic right shift by a constant, read as the logical shift with the
; sign copied into the bits it vacated: `(lshr(v, c) ^ H) - H` with
; `H = 2^(w-1-c)` (`AShrSignBit`).
;
; Left as a leaf it cuts the value it shifts off from every other reading of
; that value, and an identity the obfuscator wrote across it is not there to
; find: `zext(x ^ m) ^ sext(m)` is `sext x` only when `m` is the sign mask of
; `x`, and with the shift a leaf `m` and `x` are two independent variables.

; The obfuscator's sign extension, with the sign mask taken by `ashr` - which
; is how a lifter spells the arithmetic shift that goes with a sign extension.
; CHECK-LABEL: @ashr_sign_mask(
; CHECK: %[[S:.*]] = sext i32 %e to i64
; CHECK-NEXT: ret i64 %[[S]]
define i64 @ashr_sign_mask(i32 %e) {
  %m = ashr i32 %e, 31
  %wide = sext i32 %m to i64
  %s = xor i32 %m, %e
  %z = zext i32 %s to i64
  %r = xor i64 %z, %wide
  ret i64 %r
}

; The same with a base added into the sum, which needs the sum split as well
; (`SumGroupSliceMbaCandidates`): the whole sum is the extension of nothing.
; CHECK-LABEL: @base_plus_ashr_sign_mask(
; CHECK: %[[S:.*]] = sext i32 %e to i64
; CHECK-NEXT: %[[R:.*]] = add i64 %[[S]], %b
; CHECK-NEXT: ret i64 %[[R]]
define i64 @base_plus_ashr_sign_mask(i64 %b, i32 %e) {
  %m = ashr i32 %e, 31
  %wide = sext i32 %m to i64
  %s = xor i32 %m, %e
  %z = zext i32 %s to i64
  %x = xor i64 %z, %wide
  %r = add i64 %b, %x
  ret i64 %r
}

; The shift at an amount other than the sign bit's: `ashr(x, 8) - lshr(x, 8)`
; is the sign fill the arithmetic shift adds, which is `-(sign << 24)`. The two
; sides meet only if the model is the shift, so the `xor` of them is zero - and
; a reading off by one bit anywhere says so here.
; CHECK-LABEL: @ashr_fill(
; CHECK: ret i32 0
define i32 @ashr_fill(i32 %x) {
  %a = ashr i32 %x, 8
  %l = lshr i32 %x, 8
  %d = sub i32 %a, %l
  %s = lshr i32 %x, 31
  %f = shl i32 %s, 24
  %n = sub i32 0, %f
  %r = xor i32 %d, %n
  ret i32 %r
}

; At a shift of zero the expression is `(v ^ 2^(w-1)) - 2^(w-1)`, which is `v`
; modulo `2^w`: the shift is the identity and the tree has to read it so, or
; `(x | y) - (x & y)` is no longer `x ^ y`.
; CHECK-LABEL: @ashr_zero_is_identity(
; CHECK: ret i32 0
define i32 @ashr_zero_is_identity(i32 %x, i32 %y) {
  %a = ashr i32 %x, 0
  %o = or i32 %a, %y
  %n = and i32 %x, %y
  %d = sub i32 %o, %n
  %e = xor i32 %x, %y
  %r = xor i32 %d, %e
  ret i32 %r
}

; A variable amount has no expression in the model at all, so the shift stays a
; leaf and the identity above is out of reach - which is right: nothing here
; says what `%c` is.
; CHECK-LABEL: @ashr_variable_amount(
; CHECK: ashr i32 %e, %c
define i64 @ashr_variable_amount(i32 %e, i32 %c) {
  %m = ashr i32 %e, %c
  %wide = sext i32 %m to i64
  %s = xor i32 %m, %e
  %z = zext i32 %s to i64
  %r = xor i64 %z, %wide
  ret i64 %r
}
