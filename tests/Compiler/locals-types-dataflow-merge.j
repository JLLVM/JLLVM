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

.method public static native deopt()V
.end method

.method public static native random()I
.end method

; CHECK-LABEL: define void @"Test.test:()V"
.method public static test()V
    .limit stack 2
    .limit locals 2
    iconst_0
    istore_0
    fconst_1
    fstore_1
    invokestatic Test/random()I
    ifne handler
start:
    aconst_null
    astore_1
    invokestatic Test/random()I
    ifne handler
end:
    return

handler:
    ; Dataflow algorithm should have determined that the second local has an inconsistent type.
    ; CHECK: call void @"Static Call to Test.deopt:()V"
    ; CHECK-SAME: "deopt"(i16 {{[0-9]+}}, i16 2, i32 {{.*}}, i8 poison)
    invokestatic Test/deopt()V
    return
endHandler:
    pop
    return

.catch all from handler to endHandler using endHandler

.end method
