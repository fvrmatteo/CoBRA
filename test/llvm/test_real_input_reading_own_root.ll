; A root read over its real inputs whose tree over the support stops at the root
; itself. Read at the width of the value it reads (`narrow-roots`), the 16-bit
; `select` is a variable of its own to the collection: the answer over that
; variable is the root, which the proof accepts - the root is the root - and
; which, put in the root's place, reads its own result. The reading turns such a
; tree down, and the select stays (its condition may still be read on its own).
;
; RUN: opt -load-pass-plugin=%cobra_pass -passes='cobra-simplify<verified;real-inputs;narrow-roots;max-vars=10;timeout=1000>' -S %s | FileCheck %s
; COBRA_PARAMS: verified;real-inputs;narrow-roots;max-vars=10;timeout=1000

; CHECK-LABEL: @select_on_a_narrowed_value(
; CHECK: [[S:%.*]] = select i1 {{%.*}}, i16 1, i16 0
; CHECK-NEXT: ret i16 [[S]]
define i16 @select_on_a_narrowed_value(i64 %x) {
entry:
  %half = lshr i64 %x, 1
  %low = trunc i64 %half to i16
  %c = icmp ult i16 %low, 1
  %s = select i1 %c, i16 1, i16 0
  ret i16 %s
}
