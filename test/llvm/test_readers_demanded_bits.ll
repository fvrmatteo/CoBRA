; RUN: opt -load-pass-plugin=%cobra_pass -passes=cobra-simplify -S %s | FileCheck %s

; The demanded bits of a root are answered from its readers rather than by
; LLVM's whole-function analysis, and have to be the same. These functions put
; the readers in the shapes where the two could part: a value carried around a
; loop, whose answer is a fixpoint over the cycle rather than a sum of settled
; readers; a value read only as an address, whose reader is not an integer; and
; a value read through a `freeze`. The `LLVMPass.demanded_check.*` tests run
; this file, and every other one here, answering each question both ways.

; `(a ^ y) + ((a & y) << 1)` is `a + y`, and only its low byte is ever read -
; by the store, by the returned mask, and through the phi by the next
; iteration's copy of itself.
; CHECK-LABEL: @loop_carried(
; CHECK: ret i64
define i64 @loop_carried(i64 %x, i64 %y, i64 %n, ptr %p) {
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %acc = phi i64 [ %x, %entry ], [ %acc.next, %loop ]
  %xo = xor i64 %acc, %y
  %an = and i64 %acc, %y
  %tw = shl i64 %an, 1
  %acc.next = add i64 %xo, %tw
  %low = trunc i64 %acc.next to i8
  store i8 %low, ptr %p
  %i.next = add i64 %i, 1
  %done = icmp eq i64 %i.next, %n
  br i1 %done, label %exit, label %loop

exit:
  %m = and i64 %acc.next, 255
  ret i64 %m
}

; The same identity read only as an offset into memory: every bit is read.
; CHECK-LABEL: @address_only(
; CHECK: ret i8
define i8 @address_only(ptr %base, i64 %x, i64 %y) {
  %xo = xor i64 %x, %y
  %an = and i64 %x, %y
  %tw = shl i64 %an, 1
  %sum = add i64 %xo, %tw
  %slot = getelementptr i8, ptr %base, i64 %sum
  %v = load i8, ptr %slot
  ret i8 %v
}

; Read through a `freeze` by a single bit: the bits the `freeze`'s readers look
; at are the ones the expression is asked for.
; CHECK-LABEL: @through_freeze(
; CHECK: ret i1
define i1 @through_freeze(i32 %x, i32 %y) {
  %xo = xor i32 %x, %y
  %an = and i32 %x, %y
  %tw = shl i32 %an, 1
  %sum = add i32 %xo, %tw
  %f = freeze i32 %sum
  %bit = trunc i32 %f to i1
  ret i1 %bit
}

; Two loop-carried values reading each other, one of them dead outside the
; loop, so part of the cycle is never demanded at all.
; CHECK-LABEL: @two_carried(
; CHECK: ret i16
define i16 @two_carried(i16 %x, i16 %y, i16 %n) {
entry:
  br label %loop

loop:
  %i = phi i16 [ 0, %entry ], [ %i.next, %loop ]
  %a = phi i16 [ %x, %entry ], [ %a.next, %loop ]
  %b = phi i16 [ %y, %entry ], [ %b.next, %loop ]
  %o = or i16 %a, %b
  %c = and i16 %a, %b
  %a.next = sub i16 %o, %c
  %nb = xor i16 %b, -1
  %b.next = and i16 %nb, %a.next
  %i.next = add i16 %i, 1
  %done = icmp eq i16 %i.next, %n
  br i1 %done, label %exit, label %loop

exit:
  %r = lshr i16 %a.next, 8
  ret i16 %r
}
