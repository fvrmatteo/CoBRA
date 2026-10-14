; RUN: opt -load-pass-plugin=%cobra_pass -passes='cobra-simplify' -S %s | FileCheck %s

; A tree read through a phi takes in the cones of the phi's arms, and with them
; their leaves: here twenty, which no search sweeps. Read with the phi as a
; leaf the same root has two, and is `p + y`. The tree past the leaf cap is
; collected again with its phis as leaves, as one whose arms disagree is.

; CHECK-LABEL: @phi_over_the_leaf_cap(
; CHECK: m:
; CHECK-NEXT: %p = phi i64
; CHECK-NEXT: %[[R:.*]] = add i64 %p, %y
; CHECK-NEXT: ret i64 %[[R]]
define i64 @phi_over_the_leaf_cap(i1 %c, i64 %y, i64 %l0, i64 %l1, i64 %l2, i64 %l3, i64 %l4, i64 %l5, i64 %l6, i64 %l7, i64 %l8, i64 %l9, i64 %l10, i64 %l11, i64 %l12, i64 %l13, i64 %l14, i64 %l15, i64 %l16, i64 %l17, i64 %l18, i64 %l19) {
entry:
  br i1 %c, label %t, label %e
t:
  %t1 = add i64 %l0, %l1
  %t2 = add i64 %t1, %l2
  %t3 = add i64 %t2, %l3
  %t4 = add i64 %t3, %l4
  %t5 = add i64 %t4, %l5
  %t6 = add i64 %t5, %l6
  %t7 = add i64 %t6, %l7
  %t8 = add i64 %t7, %l8
  %t9 = add i64 %t8, %l9
  %t10 = add i64 %t9, %l10
  %t11 = add i64 %t10, %l11
  %t12 = add i64 %t11, %l12
  %t13 = add i64 %t12, %l13
  %t14 = add i64 %t13, %l14
  %t15 = add i64 %t14, %l15
  %t16 = add i64 %t15, %l16
  %t17 = add i64 %t16, %l17
  %t18 = add i64 %t17, %l18
  %t19 = add i64 %t18, %l19
  br label %m
e:
  %e1 = add i64 %l0, %l1
  %e2 = add i64 %e1, %l2
  %e3 = add i64 %e2, %l3
  %e4 = add i64 %e3, %l4
  %e5 = add i64 %e4, %l5
  %e6 = add i64 %e5, %l6
  %e7 = add i64 %e6, %l7
  %e8 = add i64 %e7, %l8
  %e9 = add i64 %e8, %l9
  %e10 = add i64 %e9, %l10
  %e11 = add i64 %e10, %l11
  %e12 = add i64 %e11, %l12
  %e13 = add i64 %e12, %l13
  %e14 = add i64 %e13, %l14
  %e15 = add i64 %e14, %l15
  %e16 = add i64 %e15, %l16
  %e17 = add i64 %e16, %l17
  %e18 = add i64 %e17, %l18
  %e19 = add i64 %e18, %l19
  br label %m
m:
  %p = phi i64 [ %t19, %t ], [ %e19, %e ]
  %xo = xor i64 %p, %y
  %an = and i64 %p, %y
  %mu = mul i64 %an, 2
  %r = add i64 %xo, %mu
  ret i64 %r
}
