; Random integer IR with mixed Boolean-arithmetic identities spliced in (nested,
; across widths and through phis), generated once and kept as it is: the input of
; the thread-parity test, which needs many candidates rather than a particular one.
define i64 @g101_0(i64 %a0, i8 %a1, i16 %a2) {
  %t1 = icmp slt i64 %a0, %a0
  br i1 %t1, label %b1, label %b2
b1:
  %t2 = or i64 %a0, %a0
  br label %b3
b2:
  %t3 = xor i64 %a0, -1
  %t4 = and i64 %a0, %t3
  %t5 = xor i64 %t4, %a0
  %t6 = and i64 %t4, %a0
  %t7 = shl i64 %t6, 1
  %t8 = add i64 %t5, %t7
  br label %b3
b3:
  %t9 = phi i64 [ %t2, %b1 ], [ %t8, %b2 ]
  %t10 = sext i16 %a2 to i32
  %t11 = and i32 %t10, 255
  %t12 = xor i32 255, -1
  %t13 = and i32 %t10, %t12
  %t14 = or i32 %t11, %t13
  %t15 = and i32 %t11, %t13
  %t16 = add i32 %t14, %t15
  %t17 = shl i64 %a0, 47
  %t18 = lshr i8 %a1, 4
  %t19 = trunc i32 %t15 to i16
  %t20 = xor i16 %a2, -1
  %t21 = xor i16 %t19, -1
  %t22 = and i16 %a2, %t21
  %t23 = and i16 %t20, %t19
  %t24 = or i16 %t22, %t23
  %t25 = sub i8 0, %t18
  %t26 = add i8 -1, %t25
  %t27 = mul i16 %t20, %t24
  ret i64 7
}

define i32 @g101_1(i16 %a0) {
  %t1 = sext i16 %a0 to i32
  %t2 = sub i32 %t1, %t1
  %t3 = trunc i32 %t2 to i8
  %t4 = icmp ult i8 101, %t3
  br i1 %t4, label %b1, label %b2
b1:
  %t5 = or i8 101, %t3
  %t6 = add i8 101, %t3
  %t7 = or i8 101, %t3
  %t8 = sub i8 %t6, %t7
  %t9 = sub i8 %t5, %t8
  br label %b3
b2:
  %t10 = or i8 101, %t3
  br label %b3
b3:
  %t11 = phi i8 [ %t9, %b1 ], [ %t10, %b2 ]
  %t12 = and i32 %t1, 15
  %t13 = and i32 %t12, -16
  %t14 = or disjoint i32 %t12, %t13
  %t15 = sub i32 %t12, %t13
  %t16 = xor i32 %t13, -1
  %t17 = and i32 %t1, %t16
  %t18 = add i32 %t17, %t13
  %t19 = lshr i8 %t3, 6
  %t20 = zext i8 %t19 to i32
  %t21 = and i32 %t20, 15
  %t22 = and i32 5, -16
  %t23 = or disjoint i32 %t21, %t22
  %t24 = trunc i32 %t21 to i8
  %t25 = icmp ult i8 %t24, %t24
  br i1 %t25, label %b4, label %b5
b4:
  %t26 = xor i8 %t24, %t24
  br label %b6
b5:
  %t27 = xor i8 %t24, -1
  %t28 = xor i8 %t24, -1
  %t29 = and i8 %t24, %t28
  %t30 = and i8 %t27, %t24
  %t31 = or i8 %t29, %t30
  br label %b6
b6:
  %t32 = phi i8 [ %t26, %b4 ], [ %t31, %b5 ]
  %t33 = and i16 %a0, 15
  %t34 = and i16 %a0, -16
  %t35 = or disjoint i16 %t33, %t34
  %t36 = add i8 %t24, %t24
  %t37 = zext i32 %t22 to i64
  %t38 = and i64 %t37, 15
  %t39 = and i64 %t38, -16
  %t40 = or disjoint i64 %t38, %t39
  ret i32 %t22
}

define i32 @g101_2(i32 %a0, i8 %a1) {
  %t1 = sext i32 %a0 to i64
  %t2 = xor i64 %t1, -1
  %t3 = and i64 %t1, %t2
  %t4 = xor i64 %t3, %t1
  %t5 = and i64 %t3, %t1
  %t6 = shl i64 %t5, 1
  %t7 = add i64 %t4, %t6
  %t8 = xor i8 %a1, -1
  %t9 = and i8 %a1, %t8
  %t10 = or i8 %t9, %a1
  %t11 = and i8 %t9, %a1
  %t12 = sub i8 %t10, %t11
  %t13 = and i8 %t9, %a1
  %t14 = shl i8 %t13, 1
  %t15 = add i8 %t12, %t14
  %t16 = add i64 %t3, 255
  %t17 = trunc i64 %t16 to i16
  %t18 = and i16 %t17, 15
  %t19 = sext i8 %t14 to i16
  %t20 = and i16 %t19, -16
  %t21 = or disjoint i16 %t18, %t20
  %t22 = freeze i16 %t18
  %t23 = and i8 0, -128
  %t24 = xor i8 -128, -1
  %t25 = and i8 0, %t24
  %t26 = xor i8 %t25, -1
  %t27 = and i8 %t23, %t26
  %t28 = add i8 %t27, %t25
  %t29 = and i32 %a0, %a0
  %t30 = and i8 %t27, 15
  %t31 = and i8 %t26, -16
  %t32 = or disjoint i8 %t30, %t31
  %t33 = sext i8 %t25 to i64
  %t34 = freeze i64 %t33
  %t35 = trunc i32 %t29 to i8
  %t36 = shl i8 %t35, 2
  ret i32 %a0
}

define i8 @g101_3(i16 %a0) {
  %t1 = mul i16 %a0, %a0
  %t2 = icmp ult i16 %a0, 0
  %t3 = zext i1 %t2 to i16
  %t4 = sext i16 %a0 to i64
  %t5 = and i64 %t4, 15
  %t6 = sext i16 %t3 to i64
  %t7 = and i64 %t6, -16
  %t8 = or disjoint i64 %t5, %t7
  %t9 = and i64 %t6, 15
  %t10 = and i64 %t9, -16
  %t11 = or disjoint i64 %t9, %t10
  %t12 = trunc i64 %t10 to i8
  %t13 = and i8 %t12, 15
  %t14 = trunc i64 %t8 to i8
  %t15 = and i8 %t14, -16
  %t16 = or disjoint i8 %t13, %t15
  %t17 = zext i8 %t15 to i64
  %t18 = mul i64 %t9, %t17
  %t19 = or i16 -19011, %t3
  %t20 = and i16 -19011, %t3
  %t21 = sub i16 %t19, %t20
  %t22 = ashr i16 %t1, 7
  %t24 = trunc i64 %t18 to i16
  %t23 = icmp eq i16 %t24, %t24
  %t25 = zext i1 %t23 to i64
  %t26 = lshr i32 7, 22
  %t27 = ashr i8 -128, 7
  %t28 = xor i8 -128, %t27
  %t29 = zext i8 %t28 to i16
  %t30 = sext i8 %t27 to i16
  %t31 = xor i16 %t29, %t30
  %t32 = and i16 %t29, %t1
  ret i8 %t27
}

define i8 @g101_4(i32 %a0, i16 %a1, i64 %a2) {
  %t1 = icmp ult i64 %a2, %a2
  br i1 %t1, label %b1, label %b2
b1:
  %t2 = or i64 %a2, %a2
  br label %b3
b2:
  %t3 = or i64 %a2, %a2
  br label %b3
b3:
  %t4 = phi i64 [ %t2, %b1 ], [ %t3, %b2 ]
  %t5 = trunc i64 %t4 to i8
  %t6 = or i8 %t5, 1
  %t7 = xor i8 %t5, 1
  %t8 = and i8 %t5, 1
  %t9 = shl i8 %t8, 1
  %t10 = add i8 %t7, %t9
  %t11 = xor i8 1, -1
  %t12 = and i8 %t5, %t11
  %t13 = add i8 %t12, 1
  %t14 = sub i8 %t10, %t13
  %t15 = add i8 %t6, %t14
  %t16 = sub i64 0, %t4
  %t17 = add i64 %a2, %t16
  %t19 = zext i8 %t15 to i64
  %t18 = icmp eq i64 %t19, -5766464305258701535
  %t20 = zext i1 %t18 to i32
  %t21 = xor i8 -1, %t13
  %t22 = and i64 %t19, -1
  %t23 = xor i64 -1, -1
  %t24 = and i64 %t19, %t23
  %t25 = or i64 %t22, %t24
  %t26 = shl i64 %t25, 1
  %t27 = xor i64 %t22, %t24
  %t28 = sub i64 %t26, %t27
  %t29 = mul i16 %a1, %a1
  %t30 = and i64 %t28, %t24
  ret i8 %t21
}

