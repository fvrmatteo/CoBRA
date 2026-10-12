; RUN: opt -load-pass-plugin=%cobra_pass -passes='cobra-simplify<verified-boolean>' -S %s | FileCheck %s
; COBRA_ENV: COBRA_FAMILIES=semilinear COBRA_TIMEOUT_MS=20000

; Two conditions that agree on every input but two. `c` is `x != 90`; the sign of
; `(x - 91) | (91 - x)` is `x != 91`. Random and boundary probes all land where both
; are 1, so they look like one condition - and taken as one, the tree would read
; `-c` and `f + 1` would become `x == 90`, which is wrong at 90 (the tree gives 2)
; and at 91 (it gives -1). The solver refutes the merge, the two stay apart, and
; whatever the pass makes of the tree reads both.
;
; CHECK-LABEL: @distinct_conditions(
; CHECK-NOT: icmp eq i64 %x, 90
; CHECK: 91
; CHECK: ret i64
define i64 @distinct_conditions(i64 noundef %x) {
  %c = icmp ne i64 %x, 90
  %s = sext i1 %c to i64
  %d = sub i64 %x, 91
  %e = sub i64 91, %x
  %o = or i64 %d, %e
  %f = call i64 @llvm.fshl.i64(i64 %s, i64 %o, i64 1)
  %r = add i64 %f, 1
  ret i64 %r
}

declare i64 @llvm.fshl.i64(i64, i64, i64)
