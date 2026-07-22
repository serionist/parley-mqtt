using System.Text.Json;
using System.Text.Json.Serialization.Metadata;

namespace Parley.Internal
{
    /// <summary>Wire message-type strings (protocol.md §4).</summary>
    internal static class MessageTypes
    {
        internal const string Request = "req";
        internal const string Heartbeat = "hb";
        internal const string Progress = "prg";
        internal const string Result = "res";
        internal const string ResultAck = "rak";
        internal const string Cancel = "cnl";
    }

    /// <summary>
    /// Envelope (de)serialization plus the §4/§4.2 robustness checks. The codec knows nothing about
    /// the typed payloads; those cross the boundary as raw <see cref="JsonElement"/> values.
    /// </summary>
    internal static class MessageCodec
    {
        /// <summary>The single protocol version this document defines.</summary>
        internal const int Version = 1;

        internal static byte[] Serialize(Envelope envelope) =>
            JsonSerializer.SerializeToUtf8Bytes(envelope, EnvelopeJsonContext.Default.Envelope);

        /// <summary>Parses a payload into an <see cref="Envelope"/>, or returns <see langword="null"/> if the
        /// bytes are not a JSON object. The raw payload <c>p</c> is cloned so it outlives the parse buffer.</summary>
        internal static Envelope? TryParse(System.ReadOnlyMemory<byte> payload)
        {
            try
            {
                Envelope? envelope = JsonSerializer.Deserialize(payload.Span, EnvelopeJsonContext.Default.Envelope);
                if (envelope is null)
                {
                    return null;
                }

                if (envelope.P.HasValue)
                {
                    envelope.P = envelope.P.Value.Clone();
                }

                return envelope;
            }
            catch (JsonException)
            {
                return null;
            }
        }

        /// <summary>Serializes a typed payload to a self-contained raw JSON element using source-gen metadata.</summary>
        internal static JsonElement ToElement<T>(T value, JsonTypeInfo<T> typeInfo)
        {
            byte[] bytes = JsonSerializer.SerializeToUtf8Bytes(value, typeInfo);
            using JsonDocument document = JsonDocument.Parse(bytes);
            return document.RootElement.Clone();
        }

        /// <summary>Serializes a typed payload for the <c>p</c> field, mapping a JSON <c>null</c> to an absent
        /// payload (protocol.md §4: "omitted/null means no payload").</summary>
        internal static JsonElement? ToPayloadElement<T>(T value, JsonTypeInfo<T> typeInfo)
        {
            JsonElement element = ToElement(value, typeInfo);
            return element.ValueKind == JsonValueKind.Null ? (JsonElement?)null : element;
        }

        /// <summary>Deserializes a raw JSON element to a typed payload using source-gen metadata.</summary>
        internal static T? FromElement<T>(JsonElement element, JsonTypeInfo<T> typeInfo) =>
            element.Deserialize(typeInfo);

        /// <summary>Validates a command id against protocol.md §4.2 (1–64 chars from <c>[A-Za-z0-9_-]</c>).</summary>
        internal static bool IsValidId(string? id)
        {
            if (id is null || id.Length < 1 || id.Length > 64)
            {
                return false;
            }

            foreach (char c in id)
            {
                bool ok = (c >= 'A' && c <= 'Z')
                    || (c >= 'a' && c <= 'z')
                    || (c >= '0' && c <= '9')
                    || c == '_'
                    || c == '-';
                if (!ok)
                {
                    return false;
                }
            }

            return true;
        }
    }
}