define i64 @g101_5(i64 %a0, i8 %a1, i32 %a2, i8 %a3) {
  %t1 = trunc i64 %a0 to i32
  %t2 = and i32 128, %t1
  %t3 = freeze i8 %a3
  %t4 = and i32 %t2, 15
  %t5 = and i32 %t2, -16
  %t6 = or disjoint i32 %t4, %t5
  %t7 = icmp ugt i8 %a3, %t3
  %t8 = sext i1 %t7 to i16
  %t9 = and i32 %t1, 0
  %t10 = xor i32 0, -1
  %t11 = and i32 %t1, %t10
  %t12 = or i32 %t9, %t11
  %t13 = and i32 %t9, %t11
  %t14 = add i32 %t12, %t13
  %t15 = trunc i32 %t12 to i8
  %t16 = ashr i8 %t15, 7
  %t17 = xor i8 %t15, %t16
  %t18 = zext i8 %t17 to i16
  %t19 = sext i8 %t16 to i16
  %t20 = xor i16 %t18, %t19
  %t21 = and i64 %a0, 2
  %t22 = xor i64 2, -1
  %t23 = and i64 %a0, %t22
  %t24 = or i64 %t21, %t23
  %t25 = xor i64 %t24, %t21
  %t26 = xor i64 %t25, %t22
  %t27 = xor i64 %t26, %t23
  %t28 = xor i64 %t27, %t24
  ret i64 %t28
}

define i8 @g101_6(i32 %a0, i32 %a1) {
  %t1 = trunc i32 %a1 to i8
  %t2 = or i8 %t1, %t1
  %t3 = shl i8 %t2, 1
  %t4 = xor i8 %t1, %t1
  %t5 = sub i8 %t3, %t4
  %t6 = zext i32 %a0 to i64
  %t7 = or i64 %t6, %t6
  %t8 = and i64 %t6, %t6
  %t9 = sub i64 %t7, %t8
  %t10 = and i32 %a1, 15
  %t11 = and i32 128, -16
  %t12 = or disjoint i32 %t10, %t11
  %t13 = and i64 %t9, 15
  %t14 = zext i32 %t11 to i64
  %t15 = and i64 %t14, -16
  %t16 = or disjoint i64 %t13, %t15
  %t17 = and i64 %t15, 1
  %t18 = xor i64 1, -1
  %t19 = and i64 %t15, %t18
  %t20 = add i64 %t17, %t19
  %t21 = and i32 %a1, 7
  %t22 = xor i32 7, -1
  %t23 = and i32 %a1, %t22
  %t24 = or i32 %t21, %t23
  %t25 = shl i32 %t24, 1
  %t26 = xor i32 %t21, %t23
  %t27 = sub i32 %t25, %t26
  %t28 = trunc i32 %t26 to i16
  %t29 = or i16 %t28, %t28
  %t30 = and i16 %t28, %t28
  %t31 = add i16 %t29, %t30
  %t32 = zext i16 %t31 to i32
  %t33 = sext i16 %t29 to i32
  %t34 = add i32 %t33, %t32
  %t35 = xor i64 %t20, -1
  %t36 = and i64 %t16, %t35
  %t37 = or i64 %t36, %t20
  %t38 = shl i64 %t37, 1
  %t39 = xor i64 %t36, %t20
  %t40 = sub i64 %t38, %t39
  %t41 = xor i64 %t16, %t20
  %t42 = and i64 %t16, %t20
  %t43 = shl i64 %t42, 1
  %t44 = add i64 %t41, %t43
  %t45 = xor i64 %t20, -1
  %t46 = and i64 %t16, %t45
  %t47 = add i64 %t46, %t20
  %t48 = sub i64 %t44, %t47
  %t49 = add i64 %t40, %t48
  %t50 = sub i8 %t1, 3
  %t51 = mul i32 %t23, 0
  %t52 = mul i8 %t4, %t5
  %t53 = trunc i64 %t47 to i8
  %t54 = xor i8 %t53, %t50
  %t55 = xor i8 %t54, %t52
  %t56 = xor i8 %t55, %t53
  ret i8 %t56
}

define i64 @g101_7(i32 %a0, i16 %a1) {
  %t1 = icmp ugt i32 %a0, %a0
  %t2 = sext i16 %a1 to i32
  %t3 = select i1 %t1, i32 %t2, i32 %a0
  %t4 = sext i32 %a0 to i64
  %t5 = and i64 %t4, 15
  %t6 = and i64 %t5, -16
  %t7 = or disjoint i64 %t5, %t6
  %t8 = icmp ult i64 %t6, %t5
  %t9 = zext i32 %t3 to i64
  %t10 = select i1 %t8, i64 %t9, i64 %t5
  %t11 = trunc i32 %t3 to i16
  %t12 = icmp slt i16 %a1, %t11
  br i1 %t12, label %b1, label %b2
b1:
  %t13 = sub i16 0, %t11
  %t14 = add i16 %a1, %t13
  br label %b3
b2:
  %t15 = sub i16 0, %t11
  %t16 = xor i16 %a1, %t15
  %t17 = and i16 %a1, %t15
  %t18 = shl i16 %t17, 1
  %t19 = add i16 %t16, %t18
  br label %b3
b3:
  %t20 = phi i16 [ %t14, %b1 ], [ %t19, %b2 ]
  %t21 = trunc i64 %t9 to i8
  %t22 = lshr i8 %t21, 5
  ret i64 %t7
}

define i8 @g101_8(i64 %a0, i64 %a1) {
  %t1 = trunc i64 %a1 to i8
  %t2 = shl i8 %t1, 5
  %t3 = trunc i64 %a0 to i16
  %t4 = and i16 %t3, 15
  %t5 = and i16 %t3, -16
  %t6 = or disjoint i16 %t4, %t5
  %t7 = shl i8 %t1, 6
  %t8 = icmp sge i64 1, %a0
  %t9 = sext i1 %t8 to i8
  %t10 = zext i8 %t1 to i16
  %t11 = shl i16 %t10, 13
  %t12 = zext i16 %t4 to i32
  %t13 = icmp eq i32 %t12, %t12
  br i1 %t13, label %b1, label %b2
b1:
  %t14 = xor i32 %t12, %t12
  br label %b3
b2:
  %t15 = sub i32 0, %t12
  %t16 = add i32 %t12, %t15
  br label %b3
b3:
  %t17 = phi i32 [ %t14, %b1 ], [ %t16, %b2 ]
  %t18 = trunc i32 %t17 to i8
  %t19 = xor i8 %t18, -1
  %t20 = and i8 %t1, %t19
  %t21 = add i8 %t20, %t18
  %t22 = and i8 %t1, %t18
  %t23 = sub i8 %t21, %t22
  %t24 = icmp eq i8 %t22, %t20
  br i1 %t24, label %b4, label %b5
b4:
  %t25 = sub i8 %t22, %t20
  br label %b6
b5:
  %t26 = sub i8 0, %t20
  %t27 = add i8 %t22, %t26
  br label %b6
b6:
  %t28 = phi i8 [ %t25, %b4 ], [ %t27, %b5 ]
  %t29 = trunc i32 %t17 to i16
  %t30 = ashr i16 %t29, 15
  %t31 = xor i16 %t29, %t30
  %t32 = zext i16 %t31 to i32
  %t33 = sext i16 %t30 to i32
  %t34 = xor i32 %t32, %t33
  %t35 = icmp sge i64 %a1, %a0
  %t36 = sext i1 %t35 to i8
  %t37 = zext i16 %t29 to i32
  %t38 = and i32 %t37, %t32
  %t39 = xor i8 %t28, %t36
  ret i8 %t39
}

define i32 @g101_9(i64 %a0, i16 %a1) {
  %t2 = trunc i64 %a0 to i8
  %t1 = icmp ult i8 %t2, %t2
  %t3 = zext i16 %a1 to i32
  %t4 = select i1 %t1, i32 %t3, i32 %t3
  %t5 = sext i8 %t2 to i64
  %t6 = and i64 %t5, 128
  %t7 = xor i64 128, -1
  %t8 = and i64 %t5, %t7
  %t9 = xor i64 %t6, -1
  %t10 = xor i64 %t8, -1
  %t11 = and i64 %t6, %t10
  %t12 = and i64 %t9, %t8
  %t13 = or i64 %t11, %t12
  %t14 = trunc i64 %t10 to i8
  %t15 = trunc i64 %t13 to i8
  %t16 = sub i8 0, %t15
  %t17 = add i8 %t14, %t16
  %t18 = add i16 %a1, %a1
  %t19 = sub i8 %t15, %t17
  %t20 = and i8 %t14, 15
  %t21 = and i8 -93, -16
  %t22 = or disjoint i8 %t20, %t21
  %t23 = and i8 %t20, 7
  %t24 = xor i8 7, -1
  %t25 = and i8 %t20, %t24
  %t26 = or i8 %t23, %t25
  %t27 = shl i8 %t26, 1
  %t28 = xor i8 %t23, %t25
  %t29 = sub i8 %t27, %t28
  %t30 = zext i8 %t24 to i32
  %t31 = xor i32 %t30, %t3
  %t32 = and i32 %t30, %t3
  %t33 = shl i32 %t32, 1
  %t34 = add i32 %t31, %t33
  %t35 = sext i32 %t33 to i64
  %t36 = xor i64 %t35, 1
  %t37 = trunc i64 %t35 to i16
  %t38 = trunc i64 %t35 to i16
  %t39 = or i16 %t37, %t38
  %t40 = shl i16 %t39, 1
  %t41 = xor i16 %t37, %t38
  %t42 = sub i16 %t40, %t41
  %t43 = xor i16 %t38, -1
  %t44 = and i16 %t37, %t43
  %t45 = or i16 %t44, %t38
  %t46 = shl i16 %t45, 1
  %t47 = xor i16 %t44, %t38
  %t48 = sub i16 %t46, %t47
  %t49 = sub i16 %t42, %t48
  ret i32 %t33
}

