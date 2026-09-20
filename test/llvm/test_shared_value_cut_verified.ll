; RUN: opt -load-pass-plugin=%cobra_pass -passes='cobra-simplify<verified>' -S %s | FileCheck %s

; An identity written over an operand that is an expression in its own right.
; With A = %a and B = %b, the average of A and B with its carry put back on
; top, less half of `A ^ B`, is `A & B`, and `((A & B) ^ B) + (A & B)` is `B`.
;
; B is a product of a word put together under bitwise operators, so expanding
; it leaves nothing to find: a product under `xor` and `and` is no sum of atoms
; over its own operands. Neither of the older cuts keeps it whole either - a
; breadth-first prefix reaches it before it reaches the operators that read it
; from further down, and the boundary cut expands it through the `add` that
; reads it beside them. The shared-value cut does: the tree reads `%b` four
; times, so it stays a variable, and the root is the value the function already
; holds. Nothing is rebuilt and the identity is all that goes.

; CHECK-LABEL: @shared_operand(
; CHECK: %b = mul i64 %w, 3
; CHECK-NOT: lshr
; CHECK-NOT: select
; CHECK: ret i64 %b
define i64 @shared_operand(i32 %x, i64 %a) {
  %sx = sext i32 %x to i64
  %hi = and i64 %sx, -281470681808896
  %lo32 = and i32 %x, 65535
  %lo = zext i32 %lo32 to i64
  %neg = icmp sgt i32 %x, -1
  %mid16 = select i1 %neg, i16 -24240, i16 24239
  %mid64 = zext i16 %mid16 to i64
  %mid = shl i64 %mid64, 32
  %w0 = or i64 %hi, %lo
  %w1 = or i64 %w0, %mid
  %w = xor i64 %w1, 177364969478570
  %b = mul i64 %w, 3
  %nb = xor i64 %b, -1
  %x1 = xor i64 %a, %b
  %half = lshr i64 %x1, 1
  %sum = add i64 %a, %b
  %carry = icmp ugt i64 %a, %nb
  %sumh = lshr i64 %sum, 1
  %top = select i1 %carry, i64 -9223372036854775808, i64 0
  %avg = or i64 %sumh, %top
  %both = sub i64 %avg, %half
  %t = xor i64 %both, %b
  %r = add i64 %t, %both
  ret i64 %r
}
