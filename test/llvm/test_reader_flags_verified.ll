; A rewrite is proved only on the bits its readers demand. `or disjoint %s, 256`
; does not demand bit 8 of %s, but it is poison if that bit is set, so a
; rewrite that differed from the tree there would make the reader poison where
; it was defined. The reader's poison-generating flags are dropped, as BDCE does.

; CHECK-LABEL: @reader_flags(
; CHECK:       %cobra.add = add i64 %x, %y
; CHECK-NEXT:  %d = or i64 %cobra.add, 256
; CHECK-NOT:   disjoint
define i64 @reader_flags(i64 %x, i64 %y) {
  %o = or i64 %x, %y
  %a = and i64 %x, %y
  %s = add i64 %o, %a
  %d = or disjoint i64 %s, 256
  ret i64 %d
}
