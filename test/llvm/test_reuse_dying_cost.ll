; RUN: opt -load-pass-plugin=%cobra_pass -passes='cobra-simplify<dying-cost>' -S %s | FileCheck %s

; A rewrite that reads what the function already holds. `(x | y) - (x & y)` is
; `x ^ y`, so the root is `(x ^ y) + z + w`: three instructions, built from the
; leaves. Replacing the root removes three as well - the `or` and the `and` are
; stored, so they stay - and a rewrite that is no smaller than what it removes
; is turned down. But the function computes `x ^ y` already, in %e, which is
; stored and stays whatever happens to the root. Reading it costs nothing, the
; rewrite builds two instructions against the three it removes, and it goes in.

; CHECK-LABEL: @reads_what_is_there(
; CHECK: %e = xor i64 %x, %y
; CHECK-NOT: sub
; CHECK-NOT: xor
; CHECK: [[SUM:%.*]] = add i64 {{%w, %z|%z, %w}}
; CHECK-NEXT: [[ROOT:%.*]] = add i64 {{.*}}%e
; CHECK-NEXT: ret i64 [[ROOT]]
define i64 @reads_what_is_there(i64 %x, i64 %y, i64 %z, i64 %w, ptr %out) {
  %e = xor i64 %x, %y
  store i64 %e, ptr %out
  %o = or i64 %x, %y
  %n = and i64 %x, %y
  %po = getelementptr i64, ptr %out, i64 1
  store i64 %o, ptr %po
  %pn = getelementptr i64, ptr %out, i64 2
  store i64 %n, ptr %pn
  %d = sub i64 %o, %n
  %s = add i64 %d, %z
  %r = add i64 %s, %w
  ret i64 %r
}

; The same root where nothing computes `x ^ y`: the rewrite would build as much
; as it removes, and the function is left alone.

; CHECK-LABEL: @builds_as_much_as_it_removes(
; CHECK: %d = sub i64 %o, %n
; CHECK: %r = add i64 %s, %w
; CHECK-NEXT: ret i64 %r
define i64 @builds_as_much_as_it_removes(i64 %x, i64 %y, i64 %z, i64 %w, ptr %out) {
  %o = or i64 %x, %y
  %n = and i64 %x, %y
  store i64 %o, ptr %out
  %pn = getelementptr i64, ptr %out, i64 1
  store i64 %n, ptr %pn
  %d = sub i64 %o, %n
  %s = add i64 %d, %z
  %r = add i64 %s, %w
  ret i64 %r
}
