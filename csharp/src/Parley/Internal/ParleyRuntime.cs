using System;
using System.Threading;

namespace Parley.Internal
{
    /// <summary>
    /// The serialized execution core for one Parley instance: a single gate that all public entry points,
    /// transport callbacks, and timer callbacks marshal into, plus a timer scheduler over a
    /// <see cref="TimeProvider"/>. Timer callbacks always run under the gate, so protocol logic is
    /// single-threaded per instance. Every scheduled timer is tracked so state stays bounded.
    /// </summary>
    internal sealed class ParleyRuntime
    {
        private readonly TimeProvider _timeProvider;

        internal ParleyRuntime(TimeProvider timeProvider)
        {
            _timeProvider = timeProvider;
        }

        /// <summary>The gate that serializes all access to this instance's protocol state.</summary>
        internal object Gate { get; } = new object();

        /// <summary>Number of live (undisposed) timers. Used by the bounded-state tests.</summary>
        internal int LiveTimerCount { get; private set; }

        /// <summary>Schedules a one-shot callback after <paramref name="due"/>. The callback runs under the gate.</summary>
        internal ParleyTimer Schedule(TimeSpan due, Action callback)
        {
            var timer = new ParleyTimer(this, _timeProvider, callback);
            timer.Arm(due);
            LiveTimerCount++;
            return timer;
        }

        private void Fire(ParleyTimer timer, int generation, Action callback)
        {
            lock (Gate)
            {
                // CR2: ITimer.Change / Dispose do not unqueue an already-queued callback. A Reschedule bumps the
                // generation, so a stale pre-reschedule fire (or a disposed timer's late fire) is ignored here.
                if (timer.Disposed || generation != timer.Generation)
                {
                    return;
                }

                try
                {
                    callback();
                }
                catch
                {
                    // CR4: adapter faults (e.g. Publish on a disconnected/disposed client) must never escape onto
                    // library timer threads — an escaped exception would tear down the process. Retries reschedule
                    // before publishing, so delivery is self-healing once the adapter recovers.
                }
            }
        }

        internal void OnTimerDisposed()
        {
            LiveTimerCount--;
        }

        /// <summary>A single logical timer: one-shot by default, rescheduled or disposed under the gate.</summary>
        internal sealed class ParleyTimer
        {
            private readonly ParleyRuntime _runtime;
            private readonly TimeProvider _timeProvider;
            private readonly Action _callback;
            private ITimer? _timer;

            internal ParleyTimer(ParleyRuntime runtime, TimeProvider timeProvider, Action callback)
            {
                _runtime = runtime;
                _timeProvider = timeProvider;
                _callback = callback;
            }

            internal bool Disposed { get; private set; }

            /// <summary>Monotonic arm counter (CR2): every Schedule/Reschedule bumps it and captures it into the
            /// fresh OS-timer registration, so <see cref="ParleyRuntime.Fire"/> can reject a stale queued fire.</summary>
            internal int Generation { get; private set; }

            /// <summary>Arms (or re-arms) the underlying timer with a fresh generation. Must be called under the gate.</summary>
            internal void Arm(TimeSpan due)
            {
                Generation++;
                int generation = Generation;

                // A new registration per arm carries the generation to the eventual Fire (Change cannot rebind
                // the callback state, so an already-queued fire keeps its old generation and is rejected).
                _timer?.Dispose();
                _timer = _timeProvider.CreateTimer(
                    static state =>
                    {
                        var (self, gen) = ((ParleyTimer, int))state!;
                        self._runtime.Fire(self, gen, self._callback);
                    },
                    (this, generation),
                    due,
                    Timeout.InfiniteTimeSpan);
            }

            /// <summary>Re-arms this timer to fire once more after <paramref name="due"/>. Must be called under the gate.</summary>
            internal void Reschedule(TimeSpan due)
            {
                if (Disposed)
                {
                    return;
                }

                Arm(due);
            }

            /// <summary>Stops and releases the timer. Idempotent. Must be called under the gate.</summary>
            internal void Dispose()
            {
                if (Disposed)
                {
                    return;
                }

                Disposed = true;
                _timer!.Dispose();
                _runtime.OnTimerDisposed();
            }
        }
    }
}
