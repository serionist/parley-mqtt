using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Threading;
using System.Threading.Tasks;
using Microsoft.Extensions.Time.Testing;
using Parley;
using Xunit;

namespace Parley.Tests.Infrastructure;

/// <summary>
/// Replays a single protocol.md §10 conformance vector against the Parley core over a fake transport and
/// a virtual clock. Supports both roles, all step directives, and exact-set <c>expect</c> matching.
/// </summary>
internal static class VectorRunner
{
    // The topic labels the vectors use double as the actual (opaque) topic strings.
    private const string RequestTopic = "request";
    private const string HeartbeatTopic = "heartbeat";
    private const string ProgressTopic = "progress";
    private const string ResultTopic = "result";

    public static void Run(string path)
    {
        JsonObject vector = (JsonNode.Parse(File.ReadAllText(path)) as JsonObject)
            ?? throw new InvalidOperationException($"Vector {path} is not a JSON object.");

        string name = vector["name"]?.GetValue<string>() ?? Path.GetFileNameWithoutExtension(path);
        string role = vector["role"]!.GetValue<string>();
        ParleyConfig config = BuildConfig(vector["config"] as JsonObject);
        JsonArray steps = vector["steps"]!.AsArray();

        var time = new FakeTimeProvider();
        var transport = new FakeTransport();
        var topics = new ParleyTopics(RequestTopic, HeartbeatTopic, ProgressTopic, ResultTopic);

        if (role == "commander")
        {
            RunCommander(name, config, steps, vector["outcomes"] as JsonObject, time, transport, topics);
        }
        else if (role == "handler")
        {
            RunHandler(name, config, steps, time, transport, topics);
        }
        else
        {
            throw new InvalidOperationException($"Vector {name}: unknown role '{role}'.");
        }
    }

    private static void RunCommander(
        string name,
        ParleyConfig config,
        JsonArray steps,
        JsonObject? outcomes,
        FakeTimeProvider time,
        FakeTransport transport,
        ParleyTopics topics)
    {
        var idGenerator = new ScriptedCommandIdGenerator();
        var commander = new ParleyCommander<JsonElement, JsonElement, JsonElement, string>(
            transport, topics, config, VectorJsonContext.Default, idGenerator, time);

        // Construction-time subscribes are not asserted (protocol.md §10).
        transport.ClearOutbound();

        var tasks = new Dictionary<string, Task<ParleyResult<JsonElement, string>>>();
        var tokenSources = new Dictionary<string, CancellationTokenSource>();
        long currentAt = 0;

        foreach (JsonNode? stepNode in steps)
        {
            JsonObject step = stepNode!.AsObject();
            long at = step["at"]!.GetValue<long>();
            AdvanceTo(time, ref currentAt, at);

            if (step["call"] is JsonObject call)
            {
                string id = call["id"]!.GetValue<string>();
                idGenerator.Enqueue(id);
                JsonElement request = NodeToElement(call["p"]);
                ParleyCallOptions? options = BuildCallOptions(call["options"] as JsonObject);
                var cts = new CancellationTokenSource();
                tokenSources[id] = cts;
                tasks[id] = commander.ExecuteAsync(request, options, progress: null, cts.Token);
            }
            else if (step["cancel"] is JsonNode cancel)
            {
                string id = cancel.GetValue<string>();
                tokenSources[id].Cancel();
            }
            else if (step["in"] is JsonObject inbound)
            {
                DeliverInbound(transport, inbound);
            }
            else if (step["connected"] is not null)
            {
                transport.FireConnected();
            }
            else if (step["expect"] is JsonArray expect)
            {
                AssertExpect(name, at, transport.Outbound, expect);
                transport.ClearOutbound();
            }
            else
            {
                throw new InvalidOperationException($"Vector {name}: unrecognized step {step.ToJsonString()}.");
            }
        }

        AssertOutcomes(name, outcomes, tasks);
    }

