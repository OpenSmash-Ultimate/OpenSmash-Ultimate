using System.IO.Compression;
using System.Net;
using System.Security.Cryptography;
using OpenSmash.Core;

static class BootstrapChecks
{
    public static async Task Run(string temporary, Action<bool,string> check)
    {
        string root=Path.Combine(temporary,"bootstrap-cases");Directory.CreateDirectory(root);
        string launcher=Path.Combine(root,"launcher.exe");File.WriteAllText(launcher,"synthetic launcher");
        byte[] Zip(string prefix="",string? bad=null,bool symlink=false,bool incomplete=false,bool legacy=false)
        {
            using var bytes=new MemoryStream();
            using(var zip=new ZipArchive(bytes,ZipArchiveMode.Create,true))
            {
                string[] files=incomplete?["readme.txt"]:["runtime/fixture.txt","tools/os_mods.py","tools/recomp.py","tools/install_windowsmash.ps1","tools/linux/install.sh","tools/mac/install.sh"];
                if(legacy)files=files.Select(f=>f.Replace("tools/linux/install.sh","install-windowsmash.sh").Replace("tools/mac/install.sh","Install WindowSmash.command")).ToArray();
                foreach(string file in files){var entry=zip.CreateEntry(prefix+file);using var writer=new StreamWriter(entry.Open());writer.Write("synthetic fixture");}
                if(bad!=null){var entry=zip.CreateEntry(bad);if(symlink)entry.ExternalAttributes=unchecked((int)0xa1ff0000);using var writer=new StreamWriter(entry.Open());writer.Write("outside");}
            }
            return bytes.ToArray();
        }
        BootstrapSource Source(byte[] zip)=>new("OpenSmash-Ultimate/OpenSmash-Ultimate",new string('a',40),"https://github.com/OpenSmash-Ultimate/OpenSmash-Ultimate/releases/download/test/OpenSmash-Source.zip",Convert.ToHexString(SHA256.HashData(zip)));
        var deployment=new LauncherDeployment(launcher,DesktopPlatform.Windows);
        int index=0;
        string Parent()=>Path.Combine(root,"case "+(++index)+" ' & spaces");
        foreach(var (prefix,legacy) in new[]{("",false),("OpenSmash-test/",false),("Legacy-source/",true)})
        {
            byte[] zip=Zip(prefix,legacy:legacy);var handler=new ArchiveHandler(zip);using var http=new HttpClient(handler);
            string parent=Parent();var progress=new List<BootstrapProgress>();
            var result=await new ProjectBootstrap(http).Install(parent,Source(zip),deployment,progress.Add,CancellationToken.None);
            check(result.ProjectFolder==Path.Combine(parent,"OpenSmash")&&Project.IsProject(result.ProjectFolder),"bootstrap accepts "+(prefix==""?"release":"GitHub")+" archive layout into the selected OpenSmash folder");
            check(File.ReadAllText(result.Launcher!)=="synthetic launcher"&&progress.Any(p=>p.Phase=="extract"),"bootstrap copies the launcher and reports real download/extract progress");
            check(File.Exists(Path.Combine(result.ProjectFolder,"tools","linux","install.sh"))&&File.Exists(Path.Combine(result.ProjectFolder,"tools","mac","install.sh")),"downloaded source uses internal setup scripts");
            check(!File.Exists(Path.Combine(result.ProjectFolder,"install-windowsmash.sh"))&&!File.Exists(Path.Combine(result.ProjectFolder,"Install WindowSmash.command")),"legacy downloaded setup wrappers are migrated out of the root");
            check(!Directory.GetDirectories(parent,".OpenSmash-download-*").Any(),"successful bootstrap cleans only its staging folder");
            var again=await new ProjectBootstrap(http).Install(parent,Source(zip),deployment,_=>{},CancellationToken.None);
            check(again.Existing&&handler.Requests==1,"existing projects are reused without another download");
        }
        async Task Rejected(byte[] zip,string title,bool wrongHash=false,Action<BootstrapProgress>? observe=null,CancellationToken ct=default)
        {
            string parent=Parent();using var http=new HttpClient(new ArchiveHandler(zip));bool rejected=false;
            try{var source=Source(zip);if(wrongHash)source=source with{Sha256=new string('0',64)};await new ProjectBootstrap(http).Install(parent,source,deployment,observe??(_=>{}),ct);}
            catch(Exception error)when(error is IOException or InvalidDataException or OperationCanceledException){rejected=true;}
            check(rejected&&!Directory.Exists(Path.Combine(parent,"OpenSmash"))&&!Directory.GetDirectories(parent,".OpenSmash-download-*").Any(),title);
        }
        await Rejected(Zip(),"checksum failures leave no installed project or partial download",wrongHash:true);
        await Rejected(Zip(incomplete:true),"incomplete archives do not become installations");
        await Rejected(Zip(bad:"../escape.txt"),"archive traversal cannot write outside staging");
        await Rejected(Zip(bad:"C:\\escape.txt"),"Windows rooted archive paths are rejected on every platform");
        await Rejected(Zip(bad:"linked",symlink:true),"archive symlinks are rejected before installation");
        {
            byte[] zip=Zip();using var http=new HttpClient(new ArchiveHandler(zip,unknownLength:true));var updates=new List<BootstrapProgress>();
            var result=await new ProjectBootstrap(http).Install(Parent(),Source(zip),deployment,updates.Add,CancellationToken.None);
            check(Project.IsProject(result.ProjectFolder)&&updates.Any(p=>p.Phase=="download"&&p.Total==-1),"downloads without Content-Length report bytes and still install correctly");
        }
        {
            string parent=Parent();byte[] zip=Zip();using var http=new HttpClient(new ArchiveHandler(zip,status:HttpStatusCode.ServiceUnavailable));bool failed=false;
            try{await new ProjectBootstrap(http).Install(parent,Source(zip),deployment,_=>{},CancellationToken.None);}catch(HttpRequestException){failed=true;}
            check(failed&&!Directory.Exists(Path.Combine(parent,"OpenSmash"))&&!Directory.GetDirectories(parent,".OpenSmash-download-*").Any(),"network failures clean staging and remain retryable");
        }
        using(var cancel=new CancellationTokenSource())
            await Rejected(Zip(),"cancelling extraction removes the partial download",observe:p=>{if(p.Phase=="extract")cancel.Cancel();},ct:cancel.Token);
        {
            string parent=Parent(),destination=Path.Combine(parent,"OpenSmash");Directory.CreateDirectory(destination);File.WriteAllText(Path.Combine(destination,"keep.txt"),"keep");
            byte[] zip=Zip();var handler=new ArchiveHandler(zip);using var http=new HttpClient(handler);bool rejected=false;
            try{await new ProjectBootstrap(http).Install(parent,Source(zip),deployment,_=>{},CancellationToken.None);}catch(IOException){rejected=true;}
            check(rejected&&handler.Requests==0&&File.ReadAllText(Path.Combine(destination,"keep.txt"))=="keep","unrelated nonempty destinations remain untouched and are never downloaded over");
        }
        {
            string source=Path.Combine(root,"Original.app","Contents","MacOS","OpenSmash");Directory.CreateDirectory(Path.GetDirectoryName(source)!);File.WriteAllText(source,"mac launcher");File.WriteAllText(Path.Combine(Path.GetDirectoryName(source)!,"library.dylib"),"native library");
            string resources=Path.Combine(root,"Original.app","Contents","Resources");Directory.CreateDirectory(resources);File.WriteAllText(Path.Combine(resources,"notice.txt"),"notice");
            string destination=Parent();Directory.CreateDirectory(destination);string installed=new LauncherDeployment(source,DesktopPlatform.MacOS).CopyInto(destination);
            check(File.Exists(installed)&&File.Exists(Path.Combine(Path.GetDirectoryName(installed)!,"library.dylib"))&&File.Exists(Path.Combine(destination,"OpenSmash.app","Contents","Resources","notice.txt")),"macOS handoff copies the complete app bundle, including native libraries and resources");
        }
    }

    sealed class ArchiveHandler(byte[] archive,bool unknownLength=false,HttpStatusCode status=HttpStatusCode.OK):HttpMessageHandler
    {
        public int Requests;
        protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request,CancellationToken cancellationToken)
        {Requests++;return Task.FromResult(new HttpResponseMessage(status){Content=unknownLength?new StreamContent(new ForwardStream(archive)):new ByteArrayContent(archive)});}
    }
    sealed class ForwardStream(byte[] bytes):MemoryStream(bytes) { public override bool CanSeek=>false; }
}
