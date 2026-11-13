using System.Diagnostics;

namespace OpenSmash.Core;

public static class ProcessRunner
{
    public static async Task<int> Run(Command command, Action<string> line, CancellationToken cancellation,
        bool game = false, string? logPath = null, Action<StreamWriter>? inputReady = null,
        IReadOnlyDictionary<string,string>? environment = null)
    {
        var info = command.StartInfo(capture: true);
        if (game) foreach (string name in Commands.ClearedGameVariables) info.Environment.Remove(name);
        if(environment!=null)foreach(var entry in environment)info.Environment[entry.Key]=entry.Value;
        using var process = new Process { StartInfo = info };
        if(logPath!=null)Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(logPath))!);
        using var log = logPath == null ? null : new StreamWriter(logPath, false) { AutoFlush = true };
        object gate = new();
        if (!process.Start()) throw new InvalidOperationException("The process could not start.");
        if(inputReady==null) process.StandardInput.Close();
        else {process.StandardInput.AutoFlush=true;inputReady(process.StandardInput);}
        using var registration = cancellation.Register(() => { try { process.Kill(entireProcessTree: true); } catch (InvalidOperationException) { } });
        async Task Read(StreamReader reader)
        {
            while (await reader.ReadLineAsync() is string text)
            {
                lock (gate) { log?.WriteLine(text); line(text); }
            }
        }
        await Task.WhenAll(Read(process.StandardOutput), Read(process.StandardError), process.WaitForExitAsync());
        cancellation.ThrowIfCancellationRequested();
        return process.ExitCode;
    }
}
