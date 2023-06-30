// RUN: javac %s -d %t
// RUN: jllvm %t/Test.class | FileCheck %s

class Test
{
    public static native void print(String i);
    public static native void print(int i);

    public int[] i = new int[5];

    public static void main(String[] args)
    {
        var other = new Other();
        new Object();
        new Object();
        new Object();
        new Object();
        // CHECK: 5
        print(other.i.length);
    }
}

class Other extends Test
{

}
