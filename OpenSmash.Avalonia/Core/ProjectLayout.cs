namespace OpenSmash.Core;





// Older pinned source archives use root-level shell entry points. Keep those


// archives installable while giving every platform the same internal layout.


public static class ProjectLayout


{


    static readonly (string Old,string Current,string Resource)[] Scripts =


    [


        ("install-windowsmash.sh","tools/linux/install.sh","layout.linux.install.sh"),


        ("Install WindowSmash.command","tools/mac/install.sh","layout.mac.install.sh"),


        ("play-ssbu.sh","tools/linux/play.sh","layout.linux.play.sh"),


        ("Play SSBU.command","tools/mac/play.sh","layout.mac.play.sh")


    ];


    public static void Normalize(string root)


    {


        bool legacy=Scripts.Any(s=>File.Exists(Path.Combine(root,s.Old)));


        if(!legacy)return;


        foreach(var script in Scripts)


        {


            string target=Path.Combine(root,script.Current);


            if(File.Exists(target))continue;


            using var resource=typeof(ProjectLayout).Assembly.GetManifestResourceStream(script.Resource)


                ??throw new InvalidOperationException("Missing internal setup script: "+script.Current);


            Directory.CreateDirectory(Path.GetDirectoryName(target)!);


            using(var output=File.Create(target))resource.CopyTo(output);


            if(!OperatingSystem.IsWindows())File.SetUnixFileMode(target,UnixFileMode.UserRead|UnixFileMode.UserWrite|UnixFileMode.UserExecute|UnixFileMode.GroupRead|UnixFileMode.GroupExecute|UnixFileMode.OtherRead|UnixFileMode.OtherExecute);


        }


        string askpass=Path.Combine(root,"tools","linux","askpass.sh");


        if(!File.Exists(askpass))


        {


            Directory.CreateDirectory(Path.GetDirectoryName(askpass)!);


            File.WriteAllText(askpass,Resource("layout.linux.askpass.sh"));


            if(!OperatingSystem.IsWindows())File.SetUnixFileMode(askpass,UnixFileMode.UserRead|UnixFileMode.UserWrite|UnixFileMode.UserExecute|UnixFileMode.GroupRead|UnixFileMode.GroupExecute|UnixFileMode.OtherRead|UnixFileMode.OtherExecute);


        }


    }
    static string Resource(string name)


    {


        using var stream=typeof(ProjectLayout).Assembly.GetManifestResourceStream(name)??throw new InvalidOperationException("Missing layout resource: "+name);


        using var reader=new StreamReader(stream);return reader.ReadToEnd();


    }


}
