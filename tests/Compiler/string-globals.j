; RUN: jasmin %s -d %t
; RUN: jllvm-jvmc --method "test:()V" %t/Test.class | FileCheck %s

.class public Test
.super java/lang/Object

.method public <init>()V
    aload_0
    invokespecial java/lang/Object/<init>()V
    return
.end method

.method public static native print(Ljava/lang/String;)V
.end method

; CHECK-LABEL: define void @"Test.test:()V"
.method public static test()V
    .limit stack 1
    ; CHECK: store ptr addrspace(1) @"'Test String", ptr %[[TOP:[[:alnum:]]+]]
    ldc "Test String"
    ; CHECK-NEXT: %[[ARG:[[:alnum:]]+]] = load ptr addrspace(1), ptr %[[TOP]],
    ; CHECK-NEXT: call void @"Static Call to Test.print:(Ljava/lang/String;)V"(ptr addrspace(1) %[[ARG]])
    invokestatic Test/print(Ljava/lang/String;)V
    return
.end method
