; RUN: rm -rf %t && split-file %s %t
; RUN: cd %t && jasmin %t/Test.j -d %t && jasmin %t/A.j
; RUN: jllvm %t/Test.class

;--- A.j

.class public A
.super java/lang/Object

.field public static a I = 0

.method public <init>()V
    .limit stack 1
    aload_0
    invokenonvirtual java/lang/Object/<init>()V
    return
.end method

;--- Test.j

.class public Test
.super java/lang/Object

.method public <init>()V
    .limit stack 1
    aload_0
    invokespecial java/lang/Object/<init>()V
    return
.end method

.method public static native print(Ljava/lang/String;)V
.end method

.method public static main([Ljava/lang/String;)V
   .limit stack 2
   iconst_0
   getstatic A/a I
   iadd
   pop
   return
.end method
