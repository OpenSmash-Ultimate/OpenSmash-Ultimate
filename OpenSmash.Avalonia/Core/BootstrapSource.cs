using System.Text.Json;
using System.Text.RegularExpressions;

namespace OpenSmash.Core;

public sealed record BootstrapSource(string Repository, string Revision, string ArchiveUrl, string Sha256 = "")
{
    public static BootstrapSource Bundled()
    {
        using var stream = typeof(BootstrapSource).Assembly.GetManifestResourceStream("BootstrapSource.json")!;
        var source = JsonSerializer.Deserialize<BootstrapSource>(stream) ?? throw new InvalidDataException("Release download information is missing.");
        source.Validate(); return source;
    }

    public void Validate()
    {
        if (!Regex.IsMatch(Repository, @"^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$") || !Regex.IsMatch(Revision, @"^[a-fA-F0-9]{40,64}$"))
            throw new InvalidDataException("The release has invalid source version information.");
        if (!Uri.TryCreate(ArchiveUrl, UriKind.Absolute, out var uri) || uri.Scheme != "https" ||
            uri.Host is not ("github.com" or "codeload.github.com") ||
            !uri.AbsolutePath.StartsWith("/" + Repository + "/", StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException("The release download must come from its GitHub repository.");
        if (Sha256.Length > 0 && !Regex.IsMatch(Sha256, @"^[a-fA-F0-9]{64}$"))
            throw new InvalidDataException("The release source checksum is invalid.");
    }
}
