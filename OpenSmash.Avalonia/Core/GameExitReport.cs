namespace OpenSmash.Core;

public static class GameExitReport
{
    public static string Describe(int exitCode, string logPath)
    {
        string reason = unchecked((uint)exitCode) switch
        {
            0xC0000005 => "Windows reported an access violation (invalid memory access).",
            0xC000001D => "Windows reported an illegal CPU instruction.",
            0xC00000FD => "Windows reported a stack overflow.",
            0xC0000409 => "Windows reported a fail-fast termination.",
            _ => "The game did not report a specific cause."
        };
        try
        {
            using var input = new FileStream(logPath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            input.Seek(Math.Max(0, input.Length - 16384), SeekOrigin.Begin);
            using var reader = new StreamReader(input);
            string? error = reader.ReadToEnd().Split('\n').LastOrDefault(line =>
                line.Contains("fatal", StringComparison.OrdinalIgnoreCase) ||
                line.Contains("exception", StringComparison.OrdinalIgnoreCase) ||
                line.Contains("[error]", StringComparison.OrdinalIgnoreCase));
            if (!string.IsNullOrWhiteSpace(error)) reason += " Last error logged: " + error.Trim()[..Math.Min(error.Trim().Length, 240)];
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
        return $"{reason} Exit code: {exitCode} (0x{unchecked((uint)exitCode):X8}). Open the logs for more details.";
    }
}
