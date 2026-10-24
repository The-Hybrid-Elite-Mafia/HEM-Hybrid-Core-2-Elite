// =============================================================================
//  installer/setup.cpp — "Hybrid Core Setup.exe"
//
//  One self-contained file. The app (HybridCore2Elite.exe) is embedded inside it.
//  Install (per user, no admin rights needed):
//    • %LOCALAPPDATA%\Programs\HEM Hybrid Core 2 Elite\HybridCore2Elite.exe
//    • Start menu shortcut + desktop shortcut
//    • .hcs session files open in Hybrid Core (double-click)
//    • listed in Settings → Apps with a working Uninstall
//  Uninstall:  "Uninstall Hybrid Core.exe" /uninstall  (what Settings → Apps runs)
//  Your stems, sessions and exported masters are never touched.
// =============================================================================
#ifndef NOMINMAX
  #define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <objbase.h>
#include <shellapi.h>
#include <string>
#include "setup_resource.h"

namespace {

const wchar_t* kTitle = L"HYBRID CORE 2 ELITE Setup";
const wchar_t* kAppName = L"HYBRID CORE 2 ELITE";
const wchar_t* kUninstKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\HEM.HybridCore2Elite";

std::wstring knownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring s;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &p)) && p) s = p;
    if (p) CoTaskMemFree(p);
    return s;
}

std::wstring installDir() {
    std::wstring base = knownFolder(FOLDERID_UserProgramFiles);          // %LOCALAPPDATA%\Programs
    if (base.empty()) {
        base = knownFolder(FOLDERID_LocalAppData);
        if (base.empty()) {
            wchar_t buf[MAX_PATH * 4];
            const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, static_cast<DWORD>(std::size(buf)));
            base = (n && n < std::size(buf)) ? std::wstring(buf) : std::wstring(L"C:\\HybridCore2Elite");
        }
        base += L"\\Programs";
    }
    return base + L"\\HEM Hybrid Core 2 Elite";
}
std::wstring appExe() { return installDir() + L"\\HybridCore2Elite.exe"; }
std::wstring uninstExe() { return installDir() + L"\\Uninstall HYBRID CORE 2 ELITE.exe"; }
std::wstring startLnk() { return knownFolder(FOLDERID_Programs) + L"\\HYBRID CORE 2 ELITE.lnk"; }
std::wstring desktopLnk() { return knownFolder(FOLDERID_Desktop) + L"\\HYBRID CORE 2 ELITE.lnk"; }

bool writeResource(int id, const std::wstring& path, std::wstring& err) {
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));   // RT_RCDATA
    HGLOBAL g = r ? LoadResource(nullptr, r) : nullptr;
    const void* data = g ? LockResource(g) : nullptr;
    const DWORD size = r ? SizeofResource(nullptr, r) : 0;
    if (!data || !size) { err = L"The installer is damaged (payload missing)."; return false; }
    const std::wstring tmp = path + L".new";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { err = L"Can't write to " + tmp; return false; }
    DWORD wrote = 0;
    const BOOL ok = WriteFile(f, data, size, &wrote, nullptr) && wrote == size;
    CloseHandle(f);
    if (!ok) { DeleteFileW(tmp.c_str()); err = L"Writing the program file failed (disk full?)."; return false; }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        err = L"Hybrid Core is running. Close it, then run Setup again.";
        return false;
    }
    return true;
}

bool makeShortcut(const std::wstring& lnk, const std::wstring& target, const std::wstring& desc) {
    IShellLinkW* sl = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) return false;
    sl->SetPath(target.c_str());
    sl->SetWorkingDirectory(installDir().c_str());
    sl->SetDescription(desc.c_str());
    sl->SetIconLocation(target.c_str(), 0);
    IPersistFile* pf = nullptr;
    bool ok = false;
    if (SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&pf)))) { ok = SUCCEEDED(pf->Save(lnk.c_str(), TRUE)); pf->Release(); }
    sl->Release();
    return ok;
}

