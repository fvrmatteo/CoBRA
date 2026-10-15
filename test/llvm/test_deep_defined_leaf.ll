; RUN: opt -load-pass-plugin=%cobra_pass -passes='cobra-simplify' -S %s | FileCheck %s

; A leaf the rewrite reads more than once is frozen unless it is known never to
; be `undef`: each read of `undef` may see a different value. What decides that
; can lie far beneath the leaf. Here the leaf `%x` is nine operations above two
; loads the host marks `!noundef`, and every operation between them only passes
; `undef` on, so `%x` is never `undef` - though LLVM's own answer gives up six
; operands down. A leaf known to be defined needs no `freeze` - and unfrozen it
; is the value the function already masks, so `%n255` is read, not built again.
;
; The root, `(x | 255) - 255 + 2 * (x & 65280) + (x ^ 7) + 2 * (x & 7)`, is
; rewritten as `(x & -256) + 2 * (x & 65280) + x + 7`, which reads `%x` three
; times; the divisions keep `%x` a leaf of the tree.

; CHECK-LABEL: @deep_over_noundef_loads(
; CHECK-NOT:   freeze
; CHECK:       and i64 %x,
; CHECK:       mul i64 2, %n255
; CHECK:       ret i64
define i64 @deep_over_noundef_loads(ptr %p, ptr %q) {
  %l0 = load i64, ptr %p, align 8, !noundef !0
  %l1 = load i64, ptr %q, align 8, !noundef !0
  %d1 = udiv i64 %l0, 3
  %d2 = udiv i64 %d1, %l1
  %d3 = udiv i64 %d2, 5
  %d4 = udiv i64 %d3, 7
  %d5 = udiv i64 %d4, 11
  %d6 = udiv i64 %d5, 13
  %d7 = udiv i64 %d6, 17
  %d8 = udiv i64 %d7, 19
  %x = udiv i64 %d8, 23
  %o = or i64 %x, 255
  %n255 = and i64 %x, 65280
  %s = sub i64 %o, 255
  %t = shl i64 %n255, 1
  %u = add i64 %s, %t
  %v = xor i64 %x, 7
  %w = and i64 %x, 7
  %w2 = shl i64 %w, 1
  %r1 = add i64 %v, %w2
  %r = add i64 %u, %r1
  ret i64 %r
}

; The same leaf over a load that is not marked may be `undef`, and is frozen.

; CHECK-LABEL: @deep_over_plain_load(
; CHECK:       %[[F:.*]] = freeze i64 %x
; CHECK:       and i64 %[[F]],
; CHECK:       ret i64
define i64 @deep_over_plain_load(ptr %p, ptr %q) {
  %l0 = load i64, ptr %p, align 8, !noundef !0
  %l1 = load i64, ptr %q, align 8
  %d1 = udiv i64 %l0, 3
  %d2 = udiv i64 %d1, %l1
  %d3 = udiv i64 %d2, 5
  %d4 = udiv i64 %d3, 7
  %d5 = udiv i64 %d4, 11
  %d6 = udiv i64 %d5, 13
  %d7 = udiv i64 %d6, 17
  %d8 = udiv i64 %d7, 19
  %x = udiv i64 %d8, 23
  %o = or i64 %x, 255
  %n255 = and i64 %x, 65280
  %s = sub i64 %o, 255
  %t = shl i64 %n255, 1
  %u = add i64 %s, %t
  %v = xor i64 %x, 7
  %w = and i64 %x, 7
  %w2 = shl i64 %w, 1
  %r1 = add i64 %v, %w2
  %r = add i64 %u, %r1
  ret i64 %r
}

; A loop-carried leaf is defined when everything its cycle reads from outside
; is: the first time the loop computes `%i.next` it reads the `!noundef` load
; through `%i`, and every later time a value computed the same way.

; CHECK-LABEL: @cycle_over_noundef_load(
; CHECK-NOT:   freeze
; CHECK:       and i64 %i,
; CHECK:       ret i64
define i64 @cycle_over_noundef_load(ptr %p, i64 noundef %n) {
entry:
  %l0 = load i64, ptr %p, align 8, !noundef !0
  br label %loop

loop:
  %i = phi i64 [ %l0, %entry ], [ %i.next, %loop ]
  %i.next = udiv i64 %i, %n
  %c = icmp ugt i64 %i.next, 9
  br i1 %c, label %loop, label %exit

exit:
  %o = or i64 %i, 255
  %n255 = and i64 %i, 65280
  %s = sub i64 %o, 255
  %t = shl i64 %n255, 1
  %u = add i64 %s, %t
  %v = xor i64 %i, 7
  %w = and i64 %i, 7
  %w2 = shl i64 %w, 1
  %r1 = add i64 %v, %w2
  %r = add i64 %u, %r1
  ret i64 %r
}

; And not when the cycle reads something that may be `undef`: here the divisor.

; CHECK-LABEL: @cycle_over_plain_load(
; CHECK:       %[[F:.*]] = freeze i64 %i
; CHECK:       and i64 %[[F]],
; CHECK:       ret i64
define i64 @cycle_over_plain_load(ptr %p, ptr %q) {
entry:
  %l0 = load i64, ptr %p, align 8, !noundef !0
  %n = load i64, ptr %q, align 8
  br label %loop

loop:
  %i = phi i64 [ %l0, %entry ], [ %i.next, %loop ]
  %i.next = udiv i64 %i, %n
  %c = icmp ugt i64 %i.next, 9
  br i1 %c, label %loop, label %exit

exit:
  %o = or i64 %i, 255
  %n255 = and i64 %i, 65280
  %s = sub i64 %o, 255
  %t = shl i64 %n255, 1
  %u = add i64 %s, %t
  %v = xor i64 %i, 7
  %w = and i64 %i, 7
  %w2 = shl i64 %w, 1
  %r1 = add i64 %v, %w2
  %r = add i64 %u, %r1
  ret i64 %r
}

!0 = !{}
