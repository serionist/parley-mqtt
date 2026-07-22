using System;
using System.Security.Cryptography;

namespace Parley
{
    /// <summary>
    /// Produces command ids (protocol.md §4.2): opaque strings of 1–64 characters from
    /// <c>[A-Za-z0-9_-]</c>, unique per topic set for as long as any peer might hold state.
    /// Injectable so constrained devices can supply a cheaper source.
    /// </summary>
    public interface IParleyCommandIdGenerator
    {
        /// <summary>Returns a fresh command id.</summary>
        /// <returns>A valid command id.</returns>
        string NewCommandId();
    }

    /// <summary>
    /// The default generator: 128 bits of cryptographic randomness rendered as URL-safe Base64
    /// (22 characters from <c>[A-Za-z0-9_-]</c>).
    /// </summary>
    public sealed class ParleyRandomCommandIdGenerator : IParleyCommandIdGenerator
    {
        /// <summary>A shared instance.</summary>
        public static readonly ParleyRandomCommandIdGenerator Instance = new();

        /// <inheritdoc />
        public string NewCommandId()
        {
            var bytes = new byte[16];
#if NETSTANDARD2_0
            using (var rng = RandomNumberGenerator.Create())
            {
                rng.GetBytes(bytes);
            }
#else
            RandomNumberGenerator.Fill(bytes);
#endif
            // URL-safe Base64 without padding: characters are all within [A-Za-z0-9_-].
            var s = Convert.ToBase64String(bytes);
            return s.Replace('+', '-').Replace('/', '_').TrimEnd('=');
        }
    }
}
