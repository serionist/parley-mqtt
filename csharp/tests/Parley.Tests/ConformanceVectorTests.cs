using System;
using System.IO;
using System.Linq;
using Parley.Tests.Infrastructure;
using Xunit;

namespace Parley.Tests;

/// <summary>
/// Layer-2 conformance: replays every <c>docs/conformance/*.json</c> vector (discovered dynamically,
/// one test case per file) against the protocol core, for both roles (protocol.md §10 / testing.md).
/// </summary>
public sealed class ConformanceVectorTests
{
    private static string ConformanceDirectory => Path.Combine(AppContext.BaseDirectory, "conformance");

    public static TheoryData<string> Vectors()
    {
        var data = new TheoryData<string>();
        foreach (string file in Directory.EnumerateFiles(ConformanceDirectory, "*.json").OrderBy(f => f, StringComparer.Ordinal))
        {
            data.Add(Path.GetFileName(file));
        }

        return data;
    }

    [Theory]
    [MemberData(nameof(Vectors))]
    public void Replay(string vectorFile)
    {
        VectorRunner.Run(Path.Combine(ConformanceDirectory, vectorFile));
    }
}
