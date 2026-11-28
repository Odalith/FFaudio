using System.Diagnostics;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using FFAudioSharp.Interop;

namespace FFAudioSharp.Tests;

/// <summary>
/// The raw layer against the C compiler: every struct's size and every field's offset as bindings/python/tests/layout.c
/// prints them (CTest builds it as ffaudio_layout and sets FFAUDIO_LAYOUT), and every exported function against
/// abi/ffaudio.symbols. A misplaced field would read garbage without any error.
/// </summary>
public class LayoutTests
{
    private static readonly Assembly Raw = typeof(Lib).Assembly;

    private static string[] RunLayout()
    {
        var exe = Environment.GetEnvironmentVariable("FFAUDIO_LAYOUT")
                  ?? throw new InvalidOperationException("FFAUDIO_LAYOUT is not set: the path of the ffaudio_layout program (CMake builds it)");
        var p = Process.Start(new ProcessStartInfo(exe) { RedirectStandardOutput = true, UseShellExecute = false })!;
        var text = p.StandardOutput.ReadToEnd();
        p.WaitForExit();
        Assert.Equal(0, p.ExitCode);
        return text.Split('\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
    }

    private static Type TypeOf(string name) =>
        Raw.GetType("FFAudioSharp.Interop." + name) ?? throw new Xunit.Sdk.XunitException($"no raw struct {name}");

    private static int SizeOf(Type t) => (int)typeof(Unsafe).GetMethod(nameof(Unsafe.SizeOf))!.MakeGenericMethod(t).Invoke(null, null)!;

    [Fact]
    public void Every_struct_and_field_is_where_the_c_compiler_puts_it()
    {
        var lines = RunLayout();
        Assert.True(lines.Length > 100, "layout program printed too little");
        var errors = new List<string>();
        foreach (var line in lines)
        {
            var space = line.LastIndexOf(' ');
            var path = line[..space];
            var expected = int.Parse(line[(space + 1)..]);
            var parts = path.Split('.');
            var type = TypeOf(parts[0]);
            int actual;
            if (parts.Length == 1)
            {
                actual = SizeOf(type);
            }
            else
            {
                actual = 0;
                for (var i = 1; i < parts.Length; i++)
                {
                    var field = type.GetField(parts[i], BindingFlags.Public | BindingFlags.Instance);
                    if (field == null) { errors.Add($"{path}: no such field {parts[i]} in {type.Name}"); actual = -1; break; }
                    actual += (int)Marshal.OffsetOf(type, parts[i]);
                    type = field.FieldType;
                }
            }
            if (actual >= 0 && actual != expected) errors.Add($"{path}: C says {expected}, C# has {actual}");
        }
        Assert.True(errors.Count == 0, string.Join("\n", errors));
    }

    [Fact]
    public void Every_exported_function_is_declared_and_nothing_else_is()
    {
        // CMake passes the path; in a checkout built in place (dotnet test), it is found above the test's folder
        var file = Environment.GetEnvironmentVariable("FFAUDIO_SYMBOLS");
        for (var d = new DirectoryInfo(AppContext.BaseDirectory); string.IsNullOrEmpty(file) && d != null; d = d.Parent)
        {
            var candidate = Path.Combine(d.FullName, "abi", "ffaudio.symbols");
            if (File.Exists(candidate)) file = candidate;
        }
        if (string.IsNullOrEmpty(file)) throw new FileNotFoundException("abi/ffaudio.symbols: set FFAUDIO_SYMBOLS");
        var exported = File.ReadAllLines(file).Select(l => l.Trim()).Where(l => l.Length > 0 && !l.StartsWith('#')).ToHashSet();
        var declared = typeof(Lib).GetMethods(BindingFlags.Public | BindingFlags.Static).Select(m => m.Name).Where(n => n.StartsWith("au_")).ToHashSet();
        Assert.Empty(exported.Except(declared));
        Assert.Empty(declared.Except(exported));
    }
}
