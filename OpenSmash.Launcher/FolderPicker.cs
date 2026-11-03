using System.Runtime.InteropServices;

namespace OpenSmash.Launcher
{
    /// <summary>The folder window used by Add mod, with a checkbox for a symbolic link.</summary>
    static class FolderPicker
    {
        const int CheckId = 1;
        const uint FosPickFolders = 0x20;
        const uint FosForceFileSystem = 0x40;
        const uint FosNoChangeDir = 0x8;
        const uint FosPathMustExist = 0x800;
        const uint SigdnFileSystemPath = 0x80058000;
        const int HResultCanceled = unchecked((int)0x800704C7);

        public static bool Show(IWin32Window owner, out string path, out bool asLink)
        {
            path = "";
            asLink = false;
            var dialog = (IFileOpenDialog)new FileOpenDialog();
            dialog.SetOptions(FosPickFolders | FosForceFileSystem | FosNoChangeDir | FosPathMustExist);
            dialog.SetTitle("Select Folder");
            var customize = (IFileDialogCustomize)dialog;
            customize.AddText(0, "Pick a mod folder");
            customize.AddCheckButton(CheckId, "Add as symbolic link", false);
            int hr = dialog.Show(owner.Handle);
            if (hr == HResultCanceled) return false;
            if (hr < 0) Marshal.ThrowExceptionForHR(hr);
            dialog.GetResult(out IShellItem item);
            item.GetDisplayName(SigdnFileSystemPath, out IntPtr name);
            try { path = Marshal.PtrToStringUni(name) ?? ""; }
            finally { Marshal.FreeCoTaskMem(name); }
            customize.GetCheckButtonState(CheckId, out asLink);
            return path != "";
        }

        [ComImport, Guid("DC1C5A9C-E88A-4dde-A5A1-60F82A20AEF7")]
        class FileOpenDialog { }

        [ComImport, Guid("d57c7288-d4ad-4768-be02-9d969532d960"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        interface IFileOpenDialog
        {
            [PreserveSig] int Show(IntPtr hwndOwner);
            void SetFileTypes(uint cFileTypes, IntPtr rgFilterSpec);
            void SetFileTypeIndex(uint iFileType);
            void GetFileTypeIndex(out uint piFileType);
            void Advise(IntPtr pfde, out uint pdwCookie);
            void Unadvise(uint dwCookie);
            void SetOptions(uint fos);
            void GetOptions(out uint fos);
            void SetDefaultFolder(IShellItem psi);
            void SetFolder(IShellItem psi);
            void GetFolder(out IShellItem ppsi);
            void GetCurrentSelection(out IShellItem ppsi);
            void SetFileName([MarshalAs(UnmanagedType.LPWStr)] string pszName);
            void GetFileName(out IntPtr pszName);
            void SetTitle([MarshalAs(UnmanagedType.LPWStr)] string pszTitle);
            void SetOkButtonLabel([MarshalAs(UnmanagedType.LPWStr)] string pszText);
            void SetFileNameLabel([MarshalAs(UnmanagedType.LPWStr)] string pszLabel);
            void GetResult(out IShellItem ppsi);
            void AddPlace(IShellItem psi, int fdap);
            void SetDefaultExtension([MarshalAs(UnmanagedType.LPWStr)] string pszDefaultExtension);
            void Close(int hr);
            void SetClientGuid(ref Guid guid);
            void ClearClientData();
            void SetFilter(IntPtr pFilter);
            void GetResults(out IntPtr ppenum);
            void GetSelectedItems(out IntPtr ppsai);
        }

        [ComImport, Guid("e6fdd21a-163f-4975-9c8c-a69f1ba37034"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        interface IFileDialogCustomize
        {
            void EnableOpenDropDown(int dwIDCtl);
            void AddMenu(int dwIDCtl, [MarshalAs(UnmanagedType.LPWStr)] string pszLabel);
            void AddPushButton(int dwIDCtl, [MarshalAs(UnmanagedType.LPWStr)] string pszLabel);
            void AddComboBox(int dwIDCtl);
            void AddRadioButtonList(int dwIDCtl);
            void AddCheckButton(int dwIDCtl, [MarshalAs(UnmanagedType.LPWStr)] string pszLabel, [MarshalAs(UnmanagedType.Bool)] bool bChecked);
            void AddEditBox(int dwIDCtl, [MarshalAs(UnmanagedType.LPWStr)] string pszText);
            void AddSeparator(int dwIDCtl);
            void AddText(int dwIDCtl, [MarshalAs(UnmanagedType.LPWStr)] string pszText);
            void SetControlLabel(int dwIDCtl, [MarshalAs(UnmanagedType.LPWStr)] string pszLabel);
            void GetControlState(int dwIDCtl, out int pdwState);
            void SetControlState(int dwIDCtl, int dwState);
            void GetEditBoxText(int dwIDCtl, out IntPtr ppszText);
            void SetEditBoxText(int dwIDCtl, [MarshalAs(UnmanagedType.LPWStr)] string pszText);
            void GetCheckButtonState(int dwIDCtl, [MarshalAs(UnmanagedType.Bool)] out bool pbChecked);
        }

        [ComImport, Guid("43826d1e-e718-42ee-bc55-a1e261c37bfe"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        interface IShellItem
        {
            void BindToHandler(IntPtr pbc, ref Guid bhid, ref Guid riid, out IntPtr ppv);
            void GetParent(out IShellItem ppsi);
            void GetDisplayName(uint sigdnName, out IntPtr ppszName);
            void GetAttributes(uint sfgaoMask, out uint psfgaoAttribs);
            void Compare(IShellItem psi, uint hint, out int piOrder);
        }
    }
}
