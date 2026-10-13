; RUN: opt -load-pass-plugin=%cobra_pass -passes='cobra-simplify<verified-sweep>' -S %s | FileCheck %s
; COBRA_ENV: COBRA_TIMEOUT_MS=20000

; A sum group is collected again with the summands it leaves out read as zero,
; and that is only the group's own sum while the collection reads no further
; than the one the split was made on. Here it would: the root's collection
; keeps both phis as leaves (`%p1`'s arms differ, and phis are read through
; all together or not at all), so `%bb` and `%hi` reach no common leaf and are
; two groups. Collected without `%bb`, `%p1` is out of reach, `%p2` - whose
; arms are both `%bb * 3` - turns transparent, and `%hi` reads the `%bb` it was
; told is zero: the group lost `(3 * %bb) << 32`, and the rewrite with it.
; The collection's leaves stay leaves when a group is collected, so `%p2` is
; one, and the high term survives.

; CHECK-LABEL: @sum_group_phi(
; CHECK-NOT: %cobra.sum = add i64 %cobra.slice, %bb
; CHECK: mul i64 12884901889, %bb
; CHECK: ret i64
define i64 @sum_group_phi(i64 %b, i32 %e, i1 %c, i64 %q1, i64 %q2) {
entry:
  br i1 %c, label %l1, label %r1
l1:
  %x1 = xor i64 %q1, 7
  br label %m1
r1:
  %x2 = add i64 %q2, 9
  br label %m1
m1:
  %p1 = phi i64 [ %x1, %l1 ], [ %x2, %r1 ]
  %bb = add i64 %b, %p1
  br i1 %c, label %l2, label %r2
l2:
  %k1 = mul i64 %bb, 3
  br label %m2
r2:
  %k2 = mul i64 %bb, 3
  br label %m2
m2:
  %p2 = phi i64 [ %k1, %l2 ], [ %k2, %r2 ]
  %lobit = lshr i32 %e, 31
  %z = zext nneg i32 %lobit to i64
  %n = sub nsw i64 0, %z
  %t = trunc nsw i64 %n to i32
  %x = xor i32 %e, %t
  %u = zext i32 %x to i64
  %a = and i64 %u, %n
  %sh = shl nuw nsw i64 %a, 1
  %hi = shl i64 %p2, 32
  %u2 = add i64 %u, %hi
  %sum = add i64 %bb, %u2
  %orr = or disjoint i64 %sh, %z
  %r = sub i64 %sum, %orr
  ret i64 %r
}
