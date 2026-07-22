using System;
using System.IO;
using System.Text.Json;
using System.Text.Json.Serialization.Metadata;

namespace Parley.Internal
{
    /// <summary>
    /// Maps a consumer's custom error type to and from the wire <c>e</c> code string (protocol.md §5).
    /// A custom error value MUST serialize to a JSON string; that string is the code. Codes beginning
    /// with <c>$</c> are reserved for the protocol and rejected on the sending side.
    /// </summary>
    internal static class ErrorCodec
    {
        /// <summary>Produces the wire <c>e</c> code for a custom error value.</summary>
        /// <exception cref="InvalidOperationException">The value does not serialize to a JSON string, or the code is <c>$</c>-reserved.</exception>
        internal static string CustomToWire<TError>(TError error, JsonTypeInfo<TError> typeInfo)
        {
            JsonElement element = MessageCodec.ToElement(error, typeInfo);
            if (element.ValueKind != JsonValueKind.String)
            {
                throw new InvalidOperationException(
                    $"A custom Parley error of type '{typeof(TError)}' must serialize to a JSON string (the error code); " +
                    $"got JSON {element.ValueKind}.");
            }

            string code = element.GetString()!;
            if (code.Length > 0 && code[0] == '$')
            {
                throw new InvalidOperationException(
                    $"Custom error codes must not begin with '$' (reserved for the protocol): '{code}'.");
            }

            return code;
        }

        /// <summary>
        /// Produces the typed custom error for a received wire code. Per the settled cross-language contract, a
        /// custom error is derived from the <c>e</c> string code only — structured error <c>p</c> stays wire-legal
        /// (§4.1) but is not surfaced by the v1 APIs. Best-effort: returns <see langword="default"/> if the code
        /// string does not map onto <typeparamref name="TError"/>.
        /// </summary>
        internal static TError? WireToCustom<TError>(string code, JsonTypeInfo<TError> typeInfo)
        {
            try
            {
                using var stream = new MemoryStream();
                using (var writer = new Utf8JsonWriter(stream))
                {
                    writer.WriteStringValue(code);
                }

                stream.Position = 0;
                using JsonDocument document = JsonDocument.Parse(stream);
                return document.RootElement.Deserialize(typeInfo);
            }
            catch (JsonException)
            {
                return default;
            }
        }
    }
}
