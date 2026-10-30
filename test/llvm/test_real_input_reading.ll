; A root whose tree ends in more leaves than `max_vars` is read over the values
; it really reads (`real-inputs`). Here the noise subtracted from `%v` is built
; from five more loads and a 128-bit product of two of them, and added back: the
; tree has six ends - five loads and the product, which the expression language
; has no word for - and the root reads one value. Probes find it, the search
; over it answers `v + 5`, and the solver proves that against the instructions,
; every load free.
;
; The second root reads all of its loads, and stays. The third is `v + 5` but
; for one value of `%a` no probe draws: the probes propose `v + 5` and the proof
; refuses it.
;
; RUN: opt -load-pass-plugin=%cobra_pass -passes='cobra-simplify<verified;real-inputs;max-vars=4;timeout=2000>' -S %s | FileCheck %s
; COBRA_PARAMS: verified;real-inputs;max-vars=4;timeout=2000

; CHECK-LABEL: @cancels(
; CHECK-NOT: mul i128
; CHECK: [[R:%.*]] = add i64 %v, 5
; CHECK-NEXT: ret i64 [[R]]
define i64 @cancels(ptr %p) {
entry:
  %v = load i64, ptr %p, align 8, !noundef !0
  %pa = getelementptr i8, ptr %p, i64 8
  %a = load i64, ptr %pa, align 8, !noundef !0
  %pb = getelementptr i8, ptr %p, i64 16
  %b = load i64, ptr %pb, align 8, !noundef !0
  %pc = getelementptr i8, ptr %p, i64 24
  %c = load i64, ptr %pc, align 8, !noundef !0
  %pd = getelementptr i8, ptr %p, i64 32
  %d = load i64, ptr %pd, align 8, !noundef !0
  %pe = getelementptr i8, ptr %p, i64 40
  %e = load i64, ptr %pe, align 8, !noundef !0
  %wa = zext i64 %a to i128
  %wb = zext i64 %b to i128
  %wide = mul i128 %wa, %wb
  %high.wide = lshr i128 %wide, 64
  %high = trunc i128 %high.wide to i64
  %n1 = xor i64 %high, %c
  %n2 = add i64 %n1, %d
  %noise = and i64 %n2, %e
  %masked = xor i64 %v, %noise
  %grown = add i64 %masked, %noise
  %shrunk = sub i64 %grown, %noise
  %unmasked = xor i64 %shrunk, %noise
  %r = add i64 %unmasked, 5
  ret i64 %r
}

; CHECK-LABEL: @reads_all(
; CHECK: mul i128
define i64 @reads_all(ptr %p) {
entry:
  %v = load i64, ptr %p, align 8, !noundef !0
  %pa = getelementptr i8, ptr %p, i64 8
  %a = load i64, ptr %pa, align 8, !noundef !0
  %pb = getelementptr i8, ptr %p, i64 16
  %b = load i64, ptr %pb, align 8, !noundef !0
  %pc = getelementptr i8, ptr %p, i64 24
  %c = load i64, ptr %pc, align 8, !noundef !0
  %pd = getelementptr i8, ptr %p, i64 32
  %d = load i64, ptr %pd, align 8, !noundef !0
  %pe = getelementptr i8, ptr %p, i64 40
  %e = load i64, ptr %pe, align 8, !noundef !0
  %wa = zext i64 %a to i128
  %wb = zext i64 %b to i128
  %wide = mul i128 %wa, %wb
  %high.wide = lshr i128 %wide, 64
  %high = trunc i128 %high.wide to i64
  %n1 = xor i64 %high, %c
  %n2 = add i64 %n1, %d
  %n3 = and i64 %n2, %e
  %r1 = xor i64 %v, %n3
  %r2 = add i64 %r1, %e
  ret i64 %r2
}

; CHECK-LABEL: @one_value_apart(
; CHECK: icmp eq i64 %a, 81985529216486895
define i64 @one_value_apart(ptr %p) {
entry:
  %v = load i64, ptr %p, align 8, !noundef !0
  %pa = getelementptr i8, ptr %p, i64 8
  %a = load i64, ptr %pa, align 8, !noundef !0
  %pb = getelementptr i8, ptr %p, i64 16
  %b = load i64, ptr %pb, align 8, !noundef !0
  %pc = getelementptr i8, ptr %p, i64 24
  %c = load i64, ptr %pc, align 8, !noundef !0
  %pd = getelementptr i8, ptr %p, i64 32
  %d = load i64, ptr %pd, align 8, !noundef !0
  %wa = zext i64 %a to i128
  %wb = zext i64 %b to i128
  %wide = mul i128 %wa, %wb
  %high.wide = lshr i128 %wide, 64
  %high = trunc i128 %high.wide to i64
  %n1 = xor i64 %high, %c
  %noise = add i64 %n1, %d
  %masked = xor i64 %v, %noise
  %unmasked = xor i64 %masked, %noise
  %sum = add i64 %unmasked, 5
  %apart = icmp eq i64 %a, 81985529216486895
  %bump = zext i1 %apart to i64
  %r = add i64 %sum, %bump
  ret i64 %r
}

!0 = !{}
