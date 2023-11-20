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

; CHECK-LABEL: define void @"Test.test:()V"
.method public static test()V
    .limit stack 2
    .limit locals 7
    iconst_0
    istore_0
    ldc2_w 3
    lstore_1
    fconst_2
    fstore_3
    aconst_null
    astore 4
    ldc2_w 8.5d
    dstore 5

    iconst_0
    ; CHECK: %[[BITCAST_F32:.*]] = bitcast float %{{.*}} to i32
    ; CHECK: %[[BITCAST_F64:.*]] = bitcast double %{{.*}} to i64
    ; CHECK: call void @"Static Call to Test.print:(I)V"
    ; CHECK-SAME: "deopt"(i16 {{[0-9]+}}, i16 7, i32 %{{.*}}, i64 %{{.*}}, i8 poison, i32 %[[BITCAST_F32]], ptr addrspace(1) %{{.*}}, i64 %[[BITCAST_F64]], i8 poison)
start:
    invokestatic Test/print(I)V
end:
    return

.catch all from start to end using end

.end method
