; RUN: opt -load-pass-plugin=%cobra_pass -passes='cobra-simplify<verified-sweep>' -S %s | FileCheck %s
; COBRA_ENV: COBRA_TIMEOUT_MS=20000

; A sum group whose residual summand is a root rewritten earlier in the same run.
;
; `%r1` is an identity of its own, `(b ^ c) + 2*(b & c)`, which is `b + c`, and a
; root: it is stored as well as added. The sum it is added into splits by leaf
; support - `%r1` on one side, the sign extension standing on `e` on the other -
; and the group on `e` is a candidate of its own whose rewrite adds `%r1` back
; as it was read when the candidate was queued. When `%r1` is rewritten first,
; that summand is a replaced root: still in the function, unread, and deleted
; with the rest of the run's leftovers. Reading it back gives it a reader the
; deletion does not expect - the candidate is stale, as one whose tree holds a
; replaced root is, and is left for the next run, which reads the sum again from
; the simplified function.
;
; What matters is that nothing the function still reads is deleted: the output
; verifies, the store reads `%b + %c`, and the returned sum still reads it too.
; CHECK-LABEL: @replaced_residual(
; CHECK: %[[S:.*]] = add i64 %b, %c
; CHECK: store i64 %[[S]], ptr %p
; CHECK: ret i64
define i64 @replaced_residual(i64 %b, i64 %c, i32 %e, ptr %p) {
  %x1 = xor i64 %b, %c
  %a1 = and i64 %b, %c
  %s1 = shl i64 %a1, 1
  %r1 = add i64 %x1, %s1
  store i64 %r1, ptr %p
  %lobit = lshr i32 %e, 31
  %z = zext nneg i32 %lobit to i64
  %n = sub nsw i64 0, %z
  %t = trunc nsw i64 %n to i32
  %x = xor i32 %e, %t
  %u = zext i32 %x to i64
  %a = and i64 %u, %n
  %sh = shl nuw nsw i64 %a, 1
  %sum = add i64 %r1, %u
  %orr = or disjoint i64 %sh, %z
  %r = sub i64 %sum, %orr
  ret i64 %r
}
