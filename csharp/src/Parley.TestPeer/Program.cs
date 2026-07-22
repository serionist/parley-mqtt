using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;
using MQTTnet;
using MQTTnet.Formatter;
using MQTTnet.Protocol;
using Parley;

namespace Parley.TestPeer
{
    // Parley layer-3 interop test peer (C#), a GENERIC interpreter of
    // docs/conformance/interop/scenarios.json. Handler role serves the five fixed harness command
    // behaviours and prints READY after all SUBACKs; commander role runs the step model, writes the
    // JSON report, and exits nonzero on any failure. Zero suite logic lives here — only the harness.
    internal static class Program
    {
        private const string SelfLang = "csharp";
        private const string ReadyLine = "PARLEY-PEER-READY";
        private static readonly string[] Commands = { "echo", "slow", "err", "cancelme", "busy1" };

        private static async Task<int> Main(string[] args)
        {
            Dictionary<string, string> opt = ParseArgs(args);
            string role = Require(opt, "role");
            (string host, int port) = ParseBroker(Require(opt, "broker"));
            string prefix = Require(opt, "prefix");
            string scenariosPath = Opt(opt, "scenarios", "scenarios.json");
            string suiteKey = Opt(opt, "suite", "matrix") == "selfchecks" ? "selfChecks" : "scenarios";
            string handlerLang = Opt(opt, "handler-lang", "-");
            string? reportPath = OptOrNull(opt, "report");
            string? scenarioFilter = OptOrNull(opt, "scenario");
            MqttQualityOfServiceLevel qos = (int.TryParse(Opt(opt, "qos", "0"), out int q) ? q : 0) >= 1
                ? MqttQualityOfServiceLevel.AtLeastOnce
                : MqttQualityOfServiceLevel.AtMostOnce;

            using JsonDocument doc = JsonDocument.Parse(File.ReadAllBytes(scenariosPath));
            JsonElement root = doc.RootElement;
            JsonElement cfg = root.GetProperty("config");
            int baseMaxActive = GetInt(cfg, "maxActiveCommands", 8);
            JsonElement timeouts = root.GetProperty("timeouts");
            int barrierMs = GetInt(timeouts, "awaitProgressMs", 6000);
            int callMs = GetInt(timeouts, "callMs", 15000);

            MqttClientFactory factory = new MqttClientFactory();
            IMqttClient client = factory.CreateMqttClient();
            MqttClientOptions mopts = new MqttClientOptionsBuilder()
                .WithTcpServer(host, port)
                .WithProtocolVersion(MqttProtocolVersion.V311)
                .Build();
            await client.ConnectAsync(mopts).ConfigureAwait(false);
            MqttNetAdapter adapter = new MqttNetAdapter(client, mopts, qos);

            if (role == "handler")
            {
                List<HarnessHandler> handlers = new List<HarnessHandler>();
                foreach (string cmd in Commands)
                {
                    int maxActive = cmd == "busy1" ? 1 : baseMaxActive;
                    handlers.Add(new HarnessHandler(adapter, Topics(prefix, cmd), BuildConfig(cfg, maxActive), KindOf(cmd)));
                }

                await adapter.WaitForSubscriptionsAsync().ConfigureAwait(false);
                Console.WriteLine(ReadyLine);
                Console.Out.Flush();
                await Task.Delay(Timeout.Infinite).ConfigureAwait(false);
                GC.KeepAlive(handlers);
                return 0;
            }

            // commander role
            ParleyConfig commanderConfig = BuildConfig(cfg, baseMaxActive);
            Dictionary<string, ParleyCommander<JsonElement, JsonElement, JsonElement, string>> commanders =
                new Dictionary<string, ParleyCommander<JsonElement, JsonElement, JsonElement, string>>();
            foreach (string cmd in Commands)
            {
                commanders[cmd] = new ParleyCommander<JsonElement, JsonElement, JsonElement, string>(
                    adapter, Topics(prefix, cmd), commanderConfig, PeerJsonContext.Default);
            }

            await adapter.WaitForSubscriptionsAsync().ConfigureAwait(false);

            List<(string Scenario, bool Pass, string Detail)> results = new List<(string, bool, string)>();
            foreach (JsonElement scenario in root.GetProperty(suiteKey).EnumerateArray())
            {
                if (scenarioFilter is not null &&
                    (scenario.GetProperty("name").GetString() ?? string.Empty) != scenarioFilter)
                {
                    continue;
                }

                results.Add(await RunScenarioAsync(scenario, commanders, barrierMs, callMs).ConfigureAwait(false));
            }

            if (scenarioFilter is not null && results.Count == 0)
            {
                results.Add((scenarioFilter, false, $"no scenario named '{scenarioFilter}' in suite '{suiteKey}'"));
            }

            bool allPass = results.All(r => r.Pass);
            string report = BuildReport(SelfLang, handlerLang, prefix, suiteKey, results, allPass);
            Console.WriteLine(report);
            if (reportPath is not null)
            {
                File.WriteAllText(reportPath, report);
            }

            foreach (var c in commanders.Values)
            {
                c.Dispose();
            }

            adapter.BeginShutdown();
            try
            {
                await client.DisconnectAsync().ConfigureAwait(false);
            }
            catch
            {
                // best-effort teardown
            }

            return allPass ? 0 : 1;
        }

