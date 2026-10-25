; COBRA_PARAMS: narrow-roots;subterms;affine;two-valued-selects;families=semilinear+bitwise;timeout=10000;max-vars=10

; A rewrite is priced on the instructions its budget counted. What it reads
; keeps alive what is under it, and for a root read wider than its type the
; budget is the tree at the root's own width, whose `trunc` the wide reading
; takes for a leaf. Priced on the instructions read alone, each tree below came
; in one instruction cheaper than itself and was rewritten into what the next
; InstCombine turned back into it - and so on every run of a fixpoint loop:
;
; - `xor (and (trunc x), 1), C` read back its own `and` and rebuilt the same
;   `xor` with its operands swapped (1 built + 1 read against 3);
; - `or disjoint (and (trunc x), 1), C` the same as the `add` InstCombine turns
;   back into the `or disjoint`;
; - a chain `gep (gep p, i << 1), 2 + v` as `gep p, -2 * ~i + v`: the four
;   instructions of the chain against three, without the `getelementptr` the
;   rewrite puts back; InstCombine splits it back into the chain.
;
; The controls are trees the rewrite does make cheaper, with the same reads.

; CHECK-LABEL: @xor_reads_its_own_and(
; CHECK:       %r = xor i16 %a, -15959
; CHECK-NEXT:  ret i16 %r
define i16 @xor_reads_its_own_and(i64 %x) {
  %s = lshr i64 %x, 62
  %t = trunc i64 %s to i16
  %a = and i16 %t, 1
  %r = xor i16 %a, -15959
  ret i16 %r
}

; CHECK-LABEL: @or_disjoint_reads_its_own_and(
; CHECK:       %r = or disjoint i16 %a, 28346
; CHECK-NEXT:  ret i16 %r
define i16 @or_disjoint_reads_its_own_and(i64 %x) {
  %s = lshr i64 %x, 62
  %t = trunc i64 %s to i16
  %a = and i16 %t, 1
  %r = or disjoint i16 %a, 28346
  ret i16 %r
}

; CHECK-LABEL: @pointer_chain_kept(
; CHECK:       %g1 = getelementptr i8, ptr %memory, i64 %s
; CHECK:       %g2 = getelementptr i8, ptr %g1, i64 %c
; CHECK-NOT:   mul
define i16 @pointer_chain_kept(ptr %memory, i64 %i, i64 %v) {
  %s = shl i64 %i, 1
  %g1 = getelementptr i8, ptr %memory, i64 %s
  %c = add i64 2, %v
  %g2 = getelementptr i8, ptr %g1, i64 %c
  %l = load i16, ptr %g2, align 1
  ret i16 %l
}

; Two flips of the bit read back are one: `(a ^ 1) ^ -15959` is
; `a ^ -15960` - or `a + -15960`, the bit being clear in the constant - one
; operation over the same `and`: three instructions against four.
; CHECK-LABEL: @double_flip_folds(
; CHECK:       {{(xor|add|or)}}{{( disjoint)?}} i16 %a, -15960
; CHECK-NOT:   xor i16
; CHECK:       ret i16
define i16 @double_flip_folds(i64 %x) {
  %s = lshr i64 %x, 62
  %t = trunc i64 %s to i16
  %a = and i16 %t, 1
  %n = xor i16 %a, 1
  %r = xor i16 %n, -15959
  ret i16 %r
}

; `(i ^ 5) + 2 * (i & 5)` is `i + 5`: the chain is one step over `i + v + 5`.
; CHECK-LABEL: @pointer_chain_folds(
; CHECK-NOT:   xor i64
; CHECK-NOT:   and i64
; CHECK:       getelementptr i8, ptr %memory
; CHECK-NOT:   getelementptr
; CHECK:       load i16
define i16 @pointer_chain_folds(ptr %memory, i64 %i, i64 %v) {
  %x = xor i64 %i, 5
  %m = and i64 %i, 5
  %d = shl i64 %m, 1
  %o = add i64 %x, %d
  %g1 = getelementptr i8, ptr %memory, i64 %o
  %g2 = getelementptr i8, ptr %g1, i64 %v
  %l = load i16, ptr %g2, align 1
  ret i16 %l
}
