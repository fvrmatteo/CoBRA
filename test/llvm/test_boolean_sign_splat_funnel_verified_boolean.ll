; RUN: opt -load-pass-plugin=%cobra_pass -passes='cobra-simplify<verified;boolean;families=semilinear;timeout=20000>' -S %s | FileCheck %s
; COBRA_PARAMS: families=semilinear;timeout=20000

; A word's "is any bit set", written as the sign of `y | -y` and carried through a
; funnel shift by one: the high part is that sign spread over the word
; (`ashr o, 63`), the low bit is the sign again (`o >> 63`), so the funnel shift is
; the high part itself, and `f + 1` is 1 when `y` is zero and 0 otherwise.
;
; Searched as a word the tree is out of reach: "is any bit of `y` set" is not a
; function of the bits of `y` one at a time, and a shift by 63 does not mean the
; same at the narrower widths the search solves in. With the sign read as one
; variable that is 0 or 1, `f` is `-c` and `f + 1` is `1 - c`, and both are built
; from the sign the function already computes.
;
; The comparison of `f` with -1 is then read on its own
; (`ReadComparisonsAsEqualities`): true exactly when `y` is not zero.
;
; CHECK-LABEL: @sign_splat(
; CHECK-NOT: @llvm.fshl
; CHECK: {{(lshr i64 %o, 63|add i64 1, %s)}}
; CHECK: store i64
; CHECK: icmp ne i64 %y, 0
define i1 @sign_splat(i64 noundef %y, ptr %p) {
  %n = sub i64 0, %y
  %o = or i64 %y, %n
  %s = ashr i64 %o, 63
  %f = call i64 @llvm.fshl.i64(i64 %s, i64 %o, i64 1)
  %r = add i64 %f, 1
  store i64 %r, ptr %p
  %c = icmp eq i64 %f, -1
  ret i1 %c
}

declare i64 @llvm.fshl.i64(i64, i64, i64)
