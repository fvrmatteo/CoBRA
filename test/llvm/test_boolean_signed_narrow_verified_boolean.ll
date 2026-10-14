; RUN: opt -load-pass-plugin=%cobra_pass -passes='cobra-simplify<verified;boolean;families=semilinear;timeout=20000>' -S %s | FileCheck %s
; COBRA_PARAMS: families=semilinear;timeout=20000

; A signed comparison of a narrow value is not taken into the boolean cut. The tree
; holds `w` zero-extended, where `w <s 0` would read a sign bit that is not there:
; read that way it is false on every input, the same condition as `w <u 0` beside it
; (which is false on every input), the two would be merged, and `f + 1` would be
; rewritten without `c` - wrong for every negative `w`. Every proof involved would
; pass, because every one is asked of that reading: what keeps it out is the
; collection, which takes in only the comparisons that are exact on zero-extended
; values (equality and the unsigned ones). `c` stays a leaf and stays read.
;
; CHECK-LABEL: @signed_narrow(
; CHECK: %c = icmp slt i16 %w, 0
; CHECK: sext i1 %c to i64
; CHECK: ret i64
define i64 @signed_narrow(i16 noundef %w) {
  %c = icmp slt i16 %w, 0
  %d = icmp ult i16 %w, 0
  %s = sext i1 %c to i64
  %t = sext i1 %d to i64
  %f = call i64 @llvm.fshl.i64(i64 %s, i64 %t, i64 1)
  %r = add i64 %f, 1
  ret i64 %r
}

declare i64 @llvm.fshl.i64(i64, i64, i64)
