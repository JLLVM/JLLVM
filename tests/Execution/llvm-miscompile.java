// RUN: javac %s -d %t
// RUN: jllvm -Xjit %t/Test.class
// RUN: jllvm -Xint %t/Test.class

class Test
{
    public int i = 3;
    public static Test sink = null;

    public static void main(String[] args)
    {
        var t = new Test[1];
        // Causes garbage collection inbetween 't' and storing it. The dead store elimination
        // would previously falsely delete storing 'Test[].class' into 't'.
        new Object();
        sink = t[0];
    }
}
