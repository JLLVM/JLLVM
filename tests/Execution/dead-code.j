; RUN: jasmin %s -d %t
; RUN: jllvm -Xenable-test-utils %t/Test.class

.class public Test
.super java/lang/Object

.method public <init>()V
    aload_0
    invokespecial java/lang/Object/<init>()V
    return
.end method

.method public static native print(Ljava/lang/String;)V
.end method

.method public static main([Ljava/lang/String;)V
   goto X
   ; CHECK-NOT: False
   ldc "False"
   invokestatic Test/print(Ljava/lang/String;)V
X:
   ; CHECK: True
   ldc "True"
   invokestatic Test/print(Ljava/lang/String;)V
   return
.end method
