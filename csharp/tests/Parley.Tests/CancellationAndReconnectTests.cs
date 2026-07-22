using System;
using System.Linq;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;
using Microsoft.Extensions.Time.Testing;
using Parley.Tests.Infrastructure;
using Xunit;

namespace Parley.Tests;

/// <summary>Cancellation paths (C7/H7) and connected-signal resubscription (§2) unit coverage.</summary>
public sealed class CancellationAndReconnectTests
{
    private static ParleyTopics Topics => new("request", "heartbeat", "progress", "result");

    private static ParleyCommander<JsonElement, JsonElement, JsonElement, string> Commander(
        FakeTransport transport, FakeTimeProvider clock, ScriptedCommandIdGenerator generator, ParleyConfig? config = null) =>
        new(transport, Topics, config ?? new ParleyConfig(), VectorJsonContext.Default, generator, clock);

    [Fact]
    public void Commander_TokenCancellation_SendsCnl_StopsRequestRetries_AndCompletesCancelled()
    {
        var clock = new FakeTimeProvider();
        var transport = new FakeTransport();
        var generator = new ScriptedCommandIdGenerator();
        var commander = Commander(transport, clock, generator);
        transport.ClearOutbound();

        generator.Enqueue("A");
        using var cts = new CancellationTokenSource();
        Task<ParleyResult<JsonElement, string>> task = commander.ExecuteAsync(Json.Element("{}"), null, null, cts.Token);
        Assert.Equal(1, transport.PublishCount("request", "req"));

        transport.ClearOutbound();
        cts.Cancel();
        Assert.Equal(1, transport.PublishCount("request", "cnl")); // C7: cnl published immediately

        // C7: req retries stop in SENDING — advancing a retry interval produces no further req.
        transport.ClearOutbound();
        clock.Advance(TimeSpan.FromSeconds(2));
        Assert.Equal(0, transport.PublishCount("request", "req"));

        transport.Deliver("result", Wire.ResultError("A", "$cancelled"));
        Assert.True(task.IsCompleted);
        ParleyResult<JsonElement, string> result = task.Result;
        Assert.True(result.IsError);
        Assert.True(result.Error.IsBuiltin);
        Assert.Equal(ParleyBuiltinError.Cancelled, result.Error.Builtin);
        Assert.Equal("$cancelled", result.Error.Code);
    }

    [Fact]
    public void Commander_ExecuteWithAlreadyCancelledToken_PublishesCnl_NoDeadlock()
    {
        var clock = new FakeTimeProvider();
        var transport = new FakeTransport();
        var generator = new ScriptedCommandIdGenerator();
        var commander = Commander(transport, clock, generator);
        transport.ClearOutbound();

        generator.Enqueue("A");
        using var cts = new CancellationTokenSource();
        cts.Cancel();

        // Registering on an already-cancelled token invokes RequestCancel synchronously, re-entering the gate
        // (reentrant lock) — this must publish cnl without deadlocking or throwing.
        Task<ParleyResult<JsonElement, string>> task = commander.ExecuteAsync(Json.Element("{}"), null, null, cts.Token);
        Assert.Equal(1, transport.PublishCount("request", "cnl"));

        // A $cancelled result then completes it promptly.
        transport.Deliver("result", Wire.ResultError("A", "$cancelled"));
        Assert.True(task.IsCompleted);
        Assert.True(task.Result.IsError);
        Assert.Equal("$cancelled", task.Result.Error.Code);
    }

    [Fact]
    public void Commander_CancelAfterSuccessfulCompletion_NoCnl_NoThrow()
    {
        var clock = new FakeTimeProvider();
        var transport = new FakeTransport();
        var generator = new ScriptedCommandIdGenerator();
        var commander = Commander(transport, clock, generator);
        transport.ClearOutbound();

        generator.Enqueue("A");
        using var cts = new CancellationTokenSource();
        Task<ParleyResult<JsonElement, string>> task = commander.ExecuteAsync(Json.Element("{}"), null, null, cts.Token);
        transport.Deliver("heartbeat", Wire.Heartbeat("A"));
        transport.Deliver("result", Wire.ResultOk("A", "{\"r\":1}"));
        Assert.True(task.IsCompleted);
        Assert.True(task.Result.IsSuccess);

        transport.ClearOutbound();

        // The registration was released on teardown (net10.0 Unregister); cancelling now must be a no-op.
        cts.Cancel();

        Assert.Equal(0, transport.PublishCount("request", "cnl"));
        Assert.True(task.Result.IsSuccess); // result unchanged
    }

    [Fact]
    public void Handler_CancelExecuting_SignalsCancellationToken()
    {
        var clock = new FakeTimeProvider();
        var transport = new FakeTransport();
        var handler = new VectorHandler(transport, Topics, new ParleyConfig(), clock);
        transport.ClearOutbound();

        transport.Deliver("request", Wire.Request("A"));
        Assert.True(handler.IsExecuting("A"));
        Assert.False(handler.IsCancellationRequested("A"));

        transport.Deliver("request", Wire.Cancel("A"));
        Assert.True(handler.IsCancellationRequested("A")); // H7: cnl triggers the cancellation signal
    }

    [Fact]
    public void Handler_CancelUnknownId_IsStatelessCancelled()
    {
        var clock = new FakeTimeProvider();
        var transport = new FakeTransport();
        var handler = new VectorHandler(transport, Topics, new ParleyConfig(), clock);
        transport.ClearOutbound();

        transport.Deliver("request", Wire.Cancel("ghost"));

        var res = transport.Publishes("result").Single();
        Assert.Equal("ghost", res["id"]!.GetValue<string>());
        Assert.Equal("$cancelled", res["e"]!.GetValue<string>());
        Assert.Equal(0, handler.CommandCount);   // no state created (H7 stateless)
        Assert.Equal(0, handler.TombstoneCount);
        Assert.Equal(0, handler.LiveTimerCount);
    }

    [Fact]
    public void Commander_Connected_ResubscribesAllThreeHandlerTopics()
    {
        var clock = new FakeTimeProvider();
        var transport = new FakeTransport();
        var generator = new ScriptedCommandIdGenerator();
        _ = Commander(transport, clock, generator);
        transport.ClearOutbound();

        transport.FireConnected();

        Assert.Equal(1, transport.SubscribeCount("heartbeat"));
        Assert.Equal(1, transport.SubscribeCount("progress"));
        Assert.Equal(1, transport.SubscribeCount("result"));
        Assert.Equal(0, transport.SubscribeCount("request"));
    }

    [Fact]
    public void Handler_Connected_ResubscribesRequestTopic()
    {
        var clock = new FakeTimeProvider();
        var transport = new FakeTransport();
        _ = new VectorHandler(transport, Topics, new ParleyConfig(), clock);
        transport.ClearOutbound();

        transport.FireConnected();

        Assert.Equal(1, transport.SubscribeCount("request"));
        Assert.Equal(0, transport.SubscribeCount("heartbeat"));
        Assert.Equal(0, transport.SubscribeCount("progress"));
        Assert.Equal(0, transport.SubscribeCount("result"));
    }
}
