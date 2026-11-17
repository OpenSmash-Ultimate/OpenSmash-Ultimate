using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;

namespace OpenSmash.Core;

public static partial class GameLaunch
{
    static bool IsGameWindow(IntPtr window)
    {
        if(window==IntPtr.Zero)return false;
        var name=new StringBuilder(64);
        return GetClassName(window,name,name.Capacity)>0&&name.ToString().Equals("ssbu",StringComparison.OrdinalIgnoreCase);
    }
    static Child StartWindows(Command command, string log)
    {
        using var output = new FileStream(log, FileMode.Create, FileAccess.Write, FileShare.ReadWrite | FileShare.Delete);
        using var input = CreateFile("NUL", 0x80000000, 3, IntPtr.Zero, 3, 0, IntPtr.Zero);
        if (input.IsInvalid) throw new Win32Exception(Marshal.GetLastWin32Error());
        if (!SetHandleInformation(output.SafeFileHandle, 1, 1) || !SetHandleInformation(input, 1, 1))
            throw new Win32Exception(Marshal.GetLastWin32Error());
        var startup = new StartupInfo { Size = Marshal.SizeOf<StartupInfo>(), Flags = 0x100,
            Input = input.DangerousGetHandle(), Output = output.SafeFileHandle.DangerousGetHandle(), Error = output.SafeFileHandle.DangerousGetHandle() };
        var info = command.StartInfo();
        foreach (string key in Commands.ClearedGameVariables) info.Environment.Remove(key);
        string environment = string.Join('\0', info.Environment.OrderBy(e => e.Key, StringComparer.OrdinalIgnoreCase).Select(e => e.Key + "=" + e.Value)) + "\0\0";
        IntPtr block = Marshal.StringToHGlobalUni(environment);
        try
        {
            var arguments = new StringBuilder(string.Join(" ", new[]{command.Executable}.Concat(command.Arguments).Select(QuoteWindowsArgument)));
            if (!CreateProcess(null, arguments, IntPtr.Zero, IntPtr.Zero, true, 0x08000400, block, command.WorkingDirectory, ref startup, out var process))
                throw new Win32Exception(Marshal.GetLastWin32Error());
            CloseHandle(process.Thread);
            return new Child(new SafeProcessHandle(process.Process, true), (int)process.Id);
        }
        finally { Marshal.FreeHGlobal(block); }
    }

    // CommandLineToArgvW / C runtime quoting; no shell expansion of %, &, quotes or spaces.
    public static string QuoteWindowsArgument(string argument)
    {
        var result = new StringBuilder("\""); int slashes = 0;
        foreach (char value in argument)
        {
            if (value == '\\') { slashes++; continue; }
            if (value == '"') result.Append('\\', slashes * 2 + 1).Append('"');
            else result.Append('\\', slashes).Append(value);
            slashes = 0;
        }
        return result.Append('\\', slashes * 2).Append('"').ToString();
    }

    sealed partial class Child
    {
        readonly SafeProcessHandle? _native;
        public Child(SafeProcessHandle handle, int id) { _native = handle; Id = id; }
        bool NativeExited => WaitForSingleObject(_native!, 0) != 258;
        int NativeExitCode { get { if (!GetExitCodeProcess(_native!, out uint code)) throw new Win32Exception(Marshal.GetLastWin32Error()); return (int)code; } }
    }

    [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)]
    struct StartupInfo
    {
        public int Size; public string? Reserved, Desktop, Title;
        public int X,Y,Width,Height,Columns,Rows,Fill,Flags; public short Show,ReservedSize;
        public IntPtr ReservedPointer,Input,Output,Error;
    }
    [StructLayout(LayoutKind.Sequential)]
    struct ProcessInfo { public IntPtr Process,Thread; public uint Id,ThreadId; }
    [DllImport("kernel32.dll",EntryPoint="CreateProcessW",CharSet=CharSet.Unicode,SetLastError=true)]
    [return:MarshalAs(UnmanagedType.Bool)]
    static extern bool CreateProcess(string? application,StringBuilder command,IntPtr processAttributes,IntPtr threadAttributes,bool inherit,uint flags,IntPtr environment,string directory,ref StartupInfo startup,out ProcessInfo process);
    [DllImport("kernel32.dll",EntryPoint="CreateFileW",CharSet=CharSet.Unicode,SetLastError=true)]
    static extern SafeFileHandle CreateFile(string name,uint access,uint share,IntPtr security,uint disposition,uint flags,IntPtr template);
    [DllImport("kernel32.dll",SetLastError=true)] static extern bool SetHandleInformation(SafeFileHandle handle,uint mask,uint flags);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
    [DllImport("kernel32.dll")] static extern uint WaitForSingleObject(SafeProcessHandle handle,uint milliseconds);
    [DllImport("kernel32.dll",SetLastError=true)] static extern bool GetExitCodeProcess(SafeProcessHandle handle,out uint code);
    [DllImport("user32.dll",EntryPoint="GetClassNameW",CharSet=CharSet.Unicode)] static extern int GetClassName(IntPtr window,StringBuilder name,int capacity);
}
