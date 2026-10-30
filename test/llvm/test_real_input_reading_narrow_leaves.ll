; A word packed from four 16-bit loads, `(a << 48) | (b << 16) | c | (d << 32)`,
; behind noise that cancels: the top lane is taken out and put back under a mask
; a comparison over a fifth load picks, `((pack & m) ^ pack) + (pack & m)` with
; `pack & m` either the top lane or nothing. The tree has five loads and the root
; reads four.
;
; Over its real inputs the tree over the support is the pack itself, which the
; semilinear search does not read as a sum: its `or`s are of shifted narrow
; values. The search on the root's values reads every variable at 64 bits, where
; a 16-bit load has only 16, and its answer fails at full width. The affine
; reading (`affine`) reads each load under its own width and finds
; `2^48 a + 2^16 b + c + 2^32 d`, which the solver proves against the
; instructions with every load free.
;
; RUN: opt -load-pass-plugin=%cobra_pass -passes='cobra-simplify<verified;real-inputs;affine;max-vars=4;timeout=2000;families=semilinear+bitwise>' -S %s | FileCheck %s
; COBRA_PARAMS: verified;real-inputs;affine;max-vars=4;timeout=2000;families=semilinear+bitwise

; CHECK-LABEL: @packed(
; CHECK-NOT: select
; CHECK-NOT: icmp
; CHECK-DAG: mul i64 281474976710656,
; CHECK-DAG: mul i64 4294967296,
; CHECK-DAG: mul i64 65536,
; CHECK: ret i64
define i64 @packed(ptr %p) {
entry:
  %v = load i64, ptr %p, align 8, !noundef !0
  %pa = getelementptr i8, ptr %p, i64 8
  %a = load i16, ptr %pa, align 2, !noundef !0
  %pb = getelementptr i8, ptr %p, i64 10
  %b = load i16, ptr %pb, align 2, !noundef !0
  %pc = getelementptr i8, ptr %p, i64 12
  %c = load i16, ptr %pc, align 2, !noundef !0
  %pd = getelementptr i8, ptr %p, i64 14
  %d = load i16, ptr %pd, align 2, !noundef !0
  %za = zext i16 %a to i64
  %ha = shl i64 %za, 48
  %zb = zext i16 %b to i64
  %hb = shl i64 %zb, 16
  %zc = zext i16 %c to i64
  %zd = zext i16 %d to i64
  %hd = shl i64 %zd, 32
  %o1 = or i64 %hb, %zc
  %o2 = or i64 %o1, %hd
  %pack = or i64 %o2, %ha
  %x0 = xor i64 %v, 24113
  %y0 = and i64 %v, 41418
  %s0 = add i64 %x0, %y0
  %h0 = lshr i64 %s0, 3
  %n0 = xor i64 %s0, %h0
  %x1 = xor i64 %n0, 12074
  %y1 = and i64 %n0, 3202245619
  %s1 = add i64 %x1, %y1
  %h1 = lshr i64 %s1, 3
  %n1 = xor i64 %s1, %h1
  %x2 = xor i64 %n1, 17989
  %y2 = and i64 %n1, 1092721676
  %s2 = add i64 %x2, %y2
  %h2 = lshr i64 %s2, 3
  %n2 = xor i64 %s2, %h2
  %x3 = xor i64 %n2, 852351322
  %y3 = and i64 %n2, 24112
  %s3 = add i64 %x3, %y3
  %h3 = lshr i64 %s3, 3
  %n3 = xor i64 %s3, %h3
  %x4 = xor i64 %n3, 391731054
  %y4 = and i64 %n3, 65534
  %s4 = add i64 %x4, %y4
  %h4 = lshr i64 %s4, 3
  %n4 = xor i64 %s4, %h4
  %x5 = xor i64 %n4, 129748633863749
  %y5 = and i64 %n4, 8191
  %s5 = add i64 %x5, %y5
  %h5 = lshr i64 %s5, 3
  %n5 = xor i64 %s5, %h5
  %x6 = xor i64 %n5, 270486122219077
  %y6 = and i64 %n5, 4294901760
  %s6 = add i64 %x6, %y6
  %h6 = lshr i64 %s6, 3
  %n6 = xor i64 %s6, %h6
  %cond = icmp slt i64 %n6, 0
  %mask = select i1 %cond, i64 -281474976710656, i64 0
  %top = and i64 %ha, %mask
  %r1 = xor i64 %top, %pack
  %r = add i64 %r1, %top
  ret i64 %r
}


!0 = !{}
