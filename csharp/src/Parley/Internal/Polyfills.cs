#if NETSTANDARD2_0
namespace System.Runtime.CompilerServices
{
    // Enables the C# `init`-only setter and record positional members on netstandard2.0.
    internal static class IsExternalInit
    {
    }
}
#endif
