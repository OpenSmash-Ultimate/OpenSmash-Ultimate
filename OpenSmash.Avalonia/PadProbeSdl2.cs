using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;

namespace OpenSmash.Launcher
{
    // Linux: the game links SDL2 (SSBU_HAVE_SDL2) and reads its GameController API. Name, type and GUID as its
    // sdl_open does (runtime/hle_pad.c): SDL_GameControllerName, sdl2_type, SDL_JoystickGetGUIDString of the
    // device index's GUID, so [device <GUID>] and the type's section are the ones the game reads.
    internal sealed class Sdl2PadSource : IPadSource
    {
        const uint SDL_INIT_GAMECONTROLLER = 0x2000;
        const int SDL_CONTROLLER_BUTTON_MAX = 21;   // (A .. TOUCHPAD, SDL 2.0.14 on: SDL3's numbering up to there)
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int InitFn(uint flags);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate void QuitFn(uint flags);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int HintFn([MarshalAs(UnmanagedType.LPUTF8Str)] string name, [MarshalAs(UnmanagedType.LPUTF8Str)] string value);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int CountFn();
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int IndexIntFn(int index);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate IntPtr IndexPtrFn(int index);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate Guid16 GuidFn(int index);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate IntPtr PtrPtrFn(IntPtr p);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int PtrIntFn(IntPtr p);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate void PtrFn(IntPtr p);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate void VoidFn();
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate byte ButtonFn(IntPtr gp, int button);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate short AxisFn(IntPtr gp, int axis);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int StateFn(int state);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate IntPtr RwFromFileFn([MarshalAs(UnmanagedType.LPUTF8Str)] string file, [MarshalAs(UnmanagedType.LPUTF8Str)] string mode);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int MappingsRwFn(IntPtr rw, int freerw);

        [StructLayout(LayoutKind.Sequential)] struct Guid16 { public ulong Lo, Hi; }

        readonly IntPtr lib;
        readonly InitFn? init; readonly QuitFn? quit; readonly CountFn? count; readonly IndexIntFn? isController, instanceId;
        readonly IndexPtrFn? open; readonly GuidFn? guid; readonly PtrPtrFn? name; readonly PtrIntFn? type, attached; readonly PtrFn? close;
        readonly VoidFn? update; readonly ButtonFn? button; readonly AxisFn? axis;
        readonly Dictionary<uint, (IntPtr Gp, PadDevice Pad)> opened = new();
        bool started;
        public string Problem { get; } = "";

        // runtime/hle_pad.c sdl2_type: SDL2's SDL_GameControllerType as SDL3 names it (controllers.ini's [device <type>]).
        internal static string TypeName(int type) => type switch
        {
            1 => "xbox360", 2 => "xboxone", 3 => "ps3", 4 => "ps4", 7 => "ps5", 5 => "switchpro",
            11 => "joyconleft", 12 => "joyconright", 13 => "joyconpair",
            _ => "standard",
        };

        public Sdl2PadSource(string library, string repo)
        {
            if (!PadSources.Load(library, out lib))
            {
                Problem = "The game reads controllers with SDL2 (libSDL2-2.0.so.0), which could not be loaded: install or update the game first.";
                return;
            }
            T? F<T>(string fn) where T : Delegate => NativeLibrary.TryGetExport(lib, fn, out var p) ? Marshal.GetDelegateForFunctionPointer<T>(p) : null;
            init = F<InitFn>("SDL_InitSubSystem"); quit = F<QuitFn>("SDL_QuitSubSystem"); count = F<CountFn>("SDL_NumJoysticks");
            isController = F<IndexIntFn>("SDL_IsGameController"); instanceId = F<IndexIntFn>("SDL_JoystickGetDeviceInstanceID");
            open = F<IndexPtrFn>("SDL_GameControllerOpen"); guid = F<GuidFn>("SDL_JoystickGetDeviceGUID"); name = F<PtrPtrFn>("SDL_GameControllerName");
            type = F<PtrIntFn>("SDL_GameControllerGetType"); attached = F<PtrIntFn>("SDL_GameControllerGetAttached"); close = F<PtrFn>("SDL_GameControllerClose");
            update = F<VoidFn>("SDL_GameControllerUpdate"); button = F<ButtonFn>("SDL_GameControllerGetButton"); axis = F<AxisFn>("SDL_GameControllerGetAxis");
            var hint = F<HintFn>("SDL_SetHint");
            var events = F<StateFn>("SDL_GameControllerEventState");
            var joyEvents = F<StateFn>("SDL_JoystickEventState");
            var rw = F<RwFromFileFn>("SDL_RWFromFile");
            var mappings = F<MappingsRwFn>("SDL_GameControllerAddMappingsFromRW");
            if (init == null || quit == null || count == null || isController == null || instanceId == null || open == null || guid == null ||
                name == null || type == null || attached == null || close == null || update == null || button == null || axis == null)
            {
                Problem = Path.GetFileName(library) + " is not the SDL 2 the game uses.";
                return;
            }
            hint?.Invoke("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1");
            hint?.Invoke("SDL_NO_SIGNAL_HANDLERS", "1");
            hint?.Invoke("SDL_GAMECONTROLLER_USE_BUTTON_LABELS", "0");   // (as the game: positional, A at the bottom)
            if (init(SDL_INIT_GAMECONTROLLER) != 0) { Problem = "SDL could not start its controller support."; return; }
            started = true;
            events?.Invoke(0); joyEvents?.Invoke(0);   // (SDL_IGNORE, as the game: the state is read, not the event queue)
            string db = Path.Combine(repo, "gamecontrollerdb.txt");
            if (rw != null && mappings != null && File.Exists(db)) { IntPtr f = rw(db, "rb"); if (f != IntPtr.Zero) mappings(f, 1); }
        }

        public IReadOnlyList<PadDevice> Pads()
        {
            var pads = new List<PadDevice>();
            if (!started) return pads;
            update!();
            foreach (var id in new List<uint>(opened.Keys))
                if (attached!(opened[id].Gp) == 0) { close!(opened[id].Gp); opened.Remove(id); }
            int n = count!();
            for (int k = 0; k < n; k++)
            {
                if (isController!(k) == 0) continue;
                uint id = (uint)instanceId!(k);
                if (!opened.TryGetValue(id, out var known))
                {
                    IntPtr gp = open!(k);
                    if (gp == IntPtr.Zero) continue;
                    var g = guid!(k);
                    string nm = Marshal.PtrToStringUTF8(name!(gp)) ?? "gamepad";
                    opened[id] = known = (gp, new PadDevice(id, nm, TypeName(type!(gp)), PadSources.GuidText(g.Lo, g.Hi)));
                }
                pads.Add(known.Pad);
            }
            return pads;
        }

        public ISet<string> Pressed(uint id)
        {
            var held = new HashSet<string>();
            if (!started || !opened.TryGetValue(id, out var known)) return held;
            update!();
            for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; b++)
                if (button!(known.Gp, b) != 0) held.Add(PadProfile.Sources[b]);
            if (axis!(known.Gp, 4) > 16000) held.Add("lefttrigger");    // (SDL_CONTROLLER_AXIS_TRIGGERLEFT, past half way)
            if (axis!(known.Gp, 5) > 16000) held.Add("righttrigger");
            return held;
        }

        public void Dispose()
        {
            foreach (var known in opened.Values) close?.Invoke(known.Gp);
            opened.Clear();
            if (started) quit?.Invoke(SDL_INIT_GAMECONTROLLER);
            started = false;
        }
    }
}
