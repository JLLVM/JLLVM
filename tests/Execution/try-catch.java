// RUN: javac %s -d %t
// RUN: jllvm -Xenable-test-utils %t/Test.class | FileCheck --match-full-lines %s

class Test
{
    static native void print(int i);
    static native void print(String i);

    public static void raiseRuntimeException()
    {
        throw new RuntimeException("A message");
    }

    public static void main(String[] args)
    {
        try
        {
            raiseRuntimeException();
        }
        catch (Exception e)
        {
            // CHECK: 5
            print(5);
        }

        try
        {
            raiseRuntimeException();
        }
        catch (OutOfMemoryError e)
        {
            // CHECK-NOT: 5
            print(5);
        }
        catch (RuntimeException e)
        {
            // CHECK: 6
            print(6);
        }
        catch (Exception e)
        {
            // CHECK-NOT: 5
            print(5);
        }

        try
        {
            try
            {
                try
                {
                    raiseRuntimeException();
                }
                catch (OutOfMemoryError e)
                {
                    // CHECK-NOT: 5
                    print(5);
                }
            }
            catch (RuntimeException e)
            {
                // CHECK: 6
                print(6);
            }
        }
        catch (Exception e)
        {
            // CHECK-NOT: 5
            print(5);
        }

        try
        {
            try
            {
                try
                {
                    raiseRuntimeException();
                }
                catch (OutOfMemoryError e)
                {
                    // CHECK-NOT: 5
                    print(5);
                }
                finally
                {
                    // CHECK: 1
                    print(1);
                }
                // CHECK-NOT: 5
                print(5);
            }
            catch (RuntimeException e)
            {
                // CHECK: 6
                print(6);
            }
            finally
            {
                // CHECK: 2
                print(2);
            }
            // CHECK: 5
            print(5);
        }
        catch (Exception e)
        {
            // CHECK-NOT: 5
            print(5);
        }
        finally
        {
            // CHECK: 3
            print(3);
        }



        try
        {
            throw new RuntimeException("From athrow to catch");
        }
        catch (OutOfMemoryError e)
        {
            // CHECK-NOT: 5
            print(5);
        }
        catch (RuntimeException e)
        {
            // CHECK: From athrow to catch
            print(e.getMessage());
        }
        catch (Exception e)
        {
            // CHECK-NOT: 5
            print(5);
        }
    }
}
