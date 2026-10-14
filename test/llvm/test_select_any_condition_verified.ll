; RUN: opt -load-pass-plugin=%cobra_pass -passes='cobra-simplify<verified>' -S %s | FileCheck %s

; A `select` reads its condition as the 0/1 value it is, whatever computes it.
; Read only when it was a comparison, a `select` on anything else - here an
; argument - was a leaf, and the two below were two unrelated values: their sum
; is `%x` only through the condition they share. (`%x` is frozen: under the
; arms of a `select` it may be poison where the root is not.)

; CHECK-LABEL: @select_on_an_argument(
; CHECK-NOT: select
; CHECK: %[[X:.*]] = freeze i64 %x
; CHECK-NEXT: ret i64 %[[X]]
define i64 @select_on_an_argument(i1 %b, i64 %x) {
  %s1 = select i1 %b, i64 %x, i64 0
  %s2 = select i1 %b, i64 0, i64 %x
  %r = add i64 %s1, %s2
  ret i64 %r
}

; CHECK-LABEL: @select_on_a_truncation(
; CHECK-NOT: select
; CHECK: %[[X:.*]] = freeze i64 %x
; CHECK-NEXT: ret i64 %[[X]]
define i64 @select_on_a_truncation(i8 %f, i64 %x) {
  %b = trunc i8 %f to i1
  %s1 = select i1 %b, i64 %x, i64 0
  %s2 = select i1 %b, i64 0, i64 %x
  %r = add i64 %s1, %s2
  ret i64 %r
}
