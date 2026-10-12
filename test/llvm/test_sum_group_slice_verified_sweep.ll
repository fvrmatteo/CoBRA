; RUN: opt -load-pass-plugin=%cobra_pass -passes='cobra-simplify<verified-sweep>' -S %s | FileCheck %s
; COBRA_ENV: COBRA_TIMEOUT_MS=20000

; A width-crossing identity with a base added into the sum.
;
; The slice reading is the one thing that gets through an identity written
; across a width boundary, and it is asked about the root: is the root the
; extension of its own low bits? A base added into the sum is enough to make the
; answer no - the high bits of `DllBase + 136 + sext(e)` are the base's - and
; the whole expression then stands as written, however many of its own
; identities would fold. So the sum is split by leaf support first and the
; question is asked of each group on its own.

; The obfuscator's sign extension as a signed sum: with `s` the sign bit of `e`,
; `n = -s` and `u = zext(e ^ trunc n)`, the sum `u - 2*(u & n) - s` is `u ^ n`,
; which is `sext e`. Read at 32 bits the same terms are `x - 2*(x & -s) - s`,
; which cancels to `e`. Here with a base added, which is how the walk computes
; the data directory's address: nothing may be read off the root, and the group
; standing on `e` is read at 32 bits and extended back.
; CHECK-LABEL: @base_plus_signed_sum(
; CHECK: %[[S:.*]] = sext i32 %e to i64
; CHECK-NEXT: %[[R:.*]] = add i64 %[[S]], %b
; CHECK-NEXT: ret i64 %[[R]]
define i64 @base_plus_signed_sum(i64 %b, i32 %e) {
  %lobit = lshr i32 %e, 31
  %z = zext nneg i32 %lobit to i64
  %n = sub nsw i64 0, %z
  %t = trunc nsw i64 %n to i32
  %x = xor i32 %e, %t
  %u = zext i32 %x to i64
  %a = and i64 %u, %n
  %sh = shl nuw nsw i64 %a, 1
  %sum = add i64 %b, %u
  %orr = or disjoint i64 %sh, %z
  %r = sub i64 %sum, %orr
  ret i64 %r
}

; The same group with the base standing at the other sign: the residual keeps
; the sign it had in the root's sum, so the rewrite subtracts it.
; CHECK-LABEL: @signed_sum_minus_base(
; CHECK: %[[S:.*]] = sext i32 %e to i64
; CHECK-NEXT: %[[R:.*]] = sub i64 %[[S]], %b
; CHECK-NEXT: ret i64 %[[R]]
define i64 @signed_sum_minus_base(i64 %b, i32 %e) {
  %lobit = lshr i32 %e, 31
  %z = zext nneg i32 %lobit to i64
  %n = sub nsw i64 0, %z
  %t = trunc nsw i64 %n to i32
  %x = xor i32 %e, %t
  %u = zext i32 %x to i64
  %a = and i64 %u, %n
  %sh = shl nuw nsw i64 %a, 1
  %orr = or disjoint i64 %sh, %z
  %s1 = add i64 %orr, %b
  %r = sub i64 %u, %s1
  ret i64 %r
}

; A sum that splits, over a group that is no extension of its own low bits: the
; high half is the value again, so the bits above the slice are not decided by
; the bits below it. The probes say so and the group is dropped - splitting the
; sum buys a question to ask, not an answer.
; CHECK-LABEL: @base_plus_no_extension(
; CHECK: shl
; CHECK: or
define i64 @base_plus_no_extension(i64 %b, i32 %e) {
  %z = zext i32 %e to i64
  %hi = shl i64 %z, 32
  %o = or disjoint i64 %hi, %z
  %r = add i64 %b, %o
  ret i64 %r
}

; An `or` that is not disjoint is not an addition, so the chain may not be
; opened there: `(b | u) - z` is no sum of `b`, `u` and `-z`, and a group taken
; out of it would not be a part of the root's value. Nothing here folds.
; CHECK-LABEL: @or_not_disjoint(
; CHECK: or i64
define i64 @or_not_disjoint(i64 %b, i32 %e) {
  %lobit = lshr i32 %e, 31
  %z = zext nneg i32 %lobit to i64
  %n = sub nsw i64 0, %z
  %t = trunc nsw i64 %n to i32
  %x = xor i32 %e, %t
  %u = zext i32 %x to i64
  %a = and i64 %u, %n
  %sh = shl nuw nsw i64 %a, 1
  %orr = or disjoint i64 %sh, %z
  %mix = or i64 %b, %u
  %r = sub i64 %mix, %orr
  ret i64 %r
}
