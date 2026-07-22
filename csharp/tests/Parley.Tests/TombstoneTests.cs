using System;
using System.Linq;
using Microsoft.Extensions.Time.Testing;
using Parley.Tests.Infrastructure;
using Xunit;

namespace Parley.Tests;

/// <summary>Tombstone TTL expiry and oldest-first eviction on both roles (C5 / H5 / H8, testing.md layer 1).</summary>
public sealed class TombstoneTests
{
    private static ParleyTopics Topics => new("request", "heartbeat", "progress", "result");

    [Fact]
    public void Commander_Tombstone_ExpiresAfterTtl()
    {
        var clock = new FakeTimeProvider();
        var transport = new FakeTransport();
        var generator = new ScriptedCommandIdGenerator();
        var config = new ParleyConfig { TombstoneTtl = TimeSpan.FromSeconds(5) };
        var commander = new ParleyCommander<System.Text.Json.JsonElement, System.Text.Json.JsonElement, System.Text.Json.JsonElement, string>(
            transport, Topics, config, VectorJsonContext.Default, generator, clock);
        transport.ClearOutbound();

        generator.Enqueue("A");
        _ = commander.ExecuteAsync(Json.Element("{}"));
        transport.Deliver("result", Wire.ResultOk("A", "{\"r\":1}"));

        Assert.Equal(0, commander.InflightCount);
        Assert.Equal(1, commander.TombstoneCount);

        clock.Advance(TimeSpan.FromSeconds(6));

        Assert.Equal(0, commander.TombstoneCount);
        Assert.Equal(0, commander.LiveTimerCount);
    }

    [Fact]
    public void Handler_Tombstone_ExpiresAfterTtl()
    {
        var clock = new FakeTimeProvider();
        var transport = new FakeTransport();
        var config = new ParleyConfig { TombstoneTtl = TimeSpan.FromSeconds(5) };
        var handler = new VectorHandler(transport, Topics, config, clock);
        transport.ClearOutbound();

        transport.Deliver("request", Wire.Request("A"));
        handler.Complete("A", "{\"r\":1}".ToElement());
        transport.Deliver("request", Wire.ResultAck("A"));

        Assert.Equal(0, handler.CommandCount);
        Assert.Equal(1, handler.TombstoneCount);

        clock.Advance(TimeSpan.FromSeconds(6));

        Assert.Equal(0, handler.TombstoneCount);
        Assert.Equal(0, handler.LiveTimerCount);
    }

    [Fact]
    public void Commander_Tombstones_EvictOldestFirst()
    {
        var clock = new FakeTimeProvider();
        var transport = new FakeTransport();
        var generator = new ScriptedCommandIdGenerator();
        var config = new ParleyConfig { MaxTombstones = 2, TombstoneTtl = TimeSpan.FromHours(1) };
        var commander = new ParleyCommander<System.Text.Json.JsonElement, System.Text.Json.JsonElement, System.Text.Json.JsonElement, string>(
            transport, Topics, config, VectorJsonContext.Default, generator, clock);
        transport.ClearOutbound();

        foreach (string id in new[] { "A", "B", "C" })
        {
            generator.Enqueue(id);
            _ = commander.ExecuteAsync(Json.Element("{}"));
            transport.Deliver("result", Wire.ResultOk(id, "{}"));
        }

        Assert.Equal(2, commander.TombstoneCount); // A (oldest) evicted when C tombstoned

        transport.ClearOutbound();
        transport.Deliver("result", Wire.ResultOk("A", "{}")); // A forgotten -> no rak (C6)
        Assert.Equal(0, transport.PublishCount("request", "rak"));

        transport.Deliver("result", Wire.ResultOk("C", "{}")); // C retained -> rak (C5)
        Assert.Equal(1, transport.PublishCount("request", "rak"));
    }

    [Fact]
    public void Handler_Tombstones_EvictOldestFirst()
    {
        var clock = new FakeTimeProvider();
        var transport = new FakeTransport();
        var config = new ParleyConfig
        {
            MaxTombstones = 2,
            TombstoneTtl = TimeSpan.FromHours(1),
            MaxConcurrentExecutions = 8,
            MaxActiveCommands = 8,
        };
        var handler = new VectorHandler(transport, Topics, config, clock);
        transport.ClearOutbound();

        foreach (string id in new[] { "A", "B", "C" })
        {
            transport.Deliver("request", Wire.Request(id));
            handler.Complete(id, "{}".ToElement());
            transport.Deliver("request", Wire.ResultAck(id));
        }

        Assert.Equal(2, handler.TombstoneCount); // A (oldest) evicted when C tombstoned

        transport.ClearOutbound();
        transport.Deliver("request", Wire.Request("A")); // A forgotten -> re-admitted
        Assert.Equal(1, handler.CommandCount);
        Assert.Equal(1, transport.PublishCount("heartbeat", "hb"));

        transport.ClearOutbound();
        transport.Deliver("request", Wire.Request("C")); // C tombstoned -> ignored
        Assert.Equal(0, transport.PublishCount("heartbeat", "hb"));
    }
}
