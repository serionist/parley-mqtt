using System.Text.Json;
using Microsoft.Extensions.Time.Testing;
using Parley.Tests.Infrastructure;
using Xunit;

namespace Parley.Tests;

/// <summary>The error surface distinguishes builtin codes (enum) from typed custom codes (protocol.md §5, design.md).</summary>
public sealed class ErrorTypingTests
{
    private static ParleyTopics Topics => new("request", "heartbeat", "progress", "result");

    private static (FakeTransport Transport, ParleyCommander<JsonElement, JsonElement, JsonElement, string> Commander, ScriptedCommandIdGenerator Generator) NewCommander()
    {
        var clock = new FakeTimeProvider();
        var transport = new FakeTransport();
        var generator = new ScriptedCommandIdGenerator();
        var commander = new ParleyCommander<JsonElement, JsonElement, JsonElement, string>(
            transport, Topics, new ParleyConfig(), VectorJsonContext.Default, generator, clock);
        transport.ClearOutbound();
        return (transport, commander, generator);
    }

    [Fact]
    public void BuiltinError_IsSurfacedAsBuiltin()
    {
        var (transport, commander, generator) = NewCommander();
        generator.Enqueue("A");
        var task = commander.ExecuteAsync(Json.Element("{}"));
        transport.Deliver("result", Wire.ResultError("A", "$busy"));

        var error = task.Result.Error;
        Assert.True(error.IsBuiltin);
        Assert.Equal(ParleyBuiltinError.Busy, error.Builtin);
        Assert.Equal("$busy", error.Code);
    }

    [Fact]
    public void CustomError_IsSurfacedTyped()
    {
        var (transport, commander, generator) = NewCommander();
        generator.Enqueue("A");
        var task = commander.ExecuteAsync(Json.Element("{}"));
        transport.Deliver("result", Wire.ResultError("A", "diskFull"));

        var error = task.Result.Error;
        Assert.False(error.IsBuiltin);
        Assert.Equal("diskFull", error.Code);
        Assert.Equal("diskFull", error.Custom); // TError = string binding
    }
}
