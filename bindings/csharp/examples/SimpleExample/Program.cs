// Plays one file or URL and shows what the player reports.
//
//   FFAUDIO_LIBRARY=/path/to/libffaudio.so.4 dotnet run -- song.flac [start_seconds]
//
// (With a package that bundles the library, FFAUDIO_LIBRARY is not needed.)

using FFAudioSharp;

if (args.Length == 0)
{
    Console.WriteLine("usage: SimpleExample <file or url> [start_seconds]");
    return 1;
}

using var cancel = new CancellationTokenSource();
Console.CancelKeyPress += (_, e) => { e.Cancel = true; cancel.Cancel(); };

FFAudio.Init("SimpleExample");
FFAudio.SetVolume(0.8f);
FFAudio.SetReplayGain(new ReplayGainSettings { Mode = ReplayGainMode.Track, PreventClipping = true });

var options = new PlayOptions { StartSeconds = args.Length > 1 ? double.Parse(args[1]) : 0 };
var handle = FFAudio.Play(args[0], options);
Console.WriteLine($"ffaudio {FFAudio.Version}: playing {args[0]}  (Ctrl+C stops)");

// The position, twice a second, while the events are read below
var ticker = Task.Run(async () =>
{
    while (!cancel.IsCancellationRequested)
    {
        var s = FFAudio.GetStatus();
        if (s.Handle != 0) Console.Write($"\r  {s.Position,7:F1} / {s.Duration:F1} s   ");
        await Task.Delay(500);
    }
});

var exitCode = 0;
await foreach (var e in FFAudio.ReadEventsAsync(cancel.Token))
{
    switch (e)
    {
        case DeviceEvent { Ok: false } d:
            Console.WriteLine($"\ncould not open an output device ({d.Name ?? "default"})");
            exitCode = 1;
            cancel.Cancel();
            break;
        case TrackOpenedEvent opened when opened.Handle == handle:
            var info = FFAudio.TrackInfo(handle)!;
            Console.WriteLine($"\n  {info.Artist} - {info.Title}  [{info.Codec}, {info.SampleRate} Hz, {info.Channels} ch]");
            break;
        case TrackEndedEvent ended when ended.Handle == handle:
            if (ended.Reason == EndReason.Error)
            {
                Console.WriteLine($"\nfailed: {ended.Message} ({ended.Error})");
                exitCode = 1;
            }
            cancel.Cancel();
            break;
        case LogEvent { Level: >= LogLevel.Warning } log:
            Console.WriteLine($"\n[{log.Level}] {log.Message}");
            break;
    }
}

await ticker;
FFAudio.Shutdown();
Console.WriteLine();
return exitCode;
