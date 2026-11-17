#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <filesystem>
#include <fstream>
#include <vector>
#include <string>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <cwctype>
#include <thread>
#include <atomic>
#include <system_error>
#include <array>
#include <stdexcept>
namespace fs = std::filesystem;

static constexpr int rowsPerPage=5;
static HWND mainWindow{}, projectText{}, statusText{}, autoCheck{}, backupButton{};
static HWND emptyText{}, pageText{}, newerButton{}, olderButton{};
static std::array<HWND,rowsPerPage> snapshotLabels{}, restoreButtons{};
static std::vector<fs::path> snapshots;
static size_t snapshotPage=0;
static HFONT titleFont{};
static fs::path project, backupRoot;
static std::atomic<bool> busy{false};
static std::atomic<bool> autoEnabled{false};
static std::atomic<bool> running{true};
static std::atomic<unsigned long long> lastFingerprint{0};
static constexpr UINT WM_FINISHED = WM_APP + 1;
static constexpr UINT WM_REFRESH = WM_APP + 2;
static constexpr int ID_PICK=101, ID_BACKUP=102, ID_AUTO=104, ID_REFRESH=105;
static constexpr int ID_NEWER=106, ID_OLDER=107, ID_COPY_PROMPT=108, ID_RESTORE_FIRST=200;