define i16 @g101_10(i8 %a0, i64 %a1, i64 %a2) {
  %t1 = zext i8 %a0 to i32
  %t2 = sext i8 %a0 to i32
  %t3 = icmp ult i32 %t1, %t2
  br i1 %t3, label %b1, label %b2
b1:
  %t4 = sub i32 %t1, %t2
  br label %b3
b2:
  %t5 = sub i32 0, %t2
  %t6 = xor i32 %t1, %t5
  %t7 = and i32 %t1, %t5
  %t8 = shl i32 %t7, 1
  %t9 = add i32 %t6, %t8
  br label %b3
b3:
  %t10 = phi i32 [ %t4, %b1 ], [ %t9, %b2 ]
  %t12 = sext i32 %t1 to i64
  %t11 = icmp eq i64 %a2, %t12
  %t13 = select i1 %t11, i8 %a0, i8 -1
  %t14 = ashr i8 %a0, 5
  %t15 = trunc i64 %a1 to i8
  %t16 = freeze i8 %t15
  %t17 = xor i64 %a1, %a2
  %t18 = trunc i64 %t17 to i16
  %t19 = xor i16 %t18, %t18
  ret i16 %t19
}

define i16 @g101_11(i64 %a0, i32 %a1, i8 %a2, i64 %a3) {
  %t1 = trunc i64 %a0 to i8
  %t2 = ashr i8 %t1, 7
  %t3 = xor i8 %t1, %t2
  %t4 = zext i8 %t3 to i16
  %t5 = sext i8 %t2 to i16
  %t6 = xor i16 %t4, %t5
  %t7 = or i16 %t5, %t5
  %t8 = and i16 %t5, %t5
  %t9 = sub i16 %t7, %t8
  %t10 = ashr i8 %t3, 5
  %t11 = add i32 %a1, %a1
  %t12 = trunc i16 %t7 to i8
  %t13 = and i8 %t12, -99
  %t14 = xor i8 -99, -1
  %t15 = and i8 %t12, %t14
  %t16 = or i8 %t13, %t15
  %t17 = shl i8 %t16, 1
  %t18 = xor i8 %t13, %t15
  %t19 = sub i8 %t17, %t18
  %t20 = ashr i64 %a0, 54
  %t22 = sext i8 %t14 to i64
  %t21 = icmp sge i64 %t20, %t22
  %t23 = sext i1 %t21 to i8
  %t24 = and i64 %t20, 15
  %t25 = and i64 %a0, -16
  %t26 = or disjoint i64 %t24, %t25
  %t27 = zext i8 %t23 to i16
  %t28 = xor i16 %t7, -1
  %t29 = xor i16 %t27, -1
  %t30 = and i16 %t7, %t29
  %t31 = and i16 %t28, %t27
  %t32 = or i16 %t30, %t31
  %t33 = sub i16 %t28, %t30
  %t34 = xor i16 %t30, %t30
  %t35 = xor i16 %t34, %t31
  %t36 = xor i16 %t35, %t32
  %t37 = xor i16 %t36, %t33
  ret i16 %t37
}

define i8 @g101_12(i64 %a0, i64 %a1, i64 %a2, i8 %a3) {
  %t1 = sext i8 %a3 to i32
  %t2 = and i32 %t1, 15
  %t3 = and i32 %t1, -16
  %t4 = or disjoint i32 %t2, %t3
  %t5 = ashr i8 %a3, 6
  %t6 = xor i8 %t5, 7
  %t7 = zext i8 %a3 to i16
  %t8 = xor i16 %t7, -1
  %t9 = and i16 %t7, %t8
  %t10 = add i16 %t9, %t7
  %t11 = add i16 %t7, %t7
  %t12 = or i16 %t7, %t7
  %t13 = sub i16 %t11, %t12
  %t14 = sub i16 %t10, %t13
  %t15 = ashr i16 %t9, 15
  ret i8 %t5
}

define i64 @g101_13(i32 %a0, i64 %a1) {
  %t1 = trunc i32 %a0 to i16
  %t2 = and i16 %t1, 15
  %t3 = and i16 %t2, -16
  %t4 = or disjoint i16 %t2, %t3
  %t5 = and i64 %a1, 3
  %t6 = trunc i64 %t5 to i8
  %t7 = xor i8 %t6, %t6
  %t8 = freeze i8 %t7
  %t9 = sext i8 %t8 to i64
  %t10 = and i64 %a1, %t9
  %t11 = xor i64 %t9, -1
  %t12 = and i64 3, %t11
  %t13 = add i64 %t12, %t9
  %t14 = trunc i64 %t12 to i16
  %t15 = and i16 %t14, 128
  %t16 = xor i16 128, -1
  %t17 = and i16 %t14, %t16
  %t18 = xor i16 %t17, -1
  %t19 = and i16 %t15, %t18
  %t20 = add i16 %t19, %t17
  %t21 = freeze i64 %t13
  %t22 = add i64 %a1, %t11
  %t23 = or i64 %a1, %t11
  %t24 = sub i64 %t22, %t23
  %t25 = trunc i64 %t23 to i8
  %t26 = xor i8 %t25, %t25
  %t28 = trunc i64 %t24 to i16
  %t27 = icmp ugt i16 %t28, %t19
  %t29 = sext i1 %t27 to i64
  %t30 = xor i64 %t24, %t29
  ret i64 %t30
}

define i32 @g101_14(i32 %a0) {
  %t1 = trunc i32 %a0 to i16
  %t2 = icmp slt i16 5, %t1
  br i1 %t2, label %b1, label %b2
b1:
  %t3 = or i16 5, %t1
  br label %b3
b2:
  %t4 = xor i16 %t1, -1
  %t5 = and i16 5, %t4
  %t6 = add i16 %t5, %t1
  %t7 = shl i16 %t6, 1
  %t8 = or i16 5, %t1
  %t9 = and i16 5, %t1
  %t10 = sub i16 %t8, %t9
  %t11 = sub i16 %t7, %t10
  br label %b3
b3:
  %t12 = phi i16 [ %t3, %b1 ], [ %t11, %b2 ]
  %t13 = trunc i16 %t12 to i8
  %t14 = or i8 1, %t13
  %t15 = shl i8 %t14, 1
  %t16 = or i8 1, %t13
  %t17 = and i8 1, %t13
  %t18 = sub i8 %t16, %t17
  %t19 = sub i8 %t15, %t18
  %t20 = or i8 %t13, %t15
  %t21 = and i8 %t13, %t15
  %t22 = add i8 %t20, %t21
  %t23 = sext i8 %t22 to i16
  %t24 = freeze i16 %t23
  %t25 = sext i8 %t20 to i64
  %t26 = add i64 %t25, %t25
  %t27 = sext i8 %t22 to i64
  %t28 = freeze i64 %t27
  ret i32 -1
}

define i32 @g101_15(i16 %a0) {
  %t1 = sext i16 %a0 to i64
  %t2 = freeze i64 %t1
  %t3 = trunc i16 %a0 to i8
  %t4 = ashr i8 %t3, 7
  %t5 = xor i8 %t3, %t4
  %t6 = zext i8 %t5 to i32
  %t7 = sext i8 %t4 to i32
  %t8 = xor i32 %t6, %t7
  %t9 = or i8 %t4, %t3
  %t10 = add i8 %t4, %t3
  %t11 = xor i8 %t3, -1
  %t12 = and i8 %t4, %t11
  %t13 = add i8 %t12, %t3
  %t14 = sub i8 %t10, %t13
  %t15 = add i8 %t9, %t14
  %t16 = add i64 %t1, %t1
  %t17 = shl i64 %t16, 46
  %t19 = trunc i64 %t17 to i16
  %t18 = icmp sge i16 %t19, %t19
  %t20 = zext i1 %t18 to i32
  %t21 = and i16 %t19, 15
  %t22 = and i16 %a0, -16
  %t23 = or disjoint i16 %t21, %t22
  %t24 = sext i16 %t23 to i32
  %t25 = xor i32 %t24, %t24
  ret i32 %t25
}