        // ---- the step model (protocol.md §6 consumer side; docs/testing.md layer 3) ----

        private sealed class CallState
        {
            public string Name = string.Empty;
            public string Command = string.Empty;
            public CancellationTokenSource Cts = new CancellationTokenSource();
            public CountingProgress Progress = new CountingProgress();
            public Task<ParleyResult<JsonElement, string>> Task = System.Threading.Tasks.Task.FromResult(default(ParleyResult<JsonElement, string>));
        }

        private static async Task<(string, bool, string)> RunScenarioAsync(
            JsonElement scenario,
            Dictionary<string, ParleyCommander<JsonElement, JsonElement, JsonElement, string>> commanders,
            int barrierMs,
            int callMs)
        {
            string name = scenario.GetProperty("name").GetString() ?? "?";
            Dictionary<string, CallState> named = new Dictionary<string, CallState>();
            List<CallState> all = new List<CallState>();
            try
            {
                foreach (JsonElement step in scenario.GetProperty("steps").EnumerateArray())
                {
                    if (step.TryGetProperty("call", out JsonElement call))
                    {
                        CallState st = StartCall(call, commanders, all);
                        ParleyResult<JsonElement, string> res = await AwaitCap(st, callMs).ConfigureAwait(false);
                        CheckExpect(name, call.GetProperty("expect"), res, st);
                    }
                    else if (step.TryGetProperty("callAsync", out JsonElement callAsync))
                    {
                        CallState st = StartCall(callAsync, commanders, all);
                        st.Name = callAsync.GetProperty("name").GetString() ?? "?";
                        named[st.Name] = st;
                    }
                    else if (step.TryGetProperty("awaitProgress", out JsonElement ap))
                    {
                        CallState st = Named(named, ap);
                        await BarrierAsync(name, st, barrierMs).ConfigureAwait(false);
                    }
                    else if (step.TryGetProperty("cancel", out JsonElement cancel))
                    {
                        Named(named, cancel).Cts.Cancel();
                    }
                    else if (step.TryGetProperty("barrier", out JsonElement barrier))
                    {
                        // Driver rendezvous: print a flushed PARLEY-BARRIER line and continue. The
                        // driver watches the log stream for this and performs the container action.
                        string label = barrier.TryGetProperty("label", out JsonElement lbl)
                            ? lbl.GetString() ?? name
                            : name;
                        Console.WriteLine($"PARLEY-BARRIER {label}");
                        Console.Out.Flush();
                    }
                    else if (step.TryGetProperty("await", out JsonElement awaitStep))
                    {
                        CallState st = Named(named, awaitStep);
                        ParleyResult<JsonElement, string> res = await AwaitCap(st, callMs).ConfigureAwait(false);
                        CheckExpect(name, awaitStep.GetProperty("expect"), res, st);
                    }
                    else
                    {
                        throw new PeerAssert($"{name}: unrecognized step {step.GetRawText()}");
                    }
                }

                return (name, true, string.Empty);
            }
            catch (PeerAssert ex)
            {
                return (name, false, ex.Message);
            }
            catch (Exception ex)
            {
                return (name, false, $"unexpected: {ex.GetType().Name}: {ex.Message}");
            }
            finally
            {
                foreach (CallState st in all)
                {
                    st.Cts.Dispose();
                }
            }
        }

