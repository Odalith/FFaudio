// Finding libffaudio. .NET's own probing would look for "ffaudio.dll" and "libffaudio.so", but the library is called
// libffaudio.dll on Windows and its file in the package is libffaudio.so.4 (the soname). This resolver tries, in order:
//   1. FFAUDIO_LIBRARY, the path of a libffaudio of your own (a build from source, a system install);
//   2. the package's runtimes/<rid>/native folder, by every name the library goes by;
//   3. the system's search path (an installed ffaudio).
// A library of another major version is skipped: the structs would not match.

using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;

namespace FFAudioSharp.Interop;

internal static unsafe partial class Lib
{
    /// <summary>The major version of the C API these declarations are for (the soname: libffaudio.so.4).</summary>
    public const int AbiMajor = 4;

    static Lib()
    {
        NativeLibrary.SetDllImportResolver(typeof(Lib).Assembly, Resolve);
    }

    private static IntPtr _handle;

    private static IntPtr Resolve(string name, Assembly assembly, DllImportSearchPath? searchPath)
    {
        if (name != Name) return IntPtr.Zero;
        lock (typeof(Lib))
        {
            if (_handle != IntPtr.Zero) return _handle;
            var tried = new StringBuilder();
            foreach (var candidate in Candidates())
            {
                var found = Open(candidate, assembly, searchPath, tried);
                if (found != IntPtr.Zero) return _handle = found;
            }
            throw new DllNotFoundException(
                $"libffaudio (version {AbiMajor}) was not found. Set FFAUDIO_LIBRARY to its path, or use the " +
                $"Odalith.FFAudioSharp package, which has it for Windows and Linux (x64).{tried}");
        }
    }

    private static IEnumerable<string> Candidates()
    {
        var env = Environment.GetEnvironmentVariable("FFAUDIO_LIBRARY");
        if (!string.IsNullOrEmpty(env))
        {
            yield return env;   // a path of your own: nothing else is tried, so a wrong one is noticed
            yield break;
        }

        string[] names = OperatingSystem.IsWindows() ? ["libffaudio.dll", $"libffaudio-{AbiMajor}.dll", "ffaudio.dll"]
                       : OperatingSystem.IsMacOS()   ? [$"libffaudio.{AbiMajor}.dylib", "libffaudio.dylib"]
                       :                               [$"libffaudio.so.{AbiMajor}", "libffaudio.so"];
        var rid = RuntimeInformation.RuntimeIdentifier;   // e.g. linux-x64, win-x64, or a distro-specific one
        var portable = (OperatingSystem.IsWindows() ? "win" : OperatingSystem.IsMacOS() ? "osx" : "linux") + "-" +
                       RuntimeInformation.ProcessArchitecture.ToString().ToLowerInvariant();
        foreach (var r in new[] { portable, rid })
            foreach (var n in names)
                yield return Path.Combine(AppContext.BaseDirectory, "runtimes", r, "native", n);
        foreach (var n in names) yield return Path.Combine(AppContext.BaseDirectory, n);
        foreach (var n in names) yield return n;   // the system's search path
    }

    private static IntPtr Open(string candidate, Assembly assembly, DllImportSearchPath? searchPath, StringBuilder tried)
    {
        var isPath = Path.IsPathRooted(candidate);
        if (isPath && !File.Exists(candidate))
        {
            if (candidate == Environment.GetEnvironmentVariable("FFAUDIO_LIBRARY")) tried.Append($"\n  {candidate}: no such file (FFAUDIO_LIBRARY)");
            return IntPtr.Zero;
        }
        IntPtr h;
        try
        {
            // A path is loaded as it is (Windows then looks for its DLLs next to it); a bare name goes through the runtime's probing
            if (!(isPath ? NativeLibrary.TryLoad(candidate, out h) : NativeLibrary.TryLoad(candidate, assembly, searchPath, out h)))
            {
                tried.Append($"\n  {candidate}: could not be loaded (a dependency may be missing: FFmpeg, SDL2)");
                return IntPtr.Zero;
            }
        }
        catch (Exception e) when (e is BadImageFormatException or DllNotFoundException)
        {
            tried.Append($"\n  {candidate}: {e.Message}");
            return IntPtr.Zero;
        }

        if (NativeLibrary.TryGetExport(h, "au_get_version", out var export))
        {
            var version = Marshal.PtrToStringUTF8((nint)((delegate* unmanaged<byte*>)export)()) ?? "";
            if (version.StartsWith(AbiMajor + ".", StringComparison.Ordinal)) return h;
            tried.Append($"\n  {candidate}: is version {version}, not {AbiMajor}");
        }
        else
        {
            tried.Append($"\n  {candidate}: is not libffaudio");
        }
        NativeLibrary.Free(h);
        return IntPtr.Zero;
    }
}