define i64 @g101_16(i16 %a0, i16 %a1) {
  %t1 = trunc i16 %a1 to i8
  %t2 = add i8 %t1, %t1
  %t3 = sext i8 %t2 to i16
  %t4 = xor i16 %a1, %t3
  %t5 = and i16 %a1, %t3
  %t6 = shl i16 %t5, 1
  %t7 = add i16 %t4, %t6
  %t8 = sext i16 %a1 to i32
  %t9 = and i32 %t8, -1
  %t10 = xor i32 -1, -1
  %t11 = and i32 %t8, %t10
  %t12 = xor i32 %t9, %t11
  %t13 = and i32 %t9, %t11
  %t14 = shl i32 %t13, 1
  %t15 = add i32 %t12, %t14
  %t16 = freeze i32 %t14
  %t17 = trunc i32 %t15 to i8
  %t18 = xor i8 %t17, -1
  %t19 = and i8 %t2, %t18
  %t20 = xor i8 %t19, %t17
  %t21 = and i8 %t19, %t17
  %t22 = shl i8 %t21, 1
  %t23 = add i8 %t20, %t22
  %t24 = mul i16 128, %t3
  %t25 = zext i8 %t21 to i64
  %t26 = xor i64 %t25, -1
  %t27 = xor i64 %t25, -1
  %t28 = and i64 %t25, %t27
  %t29 = and i64 %t26, %t25
  %t30 = or i64 %t28, %t29
  %t31 = ashr i16 %t5, 15
  %t32 = xor i16 %t5, %t31
  %t33 = zext i16 %t32 to i32
  %t34 = sext i16 %t31 to i32
  %t35 = xor i32 %t33, %t34
  %t36 = add i8 1, %t22
  %t37 = and i16 %t5, 1
  %t38 = xor i16 1, -1
  %t39 = and i16 %t5, %t38
  %t40 = xor i16 %t37, -1
  %t41 = xor i16 %t39, -1
  %t42 = and i16 %t37, %t41
  %t43 = and i16 %t40, %t39
  %t44 = or i16 %t42, %t43
  %t45 = xor i8 %t21, -1
  %t46 = and i8 %t22, %t45
  %t47 = add i8 %t46, %t21
  %t48 = shl i8 %t47, 1
  %t49 = xor i8 %t22, -1
  %t50 = xor i8 %t21, -1
  %t51 = and i8 %t22, %t50
  %t52 = and i8 %t49, %t21
  %t53 = or i8 %t51, %t52
  %t54 = sub i8 %t48, %t53
  %t55 = zext i8 %t54 to i16
  %t56 = or i16 %t40, %t55
  %t57 = and i16 %t40, %t55
  %t58 = add i16 %t56, %t57
  ret i64 %t27
}

define i8 @g101_17(i16 %a0, i16 %a1) {
  %t1 = trunc i16 %a0 to i8
  %t2 = xor i8 %t1, -1
  %t3 = and i8 %t1, %t2
  %t4 = add i8 %t3, %t1
  %t5 = shl i8 %t4, 1
  %t6 = xor i8 %t1, -1
  %t7 = xor i8 %t1, -1
  %t8 = and i8 %t1, %t7
  %t9 = and i8 %t6, %t1
  %t10 = or i8 %t8, %t9
  %t11 = sub i8 %t5, %t10
  %t12 = zext i8 %t4 to i64
  %t13 = sub i64 %t12, %t12
  %t15 = trunc i64 %t13 to i32
  %t14 = icmp slt i32 %t15, %t15
  %t16 = sext i1 %t14 to i8
  %t17 = and i32 %t15, -1453505221
  %t18 = xor i32 -1453505221, -1
  %t19 = and i32 %t15, %t18
  %t20 = xor i32 %t17, %t19
  %t21 = add i8 %t5, %t11
  %t22 = zext i8 %t21 to i16
  %t23 = zext i8 %t21 to i16
  %t24 = add i16 %t23, %t22
  %t25 = sext i16 %t24 to i32
  %t26 = zext i16 %t24 to i32
  %t27 = add i32 %t25, %t26
  %t28 = sub i64 0, %t13
  %t29 = add i64 %t12, %t28
  %t30 = mul i32 1, %t25
  %t31 = shl i32 %t30, 1
  %t32 = zext i32 %t25 to i64
  %t33 = sext i32 %t27 to i64
  %t34 = xor i64 %t32, %t33
  %t35 = add i64 %t32, %t33
  %t36 = or i64 %t32, %t33
  %t37 = sub i64 %t35, %t36
  %t38 = shl i64 %t37, 1
  %t39 = add i64 %t34, %t38
  %t40 = or i64 %t34, %t36
  %t41 = and i64 %t34, %t36
  %t42 = add i64 %t40, %t41
  %t43 = icmp ne i16 %t24, %t22
  %t44 = sext i1 %t43 to i8
  %t45 = xor i8 1, %t44
  ret i8 %t45
}

define i8 @g101_18(i8 %a0) {
  %t1 = icmp ult i8 %a0, %a0
  br i1 %t1, label %b1, label %b2
b1:
  %t2 = add i8 %a0, %a0
  %t3 = xor i8 %a0, -1
  %t4 = and i8 %a0, %t3
  %t5 = add i8 %t4, %a0
  %t6 = sub i8 %t2, %t5
  br label %b3
b2:
  %t7 = and i8 %a0, %a0
  br label %b3
b3:
  %t8 = phi i8 [ %t6, %b1 ], [ %t7, %b2 ]
  %t9 = zext i8 %t8 to i16
  %t10 = ashr i16 %t9, 6
  %t11 = zext i16 %t9 to i64
  %t12 = and i64 %t11, 5
  %t13 = xor i64 5, -1
  %t14 = and i64 %t11, %t13
  %t15 = or i64 %t12, %t14
  %t16 = xor i16 %t9, %t9
  %t17 = and i16 %t16, 15
  %t18 = and i16 %t16, -16
  %t19 = or disjoint i16 %t17, %t18
  %t20 = sext i16 %t18 to i32
  %t21 = and i32 %t20, %t20
  %t22 = shl i64 128, 5
  %t23 = xor i32 %t20, 3
  %t24 = freeze i16 %t16
  %t25 = xor i16 %t18, -1
  %t26 = and i16 %t24, %t25
  %t27 = add i16 %t26, %t18
  %t28 = and i16 %t24, %t18
  %t29 = sub i16 %t27, %t28
  %t30 = and i16 %t27, 15
  %t31 = and i16 %t30, -16
  %t32 = or disjoint i16 %t30, %t31
  %t33 = or i16 %t28, %t31
  %t34 = add i16 %t28, %t31
  %t35 = or i16 %t28, %t31
  %t36 = sub i16 %t34, %t35
  %t37 = add i16 %t33, %t36
  %t38 = sext i16 %t37 to i64
  %t39 = or i64 %t15, %t38
  %t40 = and i64 %t15, %t38
  %t41 = add i64 %t39, %t40
  ret i8 %a0
}

define i64 @g101_19(i32 %a0, i16 %a1, i16 %a2) {
  %t1 = freeze i16 %a2
  %t2 = trunc i16 %a2 to i8
  %t3 = xor i8 %t2, -1
  %t4 = and i8 %t2, %t3
  %t5 = xor i8 %t2, -1
  %t6 = and i8 %t4, %t5
  %t7 = add i8 %t6, %t2
  %t8 = shl i8 %t7, 1
  %t9 = xor i8 %t4, %t2
  %t10 = sub i8 %t8, %t9
  %t11 = and i32 63717435, %a0
  %t12 = sext i8 %t10 to i32
  %t13 = icmp ult i32 %a0, %t12
  br i1 %t13, label %b1, label %b2
b1:
  %t14 = sub i32 %a0, %t12
  br label %b3
b2:
  %t15 = or i32 %a0, %t12
  %t16 = shl i32 %t15, 1
  %t17 = xor i32 %a0, %t12
  %t18 = sub i32 %t16, %t17
  %t19 = or i32 %a0, %t12
  %t20 = sub i32 %t18, %t19
  br label %b3
b3:
  %t21 = phi i32 [ %t14, %b1 ], [ %t20, %b2 ]
  %t22 = zext i8 %t7 to i64
  %t23 = xor i64 %t22, %t22
  ret i64 %t23
}

define i16 @g101_20(i8 %a0, i64 %a1, i16 %a2) {
  %t1 = trunc i64 %a1 to i32
  %t2 = xor i32 %t1, -1
  %t3 = and i32 %t1, %t2
  %t4 = add i32 %t3, %t1
  %t5 = add i32 %t1, %t1
  %t6 = or i32 %t1, %t1
  %t7 = sub i32 %t5, %t6
  %t8 = add i32 %t4, %t7
  %t9 = add i8 %a0, %a0
  %t10 = and i64 %a1, 15
  %t11 = and i64 %t10, -16
  %t12 = or disjoint i64 %t10, %t11
  %t13 = zext i32 %t8 to i64
  %t14 = icmp ult i64 %t13, %t11
  br i1 %t14, label %b1, label %b2
b1:
  %t15 = add i64 %t13, %t11
  br label %b3
b2:
  %t16 = or i64 %t13, %t11
  %t17 = shl i64 %t16, 1
  %t18 = xor i64 %t13, -1
  %t19 = xor i64 %t11, -1
  %t20 = and i64 %t13, %t19
  %t21 = and i64 %t18, %t11
  %t22 = or i64 %t20, %t21
  %t23 = sub i64 %t17, %t22
  br label %b3
b3:
  %t24 = phi i64 [ %t15, %b1 ], [ %t23, %b2 ]
  %t25 = mul i8 %t9, %t9
  %t26 = trunc i64 %t11 to i8
  %t27 = xor i8 %t26, %t25
  %t28 = trunc i64 %t12 to i8
  %t29 = icmp slt i8 %t26, %t28
  br i1 %t29, label %b4, label %b5
b4:
  %t30 = xor i8 %t26, %t28
  br label %b6
b5:
  %t31 = xor i8 %t28, -1
  %t32 = and i8 %t26, %t31
  %t33 = add i8 %t32, %t28
  %t34 = add i8 %t26, %t28
  %t35 = or i8 %t26, %t28
  %t36 = sub i8 %t34, %t35
  %t37 = sub i8 %t33, %t36
  br label %b6
b6:
  %t38 = phi i8 [ %t30, %b4 ], [ %t37, %b5 ]
  %t39 = trunc i64 %t13 to i16
  %t40 = xor i16 %t39, %t39
  ret i16 %t40
}