        private static CallState StartCall(
            JsonElement call,
            Dictionary<string, ParleyCommander<JsonElement, JsonElement, JsonElement, string>> commanders,
            List<CallState> all)
        {
            string command = call.GetProperty("command").GetString() ?? throw new PeerAssert("call missing command");
            if (!commanders.TryGetValue(command, out var commander))
            {
                throw new PeerAssert($"unknown command '{command}'");
            }

            JsonElement request = call.TryGetProperty("p", out JsonElement p) ? p.Clone() : Elem("null");
            CallState st = new CallState { Command = command };
            all.Add(st);
            st.Task = commander.ExecuteAsync(request, new ParleyCallOptions(), st.Progress, st.Cts.Token);
            return st;
        }

        private static CallState Named(Dictionary<string, CallState> named, JsonElement directive)
        {
            string target = directive.GetProperty("name").GetString() ?? "?";
            if (!named.TryGetValue(target, out CallState? st))
            {
                throw new PeerAssert($"reference to unknown call '{target}'");
            }

            return st;
        }

        private static async Task BarrierAsync(string scenario, CallState st, int capMs)
        {
            Stopwatch sw = Stopwatch.StartNew();
            while (sw.ElapsedMilliseconds < capMs)
            {
                if (st.Progress.Count >= 1)
                {
                    return;
                }

                if (st.Task.IsCompleted)
                {
                    throw new PeerAssert($"{scenario}: awaitProgress '{st.Name}' — call reached a terminal outcome before any progress");
                }

                await Task.Delay(15).ConfigureAwait(false);
            }

            throw new PeerAssert($"{scenario}: awaitProgress '{st.Name}' — no progress within {capMs}ms");
        }

        private static async Task<ParleyResult<JsonElement, string>> AwaitCap(CallState st, int capMs)
        {
            Task completed = await Task.WhenAny(st.Task, Task.Delay(capMs)).ConfigureAwait(false);
            if (completed != st.Task)
            {
                throw new PeerAssert($"call '{st.Command}' — no terminal outcome within {capMs}ms");
            }

            return st.Task.Result;
        }

        private static void CheckExpect(string scenario, JsonElement expect, ParleyResult<JsonElement, string> res, CallState st)
        {
            bool expectOk = expect.GetProperty("ok").GetBoolean();
            if (expectOk)
            {
                if (!res.IsSuccess)
                {
                    throw new PeerAssert($"{scenario}: expected success, got error '{res.Error.Code}'");
                }

                if (expect.TryGetProperty("p", out JsonElement expectedP))
                {
                    JsonElement actual = res.Value;
                    if (actual.ValueKind == JsonValueKind.Undefined)
                    {
                        throw new PeerAssert($"{scenario}: expected a result payload but got none");
                    }

                    if (!JsonDeepEquals(actual, expectedP))
                    {
                        throw new PeerAssert($"{scenario}: result payload mismatch. expected {expectedP.GetRawText()}, got {actual.GetRawText()}");
                    }
                }
            }
            else
            {
                if (!res.IsError)
                {
                    throw new PeerAssert($"{scenario}: expected error but succeeded");
                }

                if (expect.TryGetProperty("e", out JsonElement expectedE))
                {
                    string want = expectedE.GetString() ?? string.Empty;
                    if (res.Error.Code != want)
                    {
                        throw new PeerAssert($"{scenario}: error code mismatch. expected '{want}', got '{res.Error.Code}'");
                    }
                }
            }

            if (expect.TryGetProperty("minProgress", out JsonElement mp))
            {
                int min = mp.GetInt32();
                if (st.Progress.Count < min)
                {
                    throw new PeerAssert($"{scenario}: minProgress not met. expected >= {min}, got {st.Progress.Count}");
                }
            }
        }

        // ---- harness handler (the five fixed command behaviours) ----

        private enum CommandKind
        {
            Echo,
            Slow,
            Err,
            Cancelme,
            Busy1,
        }

        private static CommandKind KindOf(string cmd) => cmd switch
        {
            "echo" => CommandKind.Echo,
            "slow" => CommandKind.Slow,
            "err" => CommandKind.Err,
            "cancelme" => CommandKind.Cancelme,
            "busy1" => CommandKind.Busy1,
            _ => throw new ArgumentException($"unknown command '{cmd}'"),
        };

