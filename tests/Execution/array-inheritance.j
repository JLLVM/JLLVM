; RUN: jasmin %s -d %t
; RUN: jllvm -Xjit %t/Test.class | FileCheck %s
; RUN: jllvm -Xint %t/Test.class | FileCheck %s

.class public Test
.super java/lang/Object

.method public <init>()V
    aload_0
    invokespecial java/lang/Object/<init>()V
    return
.end method

.method public static native print(Z)V
.end method

.method public static main([Ljava/lang/String;)V
.limit locals 1
.limit stack 4
    iconst_0
    anewarray java/lang/Object
    dup
    iconst_0
    anewarray java/lang/Object
    ; CHECK: 0
    invokevirtual [Ljava/lang/Object;.equals(Ljava/lang/Object;)Z
    invokestatic Test.print(Z)V
    dup
    ; CHECK: 1
    invokevirtual [Ljava/lang/Object;.equals(Ljava/lang/Object;)Z
    invokestatic Test.print(Z)V
    return
.end method

