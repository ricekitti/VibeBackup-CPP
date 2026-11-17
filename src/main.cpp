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
namespace fs = std::filesystem;

static HWND mainWindow{}, projectText{}, listBox{}, statusText{}, autoCheck{};
static fs::path project, backupRoot;
static std::atomic<bool> busy{false};
static std::atomic<bool> autoEnabled{false};
static std::atomic<bool> running{true};
static std::atomic<unsigned long long> lastFingerprint{0};
static constexpr UINT WM_FINISHED = WM_APP + 1;
static constexpr UINT WM_REFRESH = WM_APP + 2;
static constexpr int ID_PICK=101, ID_BACKUP=102, ID_RESTORE=103, ID_AUTO=104, ID_REFRESH=105;

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
void refreshList() {
    SendMessageW(listBox,LB_RESETCONTENT,0,0);
    if(backupRoot.empty()||!fs::exists(backupRoot)) return;
    std::vector<fs::path> files;
    for(const auto& e:fs::directory_iterator(backupRoot)) if(e.is_regular_file()&&e.path().extension()==L".zip") files.push_back(e.path());
    std::sort(files.begin(),files.end(),std::greater<fs::path>());
    for(const auto& p:files) SendMessageW(listBox,LB_ADDSTRING,0,(LPARAM)p.filename().c_str());
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
    setStatus(L"Creating snapshot...");
    std::thread([]{
        bool ok=false;
        try {ok=createSnapshot();}catch(...) {}
        lastFingerprint.store(fingerprint(project));
        PostMessageW(mainWindow,WM_FINISHED,ok?1:0,0);
    }).detach();
}
void chooseProject() {
    BROWSEINFOW bi{};bi.hwndOwner=mainWindow;bi.lpszTitle=L"Select the coding project to back up";bi.ulFlags=BIF_RETURNONLYFSDIRS|BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE id=SHBrowseForFolderW(&bi);
    if(!id) return;
    wchar_t path[MAX_PATH];
    if(SHGetPathFromIDListW(id,path)) {
        if(busy.load()) { CoTaskMemFree(id);return; }
        project=path;backupRoot=storageFor(project);
        SetWindowTextW(projectText,project.c_str());
        lastFingerprint.store(fingerprint(project));
        refreshList();setStatus(L"Project selected. Ready to back up.");
    }
    CoTaskMemFree(id);
}
void restoreSelected() {
    if(busy.load()||project.empty()) return;
    int i=(int)SendMessageW(listBox,LB_GETCURSEL,0,0);
    if(i==LB_ERR) {MessageBoxW(mainWindow,L"Select a snapshot first.",L"VibeBackup",MB_OK);return;}
    wchar_t name[256];SendMessageW(listBox,LB_GETTEXT,i,(LPARAM)name);
    fs::path archive=backupRoot/name;
    if(archive.parent_path()!=backupRoot||!fs::exists(archive)) return;
    auto dest=project.parent_path()/(project.filename().wstring()+L"_restored_"+timestamp());
    if(fs::exists(dest)) {MessageBoxW(mainWindow,L"Restore destination already exists.",L"VibeBackup",MB_OK);return;}
    fs::create_directories(dest);
    bool ok=runProcess(L"C:\\Windows\\System32\\tar.exe",L"-xf "+quote(archive.wstring())+L" -C "+quote(dest.wstring()));
    if(!ok) {MessageBoxW(mainWindow,L"Restore failed. Check the ZIP archive.",L"VibeBackup",MB_ICONERROR);return;}
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
        control(L"STATIC",L"VibeBackup",0,22,18,500,36);
        control(L"STATIC",L"Local snapshots for coding projects",0,22,54,510,24);
        control(L"BUTTON",L"Choose project",BS_PUSHBUTTON,22,92,132,34,ID_PICK);
        projectText=control(L"STATIC",L"No project selected",SS_LEFT,170,101,485,24);
        control(L"BUTTON",L"Back up now",BS_PUSHBUTTON,22,145,135,36,ID_BACKUP);
        control(L"BUTTON",L"Restore selected",BS_PUSHBUTTON,170,145,135,36,ID_RESTORE);
        control(L"BUTTON",L"Refresh",BS_PUSHBUTTON,318,145,110,36,ID_REFRESH);
        autoCheck=control(L"BUTTON",L"Auto-backup every 5 minutes (when files change)",BS_AUTOCHECKBOX,22,199,500,26,ID_AUTO);
        control(L"STATIC",L"Snapshots (newest first)",0,22,244,500,25);
        listBox=control(L"LISTBOX",L"",LBS_NOTIFY|WS_BORDER|WS_VSCROLL,22,274,630,224);
        statusText=control(L"STATIC",L"Choose a project to begin.",0,22,516,625,25);
        return 0;
    case WM_COMMAND:
        switch(LOWORD(wp)) {
        case ID_PICK:chooseProject();break;
        case ID_BACKUP:startBackup();break;
        case ID_RESTORE:restoreSelected();break;
        case ID_REFRESH:refreshList();break;
        case ID_AUTO:autoEnabled.store(SendMessageW(autoCheck,BM_GETCHECK,0,0)==BST_CHECKED);break;
        }
        return 0;
    case WM_FINISHED:
        busy.store(false);refreshList();setStatus(wp?L"Snapshot saved successfully.":L"Backup failed. Check project permissions and available disk space.");return 0;
    case WM_REFRESH:if(autoEnabled.load()&&!busy.load()&&!project.empty()) {auto fp=fingerprint(project);if(fp!=lastFingerprint.load()) startBackup();}return 0;
    case WM_DESTROY:running.store(false);PostQuitMessage(0);return 0;
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR,int show) {
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX ic{sizeof(ic),ICC_STANDARD_CLASSES};InitCommonControlsEx(&ic);
    WNDCLASSW wc{};wc.lpfnWndProc=windowProc;wc.hInstance=instance;wc.lpszClassName=L"VibeBackupWindow";
    wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);
    RegisterClassW(&wc);
    mainWindow=CreateWindowExW(0,wc.lpszClassName,L"VibeBackup — Project Snapshots",WS_OVERLAPPEDWINDOW&~WS_MAXIMIZEBOX,
        CW_USEDEFAULT,CW_USEDEFAULT,700,600,nullptr,nullptr,instance,nullptr);
    if(!mainWindow) return 1;
    ShowWindow(mainWindow,show);UpdateWindow(mainWindow);
    std::thread([]{while(running.load()) {for(int i=0;i<300&&running.load();++i) Sleep(1000);if(running.load()) PostMessageW(mainWindow,WM_REFRESH,0,0);}}).detach();
    MSG msg;while(GetMessageW(&msg,nullptr,0,0)>0) {TranslateMessage(&msg);DispatchMessageW(&msg);}
    CoUninitialize();return (int)msg.wParam;
}
