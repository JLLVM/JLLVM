// RUN: javac %s -d %t
// RUN: jllvm %t/Test.class | FileCheck %s

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
		is[0] = new Test(0);
		is[1] = new Test(1);
		is[2] = new Test(2);

		// Something random inbetween, maybe GCs.
		new Object();

		print(is[0].value);
		print(is[1].value);
		print(is[2].value);
		print(is.length);
    }
}

// CHECK: 0
// CHECK: 1
// CHECK: 2
// CHECK: 3
