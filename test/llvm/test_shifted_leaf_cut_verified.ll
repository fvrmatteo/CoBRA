; RUN: opt -load-pass-plugin=%cobra_pass -passes='cobra-simplify<verified;families=semilinear;timeout=5000>' -S %s | FileCheck %s
; COBRA_PARAMS: families=semilinear;timeout=5000

; A mixing step's halving: `((a ^ y) >> 1) + ~floor((a + y) / 2)` over 65 bits is
; `~(a & y)`, where `a` is `y` shifted. Expanded, the tree is over the one value
; `y` and the identity is gone - a shift moves bits from one position to another,
; so it is no bitwise function of what it shifts and an atom can hold it only as a
; variable of its own.
;
; CHECK-LABEL: @shift_of_a_leaf(
; CHECK: [[A:%.*]] = lshr i64 %y, 47
; CHECK: [[AND:%.*]] = and i64 [[A]], %y
; CHECK-NEXT: [[R:%.*]] = xor i64 [[AND]], -1
; CHECK-NEXT: ret i64 [[R]]
define i64 @shift_of_a_leaf(i64 %y) {
  %a = lshr i64 %y, 47
  %t1 = xor i64 %a, %y
  %t2 = lshr i64 %t1, 1
  %s = add i64 %a, %y
  %c = icmp ult i64 %s, %y
  %h = lshr i64 %s, 1
  %m = select i1 %c, i64 -9223372036854775808, i64 0
  %o = or disjoint i64 %m, %h
  %n = xor i64 %o, -1
  %r = add i64 %t2, %n
  ret i64 %r
}

; The same where what is shifted is computed - `y = (h ^ K) * M`, as a hash's mixer
; writes it - so neither the shift nor its operand is a leaf of the full tree.
;
; CHECK-LABEL: @shift_of_a_product(
; CHECK: [[Y:%.*]] = mul i64 %x, -4132994306676758123
; CHECK: [[A:%.*]] = lshr i64 [[Y]], 47
; CHECK: [[AND:%.*]] = and i64 [[A]], [[Y]]
; CHECK-NEXT: [[R:%.*]] = xor i64 [[AND]], -1
; CHECK-NEXT: ret i64 [[R]]
define i64 @shift_of_a_product(i64 %h) {
  %x = xor i64 %h, -2562077794238846474
  %y = mul i64 %x, -4132994306676758123
  %a = lshr i64 %y, 47
  %t1 = xor i64 %a, %y
  %t2 = lshr i64 %t1, 1
  %s = add i64 %a, %y
  %c = icmp ult i64 %s, %y
  %h2 = lshr i64 %s, 1
  %m = select i1 %c, i64 -9223372036854775808, i64 0
  %o = or disjoint i64 %m, %h2
  %n = xor i64 %o, -1
  %r = add i64 %t2, %n
  ret i64 %r
}

; The control: an identity whose halving is the only shift in it is read without
; any cut, and reading the halving as a variable would lose it.
;
; CHECK-LABEL: @halving_of_two_values(
; CHECK: [[AND:%.*]] = and i64 %a, %b
; CHECK-NEXT: ret i64 [[AND]]
define i64 @halving_of_two_values(i64 %a, i64 %b) {
  %t1 = xor i64 %a, %b
  %t2 = lshr i64 %t1, 1
  %s = add i64 %a, %b
  %c = icmp ult i64 %s, %b
  %h = lshr i64 %s, 1
  %m = select i1 %c, i64 -9223372036854775808, i64 0
  %o = or disjoint i64 %m, %h
  %r = sub i64 %o, %t2
  ret i64 %r
}
