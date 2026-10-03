; A `select` reads only the arm it takes, so the tree is defined when %c is
; false whatever x and y are. The rewrite, `(x + y) & -c`, reads them on every
; path, so the ones that may be poison are frozen first.

; CHECK-LABEL: @select_arm(
; CHECK:       freeze i64 %x
; CHECK:       freeze i64 %y
; CHECK:       add i64 %cobra.freeze
; CHECK:       and i64
define i64 @select_arm(i64 %x, i64 %y, i64 %n) {
  %c = icmp ult i64 %n, 32
  %xo = xor i64 %x, %y
  %an = and i64 %x, %y
  %an2 = shl i64 %an, 1
  %s1 = add i64 %xo, %an2
  %or = or i64 %x, %y
  %s2 = sub i64 %or, %an
  %s3 = add i64 %s1, %s2
  %s4 = sub i64 %s3, %xo
  %r = select i1 %c, i64 %s4, i64 0
  ret i64 %r
}

; Read through the condition as well, a leaf poisons the root by itself, and
; reading it directly adds nothing: no freeze. (%y can be poison - the `nsw` -
; but not `undef`, so it is the poison rule alone that decides here.)

; CHECK-LABEL: @select_condition_reads_leaf(
; CHECK-NOT:   freeze
; CHECK:       ret
define i64 @select_condition_reads_leaf(i64 noundef %x, i64 noundef %a, i64 noundef %b) {
  %y = add nsw i64 %a, %b
  %c = icmp ult i64 %y, 32
  %xo = xor i64 %x, %y
  %an = and i64 %x, %y
  %an2 = shl i64 %an, 1
  %s1 = add i64 %xo, %an2
  %or = or i64 %x, %y
  %s2 = sub i64 %or, %an
  %s3 = add i64 %s1, %s2
  %s4 = sub i64 %s3, %xo
  %r = select i1 %c, i64 %s4, i64 0
  ret i64 %r
}
