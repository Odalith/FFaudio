// Plays files, .m3u/.m3u8/.pls playlists and .cue sheets (or a file's chapters) one after the other, gapless or
// crossfaded, with a level meter. The next track is queued when the player asks for it (PrepareNextEvent).
//
//   FFAUDIO_LIBRARY=/path/to/libffaudio.so.4 dotnet run -- album.cue
//   FFAUDIO_LIBRARY=... dotnet run -- --crossfade 3 --device "Speakers" mix.m3u8 song.flac
//
// Options: --crossfade <seconds>, --device <name>, --list-devices

using FFAudioSharp;

var queue = new Queue<(string Path, PlayOptions? Options, string Name)>();
var crossfade = 0.0;
string? device = null;

for (var i = 0; i < args.Length; i++)
{
    switch (args[i])
    {
        case "--crossfade": crossfade = double.Parse(args[++i]); break;
        case "--device": device = args[++i]; break;
        case "--list-devices":
            foreach (var name in FFAudio.ListDevices()) Console.WriteLine(name);
            return 0;
        default: Add(args[i]); break;
    }
}

if (queue.Count == 0)
{
    Console.WriteLine("usage: PlaylistExample [--crossfade s] [--device name] [--list-devices] <files, playlists, cue sheets...>");
    return 1;
}

void Add(string path)
{
    var ext = Path.GetExtension(path).ToLowerInvariant();
    if (ext is ".m3u" or ".m3u8" or ".pls")
    {
        foreach (var e in PlaylistFile.Read(path))
            queue.Enqueue((e.Path, null, e.Title.Length > 0 ? e.Title : e.Path));
    }
    else if (ext == ".cue" || !FFAudio.IsNetworkUrl(path))
    {
        // A cue sheet, or an audio file that has its own (embedded) or chapters. Each track is a range of one
        // file; consecutive ranges join to the sample.
        try
        {
            foreach (var t in CueSheet.Read(path).Tracks)
                queue.Enqueue((t.Path, t.ToPlayOptions(), $"{t.Number:00} {t.Performer} - {t.Title}"));
        }
        catch (FFAudioException) when (ext != ".cue")
        {
            // A plain audio file (NothingToDo), or one that can not be read: the player reports why when it gets to it
            queue.Enqueue((path, null, path));
        }
    }
    else
    {
        queue.Enqueue((path, null, path));       // a URL
    }
}

using var cancel = new CancellationTokenSource();
Console.CancelKeyPress += (_, e) => { e.Cancel = true; cancel.Cancel(); };

FFAudio.Init("PlaylistExample");
if (device != null) FFAudio.SetDevice(device);
if (crossfade > 0)
    FFAudio.SetCrossfade(new Crossfade { OnTrackEnd = new Fade { Seconds = crossfade }, OnSkip = new Fade { Seconds = 0.5 } });

var names = new Dictionary<int, string>();   // handle -> what to call it

void Start(bool first)
{
    if (!queue.TryDequeue(out var next)) return;
    var h = first ? FFAudio.Play(next.Path, next.Options) : FFAudio.Queue(next.Path, next.Options);
    names[h] = next.Name;
}

Start(first: true);

// A level meter, ten times a second (the meters turn on with the first call)
var meter = Task.Run(async () =>
{
    while (!cancel.IsCancellationRequested)
    {
        var s = FFAudio.GetStatus();
        if (s.Handle != 0)
        {
            var peak = FFAudio.GetLevels(0.05).Peak.DefaultIfEmpty(0).Max();
            var bar = new string('#', (int)Math.Clamp(peak * 30, 0, 30));
            Console.Write($"\r  {s.Position,6:F1} s  {bar,-30}  ");
        }
        await Task.Delay(100);
    }
});

var remaining = 0;
await foreach (var e in FFAudio.ReadEventsAsync(cancel.Token))
{
    switch (e)
    {
        case TrackStartedEvent started:
            Console.WriteLine($"\n> {names.GetValueOrDefault(started.Handle, "?")}");
            break;
        case PrepareNextEvent:
            Start(first: false);   // gapless, or crossfaded, into the next one
            break;
        case TrackEndedEvent ended:
            if (ended.Reason == EndReason.Error)
                Console.WriteLine($"\n! {names.GetValueOrDefault(ended.Handle, "?")}: {ended.Message}");
            names.Remove(ended.Handle);
            // Done when nothing is left to play: none of our tracks is playing or queued (a failed last track too)
            if (queue.Count == 0 && names.Count == 0) cancel.Cancel();
            break;
    }
    remaining = queue.Count;
}

await meter;
FFAudio.Shutdown();
Console.WriteLine($"\n{remaining} not played");
return 0;
