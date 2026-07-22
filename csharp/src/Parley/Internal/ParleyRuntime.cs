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
            var timer = new ParleyTimer(this, callback);
            timer.Timer = _timeProvider.CreateTimer(static state => ((ParleyTimer)state!).Fire(), timer, due, Timeout.InfiniteTimeSpan);
            LiveTimerCount++;
            return timer;
        }

        private void Fire(ParleyTimer timer, Action callback)
        {
            lock (Gate)
            {
                if (timer.Disposed)
                {
                    return;
                }

                callback();
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
            private readonly Action _callback;

            internal ParleyTimer(ParleyRuntime runtime, Action callback)
            {
                _runtime = runtime;
                _callback = callback;
            }

            internal ITimer? Timer { get; set; }

            internal bool Disposed { get; private set; }

            internal void Fire() => _runtime.Fire(this, _callback);

            /// <summary>Re-arms this timer to fire once more after <paramref name="due"/>. Must be called under the gate.</summary>
            internal void Reschedule(TimeSpan due)
            {
                if (Disposed)
                {
                    return;
                }

                Timer!.Change(due, Timeout.InfiniteTimeSpan);
            }

            /// <summary>Stops and releases the timer. Idempotent. Must be called under the gate.</summary>
            internal void Dispose()
            {
                if (Disposed)
                {
                    return;
                }

                Disposed = true;
                Timer!.Dispose();
                _runtime.OnTimerDisposed();
            }
        }
    }
}
