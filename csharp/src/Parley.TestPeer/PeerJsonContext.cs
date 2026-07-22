using System.Text.Json;
using System.Text.Json.Serialization;

namespace Parley.TestPeer
{
    // Source-generated metadata for the peer's payload bindings: raw JsonElement for
    // request/progress/result (so ANY JSON round-trips verbatim) and string for the custom error code.
    // Mirrors the frozen vector runner's VectorJsonContext (csharp/tests).
    [JsonSerializable(typeof(JsonElement))]
    [JsonSerializable(typeof(string))]
    internal partial class PeerJsonContext : JsonSerializerContext
    {
    }
}
