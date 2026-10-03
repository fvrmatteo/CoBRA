; An MBA identity written across a `freeze`: (x | y) + (x & y) == x + y.
;
; Left as a leaf the `freeze` gives the tree three variables - x, y, and the
; frozen `or`, which is a function of the other two - so the identity is not
; visible and no rewrite of the tree holds. Followed, the tree has the two
; variables the program has.
;
; The rewrite reads the leaves without the `freeze` between them and the root,
; so a leaf that can be poison is frozen in its place - unless the tree also
; reads it on a path the `freeze` does not guard. Here the `and` reads both, so
; the root is poison wherever either is, and the rewrite adds no poison by
; reading them directly.

; CHECK-LABEL: @across_freeze(
; CHECK-NOT:   or i64
; CHECK-NOT:   and i64
; CHECK-NOT:   freeze
; CHECK:       add i64 %x, %y
define i64 @across_freeze(i64 %x, i64 %y) {
  %o = or i64 %x, %y
  %fo = freeze i64 %o
  %a = and i64 %x, %y
  %r = add i64 %fo, %a
  ret i64 %r
}

; Every path from the root to the leaves runs through the `freeze`, so the root
; is defined whatever the leaves are, and the rewrite has to freeze them.

; CHECK-LABEL: @behind_freeze(
; CHECK-NOT:   or i64
; CHECK-NOT:   and i64
; CHECK:       freeze i64 %x
; CHECK:       freeze i64 %y
; CHECK:       add i64
define i64 @behind_freeze(i64 %x, i64 %y) {
  %o = or i64 %x, %y
  %a = and i64 %x, %y
  %s = add i64 %o, %a
  %fs = freeze i64 %s
  %r = add i64 %fs, 0
  ret i64 %r
}

; Leaves already known to be defined need no `freeze` of their own.

; CHECK-LABEL: @across_freeze_noundef(
; CHECK-NOT:   freeze
; CHECK:       add i64 %x, %y
define i64 @across_freeze_noundef(i64 noundef %x, i64 noundef %y) {
  %o = or i64 %x, %y
  %fo = freeze i64 %o
  %a = and i64 %x, %y
  %r = add i64 %fo, %a
  ret i64 %r
}
