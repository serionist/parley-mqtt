using System.Collections.Generic;
using System.Linq;
using System.Text.Json.Nodes;

namespace Parley.Tests.Infrastructure;

/// <summary>Convenience inspectors over a <see cref="FakeTransport"/>'s recorded outbound actions.</summary>
internal static class TransportExtensions
{
    public static IEnumerable<JsonObject> Publishes(this FakeTransport transport, string topic) =>
        transport.Outbound
            .Where(a => !a.IsSubscribe && a.Topic == topic)
            .Select(a => (JsonObject)JsonNode.Parse(a.Payload!)!);

    public static int PublishCount(this FakeTransport transport, string topic, string type) =>
        transport.Publishes(topic).Count(o => o["t"]!.GetValue<string>() == type);

    public static int SubscribeCount(this FakeTransport transport, string topic) =>
        transport.Outbound.Count(a => a.IsSubscribe && a.Topic == topic);
}