        private sealed class HarnessHandler : ParleyCommandHandler<JsonElement, JsonElement, JsonElement, string>
        {
            private readonly CommandKind _kind;

            public HarnessHandler(ParleyConnectionAdapter adapter, ParleyTopics topics, ParleyConfig config, CommandKind kind)
                : base(adapter, topics, config, PeerJsonContext.Default)
            {
                _kind = kind;
            }

            protected override async Task<ParleyOutcome<JsonElement, string>> HandleCommandAsync(
                JsonElement request,
                IParleyExecution<JsonElement> execution,
                CancellationToken cancellationToken)
            {
                switch (_kind)
                {
                    case CommandKind.Echo:
                        return ParleyOutcome.Success(request);

                    case CommandKind.Err:
                        return ParleyOutcome.Error(GetString(request, "code") ?? "error");

                    case CommandKind.Slow:
                    {
                        int durationMs = GetInt(request, "durationMs", 1000);
                        int everyMs = Math.Max(1, GetInt(request, "progressEveryMs", 200));
                        execution.ReportProgress(ProgressElem(0));
                        int elapsed = 0;
                        int i = 0;
                        while (elapsed < durationMs)
                        {
                            await Task.Delay(everyMs, cancellationToken).ConfigureAwait(false);
                            elapsed += everyMs;
                            execution.ReportProgress(ProgressElem(++i));
                        }

                        return ParleyOutcome.Success(Elem("{\"done\":true}"));
                    }

                    case CommandKind.Cancelme:
                    case CommandKind.Busy1:
                    default:
                    {
                        int everyMs = Math.Max(1, GetInt(request, "progressEveryMs", 200));
                        execution.ReportProgress(ProgressElem(0));
                        int i = 0;
                        try
                        {
                            while (!cancellationToken.IsCancellationRequested)
                            {
                                await Task.Delay(everyMs, cancellationToken).ConfigureAwait(false);
                                execution.ReportProgress(ProgressElem(++i));
                            }
                        }
                        catch (OperationCanceledException)
                        {
                            // cancelled while parked
                        }

                        return ParleyOutcome.Cancelled();
                    }
                }
            }
        }

        private sealed class CountingProgress : IProgress<JsonElement>
        {
            private int _count;

            public int Count => Volatile.Read(ref _count);

            public void Report(JsonElement value) => Interlocked.Increment(ref _count);
        }

        // ---- helpers ----

        private static bool JsonDeepEquals(JsonElement a, JsonElement b)
        {
            if (a.ValueKind != b.ValueKind)
            {
                return false;
            }

            switch (a.ValueKind)
            {
                case JsonValueKind.Object:
                {
                    int countA = 0;
                    foreach (JsonProperty prop in a.EnumerateObject())
                    {
                        countA++;
                        if (!b.TryGetProperty(prop.Name, out JsonElement bv) || !JsonDeepEquals(prop.Value, bv))
                        {
                            return false;
                        }
                    }

                    int countB = 0;
                    foreach (JsonProperty _ in b.EnumerateObject())
                    {
                        countB++;
                    }

                    return countA == countB;
                }

                case JsonValueKind.Array:
                {
                    JsonElement.ArrayEnumerator ea = a.EnumerateArray();
                    JsonElement.ArrayEnumerator eb = b.EnumerateArray();
                    while (true)
                    {
                        bool na = ea.MoveNext();
                        bool nb = eb.MoveNext();
                        if (na != nb)
                        {
                            return false;
                        }

                        if (!na)
                        {
                            return true;
                        }

                        if (!JsonDeepEquals(ea.Current, eb.Current))
                        {
                            return false;
                        }
                    }
                }

                case JsonValueKind.Number:
                    return a.GetDouble() == b.GetDouble();

                case JsonValueKind.String:
                    return a.GetString() == b.GetString();

                case JsonValueKind.True:
                case JsonValueKind.False:
                case JsonValueKind.Null:
                    return true;

                default:
                    return false;
            }
        }

        private static JsonElement Elem(string json)
        {
            using JsonDocument d = JsonDocument.Parse(json);
            return d.RootElement.Clone();
        }

        private static JsonElement ProgressElem(int i) => Elem($"{{\"i\":{i}}}");

