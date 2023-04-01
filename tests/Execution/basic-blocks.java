// RUN: javac %s -d %t
// RUN: jllvm -Xenable-test-utils %t/Test.class | FileCheck %s

class Test
{
    public static native void print(int i);

    public int value;

    public Test(int value)
    {
        this.value = value;
    }

    public static void main(String[] args)
    {
        var is = new Test[3];
        for (int i = 0; i < 3; i++)
        {
            is[i] = new Test(i);
        }

	// Something random inbetween, maybe GCs.
	new Object();

        for (int i = 0; i < 3; i++)
        {
            print(is[i].value);
        }
	print(is.length);
    }
}


// CHECK: 0
// CHECK: 1
// CHECK: 2
// CHECK: 3