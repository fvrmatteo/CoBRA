; RUN: opt -load-pass-plugin=%cobra_pass -passes='cobra-simplify<verified>' -S %s | FileCheck %s

; A value written as the recombination of slices of an extension of itself.
;
; Every shape here crosses a width boundary, which is what makes it hard: the
; two halves of the expression live at different widths, so neither LLVM's folds
; nor a single-width synthesis ever presents them a common root. Read at the
; width the extensions name, each one is an ordinary cancellation; the root is
; then recovered by the extension the slice was proved to be (`SliceToRead`).

; The obfuscator's sign extension, as it writes the export data
; directory's address: `zext(x ^ m) ^ sext(m)` with `m` the sign mask of `x`,
; which `(lshr(x,31) ^ -K) + K` is for any odd K. The low 32 bits are
; `(m ^ x) ^ m`, which is `x`, and the whole is `sext x`.
; CHECK-LABEL: @export_directory(
; CHECK: %[[S:.*]] = sext i32 %e to i64
; CHECK-NEXT: ret i64 %[[S]]
define i64 @export_directory(i32 %e) {
  %lobit = lshr i32 %e, 31
  %g = xor i32 %lobit, -325934777
  %m = add nsw i32 %g, 325934777
  %wide = sext i32 %m to i64
  %s = xor i32 %m, %e
  %z = zext i32 %s to i64
  %r = xor i64 %z, %wide
  ret i64 %r
}

; The same family at another width and with `or`: the high half taken from a
; sign extension and the low half from the value itself.
; CHECK-LABEL: @halves_sign(
; CHECK: %[[S:.*]] = sext i16 %x to i32
; CHECK-NEXT: ret i32 %[[S]]
define i32 @halves_sign(i16 %x) {
  %sx = sext i16 %x to i32
  %hi = and i32 %sx, -65536
  %lo = zext i16 %x to i32
  %r = or i32 %hi, %lo
  ret i32 %r
}

; And with the high half taken from a zero extension. This one needs no slice
; and does not get one: a zero extension's high bits are zero at the root's own
; width too, so the halves do present a common root there and the ordinary
; reading folds it to a mask of the low half, which is what a zero extension is.
; The shape is here because it is the other half of the family, and to pin that
; it stays a one-instruction answer.
; CHECK-LABEL: @halves_zero(
; CHECK: %[[Z:.*]] = and i32 %lo, 65535
; CHECK-NEXT: ret i32 %[[Z]]
define i32 @halves_zero(i16 %x) {
  %zx = zext i16 %x to i32
  %hi = and i32 %zx, -65536
  %lo = zext i16 %x to i32
  %r = or i32 %hi, %lo
  ret i32 %r
}

; A value that is no extension of its low half: the high half is the value
; again, so the bits above the slice are not decided by the bits below it and
; the probes say so before any proof is asked for. Nothing may be rewritten.
; CHECK-LABEL: @not_an_extension(
; CHECK: shl
; CHECK: or
define i32 @not_an_extension(i16 %x) {
  %zx = zext i16 %x to i32
  %hi = shl i32 %zx, 16
  %r = or i32 %hi, %zx
  ret i32 %r
}
