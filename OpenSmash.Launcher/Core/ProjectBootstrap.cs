using System.Diagnostics;
using System.IO.Compression;
using System.Net.Http;
using System.Security.Cryptography;

namespace OpenSmash.Core;

public sealed record BootstrapProgress(string Phase, long Done, long Total);
public sealed record BootstrapResult(string ProjectFolder, string? Launcher, bool Existing);

public sealed class ProjectBootstrap(HttpClient client)
{
    const long MaxDownload = 4L << 30, MaxExpanded = 32L << 30;
    public static string Destination(string parent) => Path.GetFullPath(Path.Combine(parent, "OpenSmash"));

    public async Task<BootstrapResult> Install(string parent, BootstrapSource source, LauncherDeployment launcher,
        Action<BootstrapProgress> progress, CancellationToken cancellation)
    {
        source.Validate();
        string destination = Destination(parent);
        if (Project.IsProject(destination)) return new(destination, null, true);
        CheckDestination(destination);
        Directory.CreateDirectory(Path.GetDirectoryName(destination)!);
        string work = Path.Combine(Path.GetDirectoryName(destination)!, ".OpenSmash-download-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(work);
        try
        {
            string archive = Path.Combine(work, "source.zip");
            await Download(source, archive, progress, cancellation);
            string extract = Path.Combine(work, "extract");
            string root = await Task.Run(() => Extract(archive, extract, progress, cancellation), cancellation);
            cancellation.ThrowIfCancellationRequested();
            progress(new("launcher", 0, 1));
            string executable = launcher.CopyInto(root);
            string relativeLauncher = Path.GetRelativePath(root, executable);
            cancellation.ThrowIfCancellationRequested();
            CheckDestination(destination);
            // Only an empty destination is removed, immediately before the atomic rename.
            if (Directory.Exists(destination)) Directory.Delete(destination, recursive:false);
            Directory.Move(root, destination);
            progress(new("complete", 1, 1));
            return new(destination, Path.Combine(destination, relativeLauncher), false);
        }
        finally
        {
            // work is a fresh GUID child of the user-selected parent; never clean the destination.
            string expectedParent = Path.GetFullPath(Path.GetDirectoryName(destination)!) + Path.DirectorySeparatorChar;
            if (Path.GetFullPath(work).StartsWith(expectedParent, OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal) && Path.GetFileName(work).StartsWith(".OpenSmash-download-", StringComparison.Ordinal))
                try { Directory.Delete(work, recursive:true); } catch (IOException error) { Trace.WriteLine(error); } catch (UnauthorizedAccessException error) { Trace.WriteLine(error); }
        }
    }

    static void CheckDestination(string destination)
    {
        if (File.Exists(destination) || Directory.Exists(destination) &&
            ((File.GetAttributes(destination) & FileAttributes.ReparsePoint) != 0 || Directory.EnumerateFileSystemEntries(destination).Any()))
            throw new IOException("An OpenSmash folder with other files already exists here. Choose another location, or use an existing OpenSmash project.");
    }

    async Task Download(BootstrapSource source, string archive, Action<BootstrapProgress> progress, CancellationToken ct)
    {
        using var request = new HttpRequestMessage(HttpMethod.Get, source.ArchiveUrl);
        request.Headers.UserAgent.ParseAdd("OpenSmash-Setup/1.0");
        using var response = await client.SendAsync(request, HttpCompletionOption.ResponseHeadersRead, ct);
        response.EnsureSuccessStatusCode();
        long total = response.Content.Headers.ContentLength ?? -1;
        if (total > MaxDownload) throw new InvalidDataException("The project download is unexpectedly large.");
        await using var input = await response.Content.ReadAsStreamAsync(ct);
        await using var output = new FileStream(archive, FileMode.CreateNew, FileAccess.Write, FileShare.None, 81920, true);
        using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        byte[] buffer = new byte[81920]; long done = 0; var clock = Stopwatch.StartNew();
        progress(new("download", 0, total));
        int read;
        while ((read = await input.ReadAsync(buffer, ct)) > 0)
        {
            done += read;
            if (done > MaxDownload) throw new InvalidDataException("The project download is unexpectedly large.");
            await output.WriteAsync(buffer.AsMemory(0, read), ct); hash.AppendData(buffer.AsSpan(0, read));
            if (clock.ElapsedMilliseconds >= 100) { progress(new("download", done, total)); clock.Restart(); }
        }
        if (total >= 0 && total != done) throw new InvalidDataException("The download was interrupted. Try again.");
        progress(new("verify", done, done));
        string actual = Convert.ToHexString(hash.GetHashAndReset());
        if (source.Sha256.Length > 0 && !actual.Equals(source.Sha256, StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException("The download does not match this release's checksum. Nothing was installed. Try again.");
    }

    static string Extract(string archive, string destination, Action<BootstrapProgress> progress, CancellationToken ct)
    {
        Directory.CreateDirectory(destination);
        string boundary = Path.GetFullPath(destination) + Path.DirectorySeparatorChar;
        var comparison = OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal;
        using var zip = ZipFile.OpenRead(archive);
        if (zip.Entries.Count > 250_000) throw new InvalidDataException("The archive contains too many files.");
        long expanded = 0, done = 0; var clock = Stopwatch.StartNew();
        progress(new("extract", 0, zip.Entries.Count));
        foreach (var entry in zip.Entries)
        {
            ct.ThrowIfCancellationRequested();
            string name = entry.FullName.Replace('\\', '/');
            if (name.StartsWith('/') || name.Split('/').Any(p => p == ".." || p.Contains(':')) || ((entry.ExternalAttributes >> 16) & 0xf000) == 0xa000 || (entry.ExternalAttributes & (int)FileAttributes.ReparsePoint) != 0)
                throw new InvalidDataException("The download contains an unsafe archive entry.");
            string path = Path.GetFullPath(Path.Combine(destination, name));
            if (!path.StartsWith(boundary, comparison)) throw new InvalidDataException("The download contains an unsafe archive path.");
            expanded = checked(expanded + entry.Length);
            if (expanded > MaxExpanded) throw new InvalidDataException("The expanded project is unexpectedly large.");
            if (name.EndsWith('/')) Directory.CreateDirectory(path);
            else
            {
                Directory.CreateDirectory(Path.GetDirectoryName(path)!);
                using var input = entry.Open();
                using var output = new FileStream(path, FileMode.CreateNew, FileAccess.Write, FileShare.None);
                byte[] buffer = new byte[81920]; int count;
                while ((count = input.Read(buffer, 0, buffer.Length)) > 0) { ct.ThrowIfCancellationRequested(); output.Write(buffer, 0, count); }
                if (!OperatingSystem.IsWindows())
                {
                    var mode = (UnixFileMode)((entry.ExternalAttributes >> 16) & 0x1ff);
                    if (mode != 0) File.SetUnixFileMode(path, mode);
                }
            }
            done++;
            if (clock.ElapsedMilliseconds >= 100) { progress(new("extract", done, zip.Entries.Count)); clock.Restart(); }
        }
        progress(new("extract", done, zip.Entries.Count));
        string root = Project.IsProject(destination) ? destination : Directory.GetDirectories(destination) is [var only] && Directory.GetFiles(destination).Length == 0 ? only : "";
        if(root!=""&&Project.IsProject(root)&&File.Exists(Path.Combine(root,"tools","recomp.py"))&&File.Exists(Path.Combine(root,"tools","install_windowsmash.ps1")))ProjectLayout.Normalize(root);
        if (root == "" || !Project.IsProject(root) || !File.Exists(Path.Combine(root, "tools", "recomp.py")) || !File.Exists(Path.Combine(root, "tools", "install_windowsmash.ps1")) || !File.Exists(Path.Combine(root, "tools/linux/install.sh")) || !File.Exists(Path.Combine(root, "tools/mac/install.sh")))
            throw new InvalidDataException("The download does not contain a complete OpenSmash project. Nothing was installed.");
        return root;
    }
}
