// RUN: javac -encoding utf-8 %s -d %t
// RUN: jllvm -Xjit %t/Test.class | FileCheck %s
// RUN: jllvm -Xint %t/Test.class | FileCheck %s

class Test
{
    public static native void print(String s);

    public static void main(String[] args)
    {
        // CHECK: öüäß
        print("öüäß");
    }
}
