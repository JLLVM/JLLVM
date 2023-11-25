// RUN: javac %s -d %t
// RUN: jllvm -Xjit %t/Test.class | FileCheck %s
// RUN: jllvm -Xint %t/Test.class | FileCheck %s

class Test
{
    public static native void print(boolean b);

    public static void main(String[] args)
    {
        int x = 5;
        int y = -7;
        int z = 0;

        // ifne
        //CHECK: 0
        print(y == 0);
        //CHECK: 1
        print(z == 0);

        //ifeq
        //CHECK: 1
        print(y != 0);
        //CHECK: 0
        print(z != 0);

        //iflt
        //CHECK: 1
        print(x >= 0);
        //CHECK: 0
        print(y >= 0);

        //ifge
        //CHECK: 1
        print(y < 0);
        //CHECK: 0
        print(x < 0);

        //ifgt
        //CHECK: 1
        print(y <= 0);
        //CHECK: 0
        print(x <= 0);

        //ifle
        //CHECK: 1
        print(x > 0);
        //CHECK: 0
        print(y > 0);

        //ificmpge
        //CHECK: 1
        print(y < x);
        //CHECK: 0
        print(x < y);

        //ificmpeq
        //CHECK: 1
        print(x != y);
        //CHECK: 0
        print(x != 5);

        //ificmpne
        //CHECK: 1
        print(x == 5);
        //CHECK: 0
        print(x == y);

        //ificmplt
        //CHECK: 1
        print(x >= y);
        //CHECK: 0
        print(y >= x);

        //ificmpgt
        //CHECK: 1
        print(y <= x);
        //CHECK: 0
        print(x <= y);

        //ificmple
        //CHECK: 1
        print(x > y);
        //CHECK: 0
        print(y > x);

        //ifacmpeq
        //CHECK: 1
        print("a" != "b");
        //CHECK: 0
        print("a" != "a");

        //ifacmpne
        //CHECK: 1
        print("a" == "a");
        //CHECK: 0
        print("a" == "b");
    }
}
