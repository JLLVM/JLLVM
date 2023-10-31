; RUN: jasmin %s -d %t
; RUN: jllvm %t/Test.class | FileCheck %s

.class public Test
.super java/lang/Object

.method public <init>()V
    aload_0
    invokespecial java/lang/Object/<init>()V
    return
.end method

.method public static native print(F)V
.end method

.method public static native print(I)V
.end method

.method public static raise()V
    .limit stack 2
    new java/lang/RuntimeException
    dup
    invokespecial java/lang/RuntimeException/<init>()V
    athrow
.end method

; Check that smaller types such as int or float can be read and restored properly as well.
.method public static main([Ljava/lang/String;)V
    .limit locals 2
    .limit stack 2
    ldc 3.25
    fstore 0
    iconst_5
    istore_1
Lstart:
    invokestatic Test/raise()V
    return
Lend:
    fload_0
    ; CHECK: 3.25
    invokestatic Test/print(F)V
    iload_1
    ; CHECK: 5
    invokestatic Test/print(I)V
    return

.catch all from Lstart to Lend using Lend

.end method
