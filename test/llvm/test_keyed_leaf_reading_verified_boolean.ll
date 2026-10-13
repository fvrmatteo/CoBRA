; RUN: opt -load-pass-plugin=%cobra_pass -passes='cobra-simplify<verified-boolean>' -S %s | FileCheck %s
; COBRA_ENV: COBRA_FAMILIES=semilinear COBRA_TIMEOUT_MS=20000

; A value stored encrypted with a constant key `k` is read back as `a ^ k`, and
; the tree below computes `(a ^ k) - 2` from it the long way: half of
; `(a ^ k) - 1`, the borrow of that subtraction put back into the top bit as a
; comparison, and half of `a ^ k` again, spelled `(a >> 1) ^ c` with
; `c = ~(k >> 1) & (2^63 - 1)`. The tree reads `a` through the key once and
; bare three times, and over `a` no technique finds the answer: the probes sit
; at structured values of `a`, which are values of `a ^ k` with no structure.
; Read over `y = a ^ k` the same tree is `(y - 1 >> 1) + (y >> 1)` with the
; borrow, which is `y - 2`, and the answer is put back with the key.
; CHECK-LABEL: @keyed_halves
; CHECK: [[KEYED:%.*]] = xor i64 %a, 81985529216486895
; CHECK-NEXT: [[SUM:%.*]] = add i64 [[KEYED]], -2
; CHECK-NEXT: ret i64 [[SUM]]
define i64 @keyed_halves(i64 %a) {
  %bare_half = lshr i64 %a, 1
  %high = xor i64 %bare_half, 9182379272246532360
  %keyed = xor i64 %a, 81985529216486895
  %less = add i64 %keyed, -1
  %borrow = icmp eq i64 %a, 81985529216486895
  %low = lshr i64 %less, 1
  %top = select i1 %borrow, i64 -2, i64 9223372036854775806
  %sum = add i64 %top, %low
  %r = sub i64 %sum, %high
  ret i64 %r
}
