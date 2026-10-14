; RUN: opt -load-pass-plugin=%cobra_pass -passes='cobra-simplify' -S %s | FileCheck %s

; A leaf narrower than the tree is read through a mask of its width, and the
; search takes `v & 0xffffffff` for an operator over `v`: the sum below,
; `((a + b | 1) - (a ^ b) & -2) + (a ^ b)`, is `a + b` with `b` a free word and
; out of every technique's reach with `b` a 32-bit read. Read again with the leaf
; free, the answer holds for every word and so for the ones the mask lets
; through, and the mask it is put back with is the leaf's own width, which the
; rebuild drops.
; CHECK-LABEL: @narrow_leaf_sum
; CHECK: %b = zext i32 %b32 to i64
; CHECK-NEXT: [[SUM:%.*]] = add i64 %a, %b
; CHECK-NEXT: ret i64 [[SUM]]
define i64 @narrow_leaf_sum(i64 %a, ptr %p) {
  %b32 = load i32, ptr %p
  %b = zext i32 %b32 to i64
  %x = xor i64 %a, %b
  %s = add i64 %a, %b
  %o = or i64 %s, 1
  %d = sub i64 %o, %x
  %m = and i64 %d, -2
  %r = add i64 %m, %x
  ret i64 %r
}

; The same with a leaf whose high bits are known zero: a phi of two masked
; words, read through the mask of what is known of it.
; CHECK-LABEL: @known_bits_leaf_sum
; CHECK: join:
; CHECK-NEXT: %b = phi
; CHECK-NEXT: [[SUM:%.*]] = add i64 %a, %b
; CHECK-NEXT: ret i64 [[SUM]]
define i64 @known_bits_leaf_sum(i64 %a, i64 %x, i64 %y, i1 %c) {
entry:
  br i1 %c, label %left, label %right
left:
  %l = and i64 %x, 65535
  br label %join
right:
  %r0 = and i64 %y, 255
  br label %join
join:
  %b = phi i64 [ %l, %left ], [ %r0, %right ]
  %xo = xor i64 %a, %b
  %s = add i64 %a, %b
  %o = or i64 %s, 1
  %d = sub i64 %o, %xo
  %m = and i64 %d, -2
  %r = add i64 %m, %xo
  ret i64 %r
}
