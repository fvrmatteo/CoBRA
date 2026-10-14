; RUN: opt -load-pass-plugin=%cobra_pass -passes='cobra-simplify<retry-undecided-roots>' -S %s | FileCheck %s
; COBRA_PARAMS: retry-undecided-roots

; `retry_undecided_roots` holds a turned-down root's fingerprint until its
; ladder has decided - it does not drop it. This tree has no simpler form and
; every cut of it is turned down outright, so the root is fingerprinted as it is
; without the option, and the next run skips it.

; CHECK-LABEL: @rejected
; CHECK: %c = xor i32 %b, %x, !cobra.mba
define i32 @rejected(i32 %x, i32 %y, i32 %z) {
  %a = and i32 %x, %y
  %b = add i32 %a, %z
  %c = xor i32 %b, %x
  ret i32 %c
}
