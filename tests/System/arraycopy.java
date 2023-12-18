// RUN: javac %s -d %t
// RUN: jllvm -Xenable-test-utils %t/Test.class | FileCheck --match-full-lines %s

import java.util.Arrays;

class Test
{
    static native void print(String i);

    public static void main(String[] args)
    {
        // CHECK: Hello World!
        print("Hello".concat(" World!"));

        var bArr1 = new byte[]{1, 2, 3};
        var bArr2 = new byte[3];

        System.arraycopy(bArr1, 0, bArr2, 0, bArr1.length);
        // CHECK: [1, 2, 3]
        print(Arrays.toString(bArr2));

        var cArr1 = new char[]{'a', 'b', 'c'};
        var cArr2 = new char[]{'_', '_', '_', '_', '_',};

        System.arraycopy(cArr1, 0, cArr2, 1, cArr1.length);
        // CHECK: [_, a, b, c, _]
        print(Arrays.toString(cArr2));

        var dArr = new double[]{0, 1, 2, 3, 4};

        System.arraycopy(dArr, 0, dArr, 1, dArr.length - 1);
        // CHECK: [0.0, 0.0, 1.0, 2.0, 3.0]
        print(Arrays.toString(dArr));

        var fArr = new float[]{0, 1, 2, 3, 4};

        System.arraycopy(fArr, 1, fArr, 0, fArr.length - 1);
        // CHECK: [1.0, 2.0, 3.0, 4.0, 4.0]
        print(Arrays.toString(fArr));

        var iArr = new int[]{0, 1, 2, 3, 4};

        System.arraycopy(iArr, 0, iArr, 1, iArr.length - 1);
        // CHECK: [0, 0, 1, 2, 3]
        print(Arrays.toString(iArr));

        var lArr = new long[]{0, 1, 2, 3, 4};

        System.arraycopy(lArr, 1, lArr, 0, lArr.length - 1);
        // CHECK: [1, 2, 3, 4, 4]
        print(Arrays.toString(lArr));

        var sArr1 = new short[]{9, 8, 7};
        var sArr2 = new short[5];

        System.arraycopy(sArr1, 0, sArr2, 2, sArr1.length);
        // CHECK: [0, 0, 9, 8, 7]
        print(Arrays.toString(sArr2));

        var oArr = new String[]{"Foo", "Bar", null, null};

        System.arraycopy(oArr, 0, oArr, 1, 2);
        // CHECK: [Foo, Foo, Bar, null]
        print(Arrays.toString(oArr));

        var objects = new Object[]{"Foo", "Bar", new Object()};
        var strings = new String[4];

        System.arraycopy(objects, 0, strings, 0, 2);
        // CHECK: [Foo, Bar, null, null]
        print(Arrays.toString(strings));

        var zArr1 = new boolean[]{true, false, true, false};
        var zArr2 = new boolean[4];

        System.arraycopy(zArr1, 0, zArr2, 0, zArr1.length);
        // CHECK: [true, false, true, false]
        print(Arrays.toString(zArr2));

        try
        {
            System.arraycopy(null, 0, new int[0], 0, 0);
        }
        catch (NullPointerException e)
        {
            // CHECK: NullPointerException
            print("NullPointerException");
        }

        try
        {
            System.arraycopy(new Object(), 0, new int[0], 0, 0);
        }
        catch (ArrayStoreException e)
        {
            // CHECK: java.lang.ArrayStoreException: arraycopy: source type java.lang.Object is not an array
            print(e.toString());
        }

        try
        {
            System.arraycopy(new int[0], 0, new Object(), 0, 0);
        }
        catch (ArrayStoreException e)
        {
            // CHECK: java.lang.ArrayStoreException: arraycopy: destination type java.lang.Object is not an array
            print(e.toString());
        }

        try
        {
            System.arraycopy(new boolean[0], 0, new Object[0], 0, 0);
        }
        catch (ArrayStoreException e)
        {
            // CHECK: java.lang.ArrayStoreException: arraycopy: type mismatch: can not copy boolean[] into object array[]
            print(e.toString());
        }

        try
        {
            System.arraycopy(new byte[0], 0, new char[0], 0, 0);
        }
        catch (ArrayStoreException e)
        {
            // CHECK: java.lang.ArrayStoreException: arraycopy: type mismatch: can not copy byte[] into char[]
            print(e.toString());
        }

        try
        {
            System.arraycopy(new Object[]{new Object()}, 0, new Test[1], 0, 1);
        }
        catch (ArrayStoreException e)
        {
            // CHECK: java.lang.ArrayStoreException: arraycopy: element type mismatch: can not cast one of the elements of java.lang.Object[] to the type of the destination array, Test
            print(e.toString());
        }

        try
        {
            System.arraycopy(new short[0], -1, new short[1], 0, 0);
        }
        catch (IndexOutOfBoundsException e)
        {
            // CHECK: java.lang.ArrayIndexOutOfBoundsException: arraycopy: source index -1 out of bounds for short[0]
            print(e.toString());
        }

        try
        {
            System.arraycopy(new int[2], 0, new int[3], -2, 0);
        }
        catch (IndexOutOfBoundsException e)
        {
            // CHECK: java.lang.ArrayIndexOutOfBoundsException: arraycopy: destination index -2 out of bounds for int[3]
            print(e.toString());
        }

        try
        {
            System.arraycopy(new long[4], 0, new long[5], 0, -3);
        }
        catch (IndexOutOfBoundsException e)
        {
            // CHECK: java.lang.ArrayIndexOutOfBoundsException: arraycopy: length -3 is negative
            print(e.toString());
        }

        try
        {
            System.arraycopy(new float[6], 0, new float[7], 0, 8);
        }
        catch (IndexOutOfBoundsException e)
        {
            // CHECK: java.lang.ArrayIndexOutOfBoundsException: arraycopy: last source index 8 out of bounds for float[6]
            print(e.toString());
        }

        try
        {
            System.arraycopy(new double[11], 0, new double[9], 0, 10);
        }
        catch (IndexOutOfBoundsException e)
        {
            // CHECK: java.lang.ArrayIndexOutOfBoundsException: arraycopy: last destination index 10 out of bounds for double[9]
            print(e.toString());
        }
    }
}