define i8 @g101_21(i32 %a0, i64 %a1, i32 %a2, i32 %a3) {
  %t1 = icmp ult i32 %a2, %a2
  br i1 %t1, label %b1, label %b2
b1:
  %t2 = xor i32 %a2, -1
  %t3 = and i32 %a2, %t2
  %t4 = xor i32 %t3, %a2
  %t5 = and i32 %t3, %a2
  %t6 = shl i32 %t5, 1
  %t7 = add i32 %t4, %t6
  br label %b3
b2:
  %t8 = xor i32 %a2, %a2
  br label %b3
b3:
  %t9 = phi i32 [ %t7, %b1 ], [ %t8, %b2 ]
  %t10 = ashr i32 %a3, 31
  %t11 = xor i32 %a3, %t10
  %t12 = zext i32 %t11 to i64
  %t13 = sext i32 %t10 to i64
  %t14 = xor i64 %t12, %t13
  %t15 = and i32 %t9, 15
  %t16 = and i32 %a3, -16
  %t17 = or disjoint i32 %t15, %t16
  %t18 = and i32 %t16, 15
  %t19 = and i32 %t16, -16
  %t20 = or disjoint i32 %t18, %t19
  %t21 = trunc i64 %t14 to i8
  %t22 = xor i8 %t21, %t21
  ret i8 %t22
}

define i16 @g101_22(i32 %a0, i8 %a1, i32 %a2) {
  %t1 = sub i32 %a0, %a0
  %t2 = zext i8 %a1 to i16
  %t3 = xor i16 %t2, -1
  %t4 = xor i16 %t2, -1
  %t5 = and i16 %t2, %t4
  %t6 = and i16 %t3, %t2
  %t7 = or i16 %t5, %t6
  %t8 = add i16 %t2, %t2
  %t9 = or i16 %t2, %t2
  %t10 = sub i16 %t8, %t9
  %t11 = shl i16 %t10, 1
  %t12 = add i16 %t7, %t11
  %t13 = sext i16 %t12 to i32
  %t14 = add i32 %t1, %t13
  %t15 = zext i16 %t12 to i64
  %t16 = icmp ne i64 %t15, %t15
  br i1 %t16, label %b1, label %b2
b1:
  %t17 = and i64 %t15, %t15
  br label %b3
b2:
  %t18 = xor i64 %t15, -1
  %t19 = xor i64 %t15, -1
  %t20 = and i64 %t15, %t19
  %t21 = and i64 %t18, %t15
  %t22 = or i64 %t20, %t21
  br label %b3
b3:
  %t23 = phi i64 [ %t17, %b1 ], [ %t22, %b2 ]
  %t24 = and i8 -1, 3
  %t25 = xor i8 3, -1
  %t26 = and i8 -1, %t25
  %t27 = add i8 %t24, %t26
  %t28 = mul i32 %t13, %t13
  %t29 = sext i8 %t26 to i16
  %t30 = shl i16 %t29, 1
  %t31 = sext i8 %t27 to i32
  %t32 = xor i32 %a0, %t31
  %t33 = ashr i16 %t7, 15
  %t34 = xor i16 %t7, %t33
  %t35 = zext i16 %t34 to i64
  %t36 = sext i16 %t33 to i64
  %t37 = xor i64 %t35, %t36
  %t38 = xor i16 %t11, %t34
  ret i16 %t38
}

define i64 @g101_23(i64 %a0, i16 %a1, i16 %a2) {
  %t1 = sext i16 %a1 to i32
  %t2 = and i32 %t1, 2
  %t3 = xor i32 2, -1
  %t4 = and i32 %t1, %t3
  %t5 = add i32 %t2, %t4
  %t6 = lshr i32 -479802206, 16
  %t7 = xor i64 %a0, %a0
  %t8 = lshr i64 %a0, 31
  %t9 = trunc i64 %t7 to i8
  %t10 = xor i8 -128, -1
  %t11 = and i8 %t9, %t10
  %t12 = add i8 %t11, -128
  %t13 = shl i8 %t12, 1
  %t14 = xor i8 %t9, -128
  %t15 = sub i8 %t13, %t14
  %t16 = zext i8 %t15 to i32
  %t17 = or i32 %t3, %t16
  %t18 = and i32 %t3, %t16
  %t19 = add i32 %t17, %t18
  %t20 = xor i16 %a2, -1
  %t21 = and i16 %a1, %t20
  %t22 = add i16 %t21, %a2
  %t23 = and i16 %a1, %a2
  %t24 = sub i16 %t22, %t23
  %t25 = mul i8 %t14, %t13
  %t26 = sext i16 %t20 to i64
  %t27 = xor i64 %t26, %t26
  ret i64 %t27
}

define i32 @g101_24(i64 %a0, i16 %a1, i8 %a2, i8 %a3) {
  %t1 = ashr i8 %a2, 7
  %t2 = xor i8 %a2, %t1
  %t3 = zext i8 %t2 to i32
  %t4 = sext i8 %t1 to i32
  %t5 = xor i32 %t3, %t4
  %t6 = and i8 %a3, -128
  %t7 = xor i8 -128, -1
  %t8 = and i8 %a3, %t7
  %t9 = add i8 %t6, %t8
  %t10 = and i16 %a1, %a1
  %t11 = or i64 255, %a0
  %t12 = shl i64 %t11, 1
  %t13 = xor i64 255, %a0
  %t14 = sub i64 %t12, %t13
  %t15 = shl i16 %t10, 7
  %t16 = trunc i64 %t11 to i8
  %t17 = or i8 %t7, %t16
  %t18 = xor i16 %a1, %t10
  %t19 = or i64 %t12, %t13
  %t20 = trunc i64 %t14 to i8
  %t21 = xor i8 %t17, -1
  %t22 = and i8 %t20, %t21
  %t23 = add i8 %t22, %t17
  %t24 = and i8 %t20, %t17
  %t25 = add i8 %t23, %t24
  %t26 = and i8 %t25, %t21
  %t27 = sext i8 %t20 to i64
  %t28 = shl i64 %t27, 1
  ret i32 %t3
}

define i32 @g101_25(i8 %a0, i16 %a1, i64 %a2, i8 %a3) {
  %t1 = sext i8 %a3 to i32
  %t2 = and i32 %t1, 1486892162
  %t3 = xor i32 1486892162, -1
  %t4 = and i32 %t1, %t3
  %t5 = or i32 %t2, %t4
  %t6 = and i32 %t2, %t4
  %t7 = sub i32 %t5, %t6
  %t8 = and i64 %a2, 7
  %t9 = freeze i32 %t1
  %t10 = or i32 %t6, %t7
  %t11 = lshr i64 %a2, 0
  %t12 = ashr i64 %a2, 61
  %t13 = and i8 %a0, 15
  %t14 = and i8 %a0, -16
  %t15 = or disjoint i8 %t13, %t14
  %t16 = add i8 %t15, %a3
  %t17 = or i8 %t15, %a3
  %t18 = sub i8 %t16, %t17
  %t19 = sub i8 0, %t17
  %t20 = or i8 %t15, %t19
  %t21 = and i8 %t15, %t19
  %t22 = add i8 %t20, %t21
  %t23 = sext i8 %t16 to i32
  %t24 = zext i8 %t18 to i32
  %t25 = xor i32 %t24, -1
  %t26 = and i32 %t23, %t25
  %t27 = or i32 %t26, %t24
  %t28 = add i32 %t26, %t24
  %t29 = or i32 %t26, %t24
  %t30 = sub i32 %t28, %t29
  %t31 = add i32 %t27, %t30
  %t32 = zext i32 %t26 to i64
  %t33 = and i64 %t32, 15
  %t34 = zext i32 %t29 to i64
  %t35 = and i64 %t34, -16
  %t36 = or disjoint i64 %t33, %t35
  ret i32 %t31
}

define i16 @g101_26(i16 %a0, i32 %a1) {
  %t1 = sub i16 %a0, %a0
  %t2 = trunc i16 %t1 to i8
  %t3 = and i8 %t2, 15
  %t4 = and i8 %t3, -16
  %t5 = or disjoint i8 %t3, %t4
  %t6 = and i32 %a1, 15
  %t7 = and i32 %t6, -16
  %t8 = or disjoint i32 %t6, %t7
  %t9 = freeze i8 %t3
  %t10 = sext i8 %t9 to i64
  %t11 = and i64 %t10, 15
  %t12 = sext i32 %t7 to i64
  %t13 = and i64 %t12, -16
  %t14 = or disjoint i64 %t11, %t13
  %t15 = and i16 %t1, 15
  %t16 = and i16 %a0, -16
  %t17 = or disjoint i16 %t15, %t16
  %t18 = mul i64 2, %t11
  %t20 = sext i16 %t15 to i64
  %t19 = icmp ult i64 %t20, %t13
  %t21 = trunc i64 %t12 to i16
  %t22 = select i1 %t19, i16 %t21, i16 %t15
  %t23 = xor i16 %t22, %t21
  %t24 = xor i16 %t23, %t22
  ret i16 %t24
}