void setStr(HKEY k, const wchar_t* name, const std::wstring& v) {
    RegSetValueExW(k, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()), static_cast<DWORD>((v.size() + 1) * sizeof(wchar_t)));
}
void setDword(HKEY k, const wchar_t* name, DWORD v) { RegSetValueExW(k, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof v); }

void writeKey(const std::wstring& path, const wchar_t* name, const std::wstring& value) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
        setStr(k, name, value);
        RegCloseKey(k);
    }
}

DWORD fileSizeKB(const std::wstring& p) {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d)) return 0;
    return static_cast<DWORD>(((static_cast<ULONGLONG>(d.nFileSizeHigh) << 32) | d.nFileSizeLow) / 1024);
}

int install() {
    const std::wstring dir = installDir();
    const std::wstring ask = std::wstring(L"Install ") + kAppName + L"?\n\n"
        L"Location:\n" + dir + L"\n\n"
        L"• Start menu and desktop shortcuts\n"
        L"• Session files (.hcs) can open in Hybrid Core 2 Elite\n"
        L"• Uninstall from Settings → Apps\n\n"
        L"No admin rights needed.";
    if (MessageBoxW(nullptr, ask.c_str(), kTitle, MB_YESNO | MB_ICONQUESTION) != IDYES) return 1;

    // (No old-copy cleanup here: THE HYBRID CORE 2.0 is a separate app and is never touched.)
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    std::wstring err;
    if (!writeResource(IDR_PAYLOAD_APP, appExe(), err)) { MessageBoxW(nullptr, err.c_str(), kTitle, MB_ICONERROR); return 2; }

    wchar_t self[MAX_PATH * 4];
    GetModuleFileNameW(nullptr, self, static_cast<DWORD>(std::size(self)));
    if (lstrcmpiW(self, uninstExe().c_str()) != 0) CopyFileW(self, uninstExe().c_str(), FALSE);

    const bool s1 = makeShortcut(startLnk(), appExe(), L"HYBRID CORE 2 ELITE — Hybrid Elite Mafia");
    const bool s2 = makeShortcut(desktopLnk(), appExe(), L"HYBRID CORE 2 ELITE — Hybrid Elite Mafia");

    // .hcs → this app, unless another program already owns .hcs (then we
    // only add ourselves to "Open with" so nothing of theirs is changed).
    writeKey(L"Software\\Classes\\HEM.HybridCore2Elite.Session", nullptr, L"Hybrid Core 2 Elite Session");
    writeKey(L"Software\\Classes\\HEM.HybridCore2Elite.Session\\DefaultIcon", nullptr, L"\"" + appExe() + L"\",0");
    writeKey(L"Software\\Classes\\HEM.HybridCore2Elite.Session\\shell\\open\\command", nullptr, L"\"" + appExe() + L"\" \"%1\"");
    {
        HKEY k = nullptr;
        wchar_t cur[128] = {}; DWORD sz = sizeof cur;
        bool taken = false;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\.hcs", 0, KEY_READ, &k) == ERROR_SUCCESS) {
            taken = RegQueryValueExW(k, nullptr, nullptr, nullptr, reinterpret_cast<BYTE*>(cur), &sz) == ERROR_SUCCESS &&
                    cur[0] && lstrcmpW(cur, L"HEM.HybridCore2Elite.Session") != 0;
            RegCloseKey(k);
        }
        if (!taken) writeKey(L"Software\\Classes\\.hcs", nullptr, L"HEM.HybridCore2Elite.Session");
        writeKey(L"Software\\Classes\\.hcs\\OpenWithProgids", L"HEM.HybridCore2Elite.Session", L"");
    }

    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kUninstKey, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
        setStr(k, L"DisplayName", kAppName);
        setStr(k, L"DisplayVersion", L"1.0.0");
        setStr(k, L"Publisher", L"Hybrid Elite Mafia");
        setStr(k, L"DisplayIcon", appExe());
        setStr(k, L"InstallLocation", dir);
        setStr(k, L"UninstallString", L"\"" + uninstExe() + L"\" /uninstall");
        setDword(k, L"EstimatedSize", fileSizeKB(appExe()) + fileSizeKB(uninstExe()));
        setDword(k, L"NoModify", 1);
        setDword(k, L"NoRepair", 1);
        RegCloseKey(k);
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);

    std::wstring done = std::wstring(kAppName) + L" is installed.\n\n";
    if (!s1 || !s2) done += L"(A shortcut couldn't be created — the program is in " + dir + L")\n\n";
    done += L"Open it now?";
    if (MessageBoxW(nullptr, done.c_str(), kTitle, MB_YESNO | MB_ICONINFORMATION) == IDYES)
        ShellExecuteW(nullptr, L"open", appExe().c_str(), nullptr, dir.c_str(), SW_SHOWNORMAL);
    return 0;
}

