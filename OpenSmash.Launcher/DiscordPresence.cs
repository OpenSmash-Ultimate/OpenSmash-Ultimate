using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;

namespace OpenSmash.Launcher
{
    // The launcher's own Rich Presence ("In the launcher") through Discord's Social SDK, the same library and application
    // as the game's (runtime/net_discord.c): build\discord_partner_sdk.dll and build\discord.txt, from the release
    // discord-1 (tools/discord_fetch.py). Without them, with SSBU_DISCORD=0, or when anything in the SDK is missing,
    // Start returns null and nothing else happens. Shown through the Discord app while it runs; it goes before the game
    // starts, so the game's own presence takes over. Every call is made on the UI thread (Tick runs the SDK's callbacks).
    sealed class DiscordPresence : IDisposable
    {
        [StructLayout(LayoutKind.Sequential)] struct DStr { public IntPtr Ptr; public UIntPtr Size; }
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate void VoidFn();
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate void ObjFn(IntPtr obj);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate void ObjU64Fn(IntPtr obj, ulong v);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate void ObjIntFn(IntPtr obj, int v);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate void ObjObjFn(IntPtr obj, IntPtr other);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate void ObjStrFn(IntPtr obj, ref DStr s);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate void ResultCb(IntPtr result, IntPtr ud);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate void UpdateFn(IntPtr client, IntPtr activity, ResultCb cb, IntPtr free, IntPtr ud);

        const int ActivityPlaying = 0, PlatformDesktop = 1;   // (cdiscord.h, as runtime/net_discord.c)

        readonly IntPtr lib, client;
        readonly VoidFn runCallbacks;
        readonly ObjFn? clientDrop, clear, resultDrop;
        readonly UpdateFn update;
        readonly ObjFn activityInit, activityDrop, tsInit, tsDrop, assetsInit, assetsDrop;
        readonly ObjIntFn activityType, activityPlatforms;
        readonly ObjStrFn activityDetails, activityState, largeImage, largeText;
        readonly ObjObjFn activityTimestamps, activityAssets;
        readonly ObjU64Fn tsStart;
        readonly ResultCb onResult;   // (kept alive while the SDK may call it)
        readonly ulong started = (ulong)DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
        string shown = "";
        bool disposed;

        // The files beside the game, or null when the game runs without Discord (or Discord is turned off).
        public static string? SdkPath(string buildDir)
        {
            if (Environment.GetEnvironmentVariable("SSBU_DISCORD") == "0") return null;
            string dll = Path.Combine(buildDir, "discord_partner_sdk.dll");
            return File.Exists(dll) ? dll : null;
        }

        public static DiscordPresence? Start(string buildDir, string appId)
        {
            if (!ulong.TryParse(appId, out ulong id) || id == 0) return null;
            string? dll = SdkPath(buildDir);
            if (dll == null || !NativeLibrary.TryLoad(dll, out IntPtr lib)) return null;
            try { return new DiscordPresence(lib, id); }
            catch (Exception e) when (e is EntryPointNotFoundException or InvalidOperationException or MarshalDirectiveException) { return null; }
        }