define i32 @g101_27(i64 %a0, i64 %a1, i64 %a2) {
  %t1 = trunc i64 %a1 to i16
  %t2 = icmp slt i16 %t1, %t1
  br i1 %t2, label %b1, label %b2
b1:
  %t3 = xor i16 %t1, -1
  %t4 = and i16 %t1, %t3
  %t5 = add i16 %t4, %t1
  %t6 = and i16 %t1, %t1
  %t7 = sub i16 %t5, %t6
  br label %b3
b2:
  %t8 = or i16 %t1, %t1
  %t9 = and i16 %t1, %t1
  %t10 = add i16 %t8, %t9
  %t11 = xor i16 %t1, -1
  %t12 = and i16 %t1, %t11
  %t13 = add i16 %t12, %t1
  %t14 = sub i16 %t10, %t13
  br label %b3
b3:
  %t15 = phi i16 [ %t7, %b1 ], [ %t14, %b2 ]
  %t16 = zext i16 %t15 to i32
  %t17 = and i32 %t16, 15
  %t18 = and i32 %t17, -16
  %t19 = or disjoint i32 %t17, %t18
  %t20 = xor i64 %a2, -1
  %t21 = xor i64 6656252256017440773, -1
  %t22 = and i64 %a2, %t21
  %t23 = and i64 %t20, 6656252256017440773
  %t24 = or i64 %t22, %t23
  %t25 = shl i64 %t22, 20
  %t26 = sub i64 0, %t25
  %t27 = xor i64 %t21, %t26
  %t28 = and i64 %t21, %t26
  %t29 = shl i64 %t28, 1
  %t30 = add i64 %t27, %t29
  %t31 = trunc i64 %t28 to i8
  %t32 = icmp ult i8 %t31, %t31
  br i1 %t32, label %b4, label %b5
b4:
  %t33 = sub i8 %t31, %t31
  br label %b6
b5:
  %t34 = or i8 %t31, %t31
  br label %b6
b6:
  %t35 = phi i8 [ %t33, %b4 ], [ %t34, %b5 ]
  %t36 = trunc i64 %t30 to i32
  %t37 = icmp ult i32 %t17, %t36
  br i1 %t37, label %b7, label %b8
b7:
  %t38 = sub i32 0, %t36
  %t39 = or i32 %t17, %t38
  %t40 = shl i32 %t39, 1
  %t41 = xor i32 %t17, %t38
  %t42 = sub i32 %t40, %t41
  br label %b9
b8:
  %t43 = sub i32 0, %t36
  %t44 = xor i32 %t17, %t43
  %t45 = and i32 %t17, %t43
  %t46 = shl i32 %t45, 1
  %t47 = add i32 %t44, %t46
  br label %b9
b9:
  %t48 = phi i32 [ %t42, %b7 ], [ %t47, %b8 ]
  %t49 = zext i8 %t35 to i16
  %t50 = trunc i32 %t36 to i16
  %t51 = xor i16 %t49, -1
  %t52 = xor i16 %t50, -1
  %t53 = and i16 %t49, %t52
  %t54 = and i16 %t51, %t50
  %t55 = or i16 %t53, %t54
  %t56 = add i16 %t49, %t50
  %t57 = or i16 %t49, %t50
  %t58 = sub i16 %t56, %t57
  %t59 = shl i16 %t58, 1
  %t60 = add i16 %t55, %t59
  %t61 = and i32 %t16, 3
  %t62 = xor i32 3, -1
  %t63 = and i32 %t16, %t62
  %t64 = or i32 %t61, %t63
  %t65 = add i32 %t48, %t63
  %t66 = xor i64 %t26, %t29
  %t67 = and i64 %t26, %t29
  %t68 = shl i64 %t67, 1
  %t69 = add i64 %t66, %t68
  %t70 = or i64 %t66, %t66
  ret i32 %t48
}

define i32 @g101_28(i8 %a0, i64 %a1, i32 %a2, i8 %a3) {
  %t1 = icmp ugt i32 %a2, 7
  %t2 = zext i8 %a3 to i16
  %t3 = select i1 %t1, i16 %t2, i16 %t2
  %t4 = sext i8 %a0 to i16
  %t5 = icmp ne i16 %t3, %t4
  br i1 %t5, label %b1, label %b2
b1:
  %t6 = xor i16 %t4, -1
  %t7 = and i16 %t3, %t6
  %t8 = or i16 %t7, %t4
  %t9 = and i16 %t7, %t4
  %t10 = add i16 %t8, %t9
  br label %b3
b2:
  %t11 = or i16 %t3, %t4
  %t12 = and i16 %t3, %t4
  %t13 = sub i16 %t11, %t12
  %t14 = add i16 %t3, %t4
  %t15 = or i16 %t3, %t4
  %t16 = sub i16 %t14, %t15
  %t17 = shl i16 %t16, 1
  %t18 = add i16 %t13, %t17
  br label %b3
b3:
  %t19 = phi i16 [ %t10, %b1 ], [ %t18, %b2 ]
  %t20 = ashr i64 %a1, 17
  %t21 = and i16 %t19, 0
  %t22 = xor i16 0, -1
  %t23 = and i16 %t19, %t22
  %t24 = or i16 %t21, %t23
  %t25 = shl i16 %t24, 1
  %t26 = xor i16 %t21, %t23
  %t27 = sub i16 %t25, %t26
  %t28 = mul i16 %t27, %t25
  %t29 = and i32 -809163243, 15
  %t30 = and i32 %a2, -16
  %t31 = or disjoint i32 %t29, %t30
  %t32 = add i16 %t23, %t27
  %t33 = sext i16 %t32 to i32
  %t34 = sext i16 %t32 to i32
  %t35 = xor i32 %t34, %t33
  %t36 = and i32 %t34, %t33
  %t37 = shl i32 %t36, 1
  %t38 = add i32 %t35, %t37
  %t39 = icmp eq i32 %t35, %t37
  br i1 %t39, label %b4, label %b5
b4:
  %t40 = xor i32 %t37, -1
  %t41 = and i32 %t35, %t40
  %t42 = add i32 %t41, %t37
  br label %b6
b5:
  %t43 = xor i32 %t37, -1
  %t44 = and i32 %t35, %t43
  %t45 = or i32 %t44, %t37
  %t46 = and i32 %t44, %t37
  %t47 = add i32 %t45, %t46
  br label %b6
b6:
  %t48 = phi i32 [ %t42, %b4 ], [ %t47, %b5 ]
  %t49 = trunc i32 %t37 to i16
  %t50 = add i16 %t26, %t49
  %t51 = xor i32 %t34, %t38
  %t52 = xor i32 %t51, %t48
  ret i32 %t52
}

define i8 @g101_29(i64 %a0, i64 %a1, i32 %a2, i16 %a3) {
  %t1 = xor i32 5, -1
  %t2 = and i32 %a2, %t1
  %t3 = add i32 %t2, 5
  %t4 = xor i64 1, 2
  %t5 = trunc i32 %a2 to i8
  %t6 = xor i8 %t5, -1
  %t7 = xor i8 %t5, -1
  %t8 = and i8 %t5, %t7
  %t9 = and i8 %t6, %t5
  %t10 = or i8 %t8, %t9
  %t11 = lshr i64 -5138784087150610620, 4
  %t12 = zext i8 %t5 to i16
  %t13 = ashr i16 %t12, 7
  %t14 = and i8 %t7, 7
  %t15 = xor i8 7, -1
  %t16 = and i8 %t7, %t15
  %t17 = xor i8 %t14, %t16
  %t18 = and i64 255, %t4
  %t19 = sub i16 %t12, %t12
  %t20 = sext i8 %t15 to i64
  %t21 = and i64 %t20, %t4
  %t22 = icmp ne i32 %a2, 1
  br i1 %t22, label %b1, label %b2
b1:
  %t23 = xor i32 %a2, 1
  br label %b3
b2:
  %t24 = xor i32 %a2, 1
  br label %b3
b3:
  %t25 = phi i32 [ %t23, %b1 ], [ %t24, %b2 ]
  %t26 = and i16 %t12, 15
  %t27 = and i16 %a3, -16
  %t28 = or disjoint i16 %t26, %t27
  ret i8 %t9
}

define i32 @g101_30(i64 %a0, i8 %a1) {
  %t1 = trunc i64 %a0 to i8
  %t2 = add i8 %t1, %a1
  %t3 = add i64 %a0, %a0
  %t4 = or i64 %a0, %a0
  %t5 = sub i64 %t3, %t4
  %t6 = or i64 %t3, %t5
  %t7 = sext i8 %a1 to i32
  %t8 = icmp eq i32 %t7, 3
  br i1 %t8, label %b1, label %b2
b1:
  %t9 = add i32 %t7, 3
  br label %b3
b2:
  %t10 = sub i32 0, 3
  %t11 = or i32 %t7, %t10
  %t12 = shl i32 %t11, 1
  %t13 = xor i32 %t7, %t10
  %t14 = sub i32 %t12, %t13
  br label %b3
b3:
  %t15 = phi i32 [ %t9, %b1 ], [ %t14, %b2 ]
  %t16 = xor i32 %t7, %t7
  %t17 = xor i32 %t16, %t15
  ret i32 %t17
}

