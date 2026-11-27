using System.Runtime.InteropServices;

namespace FFAudioSharp.Interop;

internal static unsafe class Util
{
    /// <summary>A C string as a string; "" for NULL (the library never hands out NULL where text is promised).</summary>
    public static string Str(byte* p) => p == null ? "" : Marshal.PtrToStringUTF8((nint)p) ?? "";

    /// <summary>A C string, null for NULL.</summary>
    public static string? StrOrNull(byte* p) => p == null ? null : Marshal.PtrToStringUTF8((nint)p);

    public static bool B(byte b) => b != 0;
    public static byte B(bool b) => b ? (byte)1 : (byte)0;

    /// <summary>Throws for a negative AU_RESULT; the number otherwise.</summary>
    public static int Check(int result, string? what = null)
    {
        if (result < 0) throw new FFAudioException((ResultCode)result, what);
        return result;
    }

    /// <summary>An array of C strings for the call, freed by Dispose.</summary>
    public sealed class Utf8Array : IDisposable
    {
        private readonly nint[] _ptrs;
        public Utf8Array(IReadOnlyCollection<string> strings)
        {
            _ptrs = new nint[strings.Count];
            var i = 0;
            foreach (var s in strings) _ptrs[i++] = Marshal.StringToCoTaskMemUTF8(s);
        }
        public int Count => _ptrs.Length;
        public nint[] Pointers => _ptrs;
        public void Dispose()
        {
            foreach (var p in _ptrs) Marshal.FreeCoTaskMem(p);
        }
    }
}
