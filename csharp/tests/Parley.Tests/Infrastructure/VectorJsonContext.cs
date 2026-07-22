using System.Text.Json;
using System.Text.Json.Serialization;

namespace Parley.Tests.Infrastructure;

/// <summary>
/// Source-generated metadata for the runner's payload bindings: raw <see cref="JsonElement"/> for
/// request/progress/result payloads (so any JSON round-trips), and <see cref="string"/> for the
/// custom error code.
/// </summary>
[JsonSerializable(typeof(JsonElement))]
[JsonSerializable(typeof(string))]
internal partial class VectorJsonContext : JsonSerializerContext
{
}
