// RUN: javac %s -d %t
// RUN: jllvm -Xjit %t/Test.class | FileCheck %s
// RUN: jllvm -Xint %t/Test.class | FileCheck %s

class Test
{
    public static native void print(double d);
    public static native void print(long l);

    public static void main(String[] args)
    {

        // CHECK: 123.456
        print(123.456);
        // CHECK: -987.654
        print(-987.654);

        // larger double than Float.MAX_VALUE
        // CHECK: 4.4028235e+38
        print(4.4028235E38);

        // CHECK: 123456
        print(123456l);
        // CHECK: 987654
        print(987654l);

        // larger long than Integer.MAX_VALUE
        // CHECK: 12147483647
        print(12147483647l);
    }
}
