using System;
using System.Collections.Generic;

namespace Parley.Internal
{
    /// <summary>
    /// A bounded set of tombstoned command ids (protocol.md C5 / H5 / H8). Each entry self-expires after
    /// its TTL, and the table is additionally hard-capped with oldest-first eviction, so state stays
    /// bounded on both axes. Must be accessed under the runtime gate.
    /// </summary>
    internal sealed class TombstoneTable
    {
        private readonly ParleyRuntime _runtime;
        private readonly int _capacity;
        private readonly Dictionary<string, Entry> _entries = new();
        private readonly LinkedList<string> _order = new();

        internal TombstoneTable(ParleyRuntime runtime, int capacity)
        {
            _runtime = runtime;
            _capacity = capacity;
        }

        internal int Count => _entries.Count;

        internal bool Contains(string id) => _entries.ContainsKey(id);

        internal void Add(string id, TimeSpan ttl)
        {
            // Refresh if already present (dispose the old TTL timer and re-order).
            if (_entries.TryGetValue(id, out Entry? existing))
            {
                existing.Timer.Dispose();
                _order.Remove(existing.Node);
                _entries.Remove(id);
            }

            LinkedListNode<string> node = _order.AddLast(id);
            ParleyRuntime.ParleyTimer timer = _runtime.Schedule(ttl, () => Remove(id));
            _entries[id] = new Entry(node, timer);

            // H8: oldest-first eviction when over the hard cap.
            while (_entries.Count > _capacity && _order.First is { } oldest)
            {
                Remove(oldest.Value);
            }
        }

        internal void Remove(string id)
        {
            if (_entries.TryGetValue(id, out Entry? entry))
            {
                entry.Timer.Dispose();
                _order.Remove(entry.Node);
                _entries.Remove(id);
            }
        }

        /// <summary>Disposes every entry's TTL timer and empties the table (used on instance disposal).</summary>
        internal void Clear()
        {
            foreach (Entry entry in _entries.Values)
            {
                entry.Timer.Dispose();
            }

            _entries.Clear();
            _order.Clear();
        }

        private sealed class Entry
        {
            public Entry(LinkedListNode<string> node, ParleyRuntime.ParleyTimer timer)
            {
                Node = node;
                Timer = timer;
            }

            public LinkedListNode<string> Node { get; }

            public ParleyRuntime.ParleyTimer Timer { get; }
        }
    }
}
