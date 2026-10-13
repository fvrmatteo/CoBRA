; RUN: opt -load-pass-plugin=%cobra_pass -passes='cobra-simplify' -S %s | FileCheck %s

; A tree read through a transparent phi follows one of its arms, and that arm
; is computed in a predecessor. The arms here are the same computation - each
; is `(x >> s) ^ 5`, from its own copy of the shift - but the variable shift is
; a leaf of the tree, and the copy the followed arm reads is defined on that
; path only. A rewrite reading it at the root would not dominate its use, so
; the phi is read as a leaf instead, and `(p ^ y) + 2 * (p & y)` folds to the
; sum it is.

; CHECK-LABEL: @phi_arm_leaf(
; CHECK: m:
; CHECK-NEXT: %p = phi i64 [ %a0, %t ], [ %a1, %e ]
; CHECK-NEXT: %[[R:.*]] = add i64 %p, %y
; CHECK-NEXT: ret i64 %[[R]]
define i64 @phi_arm_leaf(i1 %c, i64 %x, i64 %s, i64 %y) {
entry:
  br i1 %c, label %t, label %e
t:
  %u0 = lshr i64 %x, %s
  %a0 = xor i64 %u0, 5
  br label %m
e:
  %u1 = lshr i64 %x, %s
  %a1 = xor i64 %u1, 5
  br label %m
m:
  %p = phi i64 [ %a0, %t ], [ %a1, %e ]
  %xo = xor i64 %p, %y
  %an = and i64 %p, %y
  %mu = mul i64 %an, 2
  %r = add i64 %xo, %mu
  ret i64 %r
}

; The arms share the leaf itself: `%u` is computed once, before the branch, so
; it is available at the root and the phi is read through.
; CHECK-LABEL: @phi_arm_shared_leaf(
; CHECK: m:
; CHECK-NOT: phi
; CHECK: ret i64
define i64 @phi_arm_shared_leaf(i1 %c, i64 %x, i64 %s, i64 %y) {
entry:
  %u = lshr i64 %x, %s
  br i1 %c, label %t, label %e
t:
  %a0 = xor i64 %u, 5
  br label %m
e:
  %a1 = xor i64 %u, 5
  br label %m
m:
  %p = phi i64 [ %a0, %t ], [ %a1, %e ]
  %xo = xor i64 %p, %y
  %an = and i64 %p, %y
  %mu = mul i64 %an, 2
  %r = add i64 %xo, %mu
  ret i64 %r
}
