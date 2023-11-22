// RUN: javac %s -d %t
// RUN: jllvm -Xjit %t/Test.class | FileCheck %s
// RUN: jllvm -Xint %t/Test.class | FileCheck %s

class Test
{
    public static native void print(int i);

    public static void lotsOfParamsThatArePassedOnStack(
        Object[] first,
        Object[] second,
        Object[] third,
        Object[] fourth,
        Object[] fifth,
        Object[] sixth,
        Object[] seventh,
        Object[] eighth,
        Object[] ninth,
        Object[] tenth,
        Object[] eleventh,
        Object[] twelveth,
        Object[] thirteenth,
        Object[] fourteenth,
        Object[] fifteenth)
    {
        new Object();
        // CHECK: 0
        print(first.length);
        new Object();
        // CHECK: 1
        print(second.length);
        new Object();
        // CHECK: 2
        print(third.length);
        new Object();
        // CHECK: 3
        print(fourth.length);
        new Object();
        // CHECK: 4
        print(fifth.length);
        new Object();
        // CHECK: 5
        print(sixth.length);
        new Object();
        // CHECK: 6
        print(seventh.length);
        new Object();
        // CHECK: 7
        print(eighth.length);
        new Object();
        // CHECK: 8
        print(ninth.length);
        new Object();
        // CHECK: 9
        print(tenth.length);
        new Object();
        // CHECK: 10
        print(eleventh.length);
        new Object();
        // CHECK: 11
        print(twelveth.length);
        new Object();
        // CHECK: 12
        print(thirteenth.length);
        new Object();
        // CHECK: 13
        print(fourteenth.length);
        new Object();
        // CHECK: 14
        print(fifteenth.length);
        new Object();
    }

    public static void main(String[] args)
    {
        var i = new int[5];
        lotsOfParamsThatArePassedOnStack(
            new Object[0],
            new Object[1],
            new Object[2],
            new Object[3],
            new Object[4],
            new Object[5],
            new Object[6],
            new Object[7],
            new Object[8],
            new Object[9],
            new Object[10],
            new Object[11],
            new Object[12],
            new Object[13],
            new Object[14]
        );
        // CHECK: 5
        print(i.length);
    }
}
