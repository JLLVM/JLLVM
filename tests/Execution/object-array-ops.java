// RUN: javac %s -d %t
// RUN: jllvm -Xjit %t/Test.class | FileCheck %s
// RUN: jllvm -Xint %t/Test.class | FileCheck %s

class Test
{
    public static native void print(int i);
    public static native void print(String s);

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

		// CHECK: 0
		print(is[0].value);
        // CHECK: 1
		print(is[1].value);
        // CHECK: 2
		print(is[2].value);
        // CHECK: 3
		print(is.length);

        is = null;

        try
        {
            var o = is[0];
        }
        catch(NullPointerException e)
        {
            // CHECK: load null
            print("load null");
        }

        try
        {
            is[0] = new Test(0);
        }
        catch(NullPointerException e)
        {
            // CHECK: store null
            print("store null");
        }

        try
        {
            var l = is.length;
        }
        catch(NullPointerException e)
        {
            // CHECK: length null
            print("length null");
        }
    }
}
