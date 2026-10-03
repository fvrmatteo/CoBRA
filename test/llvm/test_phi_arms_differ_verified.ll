; A phi is read through as the arm the collection followed only when every arm
; is the same computation. Here the arms are `false` and `x == y`: they agree on
; almost every input, so random probes took them for equal, the tree became
; `(0 | (x ^ y)) - (x ^ y)`, and the root was rewritten to 0 - which is 1 on the
; path through %A whenever x == y.

; CHECK-LABEL: @phi_arms_differ(
; CHECK:       phi i1
; CHECK:       %r = sub i32
; CHECK-NEXT:  ret i32 %r
define i32 @phi_arms_differ(i32 %x, i32 %y, i1 %c) {
entry:
  br i1 %c, label %A, label %M
A:
  %e = icmp eq i32 %x, %y
  br label %M
M:
  %p = phi i1 [false, %entry], [%e, %A]
  %z = zext i1 %p to i32
  %u = xor i32 %x, %y
  %t = or i32 %z, %u
  %r = sub i32 %t, %u
  ret i32 %r
}
