; RUN: opt -load-pass-plugin=%cobra_pass -passes=cobra-simplify -S %s | FileCheck %s

; The add-and-shift identity
;   ((x + y + ((x ^ y) & 1)) >> 1) + ((x ^ y) >> 1) - (x ^ y)
; is `x & y` in every bit but the top one, which carries the overflow of the
; sum. Read whole there is nothing to rewrite it to, and it is left alone.
; CHECK-LABEL: @and_identity_full(
; CHECK: lshr i32 {{.*}}, 1{{$}}
; CHECK: ret i32
define i32 @and_identity_full(i32 %x, i32 %y) {
  %d = xor i32 %x, %y
  %c = and i32 %d, 1
  %s = add i32 %x, %y
  %sc = add i32 %s, %c
  %avg = lshr i32 %sc, 1
  %d1 = lshr i32 %d, 1
  %r0 = add i32 %avg, %d1
  %r = sub i32 %r0, %d
  ret i32 %r
}

; Only bit 15 of the sum is read, so the pass asks for a rewrite on the bits
; below the top one: the shifts are lifted out and the identity is `x & y`.
; CHECK-LABEL: @and_identity_bit15(
; CHECK-NOT: lshr i32 {{.*}}, 1{{$}}
; CHECK: and i32 %x, %y
; CHECK: ret i32
define i32 @and_identity_bit15(i32 %x, i32 %y) {
  %d = xor i32 %x, %y
  %c = and i32 %d, 1
  %s = add i32 %x, %y
  %sc = add i32 %s, %c
  %avg = lshr i32 %sc, 1
  %d1 = lshr i32 %d, 1
  %r0 = add i32 %avg, %d1
  %r = sub i32 %r0, %d
  %hi = lshr i32 %r, 15
  %bit = and i32 %hi, 1
  ret i32 %bit
}

; The mask written into the expression itself works the same way.
; CHECK-LABEL: @and_identity_low16(
; CHECK-NOT: lshr
; CHECK: and i32
; CHECK: ret i32
define i32 @and_identity_low16(i32 %x, i32 %y) {
  %d = xor i32 %x, %y
  %c = and i32 %d, 1
  %s = add i32 %x, %y
  %sc = add i32 %s, %c
  %avg = lshr i32 %sc, 1
  %d1 = lshr i32 %d, 1
  %r0 = add i32 %avg, %d1
  %r = sub i32 %r0, %d
  %m = and i32 %r, 65535
  ret i32 %m
}

; The lane a lifted flag chain computes in: one operand of the identity reads
; a variable through a shift, which stays an atom of the answer.
; CHECK-LABEL: @lane_and_bit15(
; CHECK-NOT: lshr i32 {{.*}}, 1{{$}}
; CHECK: lshr i32 %z, 16
; CHECK: ret i32
define i32 @lane_and_bit15(i32 %a, i32 %z, i32 %w) {
  %x = xor i32 %a, 11218
  %zs = lshr i32 %z, 16
  %zk = xor i32 %zs, 27091
  %wm = and i32 %w, 65535
  %y0 = xor i32 %zk, %wm
  %y = xor i32 %y0, 25205
  %d = xor i32 %x, %y
  %c = and i32 %d, 1
  %s = add i32 %x, %y
  %sc = add i32 %s, %c
  %avg = lshr i32 %sc, 1
  %d1 = lshr i32 %d, 1
  %r0 = add i32 %avg, %d1
  %r = sub i32 %r0, %d
  %hi = lshr i32 %r, 15
  %bit = and i32 %hi, 1
  ret i32 %bit
}

; The same lane as the optimizer leaves it in a lifted flag chain: the
; operands' constants are folded together into the `x ^ y` the identity
; shifts (11218 ^ 25205 = 18855), and the carry-in reads the unmasked word.
; Nothing syntactic is left of `x ^ y`; bit by bit the scaled sum is still
; `2 * (x & y)` read through the constants, and the translation finds it.
; CHECK-LABEL: @lane_folded_bit15(
; CHECK-NOT: lshr i32 {{.*}}, 1{{$}}
; CHECK-NOT: add
; CHECK: ret i32
define i32 @lane_folded_bit15(i32 %a, i32 %z, i32 %w) {
  %x = xor i32 %a, 11218
  %zs = lshr i32 %z, 16
  %zk = xor i32 %zs, 27091
  %wm = and i32 %w, 65535
  %y0 = xor i32 %zk, %wm
  %y = xor i32 %y0, 25205
  %d0 = xor i32 %y0, 18855
  %d = xor i32 %d0, %a
  %d1 = lshr i32 %d, 1
  %c0 = xor i32 %zk, %w
  %c1 = xor i32 %c0, %a
  %c2 = and i32 %c1, 1
  %c = xor i32 %c2, 1
  %s = add i32 %x, %y
  %sc = add i32 %s, %c
  %avg = lshr i32 %sc, 1
  %r0 = sub i32 %d1, %d
  %r = add i32 %avg, %r0
  %hi = lshr i32 %r, 15
  %bit = and i32 %hi, 1
  ret i32 %bit
}
