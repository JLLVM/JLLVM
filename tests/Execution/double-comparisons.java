// RUN: javac %s -d %t
// RUN: jllvm -Xenable-test-utils %t/Test.class | FileCheck %s

class Test
{
    public static native void print(boolean b);

    public static void main(String[] args)
    {
        var nan = Double.NaN;
        var p_123 = 123.456;
        var n_123 = -123.456;

        //CHECK: 1
        print(p_123 == p_123);
        //CHECK: 0
        print(p_123 == n_123);
        //CHECK: 0
        print(p_123 == nan);
        //CHECK: 0
        print(nan == n_123);

        //CHECK: 0
        print(p_123 != p_123);
        //CHECK: 1
        print(p_123 != n_123);
        //CHECK: 1
        print(p_123 != nan);
        //CHECK: 1
        print(nan != n_123);

        //CHECK: 0
        print(p_123 < p_123);
        //CHECK: 0
        print(p_123 < n_123);
        //CHECK: 0
        print(p_123 < nan);
        //CHECK: 0
        print(nan < n_123);

        //CHECK: 1
        print(p_123 >= p_123);
        //CHECK: 1
        print(p_123 >= n_123);
        //CHECK: 0
        print(p_123 >= nan);
        //CHECK: 0
        print(nan >= n_123);

        //CHECK: 0
        print(p_123 > p_123);
        //CHECK: 1
        print(p_123 > n_123);
        //CHECK: 0
        print(p_123 > nan);
        //CHECK: 0
        print(nan > n_123);

        //CHECK: 1
        print(p_123 <= p_123);
        //CHECK: 0
        print(p_123 <= n_123);
        //CHECK: 0
        print(p_123 <= nan);
        //CHECK: 0
        print(nan <= n_123);
    }
}
