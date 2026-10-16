; RUN: opt -load-pass-plugin=%cobra_pass -passes='cobra-simplify<verified;boolean;families=semilinear;timeout=1000>' -S %s | FileCheck %s
; COBRA_PARAMS: families=semilinear;timeout=1000

; A 32-bit product held in 64 bits, its sign split off and put back on bit 31:
; `(x & 2^31) == 0 ? (x * k) : -((-x) * k)`, masked to 32 bits, is `(x * k) & mask`.
; A tree that compares is read with its conditions as variables first, and that
; reading finds the rewrite - but its proof, with the condition put back over the
; value it tests and a 64-bit product on both sides, takes the solver far longer
; than the budget here. The ordinary search proposes the same rewrite and it is
; proved at once, so when the reading's proof does not succeed the ordinary rewrite
; is proposed and proved in its place.
;
; CHECK-LABEL: @sign_split_low_word(
; CHECK-NEXT: [[C:%.*]] = sext i8 %b to i64
; CHECK-NEXT: [[X:%.*]] = xor i64
; CHECK-NEXT: [[M:%.*]] = mul i64
; CHECK-SAME: 16777619
; CHECK-NEXT: [[A:%.*]] = and i64 [[M]], 4294967295
; CHECK-NEXT: ret i64 [[A]]
define i64 @sign_split_low_word(i64 %h, i8 %b) {
  %c = sext i8 %b to i64
  %x = xor i64 %h, %c
  %neg = sub nsw i64 0, %x
  %bit = and i64 %x, 2147483648
  %pos = icmp eq i64 %bit, 0
  %v = select i1 %pos, i64 %x, i64 %neg
  %m = mul nsw i64 %v, 16777619
  %mneg = sub nsw i64 0, %m
  %r0 = select i1 %pos, i64 %m, i64 %mneg
  %r = and i64 %r0, 4294967295
  ret i64 %r
}