int uninstall() {
    if (MessageBoxW(nullptr, L"Remove THE HYBRID CORE 2.0 from this PC?\n(Other HEM programs are not affected.)\n\nYour stems, sessions and exported masters are NOT touched.",
                    L"Uninstall Hybrid Core", MB_YESNO | MB_ICONQUESTION) != IDYES) return 1;
    if (!DeleteFileW(appExe().c_str()) && GetFileAttributesW(appExe().c_str()) != INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(nullptr, L"Hybrid Core is running. Close it, then uninstall again.", L"Uninstall Hybrid Core", MB_ICONERROR);
        return 2;
    }
    DeleteFileW(startLnk().c_str());
    DeleteFileW(desktopLnk().c_str());
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\HEM.HybridCore2Elite.Session");
    {
        HKEY ow;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\.hcs\\OpenWithProgids", 0, KEY_SET_VALUE, &ow) == ERROR_SUCCESS) {
            RegDeleteValueW(ow, L"HEM.HybridCore2Elite.Session");
            RegCloseKey(ow);
        }
    }
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\.hcs", 0, KEY_READ, &k) == ERROR_SUCCESS) {
        wchar_t v[64] = {}; DWORD sz = sizeof v;
        const bool ours = RegQueryValueExW(k, nullptr, nullptr, nullptr, reinterpret_cast<BYTE*>(v), &sz) == ERROR_SUCCESS &&
                          lstrcmpW(v, L"HEM.HybridCore2Elite.Session") == 0;
        RegCloseKey(k);
        if (ours) {   // only clear the default we set; leave the key for anyone else
            HKEY w;
            if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\.hcs", 0, KEY_SET_VALUE, &w) == ERROR_SUCCESS) { RegDeleteValueW(w, nullptr); RegCloseKey(w); }
        }
    }
    RegDeleteTreeW(HKEY_CURRENT_USER, kUninstKey);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    MessageBoxW(nullptr, L"THE HYBRID CORE 2.0 was removed.", L"Uninstall Hybrid Core", MB_ICONINFORMATION);
    // This uninstaller runs from the install folder, so a hidden cmd removes
    // it (and the then-empty folder) after we exit, retrying for up to ~30 s.
    const std::wstring u = uninstExe(), d = installDir();
    std::wstring cmd = L"cmd.exe /d /c \"for /l %i in (1,1,30) do (ping 127.0.0.1 -n 2 >nul & del /f /q \"" + u +
                       L"\" 2>nul & if not exist \"" + u + L"\" (rmdir \"" + d + L"\" 2>nul & exit))\"";
    STARTUPINFOW si{}; si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    }
    return 0;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR cmdLine, int) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const std::wstring args = cmdLine ? cmdLine : L"";
    const int rc = (args.find(L"/uninstall") != std::wstring::npos) ? uninstall() : install();
    CoUninitialize();
    return rc;
}
