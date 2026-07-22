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
        /// Produces the typed custom error for a received wire code. Structured error data in <paramref name="errorPayload"/>
        /// is preferred when present; otherwise the code string itself is deserialized. Best-effort: returns
        /// <see langword="default"/> if neither can be mapped onto <typeparamref name="TError"/>.
        /// </summary>
        internal static TError? WireToCustom<TError>(string code, JsonElement? errorPayload, JsonTypeInfo<TError> typeInfo)
        {
            if (errorPayload.HasValue)
            {
                try
                {
                    return errorPayload.Value.Deserialize(typeInfo);
                }
                catch (JsonException)
                {
                    // Fall through to the code-string mapping.
                }
            }

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