        DiscordPresence(IntPtr lib, ulong appId)
        {
            this.lib = lib;
            T F<T>(string fn) where T : Delegate =>
                NativeLibrary.TryGetExport(lib, fn, out var p) ? Marshal.GetDelegateForFunctionPointer<T>(p) : throw new EntryPointNotFoundException(fn);
            T? Opt<T>(string fn) where T : Delegate => NativeLibrary.TryGetExport(lib, fn, out var p) ? Marshal.GetDelegateForFunctionPointer<T>(p) : null;
            runCallbacks = F<VoidFn>("Discord_RunCallbacks");
            var init = F<ObjFn>("Discord_Client_Init");
            var setApp = F<ObjU64Fn>("Discord_Client_SetApplicationId");
            var connect = F<ObjFn>("Discord_Client_Connect");
            update = F<UpdateFn>("Discord_Client_UpdateRichPresence");
            activityInit = F<ObjFn>("Discord_Activity_Init"); activityDrop = F<ObjFn>("Discord_Activity_Drop");
            activityType = F<ObjIntFn>("Discord_Activity_SetType"); activityPlatforms = F<ObjIntFn>("Discord_Activity_SetSupportedPlatforms");
            activityDetails = F<ObjStrFn>("Discord_Activity_SetDetails"); activityState = F<ObjStrFn>("Discord_Activity_SetState");
            activityTimestamps = F<ObjObjFn>("Discord_Activity_SetTimestamps"); activityAssets = F<ObjObjFn>("Discord_Activity_SetAssets");
            tsInit = F<ObjFn>("Discord_ActivityTimestamps_Init"); tsDrop = F<ObjFn>("Discord_ActivityTimestamps_Drop");
            tsStart = F<ObjU64Fn>("Discord_ActivityTimestamps_SetStart");
            assetsInit = F<ObjFn>("Discord_ActivityAssets_Init"); assetsDrop = F<ObjFn>("Discord_ActivityAssets_Drop");
            largeImage = F<ObjStrFn>("Discord_ActivityAssets_SetLargeImage"); largeText = F<ObjStrFn>("Discord_ActivityAssets_SetLargeText");
            resultDrop = Opt<ObjFn>("Discord_ClientResult_Drop");
            clear = Opt<ObjFn>("Discord_Client_ClearRichPresence");
            clientDrop = Opt<ObjFn>("Discord_Client_Drop");
            onResult = (result, ud) => { if (result != IntPtr.Zero) resultDrop?.Invoke(result); };
            client = Obj();
            init(client);
            setApp(client, appId);
            connect(client);   // (no account here: Rich Presence goes through the Discord app, as the game's does)
        }

        static IntPtr Obj()
        {
            IntPtr p = Marshal.AllocHGlobal(IntPtr.Size);
            Marshal.WriteIntPtr(p, IntPtr.Zero);
            return p;
        }

        // Shows details / state (again only when they change).
        public void Show(string details, string state = "")
        {
            if (disposed || details + "\n" + state == shown) return;
            shown = details + "\n" + state;
            IntPtr a = Obj(), ts = Obj(), assets = Obj();
            var pins = new System.Collections.Generic.List<GCHandle>();
            DStr S(string text)
            {
                byte[] b = Encoding.UTF8.GetBytes(text);
                var h = GCHandle.Alloc(b, GCHandleType.Pinned);
                pins.Add(h);
                return new DStr { Ptr = h.AddrOfPinnedObject(), Size = (UIntPtr)b.Length };
            }
            try
            {
                activityInit(a);
                activityType(a, ActivityPlaying);
                activityPlatforms(a, PlatformDesktop);
                var d = S(details); activityDetails(a, ref d);
                if (state != "") { var st = S(state); activityState(a, ref st); }
                tsInit(ts); tsStart(ts, started); activityTimestamps(a, ts);
                assetsInit(assets);   // (the art "opensmash" of the Developer Portal, as the game's)
                var img = S("opensmash"); largeImage(assets, ref img);
                var txt = S("OpenSmash"); largeText(assets, ref txt);
                activityAssets(a, assets);
                update(client, a, onResult, IntPtr.Zero, IntPtr.Zero);
                assetsDrop(assets); tsDrop(ts); activityDrop(a);
            }
            finally
            {
                foreach (var h in pins) h.Free();
                Marshal.FreeHGlobal(a); Marshal.FreeHGlobal(ts); Marshal.FreeHGlobal(assets);
            }
        }

        public void Tick() { if (!disposed) runCallbacks(); }

        public void Dispose()
        {
            if (disposed) return;
            disposed = true;
            try
            {
                clear?.Invoke(client);
                runCallbacks();
                clientDrop?.Invoke(client);
            }
            catch (SEHException) { }
            Marshal.FreeHGlobal(client);
        }
    }
}
