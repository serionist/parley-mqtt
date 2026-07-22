using System;
using System.Collections.Generic;
using System.Text.Json;
using System.Text.Json.Serialization;
using System.Threading;
using System.Threading.Tasks;
using Microsoft.Extensions.Time.Testing;
using Parley.Tests.Infrastructure;
using Xunit;

namespace Parley.Tests;

/// <summary>
/// Regression coverage for the C# deep-review fixes: an unserializable handler outcome aborts like a thrown
/// handler (CR3/AB1), adapter faults never escape library timer threads and retries self-heal (CR4), and
/// disposing an instance mid-flight completes in-flight calls locally and drains all state (AB4).
/// </summary>
public sealed class DeepReviewRegressionTests
{
    private static ParleyTopics Topics => new("request", "heartbeat", "progress", "result");

    [Fact]
    public void Handler_OutcomeThatFailsToSerialize_AbortsLikeThrownHandler()
    {
        var clock = new FakeTimeProvider();
        var transport = new FakeTransport();
        var handler = new NonStringErrorHandler(transport, Topics, new ParleyConfig(), clock);
        transport.ClearOutbound();

        transport.Deliver("request", Wire.Request("A"));
        Assert.Equal(1, handler.CommandCount);
        Assert.Equal(1, handler.ExecutingCount);

        // The custom TError serializes to a JSON number, so BuildOutcomeBytes throws — the settled contract
        // aborts the command exactly like a thrown handler rather than leaking the slot + heartbeat.
        transport.ClearOutbound();
        handler.FailWithBadError("A");

        Assert.Equal(0, handler.CommandCount);       // slot freed
        Assert.Equal(0, handler.ExecutingCount);     // heartbeat stopped, execution slot released
        Assert.Equal(1, handler.TombstoneCount);     // id tombstoned
        Assert.Equal(1, handler.LiveTimerCount);     // only the tombstone TTL timer remains
        Assert.Equal(0, transport.PublishCount("result", "res")); // commander-side sees no result

        clock.Advance(TimeSpan.FromSeconds(61));      // drain the tombstone
        Assert.Equal(0, handler.TombstoneCount);
        Assert.Equal(0, handler.LiveTimerCount);      // back to steady state
    }

    [Fact]
    public void AdapterPublishThrows_IsSwallowed_AndRetriesSelfHeal()
    {
        var clock = new FakeTimeProvider();
        var transport = new FakeTransport();
        var config = new ParleyConfig
        {
            ResultRetryInterval = TimeSpan.FromSeconds(2),
            ResultRetryTimeout = TimeSpan.FromMinutes(5),
        };
        var handler = new VectorHandler(transport, Topics, config, clock);
        transport.ClearOutbound();

        transport.Deliver("request", Wire.Request("A"));
        handler.Complete("A", "{}".ToElement()); // publishes res, schedules the retry cadence
        transport.ClearOutbound();

        // A retry fires on the timer thread while the adapter is down: the throw must be swallowed, not escape.
        transport.FailPublish = true;
        Exception? escaped = Record.Exception(() => clock.Advance(TimeSpan.FromSeconds(2)));
        Assert.Null(escaped);
        Assert.Equal(0, transport.PublishCount("result", "res")); // publish threw, nothing recorded

        // Once the adapter recovers, the next retry publishes — the cadence survived the fault (self-healing).
        transport.FailPublish = false;
        clock.Advance(TimeSpan.FromSeconds(2));
        Assert.True(transport.PublishCount("result", "res") >= 1);
    }

    [Fact]
    public void Commander_DisposeMidFlight_CompletesCancelled_AndClearsState()
    {
        var clock = new FakeTimeProvider();
        var transport = new FakeTransport();
        var generator = new ScriptedCommandIdGenerator();
        var commander = new ParleyCommander<JsonElement, JsonElement, JsonElement, string>(
            transport, Topics, new ParleyConfig(), VectorJsonContext.Default, generator, clock);
        transport.ClearOutbound();

        generator.Enqueue("A");
        using var cts = new CancellationTokenSource();
        Task<ParleyResult<JsonElement, string>> task = commander.ExecuteAsync(Json.Element("{}"), null, null, cts.Token);
        transport.Deliver("heartbeat", Wire.Heartbeat("A")); // LIVE — sliding + execution-cap timers running
        Assert.False(task.IsCompleted);

        commander.Dispose();

        Assert.True(task.IsCompleted);
        Assert.True(task.Result.IsError);
        Assert.True(task.Result.Error.IsBuiltin);
        Assert.Equal(ParleyBuiltinError.Cancelled, task.Result.Error.Builtin);

        Assert.Equal(0, commander.InflightCount);
        Assert.Equal(0, commander.TombstoneCount);
        Assert.Equal(0, commander.LiveTimerCount);

        // Post-dispose API misuse throws; inbound is ignored; Dispose is idempotent.
        generator.Enqueue("B");
        Assert.Throws<ObjectDisposedException>(() => { _ = commander.ExecuteAsync(Json.Element("{}")); });
        commander.Dispose();
    }

    [Fact]
    public void Handler_Dispose_StopsTimersAndClearsState()
    {
        var clock = new FakeTimeProvider();
        var transport = new FakeTransport();
        var handler = new VectorHandler(transport, Topics, new ParleyConfig(), clock);
        transport.ClearOutbound();

        transport.Deliver("request", Wire.Request("A")); // admitted + executing, heartbeat scheduled
        Assert.Equal(1, handler.CommandCount);
        Assert.True(handler.LiveTimerCount >= 1);

        handler.Dispose();

        Assert.Equal(0, handler.CommandCount);
        Assert.Equal(0, handler.QueueCount);
        Assert.Equal(0, handler.ExecutingCount);
        Assert.Equal(0, handler.TombstoneCount);
        Assert.Equal(0, handler.LiveTimerCount);

        handler.Dispose(); // idempotent
    }
}

/// <summary>A custom error type that serializes to a JSON number (not a string) — its outcome cannot be built.</summary>
internal enum BadError
{
    Boom,
}

[JsonSerializable(typeof(JsonElement))]
[JsonSerializable(typeof(BadError))]
internal partial class BadErrorJsonContext : JsonSerializerContext
{
}

/// <summary>A handler whose custom error type serializes to a JSON number (not a string), so building its
/// outcome bytes throws — used to prove the abort-like-a-throw contract (CR3/AB1).</summary>
internal sealed class NonStringErrorHandler : ParleyCommandHandler<JsonElement, JsonElement, JsonElement, BadError>
{
    private readonly Dictionary<string, TaskCompletionSource<ParleyOutcome<JsonElement, BadError>>> _pending = new();

    public NonStringErrorHandler(FakeTransport transport, ParleyTopics topics, ParleyConfig config, TimeProvider time)
        : base(transport, topics, config, BadErrorJsonContext.Default, time)
    {
    }

    protected override Task<ParleyOutcome<JsonElement, BadError>> HandleCommandAsync(
        JsonElement request, IParleyExecution<JsonElement> execution, CancellationToken cancellationToken)
    {
        // Synchronous continuations: resolving runs the library's completion inline, under the clock.
        var tcs = new TaskCompletionSource<ParleyOutcome<JsonElement, BadError>>();
        _pending[execution.CommandId] = tcs;
        return tcs.Task;
    }

    public void FailWithBadError(string id) => _pending[id].SetResult(ParleyOutcome.Error(BadError.Boom));
}
