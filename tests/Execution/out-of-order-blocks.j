; RUN: jasmin %s -d %t
; RUN: jllvm -Xenable-test-utils %t/Test.class

.class public Test
.super java/lang/Object

.method public <init>()V
    aload_0
    invokespecial java/lang/Object/<init>()V
    return
.end method

.method public static native print(I)V
.end method

.method public static main([Ljava/lang/String;)V
    .limit stack 1
Block1:
    goto Block3
Block2:
    ; CHECK: 1
    invokestatic Test/print(I)V
    goto Block4
Block3:
    iconst_1
    goto Block2
Block4:
    return
.end method
