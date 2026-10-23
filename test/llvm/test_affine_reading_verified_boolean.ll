; RUN: opt -load-pass-plugin=%cobra_pass -passes='cobra-simplify<verified;boolean;affine;families=semilinear+bitwise;timeout=20000>' -S %s | FileCheck %s
; COBRA_PARAMS: affine;families=semilinear+bitwise;timeout=20000

; The affine reading (`affine`): a root that is an affine function of its
; leaves, xored with a constant, on every input - whatever the pieces it is
; written in look like to the semilinear reading.

; `(d + K) - ((2d + 2) & (2K - 2))` over `d = x + ~y - w` is `(x - y - w) ^ (K - 1)`,
; written through `a ^ b = a + b - 2 * (a & b)`. The semilinear reading holds the
; sum under the `&` as an atom; the constant xored in is no constant of the tree.
;
; CHECK-LABEL: @xor_encoded_sum(
; CHECK-NOT: and i64
; CHECK: [[X:%[a-z0-9.]+]] = xor i64 {{%[a-z0-9.]+}}, 7640891576956012807
; CHECK-NEXT: ret i64 [[X]]
define i64 @xor_encoded_sum(i64 %x, i64 %y, i64 %w) {
  %ny = xor i64 %y, -1
  %s = sub i64 %ny, %w
  %d = add i64 %x, %s
  %a = add i64 %d, 7640891576956012808
  %t = shl i64 %d, 1
  %u = add i64 %t, 2
  %m = and i64 %u, -3164960919797526002
  %r = sub i64 %a, %m
  ret i64 %r
}

; Two halves of a word, each xored with a constant and packed with a shift and a
; disjoint or, the word xored with the constant that cancels both: the halves
; packed, `0x10000 * zext(a) + zext(b)`.
;
; CHECK-LABEL: @packed_word(
; CHECK-NOT: xor
; CHECK-DAG: [[B:%[a-z0-9.]+]] = zext i16 %b to i32
; CHECK-DAG: [[A:%[a-z0-9.]+]] = zext i16 %a to i32
; CHECK: [[H:%[a-z0-9.]+]] = mul i32 65536, [[A]]
; CHECK-NEXT: [[W:%[a-z0-9.]+]] = add i32 [[B]], [[H]]
; CHECK-NEXT: ret i32 [[W]]
define i32 @packed_word(i16 %a, i16 %b) {
  %ka = xor i16 %a, 7982
  %ea = zext i16 %ka to i32
  %ha = shl nuw i32 %ea, 16
  %kb = xor i16 %b, 15692
  %eb = zext i16 %kb to i32
  %w = or disjoint i32 %ha, %eb
  %r = xor i32 %w, 523124044
  ret i32 %r
}

; A sign extension written out by the sign bit of the value it extends:
; `(-(x >> 31) | 0x7FFFFFFF) & (x | 0xFFFFFFFF00000000)` over `x = zext v`.
; Read as an affine function of `x` and its top bit, it is `(x ^ 2^31) - 2^31`,
; which is `sext v`.
;
; CHECK-LABEL: @sign_extension(
; CHECK-NOT: lshr
; CHECK: [[F:%[a-z0-9.]+]] = xor i64 %x, 2147483648
; CHECK-NEXT: [[S:%[a-z0-9.]+]] = add i64 [[F]], -2147483648
; CHECK-NEXT: ret i64 [[S]]
define i64 @sign_extension(i32 %v) {
  %x = zext i32 %v to i64
  %s = lshr i64 %x, 31
  %n = sub nsw i64 0, %s
  %lo = or i64 %n, 2147483647
  %hi = or disjoint i64 %x, -4294967296
  %r = and i64 %lo, %hi
  ret i64 %r
}

; `2z - ((-4z >> 1) | carry)`, the carry into the top bit picked by testing the
; narrow value against zero: `-4z >> 1` is `2^63 - 2z` but at zero, where the
; carry is zero too, and the root is `4z`.
;
; CHECK-LABEL: @halving_with_carry(
; CHECK-NOT: icmp
; CHECK: [[R:%[a-z0-9.]+]] = mul i64 4, %z
; CHECK-NEXT: ret i64 [[R]]
define i64 @halving_with_carry(i16 %v) {
  %z = zext i16 %v to i64
  %twice = shl nuw nsw i64 %z, 1
  %neg = mul nsw i64 %z, -4
  %half = lshr exact i64 %neg, 1
  %zero = icmp eq i16 %v, 0
  %carry = select i1 %zero, i64 0, i64 -9223372036854775808
  %top = or disjoint i64 %half, %carry
  %r = sub i64 %twice, %top
  ret i64 %r
}

; The same index spelled another way: `carry + 2z - (-4z >> 1)`, the carry and
; the halving added and subtracted rather than or-ed. Read on the leaf, `4z`.
;
; CHECK-LABEL: @halving_with_carry_added(
; CHECK-NOT: icmp
; CHECK-NOT: lshr
; CHECK: [[R:%[a-z0-9.]+]] = mul i64 4, %z
; CHECK-NEXT: ret i64 [[R]]
define i64 @halving_with_carry_added(i16 %v) {
  %z = zext i16 %v to i64
  %neg = mul nsw i64 %z, -4
  %zero = icmp eq i16 %v, 0
  %half = lshr exact i64 %neg, 1
  %carry = select i1 %zero, i64 0, i64 -9223372036854775808
  %twice = shl nuw nsw i64 %z, 1
  %sum = add i64 %carry, %twice
  %r = sub i64 %sum, %half
  ret i64 %r
}

; The halving and its carry as summands of a sum that also adds a value of other
; leaves, which is no affine function of anything: the root is not one, but the
; summands over `v` are, read as a group of their own once the carry is read
; with the comparison that picks it, and the rest of the sum is added back.
;
; CHECK-LABEL: @carry_group_beside_another_term(
; CHECK-NOT: lshr
; CHECK: [[M:%[a-z0-9.]+]] = mul i64 4, %z
; CHECK-NEXT: [[S:%[a-z0-9.]+]] = add i64 [[M]], %sel
; CHECK-NEXT: ret i64 [[S]]
define i64 @carry_group_beside_another_term(i64 %a, i64 %b, i64 %c, i16 %v) {
  %cmp = icmp ult i64 %a, %b
  %sel = select i1 %cmp, i64 0, i64 %c
  %z = zext i16 %v to i64
  %twice = shl nuw nsw i64 %z, 1
  %neg = mul nsw i64 %z, -4
  %half = lshr exact i64 %neg, 1
  %zero = icmp eq i16 %v, 0
  %carry = select i1 %zero, i64 0, i64 -9223372036854775808
  %s1 = add i64 %sel, %twice
  %s2 = sub i64 %s1, %half
  %r = sub i64 %s2, %carry
  ret i64 %r
}