std::wstring quote(const std::wstring& s) { return L"\"" + s + L"\""; }
std::wstring timestamp() {
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t buf[64]; swprintf_s(buf,L"%04d-%02d-%02d_%02d-%02d-%02d",st.wYear,st.wMonth,st.wDay,st.wHour,st.wMinute,st.wSecond);
    return buf;
}
std::wstring safeName(std::wstring s) {
    for(auto& c:s) if(c==L'\\'||c==L'/'||c==L':'||c==L'*'||c==L'?'||c==L'"'||c==L'<'||c==L'>'||c==L'|') c=L'_';
    return s;
}
fs::path storageFor(const fs::path& p) {
    wchar_t buf[MAX_PATH]; DWORD n=GetEnvironmentVariableW(L"LOCALAPPDATA",buf,MAX_PATH);
    fs::path root = n && n<MAX_PATH ? fs::path(buf) : fs::temp_directory_path();
    // FNV-1a hash of canonical project path, to distinguish identically named folders.
    auto key=fs::weakly_canonical(p).wstring();
    std::transform(key.begin(),key.end(),key.begin(),[](wchar_t c){return (wchar_t)towlower(c);});
    unsigned long long h=14695981039346656037ull;
    for(auto c:key) { h^=(unsigned long long)c; h*=1099511628211ull; }
    std::wstringstream ss; ss<<std::hex<<h;
    return root/L"VibeBackup"/L"snapshots"/(safeName(p.filename().wstring())+L"_"+ss.str());
}
bool excluded(const fs::path& relative) {
    for(const auto& part:relative) {
        auto s=part.wstring();
        std::transform(s.begin(),s.end(),s.begin(),[](wchar_t c){return (wchar_t)towlower(c);});
        if(s==L".git"||s==L"node_modules"||s==L".venv"||s==L"venv"||s==L"__pycache__"||s==L"dist"||s==L"build"||s==L".next"||s==L".idea"||s==L".vs") return true;
    }
    return false;
}
void setStatus(const std::wstring& s) { if(statusText) SetWindowTextW(statusText,s.c_str()); }
unsigned long long fingerprint(const fs::path& p);
void copyChangelogPrompt() {
    const std::wstring prompt=
        L"Review this project's changes since its latest release. Update CHANGELOG.md with a concise, factual entry. "
        L"Choose the next semantic version based on the actual changes, update the version wherever the project defines it, "
        L"and keep version references consistent. Preserve the existing changelog history. "
        L"Do not invent changes. If the changes cannot be determined, explain what information is missing.";
    const size_t bytes=(prompt.size()+1)*sizeof(wchar_t);
    HGLOBAL memory=GlobalAlloc(GMEM_MOVEABLE,bytes);
    if(!memory) {setStatus(L"Could not copy the prompt.");return;}
    auto* data=static_cast<wchar_t*>(GlobalLock(memory));
    if(!data) {GlobalFree(memory);setStatus(L"Could not copy the prompt.");return;}
    std::copy(prompt.c_str(),prompt.c_str()+prompt.size()+1,data);
    GlobalUnlock(memory);
    if(!OpenClipboard(mainWindow)) {GlobalFree(memory);setStatus(L"Could not open the clipboard.");return;}
    if(!EmptyClipboard()||!SetClipboardData(CF_UNICODETEXT,memory)) {
        CloseClipboard();
        GlobalFree(memory);
        setStatus(L"Could not copy the prompt.");
        return;
    }
    CloseClipboard();
    setStatus(L"Prompt copied. Paste it into your coding assistant.");
}
std::wstring snapshotLabel(const fs::path& path) {
    auto name=path.stem().wstring();
    if(name.size()>=19) {
        auto time=name.substr(11,8);
        time[2]=L':';
        time[5]=L':';
        name=name.substr(0,10)+L"  "+time;
    }
    std::error_code ec;
    auto bytes=fs::file_size(path,ec);
    if(!ec) name+=L"  ·  "+std::to_wstring((bytes+1023)/1024)+L" KB";
    return name;
}
void showSnapshots() {
    const size_t start=snapshotPage*rowsPerPage;
    for(int row=0;row<rowsPerPage;++row) {
        const size_t index=start+row;
        const bool visible=index<snapshots.size();
        if(visible) SetWindowTextW(snapshotLabels[row],snapshotLabel(snapshots[index]).c_str());
        ShowWindow(snapshotLabels[row],visible?SW_SHOWNA:SW_HIDE);
        ShowWindow(restoreButtons[row],visible?SW_SHOWNA:SW_HIDE);
        EnableWindow(restoreButtons[row],!busy.load());
    }
    ShowWindow(emptyText,snapshots.empty()?SW_SHOWNA:SW_HIDE);
    if(snapshots.empty())
        SetWindowTextW(emptyText,project.empty()?L"Choose a folder to see backups.":L"No backups yet. Click Back up now.");
    const size_t pageCount=(snapshots.size()+rowsPerPage-1)/rowsPerPage;
    std::wstring page=pageCount?L"Page "+std::to_wstring(snapshotPage+1)+L" of "+std::to_wstring(pageCount):L"";
    SetWindowTextW(pageText,page.c_str());
    EnableWindow(newerButton,snapshotPage>0);
    EnableWindow(olderButton,snapshotPage+1<pageCount);
}
void refreshSnapshots() {
    snapshots.clear();
    std::error_code ec;
    if(!backupRoot.empty()&&fs::is_directory(backupRoot,ec)) {
        for(fs::directory_iterator it(backupRoot,ec),end;!ec&&it!=end;it.increment(ec)) {
            if(it->is_regular_file(ec)&&it->path().extension()==L".zip") snapshots.push_back(it->path());
        }
    }
    std::sort(snapshots.begin(),snapshots.end(),std::greater<fs::path>());
    if(snapshotPage*rowsPerPage>=snapshots.size()) snapshotPage=0;
    showSnapshots();
}
void saveSettings() {
    HKEY key{};
    if(RegCreateKeyExW(HKEY_CURRENT_USER,L"Software\\VibeBackup",0,nullptr,0,KEY_SET_VALUE,nullptr,&key,nullptr)!=ERROR_SUCCESS) return;
    const auto value=project.wstring();
    RegSetValueExW(key,L"ProjectPath",0,REG_SZ,reinterpret_cast<const BYTE*>(value.c_str()),
        static_cast<DWORD>((value.size()+1)*sizeof(wchar_t)));
    const DWORD enabled=autoEnabled.load()?1:0;
    RegSetValueExW(key,L"AutoBackup",0,REG_DWORD,reinterpret_cast<const BYTE*>(&enabled),sizeof(enabled));
    RegCloseKey(key);
}
void loadSettings() {
    DWORD enabled=0,bytes=sizeof(enabled);
    if(RegGetValueW(HKEY_CURRENT_USER,L"Software\\VibeBackup",L"AutoBackup",RRF_RT_REG_DWORD,nullptr,&enabled,&bytes)==ERROR_SUCCESS) {
        autoEnabled.store(enabled!=0);
        SendMessageW(autoCheck,BM_SETCHECK,enabled?BST_CHECKED:BST_UNCHECKED,0);
    }
    wchar_t path[32768]{};
    bytes=sizeof(path);
    if(RegGetValueW(HKEY_CURRENT_USER,L"Software\\VibeBackup",L"ProjectPath",RRF_RT_REG_SZ,nullptr,path,&bytes)!=ERROR_SUCCESS) return;
    std::error_code ec;
    if(!fs::is_directory(path,ec)) return;
    project=path;
    backupRoot=storageFor(project);
    SetWindowTextW(projectText,project.c_str());
    EnableWindow(backupButton,TRUE);
    lastFingerprint.store(fingerprint(project));
    refreshSnapshots();
    setStatus(snapshots.empty()?L"Ready to back up.":L"Last backup: "+snapshotLabel(snapshots.front()));
}
unsigned long long fingerprint(const fs::path& p) {
    unsigned long long h=14695981039346656037ull;
    std::error_code ec;
    fs::recursive_directory_iterator it(p,fs::directory_options::skip_permission_denied,ec),end;
    while(it!=end) {
        const auto path=it->path();
        if(excluded(path.lexically_relative(p))) { if(it->is_directory(ec)) it.disable_recursion_pending(); }
        else if(it->is_regular_file(ec)) {
            for(auto c:path.wstring()) { h^=(unsigned long long)c;h*=1099511628211ull; }
            auto size=it->file_size(ec); h^=size;h*=1099511628211ull;
            auto t=it->last_write_time(ec).time_since_epoch().count();h^=(unsigned long long)t;h*=1099511628211ull;
        }
        it.increment(ec);
        if(ec) ec.clear();
    }
    return h;
}
bool runProcess(const std::wstring& exe, const std::wstring& args) {
    std::wstring command=quote(exe)+L" "+args;
    STARTUPINFOW si{};si.cb=sizeof(si);si.dwFlags=STARTF_USESHOWWINDOW;si.wShowWindow=SW_HIDE;
    PROCESS_INFORMATION pi{};
    if(!CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi)) return false;
    WaitForSingleObject(pi.hProcess,INFINITE);
    DWORD code=1;GetExitCodeProcess(pi.hProcess,&code);
    CloseHandle(pi.hThread);CloseHandle(pi.hProcess);
    return code==0;
}
// tar.exe ships with supported modern Windows versions and supports ZIP with -a.
bool createSnapshot() {
    if(project.empty()||!fs::is_directory(project)) return false;
    fs::create_directories(backupRoot);
    auto stage=backupRoot/(L"stage_"+timestamp()+L"_"+std::to_wstring(GetCurrentProcessId()));
    fs::create_directories(stage);
    bool hasFiles=false;
    std::error_code ec;
    fs::recursive_directory_iterator it(project,fs::directory_options::skip_permission_denied,ec),end;
    while(it!=end) {
        auto src=it->path(),rel=src.lexically_relative(project);
        if(excluded(rel)) { if(it->is_directory(ec)) it.disable_recursion_pending(); }
        else if(it->is_regular_file(ec)&&!it->is_symlink(ec)) {
            auto dest=stage/rel;
            fs::create_directories(dest.parent_path(),ec);
            fs::copy_file(src,dest,fs::copy_options::overwrite_existing,ec);
            if(!ec) hasFiles=true;
        }
        it.increment(ec);if(ec)ec.clear();
    }
    if(!hasFiles) {fs::remove_all(stage,ec);return false;}
    auto archive=backupRoot/(timestamp()+L".zip");
    // Unique name if multiple snapshots occur within the same second.
    for(int i=1;fs::exists(archive);++i) archive=backupRoot/(timestamp()+L"_"+std::to_wstring(i)+L".zip");
    auto ok=runProcess(L"C:\\Windows\\System32\\tar.exe",L"-a -cf "+quote(archive.wstring())+L" -C "+quote(stage.wstring())+L" .");
    fs::remove_all(stage,ec);
    if(!ok) fs::remove(archive,ec);
    return ok;
}
void startBackup() {
    if(project.empty()||busy.exchange(true)) return;
    EnableWindow(backupButton,FALSE);
    showSnapshots();
    setStatus(L"Saving backup...");
    std::thread([]{
        bool ok=false;
        try {ok=createSnapshot();}catch(...) {}
        lastFingerprint.store(fingerprint(project));
        PostMessageW(mainWindow,WM_FINISHED,ok?1:0,0);
    }).detach();
}
void chooseProject() {
    BROWSEINFOW bi{};bi.hwndOwner=mainWindow;bi.lpszTitle=L"Choose a project folder";bi.ulFlags=BIF_RETURNONLYFSDIRS|BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE id=SHBrowseForFolderW(&bi);
    if(!id) return;
    wchar_t path[MAX_PATH];
    if(SHGetPathFromIDListW(id,path)) {
        if(busy.load()) { CoTaskMemFree(id);return; }
        project=path;backupRoot=storageFor(project);
        SetWindowTextW(projectText,project.c_str());
        EnableWindow(backupButton,TRUE);
        lastFingerprint.store(fingerprint(project));
        snapshotPage=0;
        refreshSnapshots();
        setStatus(snapshots.empty()?L"Ready to back up.":L"Last backup: "+snapshotLabel(snapshots.front()));
        saveSettings();
    }
    CoTaskMemFree(id);
}
void restoreSnapshot(int row) {
    if(busy.load()||project.empty()) return;
    const size_t index=snapshotPage*rowsPerPage+row;
    if(index>=snapshots.size()) return;
    const fs::path archive=snapshots[index];
    if(archive.parent_path()!=backupRoot||archive.extension()!=L".zip"||!fs::is_regular_file(archive)) return;
    auto dest=project.parent_path()/(project.filename().wstring()+L"_restored_"+timestamp());
    try {
        if(fs::exists(dest)) {MessageBoxW(mainWindow,L"Restore destination already exists.",L"VibeBackup",MB_OK);return;}
        busy.store(true);
        EnableWindow(backupButton,FALSE);
        showSnapshots();
        setStatus(L"Restoring backup...");
        UpdateWindow(mainWindow);
        fs::create_directories(dest);
        if(!runProcess(L"C:\\Windows\\System32\\tar.exe",L"-xf "+quote(archive.wstring())+L" -C "+quote(dest.wstring())))
            throw std::runtime_error("Archive extraction failed");
    } catch(...) {
        busy.store(false);
        EnableWindow(backupButton,TRUE);
        showSnapshots();
        setStatus(L"Restore failed. Check the backup file and available disk space.");
        return;
    }
    busy.store(false);
    EnableWindow(backupButton,TRUE);
    showSnapshots();
    setStatus(L"Restored to "+dest.wstring());
    auto msg=L"Restored to:\n"+dest.wstring()+L"\n\nYour original project was not changed.";
    MessageBoxW(mainWindow,msg.c_str(),L"Restore complete",MB_OK|MB_ICONINFORMATION);
}
HWND control(const wchar_t* cls,const wchar_t* label,DWORD style,int x,int y,int w,int h,int id=0) {
    HWND hwnd=CreateWindowExW(0,cls,label,WS_CHILD|WS_VISIBLE|style,x,y,w,h,mainWindow,(HMENU)(INT_PTR)id,GetModuleHandleW(nullptr),nullptr);
    SendMessageW(hwnd,WM_SETFONT,(WPARAM)GetStockObject(DEFAULT_GUI_FONT),TRUE);
    return hwnd;
}
LRESULT CALLBACK windowProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp) {
    switch(msg) {
    case WM_CREATE:
        mainWindow=hwnd;
        titleFont=CreateFontW(-25,0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        {
            HWND title=control(L"STATIC",L"VibeBackup",0,22,18,500,34);
            if(titleFont) SendMessageW(title,WM_SETFONT,(WPARAM)titleFont,TRUE);
        }
        control(L"STATIC",L"Save project backups and restore older versions.",0,22,57,650,22);
        control(L"STATIC",L"Project folder",0,22,86,500,22);
        projectText=control(L"STATIC",L"No folder selected",SS_PATHELLIPSIS,22,112,510,27);
        control(L"BUTTON",L"Change folder",BS_PUSHBUTTON,548,104,125,36,ID_PICK);
        backupButton=control(L"BUTTON",L"Back up now",BS_DEFPUSHBUTTON,22,161,145,42,ID_BACKUP);
        EnableWindow(backupButton,FALSE);
        autoCheck=control(L"BUTTON",L"Auto-backup every 5 minutes",BS_AUTOCHECKBOX,188,168,330,28,ID_AUTO);
        statusText=control(L"STATIC",L"Choose a folder to begin.",SS_PATHELLIPSIS,22,221,650,24);
        control(L"STATIC",L"",SS_ETCHEDHORZ,22,258,650,2);
        control(L"STATIC",L"Saved backups",0,22,275,440,26);
        control(L"BUTTON",L"Refresh",BS_PUSHBUTTON,584,269,89,31,ID_REFRESH);
        emptyText=control(L"STATIC",L"Choose a folder to see backups.",0,22,322,600,25);
        for(int row=0;row<rowsPerPage;++row) {
            const int y=314+row*43;
            snapshotLabels[row]=control(L"STATIC",L"",SS_LEFT,22,y+6,518,26);
            restoreButtons[row]=control(L"BUTTON",L"Restore",BS_PUSHBUTTON,558,y,115,32,ID_RESTORE_FIRST+row);
        }
        newerButton=control(L"BUTTON",L"Newer",BS_PUSHBUTTON,22,540,92,31,ID_NEWER);
        pageText=control(L"STATIC",L"",SS_CENTER,205,547,285,22);
        olderButton=control(L"BUTTON",L"Older",BS_PUSHBUTTON,581,540,92,31,ID_OLDER);
        control(L"BUTTON",L"Copy changelog + version prompt",BS_PUSHBUTTON,22,588,240,33,ID_COPY_PROMPT);
        control(L"STATIC",L"Paste into your coding assistant to update a project.",0,274,596,399,22);
        showSnapshots();
        return 0;
    case WM_COMMAND:
        if(LOWORD(wp)>=ID_RESTORE_FIRST&&LOWORD(wp)<ID_RESTORE_FIRST+rowsPerPage) {
            restoreSnapshot(LOWORD(wp)-ID_RESTORE_FIRST);
            return 0;
        }
        switch(LOWORD(wp)) {
        case ID_PICK:chooseProject();break;
        case ID_BACKUP:startBackup();break;
        case ID_REFRESH:refreshSnapshots();break;
        case ID_AUTO:
            autoEnabled.store(SendMessageW(autoCheck,BM_GETCHECK,0,0)==BST_CHECKED);
            saveSettings();
            break;
        case ID_COPY_PROMPT:copyChangelogPrompt();break;
        case ID_NEWER:if(snapshotPage>0) {--snapshotPage;showSnapshots();}break;
        case ID_OLDER:if((snapshotPage+1)*rowsPerPage<snapshots.size()) {++snapshotPage;showSnapshots();}break;
        }
        return 0;
    case WM_FINISHED:
        busy.store(false);
        EnableWindow(backupButton,TRUE);
        snapshotPage=0;
        refreshSnapshots();
        setStatus(wp&&!snapshots.empty()?L"Last backup: "+snapshotLabel(snapshots.front()):
            L"Backup failed. Check folder permissions and available disk space.");
        return 0;
    case WM_CTLCOLORSTATIC:
        SetBkMode(reinterpret_cast<HDC>(wp),TRANSPARENT);
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    case WM_REFRESH:if(autoEnabled.load()&&!busy.load()&&!project.empty()) {auto fp=fingerprint(project);if(fp!=lastFingerprint.load()) startBackup();}return 0;
    case WM_DESTROY:
        running.store(false);
        if(titleFont) DeleteObject(titleFont);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR,int show) {
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX ic{sizeof(ic),ICC_STANDARD_CLASSES};InitCommonControlsEx(&ic);
    WNDCLASSW wc{};wc.lpfnWndProc=windowProc;wc.hInstance=instance;wc.lpszClassName=L"VibeBackupWindow";
    wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);
    RegisterClassW(&wc);
    mainWindow=CreateWindowExW(0,wc.lpszClassName,L"VibeBackup — Project Snapshots",
        WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,
        CW_USEDEFAULT,CW_USEDEFAULT,700,690,nullptr,nullptr,instance,nullptr);
    if(!mainWindow) return 1;
    loadSettings();
    ShowWindow(mainWindow,show);UpdateWindow(mainWindow);
    std::thread([]{while(running.load()) {for(int i=0;i<300&&running.load();++i) Sleep(1000);if(running.load()) PostMessageW(mainWindow,WM_REFRESH,0,0);}}).detach();
    MSG msg;while(GetMessageW(&msg,nullptr,0,0)>0) {TranslateMessage(&msg);DispatchMessageW(&msg);}
    CoUninitialize();return (int)msg.wParam;
}
