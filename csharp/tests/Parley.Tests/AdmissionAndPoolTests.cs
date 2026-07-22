using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using Microsoft.Extensions.Time.Testing;
using Parley.Tests.Infrastructure;
using Xunit;

namespace Parley.Tests;

/// <summary>Atomic admission (H1/H2) and pool-overflow $busy (H2/H8) unit coverage (testing.md layer 1).</summary>
public sealed class AdmissionAndPoolTests
{
    private static ParleyTopics Topics => new("request", "heartbeat", "progress", "result");

    [Fact]
    public void ConcurrentDuplicateRequests_AdmitExactlyOnce()
    {
        var clock = new FakeTimeProvider();
        var transport = new FakeTransport();
        var handler = new VectorHandler(transport, Topics, new ParleyConfig(), clock);
        transport.ClearOutbound();

        // Deliver the same req id from two threads simultaneously.
        using var barrier = new Barrier(2);
        void Deliver()
        {
            barrier.SignalAndWait();
            transport.Deliver("request", Wire.Request("A"));
        }

        Task.WaitAll(Task.Run(Deliver), Task.Run(Deliver));

        Assert.Equal(1, handler.CommandCount);       // dedup + capacity + admit is atomic — exactly one admission
        Assert.Equal(1, handler.ExecutingCount);
        Assert.Equal(0, transport.PublishCount("result", "res"));       // never $busy for a duplicate
        Assert.Equal(2, transport.PublishCount("heartbeat", "hb"));     // admission hb + H1 duplicate hb
    }

    [Fact]
    public void PoolOverflow_YieldsStatelessBusy_WithoutAdmitting()
    {
        var clock = new FakeTimeProvider();
        var transport = new FakeTransport();
        var config = new ParleyConfig { MaxActiveCommands = 2 };
        var handler = new VectorHandler(transport, Topics, config, clock);
        transport.ClearOutbound();

        transport.Deliver("request", Wire.Request("A")); // admitted, executing
        transport.Deliver("request", Wire.Request("B")); // admitted, queued (serial handler)
        transport.ClearOutbound();

        transport.Deliver("request", Wire.Request("C")); // pool full -> stateless $busy

        Assert.Equal(2, handler.CommandCount);       // A and B only
        Assert.Equal(1, handler.ExecutingCount);
        Assert.Equal(1, handler.QueueCount);
        Assert.Equal(0, handler.TombstoneCount);     // $busy creates no state

        var busy = transport.Publishes("result").Single();
        Assert.Equal("C", busy["id"]!.GetValue<string>());
        Assert.Equal("$busy", busy["e"]!.GetValue<string>());
    }

    [Fact]
    public void DuplicateOfAdmittedId_OnFullPool_GetsHeartbeatNotBusy()
    {
        var clock = new FakeTimeProvider();
        var transport = new FakeTransport();
        var config = new ParleyConfig { MaxActiveCommands = 1 };
        var handler = new VectorHandler(transport, Topics, config, clock);
        transport.ClearOutbound();

        transport.Deliver("request", Wire.Request("A")); // admitted, pool now full
        transport.ClearOutbound();

        transport.Deliver("request", Wire.Request("A")); // duplicate of admitted id — dedup precedes capacity

        Assert.Equal(1, transport.PublishCount("heartbeat", "hb"));
        Assert.Equal(0, transport.PublishCount("result", "res")); // not $busy
    }
}
