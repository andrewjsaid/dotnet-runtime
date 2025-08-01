﻿using Mono.Linker.Tests.Cases.Expectations.Assertions;
using Mono.Linker.Tests.Cases.Expectations.Metadata;

namespace Mono.Linker.Tests.Cases.LinkXml
{
    [SetupLinkerDescriptorFile("UnusedTypePreservedByLinkXmlWithCommentIsKept.xml")]
    class UnusedTypePreservedByLinkXmlWithCommentIsKept
    {
        public static void Main()
        {
        }
    }

    [Kept]
    [KeptMember(".ctor()")]
    class UnusedTypePreservedByLinkXmlWithCommentIsKeptUnusedType
    {
    }
}
