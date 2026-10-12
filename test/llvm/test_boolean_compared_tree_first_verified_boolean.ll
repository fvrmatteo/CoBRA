; RUN: opt -load-pass-plugin=%cobra_pass -passes='cobra-simplify<verified-boolean>' -S %s | FileCheck %s
; COBRA_ENV: COBRA_FAMILIES=semilinear COBRA_TIMEOUT_MS=20000

; A tree that compares is read with its conditions as variables before it is
; searched. `d != 0` and the sign of `d | (K - y)` are one condition (`d = y - K`),
; so the funnel shift is its sign extension; the value `y` is a `select` over a
; hash of a `select` over a frozen narrow read. On the reading that keeps `y` whole,
; the search proposes a rewrite fitted to random probes - they almost never land on
; `y = K`, where the comparison turns - and the proof refutes it; refuted, the
; candidate was done, and the reading of its conditions, which finds the right
; answer, was never asked.
;
; CHECK-LABEL: @compared_tree_first(
; CHECK-NOT: @llvm.fshl.i64(i64
; CHECK: icmp eq i64 %{{.*}}, 0
; CHECK: ret i1
define i1 @compared_tree_first(i64 %t0, i8 %t1, i64 %t2, i64 %t3) {
  %t4 = freeze i8 %t1
  %t5 = sext i8 %t4 to i64
  %t6 = or i64 %t0, %t5
  %t7 = and i64 %t0, %t5
  %t8 = icmp slt i64 %t6, %t7
  %t9 = sub i64 %t2, %t3
  %t10 = xor i64 %t7, -1
  %t11 = xor i64 %t9, %t10
  %t12 = lshr i64 %t11, 1
  %t13 = sub i64 %t9, %t10
  %t14 = lshr i64 %t13, 1
  %t15 = icmp eq i64 %t0, %t5
  %t16 = select i1 %t15, i64 0, i64 -9223372036854775808
  %t17 = or disjoint i64 %t14, %t16
  %t18 = sub i64 %t12, %t17
  %t19 = sub i64 0, %t18
  %t20 = and i64 %t18, -9223372036854775808
  %t21 = or i64 %t9, %t7
  %t22 = and i64 %t21, 9223372036854775807
  %t23 = or disjoint i64 %t20, %t22
  %t24 = xor i64 %t23, 9223372036854775807
  %t25 = select i1 %t8, i64 %t19, i64 %t24
  %t26 = mul i64 %t25, 1099511628211
  %t27 = sub i64 0, %t26
  %t28 = select i1 %t8, i64 %t27, i64 %t26
  %t29 = add i64 %t28, -24041939960651397
  %t30 = icmp ne i64 %t29, 0
  %t31 = sext i1 %t30 to i64
  %t32 = sub i64 24041939960651397, %t28
  %t33 = or i64 %t29, %t32
  %t34 = call i64 @llvm.fshl.i64(i64 %t31, i64 %t33, i64 1)
  %t35 = icmp eq i64 %t34, -1
  ret i1 %t35
}

declare i64 @llvm.fshl.i64(i64, i64, i64)
