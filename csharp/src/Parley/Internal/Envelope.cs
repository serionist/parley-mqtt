using System.Text.Json;
using System.Text.Json.Serialization;

namespace Parley.Internal
{
    /// <summary>
    /// The wire envelope (protocol.md §4). The payload <c>p</c> is kept as raw JSON (a
    /// <see cref="JsonElement"/>) so it can be (de)serialized by the consumer-supplied
    /// <see cref="System.Text.Json.Serialization.Metadata.JsonTypeInfo{T}"/> rather than by this codec.
    /// </summary>
    internal sealed class Envelope
    {
        /// <summary>Protocol version (<c>v</c>). This document defines <c>1</c>.</summary>
        [JsonPropertyName("v")]
        public int V { get; set; }

        /// <summary>Message type (<c>t</c>): <c>req hb prg res rak cnl</c>.</summary>
        [JsonPropertyName("t")]
        public string? T { get; set; }

        /// <summary>Command id (<c>id</c>).</summary>
        [JsonPropertyName("id")]
        public string? Id { get; set; }

        /// <summary>Raw payload (<c>p</c>); absent when there is no payload.</summary>
        [JsonPropertyName("p")]
        [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
        public JsonElement? P { get; set; }

        /// <summary>Result success flag (<c>ok</c>); present only on <c>res</c>.</summary>
        [JsonPropertyName("ok")]
        [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
        public bool? Ok { get; set; }

        /// <summary>Error code (<c>e</c>); present on error <c>res</c>.</summary>
        [JsonPropertyName("e")]
        [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
        public string? E { get; set; }

        /// <summary>Human-readable error detail (<c>m</c>); optional on error <c>res</c>.</summary>
        [JsonPropertyName("m")]
        [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
        public string? M { get; set; }
    }

    [JsonSourceGenerationOptions(GenerationMode = JsonSourceGenerationMode.Metadata)]
    [JsonSerializable(typeof(Envelope))]
    internal partial class EnvelopeJsonContext : JsonSerializerContext
    {
    }
}
