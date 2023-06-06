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

.method public static goto_w(I)V
    .limit stack 2
    iload_0
    iconst_1
    if_icmpne Else
    iconst_0
    invokestatic  Test/print(I)V
    goto_w End
Else:
    iconst_1
    invokestatic  Test/print(I)V
End:
    return
.end method

.method public static main([Ljava/lang/String;)V
.limit stack 4
    iconst_0
    ; CHECK: 1
    invokestatic  Test/goto_w(I)V

    iconst_1
    ; CHECK: 0
    invokestatic  Test/goto_w(I)V

    return
.end method