define i8 @g101_31(i32 %a0) {
  %t1 = trunc i32 %a0 to i8
  %t2 = icmp slt i8 %t1, %t1
  br i1 %t2, label %b1, label %b2
b1:
  %t3 = xor i8 %t1, -1
  %t4 = and i8 %t1, %t3
  %t5 = add i8 %t4, %t1
  br label %b3
b2:
  %t6 = or i8 %t1, %t1
  br label %b3
b3:
  %t7 = phi i8 [ %t5, %b1 ], [ %t6, %b2 ]
  %t8 = trunc i32 %a0 to i16
  %t9 = or i16 %t8, %t8
  %t10 = sext i32 %a0 to i64
  %t11 = and i64 %t10, 0
  %t12 = xor i64 0, -1
  %t13 = and i64 %t10, %t12
  %t14 = or i64 %t11, %t13
  %t15 = and i32 %a0, 15
  %t16 = sext i16 %t9 to i32
  %t17 = and i32 %t16, -16
  %t18 = or disjoint i32 %t15, %t17
  %t19 = shl i32 %t16, 22
  %t20 = freeze i32 %t18
  ret i8 %t7
}

define i64 @g101_32(i32 %a0) {
  %t1 = xor i32 %a0, %a0
  %t2 = sext i32 %t1 to i64
  %t3 = zext i32 %a0 to i64
  %t4 = add i64 %t2, %t3
  %t5 = xor i32 %a0, -1
  %t6 = and i32 %t1, %t5
  %t7 = add i32 %t6, %a0
  %t8 = and i32 %t1, %a0
  %t9 = sub i32 %t7, %t8
  %t11 = trunc i64 %t4 to i16
  %t10 = icmp ugt i16 %t11, 5
  %t12 = trunc i32 %t8 to i8
  %t13 = select i1 %t10, i8 %t12, i8 %t12
  %t14 = xor i8 %t13, %t12
  %t15 = and i8 %t12, 15
  %t16 = and i8 %t14, -16
  %t17 = or disjoint i8 %t15, %t16
  %t18 = sext i8 %t15 to i64
  %t19 = xor i64 %t2, -1
  %t20 = xor i64 %t18, -1
  %t21 = and i64 %t2, %t20
  %t22 = and i64 %t19, %t18
  %t23 = or i64 %t21, %t22
  %t24 = shl i16 %t11, 14
  %t25 = ashr i8 %t13, 7
  %t26 = xor i8 %t13, %t25
  %t27 = zext i8 %t26 to i16
  %t28 = sext i8 %t25 to i16
  %t29 = xor i16 %t27, %t28
  %t30 = zext i16 %t24 to i64
  %t31 = xor i64 %t30, %t30
  ret i64 %t31
}

define i16 @g101_33(i64 %a0) {
  %t1 = sub i32 0, 2
  %t2 = add i32 1, %t1
  %t3 = xor i32 %t2, -1
  %t4 = and i32 %t2, %t3
  %t5 = add i32 %t4, %t2
  %t6 = trunc i32 %t3 to i16
  %t7 = add i16 %t6, %t6
  %t8 = zext i16 %t7 to i64
  %t9 = or i64 %a0, %t8
  %t10 = shl i64 %t9, 1
  %t11 = xor i64 %a0, %t8
  %t12 = sub i64 %t10, %t11
  %t13 = or i16 %t7, 255
  %t14 = mul i32 %t4, %t2
  %t15 = mul i16 5, %t6
  %t16 = xor i16 %t7, %t13
  %t17 = xor i16 %t16, %t15
  ret i16 %t17
}

define i64 @g101_34(i32 %a0, i8 %a1) {
  %t1 = trunc i32 %a0 to i16
  %t2 = icmp ult i16 %t1, %t1
  br i1 %t2, label %b1, label %b2
b1:
  %t3 = add i16 %t1, %t1
  br label %b3
b2:
  %t4 = and i16 %t1, %t1
  br label %b3
b3:
  %t5 = phi i16 [ %t3, %b1 ], [ %t4, %b2 ]
  %t6 = and i16 %t5, 128
  %t7 = xor i16 128, -1
  %t8 = and i16 %t5, %t7
  %t9 = or i16 %t6, %t8
  %t10 = and i16 %t6, %t8
  %t11 = add i16 %t9, %t10
  %t12 = xor i8 %a1, -1
  %t13 = and i8 %a1, %t12
  %t14 = or i8 %t13, %a1
  %t15 = shl i8 %t14, 1
  %t16 = xor i8 %t13, %a1
  %t17 = sub i8 %t15, %t16
  %t18 = zext i8 %t13 to i32
  %t19 = sub i32 0, 5
  %t20 = or i32 %t18, %t19
  %t21 = and i32 %t18, %t19
  %t22 = sub i32 %t20, %t21
  %t23 = add i32 %t18, %t19
  %t24 = or i32 %t18, %t19
  %t25 = sub i32 %t23, %t24
  %t26 = shl i32 %t25, 1
  %t27 = add i32 %t22, %t26
  %t28 = icmp eq i32 %t22, %t22
  br i1 %t28, label %b4, label %b5
b4:
  %t29 = and i32 %t22, %t22
  br label %b6
b5:
  %t30 = or i32 %t22, %t22
  %t31 = and i32 %t22, %t22
  %t32 = sub i32 %t30, %t31
  %t33 = add i32 %t22, %t22
  %t34 = or i32 %t22, %t22
  %t35 = sub i32 %t33, %t34
  %t36 = shl i32 %t35, 1
  %t37 = add i32 %t32, %t36
  br label %b6
b6:
  %t38 = phi i32 [ %t29, %b4 ], [ %t37, %b5 ]
  %t39 = zext i32 %t38 to i64
  %t40 = xor i64 %t39, %t39
  ret i64 %t40
}

define i8 @g101_35(i32 %a0, i8 %a1) {
  %t1 = and i32 %a0, -718576547
  %t2 = xor i32 -718576547, -1
  %t3 = and i32 %a0, %t2
  %t4 = xor i32 %t1, %t3
  %t5 = and i32 %t1, %t3
  %t6 = shl i32 %t5, 1
  %t7 = add i32 %t4, %t6
  %t8 = sext i32 %t7 to i64
  %t9 = xor i64 %t8, -1
  %t10 = xor i64 %t8, -1
  %t11 = and i64 %t8, %t10
  %t12 = and i64 %t9, %t8
  %t13 = or i64 %t11, %t12
  %t14 = freeze i8 %a1
  %t15 = and i32 %t3, 845457367
  %t16 = xor i32 845457367, -1
  %t17 = and i32 %t3, %t16
  %t18 = xor i32 %t15, -1
  %t19 = xor i32 %t17, -1
  %t20 = and i32 %t15, %t19
  %t21 = and i32 %t18, %t17
  %t22 = or i32 %t20, %t21
  %t23 = xor i8 %t14, %t14
  %t24 = add i8 %t14, %t14
  %t25 = or i8 %t14, %t14
  %t26 = sub i8 %t24, %t25
  %t27 = shl i8 %t26, 1
  %t28 = add i8 %t23, %t27
  %t29 = xor i8 %t14, -1
  %t30 = and i8 %t14, %t29
  %t31 = or i8 %t30, %t14
  %t32 = shl i8 %t31, 1
  %t33 = xor i8 %t30, %t14
  %t34 = sub i8 %t32, %t33
  %t35 = sub i8 %t28, %t34
  %t36 = zext i8 %t33 to i64
  %t37 = icmp slt i64 %t36, %t36
  br i1 %t37, label %b1, label %b2
b1:
  %t38 = xor i64 %t36, -1
  %t39 = and i64 %t36, %t38
  %t40 = or i64 %t39, %t36
  %t41 = shl i64 %t40, 1
  %t42 = xor i64 %t39, %t36
  %t43 = sub i64 %t41, %t42
  br label %b3
b2:
  %t44 = sub i64 0, %t36
  %t45 = xor i64 %t36, %t44
  %t46 = and i64 %t36, %t44
  %t47 = shl i64 %t46, 1
  %t48 = add i64 %t45, %t47
  br label %b3
b3:
  %t49 = phi i64 [ %t43, %b1 ], [ %t48, %b2 ]
  %t50 = and i64 %t13, 1
  %t51 = or i64 %t11, %t49
  %t52 = shl i64 %t51, 1
  %t53 = or i64 %t11, %t49
  %t54 = and i64 %t11, %t49
  %t55 = sub i64 %t53, %t54
  %t56 = sub i64 %t52, %t55
  %t57 = sub i64 0, %t54
  %t58 = xor i64 %t57, -1
  %t59 = and i64 %t54, %t58
  %t60 = add i64 %t59, %t57
  %t61 = add i64 %t54, %t57
  %t62 = or i64 %t54, %t57
  %t63 = sub i64 %t61, %t62
  %t64 = add i64 %t60, %t63
  %t65 = freeze i32 255
  ret i8 %t33
}

