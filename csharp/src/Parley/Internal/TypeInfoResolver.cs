using System;
using System.Text.Json.Serialization;
using System.Text.Json.Serialization.Metadata;

namespace Parley.Internal
{
    /// <summary>Resolves strongly-typed source-generated metadata from a <see cref="JsonSerializerContext"/>.</summary>
    internal static class TypeInfoResolver
    {
        internal static JsonTypeInfo<T> Resolve<T>(JsonSerializerContext context)
        {
            JsonTypeInfo? info = context.GetTypeInfo(typeof(T));
            if (info is JsonTypeInfo<T> typed)
            {
                return typed;
            }

            throw new ArgumentException(
                $"The supplied JsonSerializerContext does not provide source-generated metadata for '{typeof(T)}'. " +
                "Add a [JsonSerializable(typeof(...))] attribute for it.",
                nameof(context));
        }
    }
}
