; The long negation of `test_wide_root_cut_verified.ll` one level deeper: `~y` written
; as `-y - 1`. A cut of the nodes nearest the root expands the value the negation is
; over, `x`, into the bytes before it reaches `-y`, so no breadth-first cut is the
; identity over one value. The tree reads `y` twice, and the cut that holds what the
; tree reads more than once is the identity over `y` whatever its depth.
;
; RUN: opt -load-pass-plugin=%cobra_pass -passes='cobra-simplify<verified;wide-roots;max-vars=4;timeout=2000>' -S %s | FileCheck %s
; COBRA_PARAMS: wide-roots;max-vars=4;timeout=2000
;
; CHECK-LABEL: @deeper_negation(
; CHECK-NOT: = shl
; CHECK-NOT: = or i64
; CHECK: ret i64
define i64 @deeper_negation(ptr %bytes, ptr %out) {
entry:
  %v1 = getelementptr i8, ptr %bytes, i64 0
  %v2 = load i8, ptr %v1, align 1, !noundef !0
  %v3 = sext i8 %v2 to i64
  %v4 = xor i64 -3750763034362895579, %v3
  %v5 = mul i64 %v4, 1099511628211
  %v6 = getelementptr i8, ptr %bytes, i64 1
  %v7 = load i8, ptr %v6, align 1, !noundef !0
  %v8 = sext i8 %v7 to i64
  %v9 = xor i64 %v5, %v8
  %v10 = mul i64 %v9, 1099511628211
  %v11 = getelementptr i8, ptr %bytes, i64 2
  %v12 = load i8, ptr %v11, align 1, !noundef !0
  %v13 = sext i8 %v12 to i64
  %v14 = xor i64 %v10, %v13
  %v15 = mul i64 %v14, 1099511628211
  %v16 = getelementptr i8, ptr %bytes, i64 3
  %v17 = load i8, ptr %v16, align 1, !noundef !0
  %v18 = sext i8 %v17 to i64
  %v19 = xor i64 %v15, %v18
  %v20 = mul i64 %v19, 1099511628211
  %v21 = getelementptr i8, ptr %bytes, i64 4
  %v22 = load i8, ptr %v21, align 1, !noundef !0
  %v23 = sext i8 %v22 to i64
  %v24 = xor i64 %v20, %v23
  %v25 = mul i64 %v24, 1099511628211
  %v26 = getelementptr i8, ptr %bytes, i64 5
  %v27 = load i8, ptr %v26, align 1, !noundef !0
  %v28 = sext i8 %v27 to i64
  %v29 = xor i64 %v25, %v28
  %v30 = mul i64 %v29, -1099511628211
  %v31 = mul i64 %v30, -1
  %v32 = sub i64 %v31, 1
  %v33 = shl i64 %v32, 1
  %v34 = or i64 %v33, 2
  %v35 = xor i64 %v30, -2
  %v36 = sub i64 %v34, %v35
  ret i64 %v36
}

!0 = !{}