define i32 @g101_36(i64 %a0) {
  %t1 = add i64 %a0, %a0
  %t3 = trunc i64 %a0 to i32
  %t2 = icmp ugt i32 %t3, %t3
  %t4 = zext i1 %t2 to i64
  %t5 = icmp slt i64 %a0, %t4
  br i1 %t5, label %b1, label %b2
b1:
  %t6 = add i64 %a0, %t4
  br label %b3
b2:
  %t7 = add i64 %a0, %t4
  br label %b3
b3:
  %t8 = phi i64 [ %t6, %b1 ], [ %t7, %b2 ]
  %t9 = ashr i32 %t3, 31
  %t10 = xor i32 %t3, %t9
  %t11 = zext i32 %t10 to i64
  %t12 = sext i32 %t9 to i64
  %t13 = xor i64 %t11, %t12
  %t14 = lshr i32 1, 23
  %t15 = trunc i32 %t10 to i16
  %t16 = and i16 %t15, -1
  %t17 = xor i16 -1, -1
  %t18 = and i16 %t15, %t17
  %t19 = xor i16 %t18, -1
  %t20 = and i16 %t16, %t19
  %t21 = add i16 %t20, %t18
  %t22 = trunc i32 %t14 to i8
  %t23 = xor i8 %t22, -1
  %t24 = xor i8 %t22, -1
  %t25 = and i8 %t22, %t24
  %t26 = and i8 %t23, %t22
  %t27 = or i8 %t25, %t26
  %t28 = lshr i16 %t16, 11
  %t29 = trunc i16 %t21 to i8
  %t30 = freeze i8 %t29
  %t31 = or i16 %t18, %t21
  %t32 = shl i16 %t31, 1
  %t33 = xor i16 %t18, %t21
  %t34 = sub i16 %t32, %t33
  %t35 = or i16 %t18, %t21
  %t36 = sub i16 %t34, %t35
  %t37 = xor i8 %t24, -1
  %t38 = and i8 %t25, %t37
  %t39 = add i8 %t38, %t24
  %t40 = shl i8 %t39, 1
  %t41 = xor i8 %t25, %t24
  %t42 = sub i8 %t40, %t41
  %t43 = sext i8 %t42 to i16
  %t44 = xor i16 %t36, %t43
  %t45 = and i16 %t36, %t43
  %t46 = shl i16 %t45, 1
  %t47 = add i16 %t44, %t46
  %t48 = freeze i8 %t39
  ret i32 %t3
}

define i8 @g101_37(i16 %a0, i8 %a1) {
  %t1 = zext i8 %a1 to i16
  %t2 = ashr i16 %t1, 15
  %t3 = xor i16 %t1, %t2
  %t4 = zext i16 %t3 to i32
  %t5 = sext i16 %t2 to i32
  %t6 = xor i32 %t4, %t5
  %t7 = xor i16 3, %a0
  %t8 = xor i8 %a1, -1
  %t9 = and i8 %a1, %t8
  %t10 = add i8 %t9, %a1
  %t11 = add i8 %a1, %a1
  %t12 = or i8 %a1, %a1
  %t13 = sub i8 %t11, %t12
  %t14 = add i8 %t10, %t13
  %t15 = zext i8 %t14 to i32
  %t16 = zext i8 %t12 to i32
  %t17 = or i32 %t16, %t15
  %t18 = shl i32 %t17, 1
  %t19 = xor i32 %t16, %t15
  %t20 = sub i32 %t18, %t19
  %t21 = trunc i32 %t19 to i8
  %t22 = and i8 %t21, 15
  %t23 = and i8 %t12, -16
  %t24 = or disjoint i8 %t22, %t23
  %t25 = ashr i8 %t14, 7
  %t26 = xor i8 %t14, %t25
  %t27 = zext i8 %t26 to i16
  %t28 = sext i8 %t25 to i16
  %t29 = xor i16 %t27, %t28
  %t30 = sext i16 %t28 to i64
  %t31 = freeze i64 %t30
  %t32 = zext i8 %t26 to i16
  %t33 = icmp ne i16 %t32, %t29
  br i1 %t33, label %b1, label %b2
b1:
  %t34 = or i16 %t32, %t29
  %t35 = and i16 %t32, %t29
  %t36 = sub i16 %t34, %t35
  %t37 = add i16 %t32, %t29
  %t38 = or i16 %t32, %t29
  %t39 = sub i16 %t37, %t38
  %t40 = shl i16 %t39, 1
  %t41 = add i16 %t36, %t40
  br label %b3
b2:
  %t42 = xor i16 %t32, -1
  %t43 = xor i16 %t29, -1
  %t44 = and i16 %t32, %t43
  %t45 = and i16 %t42, %t29
  %t46 = or i16 %t44, %t45
  %t47 = and i16 %t32, %t29
  %t48 = shl i16 %t47, 1
  %t49 = add i16 %t46, %t48
  br label %b3
b3:
  %t50 = phi i16 [ %t41, %b1 ], [ %t49, %b2 ]
  %t51 = lshr i8 %t23, 1
  %t52 = xor i8 %t51, %t51
  ret i8 %t52
}

define i32 @g101_38(i32 %a0, i8 %a1) {
  %t1 = and i32 %a0, 15
  %t2 = and i32 %t1, -16
  %t3 = or disjoint i32 %t1, %t2
  %t4 = trunc i32 %a0 to i8
  %t5 = and i8 %t4, 15
  %t6 = and i8 %a1, -16
  %t7 = or disjoint i8 %t5, %t6
  %t8 = sext i32 %t3 to i64
  %t9 = zext i8 %t7 to i64
  %t10 = mul i64 %t8, %t9
  %t12 = zext i8 %t7 to i32
  %t13 = zext i8 %t4 to i32
  %t11 = icmp ne i32 %t12, %t13
  %t14 = sext i8 %t5 to i64
  %t15 = select i1 %t11, i64 %t14, i64 %t9
  %t16 = freeze i64 %t14
  %t17 = sub i64 %t10, %t16
  %t18 = trunc i64 %t16 to i16
  %t19 = xor i16 %t18, %t18
  ret i32 %t1
}

define i32 @g101_39(i64 %a0, i64 %a1, i8 %a2, i32 %a3) {
  %t1 = trunc i64 %a0 to i32
  %t2 = icmp ult i32 %a3, %t1
  br i1 %t2, label %b1, label %b2
b1:
  %t3 = xor i32 %t1, -1
  %t4 = and i32 %a3, %t3
  %t5 = add i32 %t4, %t1
  %t6 = and i32 %a3, %t1
  %t7 = sub i32 %t5, %t6
  br label %b3
b2:
  %t8 = xor i32 %a3, %t1
  br label %b3
b3:
  %t9 = phi i32 [ %t7, %b1 ], [ %t8, %b2 ]
  %t10 = xor i8 %a2, -1
  %t11 = xor i8 %a2, -1
  %t12 = and i8 %a2, %t11
  %t13 = and i8 %t10, %a2
  %t14 = or i8 %t12, %t13
  %t15 = sext i8 %t13 to i16
  %t16 = and i16 %t15, 15
  %t17 = and i16 %t16, -16
  %t18 = or disjoint i16 %t16, %t17
  %t19 = or i64 %a0, %a0
  %t20 = and i64 %a0, %a0
  %t21 = add i64 %t19, %t20
  %t22 = xor i64 %a0, -1
  %t23 = and i64 %a0, %t22
  %t24 = add i64 %t23, %a0
  %t25 = sub i64 %t21, %t24
  %t26 = xor i8 %t10, 5
  %t27 = add i8 %t10, 5
  %t28 = or i8 %t10, 5
  %t29 = sub i8 %t27, %t28
  %t30 = shl i8 %t29, 1
  %t31 = add i8 %t26, %t30
  %t32 = xor i8 5, -1
  %t33 = and i8 %t10, %t32
  %t34 = or i8 %t33, 5
  %t35 = and i8 %t33, 5
  %t36 = add i8 %t34, %t35
  %t37 = sub i8 %t31, %t36
  %t38 = add i8 %t37, %t32
  %t39 = zext i8 %t38 to i32
  %t40 = add i32 %t1, %t39
  %t41 = xor i8 %t37, %t34
  %t42 = xor i8 %t38, -1
  %t43 = xor i8 %t41, -1
  %t44 = and i8 %t38, %t43
  %t45 = and i8 %t42, %t41
  %t46 = or i8 %t44, %t45
  %t47 = icmp slt i32 %t39, %t9
  br i1 %t47, label %b4, label %b5
b4:
  %t48 = sub i32 %t39, %t9
  br label %b6
b5:
  %t49 = or i32 %t39, %t9
  br label %b6
b6:
  %t50 = phi i32 [ %t48, %b4 ], [ %t49, %b5 ]
  %t51 = and i32 %t50, -1
  %t52 = xor i32 -1, -1
  %t53 = and i32 %t50, %t52
  %t54 = or i32 %t51, %t53
  %t55 = shl i32 %t54, 1
  %t56 = xor i32 %t51, %t53
  %t57 = sub i32 %t55, %t56
  %t59 = trunc i32 %t57 to i8
  %t58 = icmp ne i8 %t11, %t59
  %t60 = zext i1 %t58 to i64
  %t61 = xor i32 %t56, %t56
  %t62 = xor i32 %t61, %t57
  ret i32 %t62
}

