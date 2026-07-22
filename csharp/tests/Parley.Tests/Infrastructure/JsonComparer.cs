using System.Text.Json;
using System.Text.Json.Nodes;

namespace Parley.Tests.Infrastructure;

/// <summary>
/// Deep JSON equality up to object key order (protocol.md §10 exact-set matching): objects compare by
/// key set and values, arrays element-wise in order, numbers by value, everything else by content.
/// </summary>
internal static class JsonComparer
{
    public static bool Equal(JsonNode? a, JsonNode? b)
    {
        if (a is null && b is null)
        {
            return true;
        }

        if (a is null || b is null)
        {
            return false;
        }

        JsonValueKind ka = a.GetValueKind();
        JsonValueKind kb = b.GetValueKind();
        if (ka != kb)
        {
            return false;
        }

        switch (ka)
        {
            case JsonValueKind.Object:
                return ObjectsEqual(a.AsObject(), b.AsObject());
            case JsonValueKind.Array:
                return ArraysEqual(a.AsArray(), b.AsArray());
            case JsonValueKind.String:
                return a.GetValue<string>() == b.GetValue<string>();
            case JsonValueKind.Number:
                return NumbersEqual(a, b);
            case JsonValueKind.True:
            case JsonValueKind.False:
                return a.GetValue<bool>() == b.GetValue<bool>();
            case JsonValueKind.Null:
                return true;
            default:
                return false;
        }
    }

    private static bool ObjectsEqual(JsonObject a, JsonObject b)
    {
        if (a.Count != b.Count)
        {
            return false;
        }

        foreach (var kv in a)
        {
            if (!b.TryGetPropertyValue(kv.Key, out JsonNode? other))
            {
                return false;
            }

            if (!Equal(kv.Value, other))
            {
                return false;
            }
        }

        return true;
    }

    private static bool ArraysEqual(JsonArray a, JsonArray b)
    {
        if (a.Count != b.Count)
        {
            return false;
        }

        for (int i = 0; i < a.Count; i++)
        {
            if (!Equal(a[i], b[i]))
            {
                return false;
            }
        }

        return true;
    }

    private static bool NumbersEqual(JsonNode a, JsonNode b)
    {
        if (a.AsValue().TryGetValue(out decimal da) && b.AsValue().TryGetValue(out decimal db))
        {
            return da == db;
        }

        return a.GetValue<double>() == b.GetValue<double>();
    }
}
