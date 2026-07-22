using System;

namespace Parley
{
    /// <summary>
    /// The four consumer-supplied topic strings that identify one command type
    /// (see protocol.md §3). Parley imposes no naming scheme or structure: the strings are opaque,
    /// must be distinct within the set, and must not contain MQTT wildcards.
    /// </summary>
    public sealed class ParleyTopics
    {
        /// <summary>Commander → handler topic, carrying <c>req</c>, <c>rak</c>, and <c>cnl</c> messages.</summary>
        public string Request { get; }

        /// <summary>Handler → commander topic, carrying <c>hb</c> (heartbeat) messages.</summary>
        public string Heartbeat { get; }

        /// <summary>Handler → commander topic, carrying <c>prg</c> (progress) messages.</summary>
        public string Progress { get; }

        /// <summary>Handler → commander topic, carrying <c>res</c> (result) messages.</summary>
        public string Result { get; }

        /// <summary>Creates a topic set. Fails fast if any string is null/empty, contains an MQTT
        /// wildcard (<c>+</c> or <c>#</c>), or is not distinct within the set.</summary>
        /// <param name="request">The request topic (commander → handler).</param>
        /// <param name="heartbeat">The heartbeat topic (handler → commander).</param>
        /// <param name="progress">The progress topic (handler → commander).</param>
        /// <param name="result">The result topic (handler → commander).</param>
        /// <exception cref="ArgumentException">A topic is null/empty, contains a wildcard, or duplicates another.</exception>
        public ParleyTopics(string request, string heartbeat, string progress, string result)
        {
            Request = Validate(request, nameof(request));
            Heartbeat = Validate(heartbeat, nameof(heartbeat));
            Progress = Validate(progress, nameof(progress));
            Result = Validate(result, nameof(result));

            if (Request == Heartbeat || Request == Progress || Request == Result ||
                Heartbeat == Progress || Heartbeat == Result ||
                Progress == Result)
            {
                throw new ArgumentException("Parley topics must be distinct within the set.");
            }
        }

        private static string Validate(string topic, string name)
        {
            if (string.IsNullOrEmpty(topic))
            {
                throw new ArgumentException("Parley topic must be a non-empty string.", name);
            }

            if (topic.IndexOf('+') >= 0 || topic.IndexOf('#') >= 0)
            {
                throw new ArgumentException("Parley topics must not contain MQTT wildcards ('+' or '#').", name);
            }

            return topic;
        }
    }
}
