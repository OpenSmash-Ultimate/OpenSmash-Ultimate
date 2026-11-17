using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;

namespace OpenSmash.Launcher
{
    // A connected pad as the game sees it: SDL's name, type and GUID (runtime/hle_pad.c sdl_open), so the section the
    // remap page writes is the one the game picks for it.
    internal sealed record PadDevice(uint Id, string Name, string Type, string Guid);

    internal interface IPadSource : IDisposable
    {
        string Problem { get; }                 // why no pad can be read ("" when they can)
        IReadOnlyList<PadDevice> Pads();
        ISet<string> Pressed(uint id);          // the controls held now, as PadProfile.Sources names them
    }

    // The game's own SDL3.dll (build\SDL3.dll), loaded from the build folder, read the way the game reads it.
    internal sealed class SdlPadSource : IPadSource
    {
        const uint SDL_INIT_GAMEPAD = 0x2000;
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] [return: MarshalAs(UnmanagedType.U1)] delegate bool InitFn(uint flags);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate void QuitFn(uint flags);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] [return: MarshalAs(UnmanagedType.U1)] delegate bool HintFn([MarshalAs(UnmanagedType.LPUTF8Str)] string name, [MarshalAs(UnmanagedType.LPUTF8Str)] string value);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate IntPtr ListFn(out int count);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate IntPtr OpenFn(uint id);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate void PtrFn(IntPtr p);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate IntPtr NameFn(IntPtr gp);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate Guid16 GuidFn(uint id);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int TypeFn(IntPtr gp);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate IntPtr TypeNameFn(int type);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate void VoidFn();
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] [return: MarshalAs(UnmanagedType.U1)] delegate bool ButtonFn(IntPtr gp, int button);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate short AxisFn(IntPtr gp, int axis);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate void EnableFn([MarshalAs(UnmanagedType.U1)] bool on);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int MappingsFn([MarshalAs(UnmanagedType.LPUTF8Str)] string file);

        [StructLayout(LayoutKind.Sequential)] struct Guid16 { public ulong Lo, Hi; }

        readonly IntPtr lib;
        readonly InitFn? init; readonly QuitFn? quit; readonly ListFn? list; readonly OpenFn? open; readonly PtrFn? close, free;
        readonly NameFn? name; readonly GuidFn? guid; readonly TypeFn? type; readonly TypeNameFn? typeName; readonly VoidFn? update;
        readonly ButtonFn? button; readonly AxisFn? axis;
        readonly Dictionary<uint, IntPtr> opened = new();
        bool started;
        public string Problem { get; } = "";

        public SdlPadSource(string buildDir, string repo)
        {
            string dll = Path.Combine(buildDir, "SDL3.dll");
            if (!File.Exists(dll) || !NativeLibrary.TryLoad(dll, out lib))
            {
                Problem = "The game reads controllers with SDL3.dll, which is not in the build folder yet: install or update the game first.";
                return;
            }
            T? F<T>(string fn) where T : Delegate => NativeLibrary.TryGetExport(lib, fn, out var p) ? Marshal.GetDelegateForFunctionPointer<T>(p) : null;
            init = F<InitFn>("SDL_Init"); quit = F<QuitFn>("SDL_QuitSubSystem"); list = F<ListFn>("SDL_GetGamepads");
            open = F<OpenFn>("SDL_OpenGamepad"); close = F<PtrFn>("SDL_CloseGamepad"); free = F<PtrFn>("SDL_free");
            name = F<NameFn>("SDL_GetGamepadName"); guid = F<GuidFn>("SDL_GetGamepadGUIDForID"); type = F<TypeFn>("SDL_GetGamepadType");
            typeName = F<TypeNameFn>("SDL_GetGamepadStringForType"); update = F<VoidFn>("SDL_UpdateGamepads");
            button = F<ButtonFn>("SDL_GetGamepadButton"); axis = F<AxisFn>("SDL_GetGamepadAxis");
            var hint = F<HintFn>("SDL_SetHint");
            var events = F<EnableFn>("SDL_SetGamepadEventsEnabled");
            var mappings = F<MappingsFn>("SDL_AddGamepadMappingsFromFile");
            if (init == null || quit == null || list == null || open == null || close == null || free == null || name == null || guid == null ||
                type == null || typeName == null || update == null || button == null || axis == null)
            {
                Problem = "build\\SDL3.dll is not the SDL 3 the game uses.";
                return;
            }
            hint?.Invoke("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1");
            hint?.Invoke("SDL_NO_SIGNAL_HANDLERS", "1");
            if (!init(SDL_INIT_GAMEPAD)) { Problem = "SDL could not start its controller support."; return; }
            started = true;
            events?.Invoke(false);   // (as the game: the state is read, not the event queue)
            string db = Path.Combine(repo, "gamecontrollerdb.txt");
            if (mappings != null && File.Exists(db)) mappings(db);
        }

        // SDL_GUIDToString's text: the 16 bytes in order, two hex digits each.
        static string GuidText(Guid16 g)
        {
            var b = new byte[16];
            BitConverter.TryWriteBytes(b.AsSpan(0, 8), g.Lo);
            BitConverter.TryWriteBytes(b.AsSpan(8, 8), g.Hi);
            return Convert.ToHexString(b).ToLowerInvariant();
        }

        public IReadOnlyList<PadDevice> Pads()
        {
            var pads = new List<PadDevice>();
            if (!started) return pads;
            update!();
            IntPtr ids = list!(out int n);
            var seen = new HashSet<uint>();
            if (ids != IntPtr.Zero)
            {
                for (int i = 0; i < n; i++)
                {
                    uint id = (uint)Marshal.ReadInt32(ids, i * 4);
                    seen.Add(id);
                    if (!opened.TryGetValue(id, out var gp))
                    {
                        gp = open!(id);
                        if (gp == IntPtr.Zero) continue;
                        opened[id] = gp;
                    }
                    string nm = Marshal.PtrToStringUTF8(name!(gp)) ?? "gamepad";
                    string ty = Marshal.PtrToStringUTF8(typeName!(type!(gp))) ?? "gamepad";
                    pads.Add(new PadDevice(id, nm, ty, GuidText(guid!(id))));
                }
                free!(ids);
            }
            foreach (var id in new List<uint>(opened.Keys))
                if (!seen.Contains(id)) { close!(opened[id]); opened.Remove(id); }
            return pads;
        }

        public ISet<string> Pressed(uint id)
        {
            var held = new HashSet<string>();
            if (!started || !opened.TryGetValue(id, out var gp)) return held;
            update!();
            for (int b = 0; b < 26; b++)
                if (button!(gp, b)) held.Add(PadProfile.Sources[b]);
            if (axis!(gp, 4) > 16000) held.Add("lefttrigger");    // (SDL_GAMEPAD_AXIS_LEFT_TRIGGER, past half way)
            if (axis!(gp, 5) > 16000) held.Add("righttrigger");
            return held;
        }

        public void Dispose()
        {
            foreach (var gp in opened.Values) close?.Invoke(gp);
            opened.Clear();
            if (started) quit?.Invoke(SDL_INIT_GAMEPAD);
            started = false;
        }
    }
}
