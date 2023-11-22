// RUN: javac %s -d %t
// RUN: jllvm -Xjit %t/Test.class | FileCheck %s
// RUN: jllvm -Xint %t/Test.class | FileCheck %s

class Test
{
    public static native void print(int i);

    public static void main(String[] args)
    {
        // CHECK: 3
        print(tableswitch(3));
        // CHECK: 0
        print(tableswitch(4));
        // CHECK: 1
        print(tableswitch(5));
        // CHECK: 2
        print(tableswitch(6));
        // CHECK: 3
        print(tableswitch(7));

        // CHECK: 3
        print(lookupswitch(30));
        // CHECK: 0
        print(lookupswitch(40));
        // CHECK: 1
        print(lookupswitch(50));
        // CHECK: 2
        print(lookupswitch(60));
        // CHECK: 3
        print(lookupswitch(70));
    }

    public static int tableswitch(int val)
    {
        switch(val)
        {
            case 4:  return 0;
            case 5:  return 1;
            case 6:  return 2;
            default: return 3;
        }
    }

    public static int lookupswitch(int val)
    {
        switch(val)
        {
            case 40:  return 0;
            case 50:  return 1;
            case 60:  return 2;
            default: return 3;
        }
    }
}
