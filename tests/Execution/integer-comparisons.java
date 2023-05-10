// RUN: javac %s -d %t
// RUN: jllvm -Xenable-test-utils %t/Test.class | FileCheck %s

class Test
{
    public static native void print(int i);

    public static void main(String[] args)
    {
        int x = 5;
        int y = -7;
        int z = 0;

        // ifne
        //CHECK: 0
        if (y == 0)
        {
            print(1);
        }
        else
        {
            print(0);
        }
        //CHECK: 1
        if (z == 0)
        {
            print(1);
        }
        else
        {
            print(0);
        }

        //ifeq
        //CHECK: 1
        if (y != 0)
        {
            print(1);
        }
        else
        {
            print(0);
        }
        //CHECK: 0
        if (z != 0)
        {
            print(1);
        }
        else
        {
            print(0);
        }

        //iflt
        //CHECK: 1
        if (x >= 0)
        {
            print(1);
        }
        else
        {
            print(0);
        }
        //CHECK: 0
        if (y >= 0)
        {
            print(1);
        }
        else
        {
            print(0);
        }

        //ifge
        //CHECK: 1
        if (y < 0)
        {
            print(1);
        }
        else
        {
            print(0);
        }
        //CHECK: 0
        if (x < 0)
        {
            print(1);
        }
        else
        {
            print(0);
        }

        //ifgt
        //CHECK: 1
        if (y <= 0)
        {
            print(1);
        }
        else
        {
            print(0);
        }
        //CHECK: 0
        if (x <= 0)
        {
            print(1);
        }
        else
        {
            print(0);
        }

        //ifle
        //CHECK: 1
        if (x > 0)
        {
            print(1);
        }
        else
        {
            print(0);
        }
        //CHECK: 0
        if (y > 0)
        {
            print(1);
        }
        else
        {
            print(0);
        }

        //ificmpge
        //CHECK: 1
        if (y < x)
        {
            print(1);
        }
        else
        {
            print(0);
        }
        //CHECK: 0
        if (x < y)
        {
            print(1);
        }
        else
        {
            print(0);
        }

        //ificmpeq
        //CHECK: 1
        if (x != y)
        {
            print(1);
        }
        else
        {
            print(0);
        }
        //CHECK: 0
        if (x != 5)
        {
            print(1);
        }
        else
        {
            print(0);
        }

        //ificmpne
        //CHECK: 1
        if (x == 5)
        {
            print(1);
        }
        else
        {
            print(0);
        }
        //CHECK: 0
        if (x == y)
        {
            print(1);
        }
        else
        {
            print(0);
        }

        //ificmplt
        //CHECK: 1
        if (x >= y)
        {
            print(1);
        }
        else
        {
            print(0);
        }
        //CHECK: 0
        if (y >= x)
        {
            print(1);
        }
        else
        {
            print(0);
        }

        //ificmpgt
        //CHECK: 1
        if (y <= x)
        {
            print(1);
        }
        else
        {
            print(0);
        }
        //CHECK: 0
        if (x <= y)
        {
            print(1);
        }
        else
        {
            print(0);
        }

        //ificmple
        //CHECK: 1
        if (x > y)
        {
            print(1);
        }
        else
        {
            print(0);
        }
        //CHECK: 0
        if (y > x)
        {
            print(1);
        }
        else
        {
            print(0);
        }
    }
}