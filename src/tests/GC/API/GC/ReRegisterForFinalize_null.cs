// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

// Tests ReRegisterForFinalize()

using System;
using Xunit;

public class Test_ReRegisterForFinalize_null
{
    public bool RunTest()
    {
        try
        {
            GC.ReRegisterForFinalize(null); // should call Finalize() for obj1 now.
        }
        catch (ArgumentNullException)
        {
            return true;
        }
        catch (Exception)
        {
            Console.WriteLine("Unexpected Exception!");
        }

        return false;
    }


    [Fact]
    public static int TestEntryPoint()
    {
        Test_ReRegisterForFinalize_null t = new Test_ReRegisterForFinalize_null();
        if (t.RunTest())
        {
            Console.WriteLine("Null Test for ReRegisterForFinalize() passed!");
            return 100;
        }

        Console.WriteLine("Null Test for ReRegisterForFinalize() failed!");
        return 1;
    }
}
