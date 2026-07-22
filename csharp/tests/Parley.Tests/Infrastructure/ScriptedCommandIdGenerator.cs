using System;
using System.Collections.Generic;
using Parley;

namespace Parley.Tests.Infrastructure;

/// <summary>
/// A command-id generator that returns scripted ids in FIFO order, so vector runners can bypass the
/// random §4.2 generator and use the ids the vector specifies (protocol.md §10).
/// </summary>
internal sealed class ScriptedCommandIdGenerator : IParleyCommandIdGenerator
{
    private readonly Queue<string> _ids = new();

    public void Enqueue(string id) => _ids.Enqueue(id);

    public string NewCommandId() =>
        _ids.Count > 0 ? _ids.Dequeue() : throw new InvalidOperationException("No scripted command id available.");
}
