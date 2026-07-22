using System;
using System.Collections.Generic;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;
using Microsoft.Extensions.Time.Testing;
using Parley;

namespace Parley.Tests.Infrastructure;

/// <summary>
/// A scripted, time-driven handler stub for vector replay. Each admitted command's
/// <see cref="HandleCommandAsync"/> parks on a completion source; the runner resolves it when the
/// vector's <c>handler</c> action fires (<c>complete</c>/<c>fail</c>/<c>progress</c>).
/// </summary>
internal sealed class VectorHandler : ParleyCommandHandler<JsonElement, JsonElement, JsonElement, string>
{
    private readonly Dictionary<string, PendingCommand> _pending = new();

    public VectorHandler(FakeTransport transport, ParleyTopics topics, ParleyConfig config, TimeProvider time)
        : base(transport, topics, config, VectorJsonContext.Default, time)
    {
    }

    protected override Task<ParleyOutcome<JsonElement, string>> HandleCommandAsync(
        JsonElement request,
        IParleyExecution<JsonElement> execution,
        CancellationToken cancellationToken)
    {
        // Synchronous continuations: resolving the source runs the library's completion inline, under the clock.
        var tcs = new TaskCompletionSource<ParleyOutcome<JsonElement, string>>();
        _pending[execution.CommandId] = new PendingCommand(tcs, execution);
        return tcs.Task;
    }

    public void Complete(string id, JsonElement payload)
    {
        ParleyOutcome<JsonElement, string> outcome = ParleyOutcome.Success(payload);
        Resolve(id).CompletionSource.SetResult(outcome);
    }

    public void Fail(string id, string code, string? message)
    {
        ParleyOutcome<JsonElement, string> outcome = code == ParleyErrorCodes.Cancelled
            ? ParleyOutcome.Cancelled()
            : ParleyOutcome.Error(code, message);
        Resolve(id).CompletionSource.SetResult(outcome);
    }

    public void EmitProgress(string id, JsonElement payload) => Resolve(id).Execution.ReportProgress(payload);

    public bool IsCancellationRequested(string id) => Resolve(id).Execution.CancellationToken.IsCancellationRequested;

    public bool IsExecuting(string id) => _pending.ContainsKey(id);

    private PendingCommand Resolve(string id) =>
        _pending.TryGetValue(id, out PendingCommand? pending)
            ? pending
            : throw new InvalidOperationException($"No admitted command '{id}' is executing.");

    private sealed class PendingCommand
    {
        public PendingCommand(TaskCompletionSource<ParleyOutcome<JsonElement, string>> completionSource, IParleyExecution<JsonElement> execution)
        {
            CompletionSource = completionSource;
            Execution = execution;
        }

        public TaskCompletionSource<ParleyOutcome<JsonElement, string>> CompletionSource { get; }

        public IParleyExecution<JsonElement> Execution { get; }
    }
}
