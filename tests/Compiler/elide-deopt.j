; RUN: jasmin %s -d %t
; RUN: jllvm-jvmc --method "noExcept:()V" %t/Test.class | FileCheck %s --check-prefix=NO_EXCEPT
; RUN: jllvm-jvmc --method "except:()V" %t/Test.class | FileCheck %s --check-prefix=EXCEPT

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

; CHECK-LABEL: define void @"Test.noExcept:()V"
.method public static noExcept()V
    .limit stack 2
    .limit locals 1
    iconst_0
    ; NO_EXCEPT: call void @"Static Call to Test.print:(I)V"
    ; NO_EXCEPT-SAME: "deopt"(i16 1, i16 0)
    invokestatic Test/print(I)V
    return
.end method

; CHECK-LABEL: define void @"Test.except:()V"
.method public static except()V
    .limit stack 2
    .limit locals 1
    iconst_0
    ; EXCEPT: call void @"Static Call to Test.print:(I)V"
    ; EXCEPT-SAME: "deopt"(i16 1, i16 1, {{[^,]*}})
start:
    invokestatic Test/print(I)V
end:
    return

.catch all from start to end using end

.end method
