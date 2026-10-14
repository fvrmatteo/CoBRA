; RUN: opt -load-pass-plugin=%cobra_pass -passes='cobra-simplify<skip-loop-carried-phis>' -S %s | FileCheck %s
; COBRA_PARAMS: skip-loop-carried-phis

; A phi with an unreachable predecessor is not carried by a loop: the dominator
; tree says every block dominates an unreachable one, which is no back edge.
; With `skip_loop_carried_phis` the tree standing on it is still simplified.

; CHECK-LABEL: @unreachable_predecessor
; CHECK: add i32 %p, %y
; CHECK-NOT: xor
define i32 @unreachable_predecessor(i32 %x, i32 %y) {
entry:
  br label %join

dead:
  br label %join

join:
  %p = phi i32 [ %x, %entry ], [ %y, %dead ]
  %a = xor i32 %p, %y
  %b = and i32 %p, %y
  %s = shl i32 %b, 1
  %r = add i32 %a, %s
  ret i32 %r
}
