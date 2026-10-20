; COBRA_PARAMS: sweep;families=semilinear;timeout=10000;max-vars=10

; A `freeze` the rest of the function keeps is not a saving.
;
; The two roots read a hash through `%fr`, which a store reads too. Collected
; through it, each tree is 28 instructions over `%p` and `%q`, and the same
; arithmetic read around the freeze is 27: priced with the freeze, the rewrite
; looked one instruction cheaper and was taken. It removed nothing - the freeze
; and the chain above it stay for the store - and once the clean-up merged the
; copy with the chain, `InstCombine` read the rebuilt `or` through the freeze
; again, so the next run took the same rewrite. In a fixpoint that is a copy of
; the whole hash built and merged on every round, and a copy that hides that a
; later test of the same hash is this one.
;
; Priced without the kept freeze, the rewrite is no cheaper than the tree and
; nothing changes.

; CHECK-LABEL: @kept_freeze(
; CHECK-NOT:   %cobra.
; CHECK:       %fr = freeze i64 %join
; CHECK:       %c = icmp ult i64 %top, 1957687296
; CHECK-NEXT:  %cs = sext i1 %c to i64
; CHECK-NEXT:  %r16 = shl nsw i64 %cs, 16
; CHECK-NEXT:  %r48 = shl nsw i64 %cs, 48
; CHECK-NOT:   %cobra.
; CHECK:       ret void
define void @kept_freeze(i64 %p, i64 %q, ptr %out) {
  %add = add i64 %q, %p
  %h48 = lshr i64 %add, 48
  %h16 = lshr i64 %add, 16
  %hi = xor i64 %h48, %h16
  %h32 = lshr i64 %add, 32
  %lo = xor i64 %h32, %add
  %lom = and i64 %lo, 61440
  %his = shl nuw i64 %hi, 16
  %join = or disjoint i64 %his, %lom
  %fr = freeze i64 %join
  %frs = lshr i64 %fr, 12
  %frt = trunc i64 %frs to i16
  store i16 %frt, ptr %out, align 2
  %hi12 = lshr i64 %hi, 12
  %hi12m = and i64 %hi12, 15
  %mix = xor i64 %hi12m, %hi
  %low = xor i64 %frs, %lo
  %lowm = and i64 %low, 65535
  %mixs = shl nuw i64 %mix, 16
  %word = or disjoint i64 %lowm, %mixs
  %k = xor i64 %word, 3145569996
  %m1 = mul i64 %k, 1431655765
  %s1 = lshr i64 %m1, 12
  %s1m = and i64 %s1, 1048575
  %x2 = xor i64 %s1m, %m1
  %m2 = mul i64 %x2, 2654435769
  %top = and i64 %m2, 4294963200
  %c = icmp ult i64 %top, 1957687296
  %cs = sext i1 %c to i64
  %r16 = shl nsw i64 %cs, 16
  %r48 = shl nsw i64 %cs, 48
  %o16 = getelementptr i8, ptr %out, i64 8
  store i64 %r16, ptr %o16, align 8
  %o48 = getelementptr i8, ptr %out, i64 16
  store i64 %r48, ptr %o48, align 8
  ret void
}
