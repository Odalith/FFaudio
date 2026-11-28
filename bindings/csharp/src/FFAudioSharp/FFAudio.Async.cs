using System.Runtime.CompilerServices;

namespace FFAudioSharp;

// Not in FFAudio.cs: that part is an unsafe context, where async methods are not allowed.
public static partial class FFAudio
{
    /// <summary>
    /// The events as they come, until cancelled or <see cref="Shutdown"/>:
    /// <c>await foreach (var e in FFAudio.ReadEventsAsync(ct))</c>. Only without a handler passed to <see cref="Init"/>.
    /// </summary>
    public static async IAsyncEnumerable<Event> ReadEventsAsync([EnumeratorCancellation] CancellationToken cancellationToken = default)
    {
        while (_initialized && !cancellationToken.IsCancellationRequested)
        {
            var e = await Task.Run(() => WaitEvent(TimeSpan.FromMilliseconds(250)), CancellationToken.None).ConfigureAwait(false);
            if (e != null) yield return e;
        }
    }
}
