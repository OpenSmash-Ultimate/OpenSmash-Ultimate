using System.Reflection;
using Avalonia.Headless;
using OpenSmash;
using OpenSmash.Launcher;

// The Discord account, the lobby feed and Rich Presence (ported from main's launcher). The vectors are
// tools/nex/board.py's (player_id, account_hash, signature) and RFC 7636's PKCE example.
static class CommunityChecks
{
    static void Check(bool ok,string message){if(!ok)throw new Exception(message);Console.WriteLine("PASS "+message);}
    public static void Run(string temporary)
    {
        string root=Path.Combine(temporary,"community "+Guid.NewGuid().ToString("N"));Directory.CreateDirectory(root);
        var token=new DiscordToken{Access="acc",Refresh="ref",Scope="identify openid",UserId="123456789012345678",Username="user",DisplayName="Shown Name",Avatar="abc",ExpiresAt=1900000000};
        DiscordAccount.Save(root,token);
        Check(File.Exists(Path.Combine(root,"config","discord_token.txt")),"the Discord token goes to config/discord_token.txt (shared with the game)");
        var back=DiscordAccount.Load(root);
        Check(back!=null&&back.Access=="acc"&&back.Refresh=="ref"&&back.Scope=="identify openid"&&back.UserId==token.UserId&&back.Shown=="Shown Name"&&back.Avatar=="abc"&&back.ExpiresAt==1900000000,"the Discord token file round-trips every field");
        DiscordAccount.SignOut(root);
        Check(DiscordAccount.Load(root)==null,"signing out deletes the token");
        Check(DiscordAccount.Challenge("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk")=="E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM","PKCE S256 challenge matches RFC 7636");
        string nsa="0123456789abcdef",player=DiscordAccount.PlayerId(nsa);
        Check(player=="c482daf7b5ec44a2","player id matches tools/nex/board.py");
        Check(DiscordAccount.AccountHash("123456789012345678")=="f747edf36c519329","account hash matches tools/nex/board.py");
        Check(DiscordAccount.Sign(nsa,"me",player,1700000000,"")=="d84f8e4fe1b9ee8aa98f1288165d248f704d5a6ac6aadc9ba400c055b89ff884","GET /gsp/me signature matches tools/nex/board.py");
        Check(DiscordAccount.Sign(nsa,"link",player,1700000000,"tok\n1")=="a0b324b5f829e92476cfa7fd1e5907bba90f47e529239ced95c38c6a58822a63","POST /gsp/link signature matches tools/nex/board.py");
        Check(DiscordAccount.FighterName(29)=="Mr. Game & Watch"&&DiscordAccount.FighterName(85)=="Sora","GSP fighter names follow the records' save numbers (#394)");

        var status=CommunityFeed.ParseStatus("{\"online\":12,\"rooms\":4,\"open_rooms\":3,\"arenas\":1,\"playing\":8,\"ranked\":40}");
        Check(status!=null&&status.Online==12&&status.OpenRooms==3&&status.Rooms==4,"status.json gives players online and open rooms");
        Check(CommunityFeed.ParseStatus("<html>")==null&&CommunityFeed.ParseStatus("{}")==null,"a page that is not status.json is refused");
        var rows=string.Join(",",Enumerable.Range(1,12).Select(i=>$"{{\"rank\":{13-i},\"id\":\"id{13-i}\",\"gsp\":{1000000-i},\"fighter\":29{(i==12?",\"name\":\"Me\\u0007 \"":"")}}}"));
        string board="{\"ranked\":120,\"players\":["+rows+"]}";
        var parsed=CommunityFeed.ParseBoard(board);
        Check(parsed is {Ranked:120} p&&p.Top.Count==10&&p.Top[0].Rank==1&&p.Top[0].Name=="Me"&&p.Top[9].Rank==10,"leaderboard.json gives the top 10, best first, names cleaned");
        Check(CommunityFeed.Who(parsed!.Value.Top[1])=="Mr. Game & Watch player","an unnamed row shows its main fighter");
        var view=new OpenSmash.Visuals.CommunityView();
        Check(view.Headline=="ASKING THE LOBBY...","before the first answer the lobby is being asked");
        view.Apply(status,board,"id2");
        Check(view.Headline=="ONLINE NOW"&&view.Top.Count==10&&view.Mine?.Rank==2&&view.Ranked==120,"a lobby that answers shows online numbers, the top 10 and the player's row");
        view.Apply(null,null,"id2");
        Check(view.Headline=="LOBBY OFFLINE"&&view.Lobby?.Online==12&&view.Top.Count==10,"a lobby that does not answer shows Lobby offline, the last numbers kept");

        string build=Path.Combine(root,"build");Directory.CreateDirectory(build);
        string? before=Environment.GetEnvironmentVariable("SSBU_DISCORD");
        try
        {
            Environment.SetEnvironmentVariable("SSBU_DISCORD",null);
            Check(DiscordPresence.SdkPath(build)==null&&DiscordPresence.Start(build,"123456789012345678")==null,"Rich Presence is off without the SDK library");
            File.WriteAllText(Path.Combine(build,"discord_partner_sdk.dll"),"not a library");
            Check((DiscordPresence.SdkPath(build)!=null)==OperatingSystem.IsWindows(),"the SDK library is looked for on Windows only (the only system it is fetched for)");
            Check(DiscordPresence.Start(build,"")==null,"Rich Presence is off without discord.txt's application id");
            Check(DiscordPresence.Start(build,"123456789012345678")==null,"a library that does not load leaves Rich Presence off");
            Environment.SetEnvironmentVariable("SSBU_DISCORD","0");
            Check(DiscordPresence.SdkPath(build)==null&&DiscordPresence.Start(build,"123456789012345678")==null,"SSBU_DISCORD=0 turns Rich Presence off");
        }
        finally{Environment.SetEnvironmentVariable("SSBU_DISCORD",before);}
    }

    // The column on the home page: drawn once the feed starts, with its links hit-testable.
    public static void Render(string root,string? screenshotDir)
    {
        var window=new MainWindow([],root,enableBackgroundActions:false);window.Show();
        var surface=window.Surface;surface.FrozenTime=10;
        Check(surface.CurrentView=="home","the community column is checked on the home page");var c=surface.Community;
        c.Enabled=true;c.HasApp=true;
        c.Apply(CommunityFeed.ParseStatus("{\"online\":7,\"open_rooms\":2}"),"{\"ranked\":3,\"players\":[{\"rank\":1,\"id\":\"a\",\"gsp\":9876543,\"fighter\":29,\"name\":\"A very long player name for testing\"},{\"rank\":2,\"id\":\"b\",\"gsp\":123456,\"fighter\":0}]}","");
        var hits=(List<(string Id,System.Drawing.RectangleF R)>)typeof(OpenSmash.Visuals.ClassicSurface).GetField("_hits",BindingFlags.Instance|BindingFlags.NonPublic)!.GetValue(surface)!;
        foreach(var (w,h) in new[]{(1440,810),(1280,720),(960,540)})
        {
            window.Width=w;window.Height=h;surface.InvalidateVisual();
            using var frame=window.CaptureRenderedFrame();
            if(screenshotDir!=null)frame!.Save(Path.Combine(screenshotDir,$"community-{w}.png"));
            var board=hits.FirstOrDefault(x=>x.Id=="community:board");var signin=hits.FirstOrDefault(x=>x.Id=="community:signin");
            var play=hits.First(x=>x.Id=="menu:3");
            Check(board.Id!=null&&signin.Id!=null&&board.R.Right<=frame!.PixelSize.Width&&signin.R.Left>play.R.Right,$"home right column fits beside the buttons at {w}x{h}");
        }
        c.Account=new DiscordToken{Access="x",UserId="1",DisplayName="Player"};c.Open=true;c.Records=[new FighterGsp{Fighter=29,Gsp=500000}];
        surface.InvalidateVisual();using(var frame=window.CaptureRenderedFrame()){if(screenshotDir!=null)frame!.Save(Path.Combine(screenshotDir,"community-card.png"));}
        Check(hits.Any(x=>x.Id=="community:signout")&&hits.Any(x=>x.Id=="community:shown"),"the account card offers the leaderboard name choice and sign out");
        window.Close();
    }
}
