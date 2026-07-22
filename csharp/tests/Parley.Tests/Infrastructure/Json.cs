using System.Text.Json;

namespace Parley.Tests.Infrastructure;

/// <summary>Small helpers for building test payloads.</summary>
internal static class Json
{
    public static JsonElement Element(string json)
    {
        using JsonDocument document = JsonDocument.Parse(json);
        return document.RootElement.Clone();
    }

    public static JsonElement ToElement(this string json) => Element(json);
}
