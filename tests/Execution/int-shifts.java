// RUN: javac %s -d %t
// RUN: jllvm -Xjit %t/Test.class | FileCheck %s
// RUN: jllvm -Xint %t/Test.class | FileCheck %s

class Test
{
    public static native void print(int i);

    public static void main(String[] args)
    {
        int x = 5;
        int y = 2;
        int z = -1;

        //CHECK: 20
        print(x << y);
        //CHECK: 64
        print(y << x);
        //CHECK: -4
        print(z << y);

        //CHECK: 1
        print(x >> y);
        //CHECK: 0
        print(y >> x);
        //CHECK: -1
        print(z >> y);

        //CHECK: 1
        print(x >>> y);
        //CHECK: 0
        print(y >>> x);
        //CHECK: 1073741823
        print(z >>> y);
    }
}
