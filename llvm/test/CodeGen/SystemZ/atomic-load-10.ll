; NOTE: Assertions have been autogenerated by utils/update_llc_test_checks.py UTC_ARGS: --version 5
; Test fp16 atomic loads.
;
; RUN: llc < %s -mtriple=s390x-linux-gnu -verify-machineinstrs | FileCheck %s -check-prefix=CHECK
; RUN: llc < %s -mtriple=s390x-linux-gnu -verify-machineinstrs -mcpu=z16 | FileCheck %s -check-prefix=VECTOR

define half @f1(ptr %src) {
; CHECK-LABEL: f1:
; CHECK:       # %bb.0:
; CHECK-NEXT:    lgh %r0, 0(%r2)
; CHECK-NEXT:    sllg %r0, %r0, 48
; CHECK-NEXT:    ldgr %f0, %r0
; CHECK-NEXT:    # kill: def $f0h killed $f0h killed $f0d
; CHECK-NEXT:    br %r14
;
; VECTOR-LABEL: f1:
; VECTOR:       # %bb.0:
; VECTOR-NEXT:    vlreph %v0, 0(%r2)
; VECTOR-NEXT:    br %r14
  %val = load atomic half, ptr %src seq_cst, align 2
  ret half %val
}