        private static string? GetString(JsonElement o, string key) =>
            o.ValueKind == JsonValueKind.Object && o.TryGetProperty(key, out JsonElement v) && v.ValueKind == JsonValueKind.String
                ? v.GetString()
                : null;

        private static int GetInt(JsonElement o, string key, int def) =>
            o.ValueKind == JsonValueKind.Object && o.TryGetProperty(key, out JsonElement v) && v.ValueKind == JsonValueKind.Number
                ? v.GetInt32()
                : def;

        private static ParleyTopics Topics(string prefix, string cmd) =>
            new ParleyTopics($"{prefix}/{cmd}/req", $"{prefix}/{cmd}/hb", $"{prefix}/{cmd}/prg", $"{prefix}/{cmd}/res");

        private static ParleyConfig BuildConfig(JsonElement c, int maxActiveCommands) =>
            new ParleyConfig
            {
                RequestRetryInterval = Ms(c, "requestRetryInterval", 400),
                NoAckTimeout = Ms(c, "noAckTimeout", 2000),
                StaleTimeout = Ms(c, "staleTimeout", 1500),
                ExecutionTimeout = Ms(c, "executionTimeout", 10000),
                CancelRetryInterval = Ms(c, "cancelRetryInterval", 400),
                HeartbeatInterval = Ms(c, "heartbeatInterval", 400),
                ResultRetryInterval = Ms(c, "resultRetryInterval", 400),
                ResultRetryTimeout = Ms(c, "resultRetryTimeout", 5000),
                TombstoneTtl = Ms(c, "tombstoneTtl", 10000),
                MaxConcurrentExecutions = GetInt(c, "maxConcurrentExecutions", 1),
                MaxActiveCommands = maxActiveCommands,
            };

        private static TimeSpan Ms(JsonElement c, string key, int def) => TimeSpan.FromMilliseconds(GetInt(c, key, def));

        private static string BuildReport(
            string commander,
            string handler,
            string prefix,
            string suite,
            List<(string Scenario, bool Pass, string Detail)> results,
            bool allPass)
        {
            using MemoryStream stream = new MemoryStream();
            using (Utf8JsonWriter w = new Utf8JsonWriter(stream, new JsonWriterOptions { Indented = true }))
            {
                w.WriteStartObject();
                w.WriteString("commander", commander);
                w.WriteString("handler", handler);
                w.WriteString("prefix", prefix);
                w.WriteString("suite", suite);
                w.WriteStartArray("results");
                foreach ((string scenario, bool pass, string detail) in results)
                {
                    w.WriteStartObject();
                    w.WriteString("scenario", scenario);
                    w.WriteBoolean("pass", pass);
                    w.WriteString("detail", detail);
                    w.WriteEndObject();
                }

                w.WriteEndArray();
                w.WriteBoolean("pass", allPass);
                w.WriteEndObject();
            }

            return System.Text.Encoding.UTF8.GetString(stream.ToArray());
        }

        private static Dictionary<string, string> ParseArgs(string[] args)
        {
            Dictionary<string, string> result = new Dictionary<string, string>();
            for (int i = 0; i < args.Length; i++)
            {
                if (args[i].StartsWith("--", StringComparison.Ordinal) && i + 1 < args.Length)
                {
                    result[args[i].Substring(2)] = args[i + 1];
                    i++;
                }
            }

            return result;
        }

        private static string Require(Dictionary<string, string> opt, string key) =>
            opt.TryGetValue(key, out string? v) ? v : throw new ArgumentException($"missing required --{key}");

        private static string Opt(Dictionary<string, string> opt, string key, string def) =>
            opt.TryGetValue(key, out string? v) ? v : def;

        private static string? OptOrNull(Dictionary<string, string> opt, string key) =>
            opt.TryGetValue(key, out string? v) ? v : null;

        private static (string, int) ParseBroker(string broker)
        {
            string s = broker;
            int scheme = s.IndexOf("://", StringComparison.Ordinal);
            if (scheme >= 0)
            {
                s = s.Substring(scheme + 3);
            }

            int colon = s.LastIndexOf(':');
            if (colon < 0)
            {
                return (s, 1883);
            }

            return (s.Substring(0, colon), int.Parse(s.Substring(colon + 1)));
        }

        private sealed class PeerAssert : Exception
        {
            public PeerAssert(string message)
                : base(message)
            {
            }
        }
    }
}
