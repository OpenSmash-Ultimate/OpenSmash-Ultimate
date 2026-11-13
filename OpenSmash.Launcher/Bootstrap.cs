using System.Diagnostics;
using System.Net.Http;
using Avalonia.Platform.Storage;
using Avalonia.Threading;
using OpenSmash.Core;

namespace OpenSmash;

public sealed partial class MainWindow
{
    static readonly HttpClient BootstrapHttp = new() { Timeout = TimeSpan.FromMinutes(30) };
    CancellationTokenSource? _bootstrapCancel;
    bool _closeAfterBootstrap;

    static string DefaultBootstrapParent() => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "Games");

    async Task ChooseBootstrapParent()
    {
        if (_bootstrapCancel != null) return;
        var picked = await StorageProvider.OpenFolderPickerAsync(new FolderPickerOpenOptions { Title = "Choose where to create the OpenSmash folder", AllowMultiple = false });
        if (picked.FirstOrDefault()?.TryGetLocalPath() is string parent) Surface.SetBootstrapParent(parent);
    }

    async Task DownloadProject()
    {
        if (_bootstrapCancel != null) return;
        _bootstrapCancel = new CancellationTokenSource(); Surface.StartBootstrap();
        try
        {
            var result = await new ProjectBootstrap(BootstrapHttp).Install(Surface.BootstrapParent, BootstrapSource.Bundled(), LauncherDeployment.Current,
                progress => Dispatcher.UIThread.Post(() => Surface.BootstrapProgress(progress)), _bootstrapCancel.Token);
            Surface.FinishBootstrap(); SetProject(result.ProjectFolder);
            // Continue from the installed copy. Existing projects remain in this window.
            if (result.Launcher is { } executable)
            {
                try
                {
                    var start = new ProcessStartInfo(executable) { WorkingDirectory = result.ProjectFolder, UseShellExecute = false };
                    start.ArgumentList.Add("--project"); start.ArgumentList.Add(result.ProjectFolder);
                    using var child = Process.Start(start) ?? throw new IOException("The installed launcher did not start.");
                    _closeAfterBootstrap = true;
                }
                catch (Exception error) { Surface.Alert("OPENSMASH IS READY", "The project was installed. Setup can continue in this window.\n\n" + error.Message); }
            }
        }
        catch (OperationCanceledException) { Surface.FinishBootstrap("Download cancelled. No existing project files were changed."); }
        catch (Exception error) { if(Surface.IsFirstRun)Surface.FinishBootstrap(error.Message);else Surface.Alert("SETUP",error.Message); }
        finally
        {
            _bootstrapCancel.Dispose(); _bootstrapCancel = null;
            if (_closeAfterBootstrap) Close();
        }
    }
}
