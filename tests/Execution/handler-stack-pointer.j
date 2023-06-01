; RUN: jasmin %s -d %t
; RUN: jllvm -Xenable-test-utils %t/Test.class

.class public Test
.super java/lang/Object

.field public static foo Ljava/lang/RuntimeException;

.method public <init>()V
    aload_0
    invokespecial java/lang/Object/<init>()V
    return
.end method

.method public static native print(Ljava/lang/String;)V
.end method

.method public static main([Ljava/lang/String;)V
.limit stack 4
.catch java/lang/RuntimeException from L1 to Handler using Handler
L1:
    iconst_0 ; bottom of stack is the integer

    new java/lang/RuntimeException
    dup
    ldc "Hello"
    invokespecial java/lang/RuntimeException/<init>(Ljava/lang/String;)V
    athrow

Handler:
    putstatic Test/foo Ljava/lang/RuntimeException;
    return
.end method
