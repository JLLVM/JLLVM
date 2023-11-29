; RUN: jasmin %s -d %t
; RUN: jllvm-jvmc --method "test:()V" %t/Test.class | FileCheck %s

.class public Test
.super java/lang/Object

.field public static foo Ljava/lang/Object;

.method public <init>()V
    aload_0
    invokespecial java/lang/Object/<init>()V
    return
.end method

.method public static native print(I)V
.end method

; CHECK-LABEL: define void @"LTest;.test:()V"
.method public static test()V
    .limit stack 2
    iconst_1
    ; CHECK: call {{.*}} @jllvm_build_negative_array_size_exception(
    ; CHECK-SAME: "deopt"(i16 {{.*}}
    ; CHECK: call {{.*}} @jllvm_throw(
    ; CHECK-SAME: "deopt"(i16 {{.*}}
    anewarray Ljava/lang/Object;
    iconst_0
    ; CHECK: call {{.*}} @jllvm_build_null_pointer_exception(
    ; CHECK-SAME: "deopt"(i16 [[AALOAD_OFFSET:[0-9]+]]
    ; CHECK: call {{.*}} @jllvm_build_array_index_out_of_bounds_exception(
    ; CHECK-SAME: "deopt"(i16 [[AALOAD_OFFSET]]
    aaload
    ; CHECK: call {{.*}} @jllvm_build_class_cast_exception(
    ; CHECK-SAME: "deopt"(i16 {{.*}}
    checkcast Ljava/lang/Object;
    return
.end method
