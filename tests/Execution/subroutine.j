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

.method public static oneCall()V
    .limit stack 2
    ldc "Test 0"
    ; CHECK: "Test 0"
    jsr Print

    return
Print:
    astore_0
    invokestatic Test/print(Ljava/lang/String;)V
    ret 0
.end method

.method public static multipleCalls()V
    .limit stack 2
    ldc "Test 1"
    ; CHECK: "Test 1"
    jsr Print

    ldc "Test 2"
    ; CHECK: "Test 2"
    jsr Print

    ldc "Test 3"
    ; CHECK: "Test 2"
    jsr Print

    return
Print:
    astore_0
    invokestatic Test/print(Ljava/lang/String;)V
    ret 0
.end method

.method public static nestedSubroutines()V
    .limit stack 2
    .limit locals 2
    ; CHECK: "Test 4"
    jsr Print

    return
Print:
    astore_0
    jsr Load
    invokestatic Test/print(Ljava/lang/String;)V
    ret 0
Load:
    astore_1
    ldc "Test 4"
    ret 1
.end method

.method public static outOfLineRet()V
    .limit stack 2
    ; CHECK: "Test 5"
    ldc "Test 5"
    jsr Print

    return
Ret:
    ret 0
Print:
    astore_0
    invokestatic Test/print(Ljava/lang/String;)V
    goto Ret
.end method

.method public static branchInSubroutine()V
    .limit stack 3
    ; CHECK: "Test 6"
    ldc "Test 6"
    iconst_0
    jsr PrintIf

    ; CHECK-NOT: "Test X0"
    ldc "Test X0"
    iconst_1
    jsr PrintIf

    ; CHECK-NOT: "Test X1"
    ldc "Test X1"
    iconst_0
    jsr PrintIf

    return
PrintIf:
    astore_0
    ifeq Print
    return
Print:
    invokestatic Test/print(Ljava/lang/String;)V
    ret 0
.end method

.method public static subroutineReturning()V
    .limit stack 2
    ; CHECK: "Test 7"
    jsr Load
    invokestatic Test/print(Ljava/lang/String;)V

    return
Load:
    astore_0
    ldc "Test 7"
    ret 0
.end method

.method public static wideSubroutine()V
    .limit stack 2
    .limit locals 300
    ldc "Test 8"
    ; CHECK: "Test 8"
    jsr_w Print

    return
Print:
    astore 280
    invokestatic Test/print(Ljava/lang/String;)V
    ret_w 280
.end method

.method public static retFromParentSubroutine()V
    .limit stack 2
    .limit locals 2
    ; CHECK: "Test 9"
    ; CHECK-NOT: Test X2
    jsr SR0

    return
SR0:
    astore_0
    jsr SR1
    ldc "Test X2"
    invokestatic Test/print(Ljava/lang/String;)V
    ret 0
SR1:
    pop
    ldc "Test 9"
    invokestatic Test/print(Ljava/lang/String;)V
    ret 0
.end method

.method public static main([Ljava/lang/String;)V
    invokestatic  Test/oneCall()V
    invokestatic  Test/multipleCalls()V
    invokestatic  Test/nestedSubroutines()V
    invokestatic  Test/outOfLineRet()V
    invokestatic  Test/branchInSubroutine()V
    invokestatic  Test/subroutineReturning()V
    invokestatic  Test/wideSubroutine()V
    invokestatic  Test/retFromParentSubroutine()V
    return
.end method