    private static void RunHandler(
        string name,
        ParleyConfig config,
        JsonArray steps,
        FakeTimeProvider time,
        FakeTransport transport,
        ParleyTopics topics)
    {
        var handler = new VectorHandler(transport, topics, config, time);

        // Construction-time subscribes are not asserted (protocol.md §10).
        transport.ClearOutbound();

        long currentAt = 0;

        foreach (JsonNode? stepNode in steps)
        {
            JsonObject step = stepNode!.AsObject();
            long at = step["at"]!.GetValue<long>();
            AdvanceTo(time, ref currentAt, at);

            if (step["in"] is JsonObject inbound)
            {
                DeliverInbound(transport, inbound);
            }
            else if (step["handler"] is JsonObject action)
            {
                ApplyHandlerAction(name, handler, action);
            }
            else if (step["connected"] is not null)
            {
                transport.FireConnected();
            }
            else if (step["expect"] is JsonArray expect)
            {
                AssertExpect(name, at, transport.Outbound, expect);
                transport.ClearOutbound();
            }
            else
            {
                throw new InvalidOperationException($"Vector {name}: unrecognized step {step.ToJsonString()}.");
            }
        }
    }

    private static void ApplyHandlerAction(string name, VectorHandler handler, JsonObject action)
    {
        string id = action["id"]!.GetValue<string>();
        string what = action["action"]!.GetValue<string>();
        switch (what)
        {
            case "complete":
                handler.Complete(id, NodeToElement(action["p"]));
                break;
            case "fail":
                handler.Fail(id, action["e"]!.GetValue<string>(), action["m"]?.GetValue<string>());
                break;
            case "progress":
                handler.EmitProgress(id, NodeToElement(action["p"]));
                break;
            default:
                throw new InvalidOperationException($"Vector {name}: unknown handler action '{what}'.");
        }
    }

    private static void DeliverInbound(FakeTransport transport, JsonObject inbound)
    {
        string topic = inbound["topic"]!.GetValue<string>();
        byte[] payload = inbound["raw"] is JsonNode raw
            ? Encoding.UTF8.GetBytes(raw.GetValue<string>())
            : Encoding.UTF8.GetBytes(inbound["msg"]!.ToJsonString());
        transport.Deliver(topic, payload);
    }

    private static void AdvanceTo(FakeTimeProvider time, ref long currentAt, long at)
    {
        long delta = at - currentAt;
        if (delta < 0)
        {
            throw new InvalidOperationException("Vector steps must be in non-decreasing time order.");
        }

        if (delta > 0)
        {
            time.Advance(TimeSpan.FromMilliseconds(delta));
            currentAt = at;
        }
    }

    private static void AssertExpect(string name, long at, List<OutboundAction> actual, JsonArray expected)
    {
        var unmatched = new List<OutboundAction>(actual);

        foreach (JsonNode? expNode in expected)
        {
            JsonObject exp = expNode!.AsObject();
            int index;
            if (exp["subscribe"] is JsonNode sub)
            {
                string topic = sub.GetValue<string>();
                index = unmatched.FindIndex(a => a.IsSubscribe && a.Topic == topic);
            }
            else
            {
                string topic = exp["topic"]!.GetValue<string>();
                JsonNode? msg = exp["msg"];
                index = unmatched.FindIndex(a =>
                    !a.IsSubscribe && a.Topic == topic && JsonComparer.Equal(JsonNode.Parse(a.Payload!), msg));
            }

            if (index < 0)
            {
                Assert.Fail(
                    $"Vector '{name}' at t={at}: expected outbound action not found: {exp.ToJsonString()}\n" +
                    $"Actual outbound: {Dump(actual)}");
            }

            unmatched.RemoveAt(index);
        }

        if (unmatched.Count > 0)
        {
            Assert.Fail(
                $"Vector '{name}' at t={at}: unexpected extra outbound action(s): {Dump(unmatched)}\n" +
                $"Expected: {expected.ToJsonString()}");
        }
    }

