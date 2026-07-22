using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.Json;
using System.Threading;
using Microsoft.Extensions.Time.Testing;
using Parley.Tests.Infrastructure;
using Xunit;

namespace Parley.Tests;

/// <summary>
/// The hard bounded-state requirement (testing.md layer 1): run several thousand command cycles through
/// both roles over mixed success / failure / timeout / $busy / cancel paths, then assert every internal
/// collection (in-flight, dedup, result cache, tombstones, timer registrations) is back at steady state.
/// </summary>
public sealed class BoundedStateTests
{
    private const int Cycles = 3000;

    private static ParleyTopics Topics => new("request", "heartbeat", "progress", "result");

    [Fact]
    public void Commander_SustainedMixedLoad_ReturnsToSteadyState()
    {
        var clock = new FakeTimeProvider();
        var transport = new FakeTransport();
        var generator = new ScriptedCommandIdGenerator();
        var config = new ParleyConfig
        {
            RequestRetryInterval = TimeSpan.FromSeconds(1),
            NoAckTimeout = TimeSpan.FromMilliseconds(2500),
            StaleTimeout = TimeSpan.FromMilliseconds(2500),
            ExecutionTimeout = TimeSpan.FromSeconds(10),
            CancelRetryInterval = TimeSpan.FromSeconds(1),
            TombstoneTtl = TimeSpan.FromSeconds(3),
        };
        var commander = new ParleyCommander<JsonElement, JsonElement, JsonElement, string>(
            transport, Topics, config, VectorJsonContext.Default, generator, clock);
        transport.ClearOutbound();

        for (int i = 0; i < Cycles; i++)
        {
            string id = "c" + i;
            generator.Enqueue(id);
            using var cts = new CancellationTokenSource();
            var task = commander.ExecuteAsync(Json.Element("{}"), null, null, cts.Token);

            switch (i % 5)
            {
                case 0: // success
                    transport.Deliver("heartbeat", Wire.Heartbeat(id));
                    transport.Deliver("result", Wire.ResultOk(id, "{\"r\":1}"));
                    break;
                case 1: // custom failure
                    transport.Deliver("result", Wire.ResultError(id, "domainErr"));
                    break;
                case 2: // $busy
                    transport.Deliver("result", Wire.ResultError(id, "$busy"));
                    break;
                case 3: // $noAck timeout (never any sign of life)
                    clock.Advance(TimeSpan.FromMilliseconds(2600));
                    break;
                case 4: // cancel, then a $cancelled result wins
                    cts.Cancel();
                    transport.Deliver("result", Wire.ResultError(id, "$cancelled"));
                    break;
            }

            Assert.True(task.IsCompleted, $"cycle {i}: command did not terminate.");
            transport.ClearOutbound();
            clock.Advance(TimeSpan.FromMilliseconds(1));
        }

        // Drain all tombstones.
        clock.Advance(TimeSpan.FromSeconds(5));

        Assert.Equal(0, commander.InflightCount);
        Assert.Equal(0, commander.TombstoneCount);
        Assert.Equal(0, commander.LiveTimerCount);
    }

    [Fact]
    public void Handler_SustainedMixedLoad_ReturnsToSteadyState()
    {
        var clock = new FakeTimeProvider();
        var transport = new FakeTransport();
        var config = new ParleyConfig
        {
            HeartbeatInterval = TimeSpan.FromSeconds(1),
            ResultRetryInterval = TimeSpan.FromSeconds(1),
            ResultRetryTimeout = TimeSpan.FromMilliseconds(2500),
            TombstoneTtl = TimeSpan.FromSeconds(3),
            MaxConcurrentExecutions = 4,
            MaxActiveCommands = 8,
        };
        var handler = new VectorHandler(transport, Topics, config, clock);
        transport.ClearOutbound();

        for (int i = 0; i < Cycles; i++)
        {
            string id = "h" + i;
            transport.Deliver("request", Wire.Request(id));

            switch (i % 6)
            {
                case 0: // complete + rak
                case 5:
                    handler.Complete(id, "{}".ToElement());
                    transport.Deliver("request", Wire.ResultAck(id));
                    break;
                case 1: // custom fail + rak
                    handler.Fail(id, "domainErr", null);
                    transport.Deliver("request", Wire.ResultAck(id));
                    break;
                case 2: // complete, no rak -> resultRetryTimeout give-up -> tombstone
                    handler.Complete(id, "{}".ToElement());
                    clock.Advance(TimeSpan.FromMilliseconds(2600));
                    break;
                case 3: // cancel executing -> $cancelled + rak
                    transport.Deliver("request", Wire.Cancel(id));
                    handler.Fail(id, "$cancelled", null);
                    transport.Deliver("request", Wire.ResultAck(id));
                    break;
                case 4: // stateless cancel of an unknown id, then complete this one
                    transport.Deliver("request", Wire.Cancel("ghost" + i));
                    handler.Complete(id, "{}".ToElement());
                    transport.Deliver("request", Wire.ResultAck(id));
                    break;
            }

            transport.ClearOutbound();
            clock.Advance(TimeSpan.FromMilliseconds(1));
        }

        // A few $busy bursts: fill the pool with parked commands, overflow, then drain.
        for (int burst = 0; burst < 40; burst++)
        {
            var parked = new List<string>();
            for (int k = 0; k < config.MaxActiveCommands; k++)
            {
                string id = $"b{burst}_{k}";
                transport.Deliver("request", Wire.Request(id));
                parked.Add(id);
            }

            // Pool is full -> stateless $busy, no state created.
            transport.Deliver("request", Wire.Request($"overflow{burst}"));
            Assert.Equal(config.MaxActiveCommands, handler.CommandCount);

            // Drain: complete whatever is executing until every parked command is tombstoned.
            var remaining = new HashSet<string>(parked);
            while (remaining.Count > 0)
            {
                bool progressed = false;
                foreach (string id in remaining.ToList())
                {
                    if (handler.IsExecuting(id))
                    {
                        handler.Complete(id, "{}".ToElement());
                        transport.Deliver("request", Wire.ResultAck(id));
                        remaining.Remove(id);
                        progressed = true;
                    }
                }

                Assert.True(progressed, $"burst {burst}: drain stalled.");
            }

            transport.ClearOutbound();
            clock.Advance(TimeSpan.FromMilliseconds(1));
        }

        // Drain all tombstones and any lingering result-retry state.
        clock.Advance(TimeSpan.FromSeconds(5));

        Assert.Equal(0, handler.CommandCount);
        Assert.Equal(0, handler.QueueCount);
        Assert.Equal(0, handler.ExecutingCount);
        Assert.Equal(0, handler.TombstoneCount);
        Assert.Equal(0, handler.LiveTimerCount);
    }
}
