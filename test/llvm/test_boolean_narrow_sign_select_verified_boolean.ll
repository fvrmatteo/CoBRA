; RUN: opt -load-pass-plugin=%cobra_pass -passes='cobra-simplify<verified;boolean;families=semilinear;timeout=20000>' -S %s | FileCheck %s
; COBRA_PARAMS: families=semilinear;timeout=20000

; A sign extension written in pieces: the low half of `x` (two bytes of it xored
; back), the extension's own bits where the mask keeps them, and the bits the mask
; drops supplied by a `select` on the sign of `x`, xored back with the same
; constant. The value is `sext x`.
;
; Read whole, the `select` is a leaf - its condition is a comparison of a value
; narrower than the tree - and with it free the root is the extension of nothing.
; The exact reading takes the comparison in, as the signed comparison of the
; operand's sign extension at the tree width, so asked of that reading the root
; *is* the sign extension of its low 32 bits; the claim is proved over it, and the
; 32-bit slice, where the `select` contributes no bit, is `x`.
;
; CHECK-LABEL: @sign_select(
; CHECK-NEXT: [[S:%.*]] = sext i32 %x to i64
; CHECK-NEXT: ret i64 [[S]]
define i64 @sign_select(i32 noundef %x) {
  %c = icmp sgt i32 %x, -1
  %s1 = select i1 %c, i64 177364969455616, i64 104105712287744
  %s = sext i32 %x to i64
  %a = and i64 %s, -281470681808896
  %o = or disjoint i64 %s1, %a
  %l = and i32 %x, 65535
  %lx = xor i32 %l, 22954
  %z = zext nneg i32 %lx to i64
  %o2 = or disjoint i64 %o, %z
  %r = xor i64 %o2, 177364969478570
  ret i64 %r
}

; The control: the same tree on a comparison that is not the sign of `x`. Read
; exactly it is no extension of its low bits, and nothing is rewritten into one.
;
; CHECK-LABEL: @not_a_sign_test(
; CHECK: icmp slt i32 %x, 37
; CHECK: select i1
; CHECK-NOT: sext i32 %x to i64
; CHECK: ret i64
define i64 @not_a_sign_test(i32 noundef %x) {
  %c = icmp slt i32 %x, 37
  %s1 = select i1 %c, i64 177364969455616, i64 104105712287744
  %l = and i32 %x, 65535
  %lx = xor i32 %l, 22954
  %z = zext nneg i32 %lx to i64
  %o2 = or disjoint i64 %s1, %z
  %r = xor i64 %o2, 177364969478570
  ret i64 %r
}