    private static void AssertOutcomes(
        string name,
        JsonObject? outcomes,
        Dictionary<string, Task<ParleyResult<JsonElement, string>>> tasks)
    {
        if (outcomes is null)
        {
            return;
        }

        foreach (var kv in outcomes)
        {
            string id = kv.Key;
            JsonObject expected = kv.Value!.AsObject();

            Assert.True(tasks.TryGetValue(id, out Task<ParleyResult<JsonElement, string>>? task),
                $"Vector '{name}': outcome expected for call '{id}' that was never started.");
            Assert.True(task!.IsCompleted,
                $"Vector '{name}': call '{id}' did not reach a terminal outcome.");

            ParleyResult<JsonElement, string> result = task.Result;
            bool expectedOk = expected["ok"]!.GetValue<bool>();

            if (expectedOk)
            {
                Assert.True(result.IsSuccess,
                    $"Vector '{name}': call '{id}' expected success but got error '{(result.IsError ? result.Error.Code : "?")}'.");
                if (expected["p"] is JsonNode expectedPayload)
                {
                    JsonNode? actualPayload = result.Value.ValueKind == JsonValueKind.Undefined
                        ? null
                        : JsonNode.Parse(result.Value.GetRawText());
                    Assert.True(JsonComparer.Equal(actualPayload, expectedPayload),
                        $"Vector '{name}': call '{id}' payload mismatch. Expected {expectedPayload.ToJsonString()}, got {actualPayload?.ToJsonString() ?? "null"}.");
                }
            }
            else
            {
                Assert.True(result.IsError,
                    $"Vector '{name}': call '{id}' expected error but succeeded.");
                string expectedCode = expected["e"]!.GetValue<string>();
                Assert.True(result.Error.Code == expectedCode,
                    $"Vector '{name}': call '{id}' error code mismatch. Expected '{expectedCode}', got '{result.Error.Code}'.");
            }
        }
    }

    private static string Dump(List<OutboundAction> actions)
    {
        var sb = new StringBuilder("[");
        for (int i = 0; i < actions.Count; i++)
        {
            OutboundAction a = actions[i];
            if (i > 0)
            {
                sb.Append(", ");
            }

            sb.Append(a.IsSubscribe
                ? $"{{subscribe: {a.Topic}}}"
                : $"{{topic: {a.Topic}, msg: {Encoding.UTF8.GetString(a.Payload!)}}}");
        }

        return sb.Append(']').ToString();
    }

    private static JsonElement NodeToElement(JsonNode? node)
    {
        string json = node?.ToJsonString() ?? "null";
        using JsonDocument document = JsonDocument.Parse(json);
        return document.RootElement.Clone();
    }

    private static ParleyConfig BuildConfig(JsonObject? config)
    {
        var defaults = new ParleyConfig();
        if (config is null)
        {
            return defaults;
        }

        TimeSpan? Ms(string key) =>
            config[key] is JsonNode n ? TimeSpan.FromMilliseconds(n.GetValue<long>()) : null;

        int? Count(string key) =>
            config[key] is JsonNode n ? n.GetValue<int>() : null;

        return new ParleyConfig
        {
            RequestRetryInterval = Ms("requestRetryInterval") ?? defaults.RequestRetryInterval,
            NoAckTimeout = Ms("noAckTimeout") ?? defaults.NoAckTimeout,
            StaleTimeout = Ms("staleTimeout") ?? defaults.StaleTimeout,
            ExecutionTimeout = Ms("executionTimeout") ?? defaults.ExecutionTimeout,
            CancelRetryInterval = Ms("cancelRetryInterval") ?? defaults.CancelRetryInterval,
            HeartbeatInterval = Ms("heartbeatInterval") ?? defaults.HeartbeatInterval,
            ResultRetryInterval = Ms("resultRetryInterval") ?? defaults.ResultRetryInterval,
            ResultRetryTimeout = Ms("resultRetryTimeout") ?? defaults.ResultRetryTimeout,
            TombstoneTtl = Ms("tombstoneTtl") ?? defaults.TombstoneTtl,
            MaxConcurrentExecutions = Count("maxConcurrentExecutions") ?? defaults.MaxConcurrentExecutions,
            MaxActiveCommands = Count("maxActiveCommands") ?? defaults.MaxActiveCommands,
        };
    }

    private static ParleyCallOptions? BuildCallOptions(JsonObject? options)
    {
        if (options is null)
        {
            return null;
        }

        TimeSpan? Ms(string key) =>
            options[key] is JsonNode n ? TimeSpan.FromMilliseconds(n.GetValue<long>()) : null;

        return new ParleyCallOptions
        {
            RequestRetryInterval = Ms("requestRetryInterval"),
            NoAckTimeout = Ms("noAckTimeout"),
            StaleTimeout = Ms("staleTimeout"),
            ExecutionTimeout = Ms("executionTimeout"),
            CancelRetryInterval = Ms("cancelRetryInterval"),
            TombstoneTtl = Ms("tombstoneTtl"),
        };
    }
}
