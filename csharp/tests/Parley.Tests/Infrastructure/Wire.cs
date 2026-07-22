using System.Text;

namespace Parley.Tests.Infrastructure;

/// <summary>Builds raw wire payloads for delivering scripted inbound messages in unit tests.</summary>
internal static class Wire
{
    public static byte[] Request(string id, string payloadJson = "{}") =>
        Utf8($"{{\"v\":1,\"t\":\"req\",\"id\":\"{id}\",\"p\":{payloadJson}}}");

    public static byte[] Heartbeat(string id) =>
        Utf8($"{{\"v\":1,\"t\":\"hb\",\"id\":\"{id}\"}}");

    public static byte[] Progress(string id, string payloadJson) =>
        Utf8($"{{\"v\":1,\"t\":\"prg\",\"id\":\"{id}\",\"p\":{payloadJson}}}");

    public static byte[] ResultOk(string id, string payloadJson) =>
        Utf8($"{{\"v\":1,\"t\":\"res\",\"id\":\"{id}\",\"ok\":true,\"p\":{payloadJson}}}");

    public static byte[] ResultError(string id, string code) =>
        Utf8($"{{\"v\":1,\"t\":\"res\",\"id\":\"{id}\",\"ok\":false,\"e\":\"{code}\"}}");

    public static byte[] ResultAck(string id) =>
        Utf8($"{{\"v\":1,\"t\":\"rak\",\"id\":\"{id}\"}}");

    public static byte[] Cancel(string id) =>
        Utf8($"{{\"v\":1,\"t\":\"cnl\",\"id\":\"{id}\"}}");

    private static byte[] Utf8(string s) => Encoding.UTF8.GetBytes(s);
}
