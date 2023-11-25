; RUN: jasmin %s -d %t
; RUN: jllvm -Xjit %t/Test.class
; RUN: jllvm -Xint %t/Test.class

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

.method public static main([Ljava/lang/String;)V
.limit stack 2
Block1:
    new java/lang/Object
    dup
    invokespecial java/lang/Object/<init>()V
    iconst_1
    ifge Block3
Block2:
    pop
    iconst_0
    goto Block4
Block3:
    putstatic Test/foo Ljava/lang/Object;
    iconst_1
Block4:
    ; CHECK: 1
    invokestatic Test/print(I)V
    return
.end method
