; A word-by-word decryption whose key is the sum of the words already decrypted.
; Each word is written back as an expression of the word read and the key that
; is the same constant whatever the word read holds, so every store is a
; constant - but only once its key is. Word n's key is word n-1's result plus
; word n-1's key, and a tree reaches about two words back, so without
; `propagate-constants` a run rewrites the words whose key is already a
; constant and leaves the rest for the runs after it.
;
; With it, a root rewritten to a constant is folded into its readers, and the
; roots reading what it changed are collected again in the same run: the one
; run decrypts every word, and the loads it no longer needs go with it.
;
; RUN: opt -load-pass-plugin=%cobra_pass -passes='cobra-simplify<verified>' -S %s | FileCheck %s
; COBRA_PARAMS: propagate-constants;families=semilinear;timeout=10000
;
; CHECK-LABEL: @decrypt_words(
; CHECK-NOT: load
; CHECK: store i32 675353482
; CHECK: store i32 -369081131
; CHECK: store i32 1227590988
; CHECK: store i32 -545733247
; CHECK: store i32 -2008948528
; CHECK: store i32 790661269
; CHECK: ret void
define void @decrypt_words(ptr %words) {
  %p0 = getelementptr i8, ptr %words, i64 0
  %w0 = load i32, ptr %p0, align 1
  %t0 = xor i32 %w0, 675353482
  %t1 = or i32 %w0, 1
  %t2 = add i32 %t1, 675353482
  %t3 = sub i32 %t2, %t0
  %t4 = and i32 %t3, -2
  %t5 = sub i32 %t0, %w0
  %t6 = add i32 %t5, %t4
  store i32 %t6, ptr %p0, align 1
  %t7 = add i32 %t6, -714939979
  %p1 = getelementptr i8, ptr %words, i64 4
  %w1 = load i32, ptr %p1, align 1
  %t8 = xor i32 %t7, -396603883
  %t9 = xor i32 %t7, 396603882
  %t10 = or i32 %t9, %w1
  %t11 = xor i32 %w1, -1
  %t12 = or i32 %t8, %t11
  %t13 = xor i32 %t8, %w1
  %t14 = xor i32 %t13, -1
  %t15 = sub i32 %t12, %t10
  %t16 = and i32 %t15, -2
  %t17 = sub i32 %t13, %t16
  %t18 = and i32 %t17, -2
  %t19 = add i32 %t18, %t14
  %t20 = xor i32 %t19, %w1
  %t21 = and i32 %t20, %w1
  %t22 = and i32 %t20, %t19
  %t23 = sub i32 %t22, %t21
  store i32 %t23, ptr %p1, align 1
  %t24 = add i32 %t23, %t7
  %p2 = getelementptr i8, ptr %words, i64 8
  %w2 = load i32, ptr %p2, align 1
  %t25 = xor i32 %t24, 1366313127
  %t26 = xor i32 %t24, -1366313128
  %t27 = or i32 %t26, %w2
  %t28 = xor i32 %w2, -1
  %t29 = or i32 %t25, %t28
  %t30 = xor i32 %t25, %w2
  %t31 = xor i32 %t30, -1
  %t32 = sub i32 %t29, %t27
  %t33 = and i32 %t32, -2
  %t34 = sub i32 %t30, %t33
  %t35 = and i32 %t34, -2
  %t36 = add i32 %t35, %t31
  %t37 = xor i32 %t36, %w2
  %t38 = and i32 %t37, %w2
  %t39 = and i32 %t37, %t36
  %t40 = sub i32 %t39, %t38
  store i32 %t40, ptr %p2, align 1
  %t41 = add i32 %t40, %t24
  %p3 = getelementptr i8, ptr %words, i64 12
  %w3 = load i32, ptr %p3, align 1
  %t42 = xor i32 %t41, 273218846
  %t43 = xor i32 %t41, -273218847
  %t44 = or i32 %t43, %w3
  %t45 = xor i32 %w3, -1
  %t46 = or i32 %t42, %t45
  %t47 = xor i32 %t42, %w3
  %t48 = xor i32 %t47, -1
  %t49 = sub i32 %t46, %t44
  %t50 = and i32 %t49, -2
  %t51 = sub i32 %t47, %t50
  %t52 = and i32 %t51, -2
  %t53 = add i32 %t52, %t48
  %t54 = xor i32 %t53, %w3
  %t55 = and i32 %t54, %w3
  %t56 = and i32 %t54, %t53
  %t57 = sub i32 %t56, %t55
  store i32 %t57, ptr %p3, align 1
  %t58 = add i32 %t57, %t41
  %p4 = getelementptr i8, ptr %words, i64 16
  %w4 = load i32, ptr %p4, align 1
  %t59 = xor i32 %t58, 1744212942
  %t60 = xor i32 %t58, -1744212943
  %t61 = or i32 %t60, %w4
  %t62 = xor i32 %w4, -1
  %t63 = or i32 %t59, %t62
  %t64 = xor i32 %t59, %w4
  %t65 = xor i32 %t64, -1
  %t66 = sub i32 %t63, %t61
  %t67 = and i32 %t66, -2
  %t68 = sub i32 %t64, %t67
  %t69 = and i32 %t68, -2
  %t70 = add i32 %t69, %t65
  %t71 = xor i32 %t70, %w4
  %t72 = and i32 %t71, %w4
  %t73 = and i32 %t71, %t70
  %t74 = sub i32 %t73, %t72
  store i32 %t74, ptr %p4, align 1
  %t75 = add i32 %t74, %t58
  %p5 = getelementptr i8, ptr %words, i64 20
  %w5 = load i32, ptr %p5, align 1
  %t76 = xor i32 %t75, 1213536987
  %t77 = xor i32 %t75, -1213536988
  %t78 = or i32 %t77, %w5
  %t79 = xor i32 %w5, -1
  %t80 = or i32 %t76, %t79
  %t81 = xor i32 %t76, %w5
  %t82 = xor i32 %t81, -1
  %t83 = sub i32 %t80, %t78
  %t84 = and i32 %t83, -2
  %t85 = sub i32 %t81, %t84
  %t86 = and i32 %t85, -2
  %t87 = add i32 %t86, %t82
  %t88 = xor i32 %t87, %w5
  %t89 = and i32 %t88, %w5
  %t90 = and i32 %t88, %t87
  %t91 = sub i32 %t90, %t89
  store i32 %t91, ptr %p5, align 1
  ret void
}
