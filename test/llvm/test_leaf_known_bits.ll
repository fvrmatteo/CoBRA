; RUN: opt -load-pass-plugin=%cobra_pass -passes=cobra-simplify -S %s | FileCheck %s

; A leaf carries what LLVM's value tracking knows of its bits. The phi below is a
; leaf of the tree - its arms are not the same computation - and has no bit
; outside the mask on either arm, so (slot ^ C) + slot is C: the xor is C minus
; slot, and adding slot back gives C. Without the fact the tree reads the phi as
; a free word, and (v ^ C) + v is no constant. The byte gep roots the tree, as a
; slot address computed this way does.
; CHECK-LABEL: @masked_phi_leaf
; CHECK: join:
; CHECK-NOT: xor
; CHECK: getelementptr i8, ptr %base, i64 5813524864
define ptr @masked_phi_leaf(ptr %base, i64 %x, i64 %y, i1 %c) {
entry:
  br i1 %c, label %left, label %right
left:
  %d = sub i64 %x, %y
  %m = and i64 %d, 5813524864
  br label %join
right:
  br label %join
join:
  %slot = phi i64 [ 5813524864, %right ], [ %m, %left ]
  %t = xor i64 %slot, 5813524864
  %r = add i64 %t, %slot
  %p = getelementptr i8, ptr %base, i64 %r
  ret ptr %p
}

; Known ones as well as known zeros: both arms set bit 0, so the low bit of the
; leaf is one and the tree over it is a constant.
; CHECK-LABEL: @set_bit_phi_leaf
; CHECK: join:
; CHECK-NOT: xor
; CHECK: ret i64 16
define i64 @set_bit_phi_leaf(i64 %x, i1 %c) {
entry:
  br i1 %c, label %left, label %right
left:
  %o = or i64 %x, 1
  br label %join
right:
  br label %join
join:
  %v = phi i64 [ 3, %right ], [ %o, %left ]
  %low = and i64 %v, 1
  %flip = xor i64 %low, 1
  %r = add i64 %flip, 8
  %s = mul i64 %r, 2
  ret i64 %s
}

; A leaf nothing is known about is read as it always was: no mask is added, and
; a tree that is no identity over a free word is left alone.
; CHECK-LABEL: @free_leaf
; CHECK: %t = xor i64 %v, 5813524864
; CHECK-NEXT: %r = add i64 %t, %v
; CHECK-NEXT: %p = getelementptr i8, ptr %base, i64 %r
define ptr @free_leaf(ptr %base, i64 %v) {
entry:
  %t = xor i64 %v, 5813524864
  %r = add i64 %t, %v
  %p = getelementptr i8, ptr %base, i64 %r
  ret ptr %p
}
