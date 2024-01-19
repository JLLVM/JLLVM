// RUN: javac %s -d %t
// RUN: jllvm %t/Test.class | FileCheck --match-full-lines %s

import java.lang.reflect.Array;

class Test
{
    public static void main(String[] args)
    {
        newInstance();
        getLength();
    }

    private static void newInstance()
    {
        // CHECK: [C@{{[a-f0-9]*}}
        System.out.println(Array.newInstance(char.class, 1));
        // CHECK: {{\[\[\[D@[a-f0-9]*}}
        System.out.println( Array.newInstance(double.class, 1, 2, 3));

        try
        {
            Array.newInstance(null, 1);
        }
        catch (NullPointerException e)
        {
            // CHECK: java.lang.NullPointerException
            System.out.println(e.getClass().getName());
        }

        try
        {
            Array.newInstance(void.class, 1);
        }
        catch (IllegalArgumentException e)
        {
            // CHECK: java.lang.IllegalArgumentException
            System.out.println(e);
        }

        try
        {
            Array.newInstance(double.class, -1);
        }
        catch (NegativeArraySizeException e)
        {
            // CHECK: java.lang.NegativeArraySizeException: -1
            System.out.println(e);
        }
    }

    private static void getLength()
    {
        // CHECK: 4
        System.out.println(Array.getLength(new float[4]));

        try
        {
            Array.getLength(null);
        }
        catch (NullPointerException e)
        {
            // CHECK: java.lang.NullPointerException
            System.out.println(e.getClass().getName());
        }

        try
        {
            Array.getLength(new Object());
        }
        catch (IllegalArgumentException e)
        {
            // CHECK: java.lang.IllegalArgumentException: Argument is not an array
            System.out.println(e);
        }
    }
}
