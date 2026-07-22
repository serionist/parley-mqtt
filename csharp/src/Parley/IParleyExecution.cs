using System.Threading;

namespace Parley
{
    /// <summary>
    /// The execution context handed to a command handler: the command id, a progress emitter, and the
    /// cancellation signal that a wire-level <c>cnl</c> triggers.
    /// </summary>
    /// <typeparam name="TProgress">The progress payload type.</typeparam>
    public interface IParleyExecution<in TProgress>
    {
        /// <summary>The command id being executed (protocol.md §4.2).</summary>
        string CommandId { get; }

        /// <summary>Signalled when a <c>cnl</c> for this command arrives (protocol.md H7).</summary>
        CancellationToken CancellationToken { get; }

        /// <summary>Emits one advisory progress update; each call publishes one <c>prg</c> (protocol.md C8/H4).
        /// Delivery is best-effort: loss, duplication, and reordering are tolerated and never acknowledged.</summary>
        /// <param name="progress">The progress payload.</param>
        void ReportProgress(TProgress progress);
    }
}
