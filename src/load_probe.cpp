#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <filesystem>
#include <iostream>
#include <string>

int wmain(int argc,wchar_t** argv) {
    if (argc!=2) {std::wcerr<<L"Usage: load-probe itemization-probe.dll\n";return 2;}
    auto path=std::filesystem::absolute(argv[1]);
    if (path.filename()!=L"itemization-probe.dll" || !std::filesystem::is_regular_file(path)) {
        std::wcerr<<L"Expected the built itemization-probe.dll\n";return 2;
    }
    DWORD pid=0;uintptr_t game_base=0,remote_loader_base=0;
    HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    if (snapshot==INVALID_HANDLE_VALUE) return 3;
    PROCESSENTRY32W process_entry{};process_entry.dwSize=sizeof(process_entry);
    if (Process32FirstW(snapshot,&process_entry)) do {
        if (_wcsicmp(process_entry.szExeFile,L"DRAGON QUEST XI S.exe")==0) {
            if (pid) {CloseHandle(snapshot);std::wcerr<<L"Close extra copies of XI S\n";return 3;}
            pid=process_entry.th32ProcessID;
        }
    } while (Process32NextW(snapshot,&process_entry));
    CloseHandle(snapshot);
    if (!pid) {std::wcerr<<L"XI S is not running\n";return 3;}
    auto loader=GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"LoadLibraryW");
    HMODULE local_loader_module=nullptr;
    if (!loader || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(loader),&local_loader_module)) return 4;
    wchar_t loader_module_path[32768]{};
    if (!GetModuleFileNameW(local_loader_module,loader_module_path,32768)) return 4;
    auto loader_module_name=std::filesystem::path(loader_module_path).filename().wstring();
    snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32,pid);
    if (snapshot==INVALID_HANDLE_VALUE) return 4;
    MODULEENTRY32W module{};module.dwSize=sizeof(module);
    bool already_loaded=false;
    if (Module32FirstW(snapshot,&module)) do {
        if (_wcsicmp(module.szModule,L"DRAGON QUEST XI S.exe")==0) game_base=reinterpret_cast<uintptr_t>(module.modBaseAddr);
        if (_wcsicmp(module.szModule,loader_module_name.c_str())==0) remote_loader_base=reinterpret_cast<uintptr_t>(module.modBaseAddr);
        if (_wcsicmp(module.szModule,L"itemization-probe.dll")==0) already_loaded=true;
    } while (Module32NextW(snapshot,&module));
    CloseHandle(snapshot);
    if (already_loaded) {std::wcout<<L"Diagnostic already loaded; restart the game before replacing it\n";return 0;}
    if (!game_base || !remote_loader_base) {std::wcerr<<L"Cannot locate game/loader modules\n";return 4;}
    HANDLE process=OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_CREATE_THREAD|PROCESS_VM_OPERATION|PROCESS_VM_WRITE|PROCESS_VM_READ,FALSE,pid);
    if (!process) {std::wcerr<<L"Cannot open XI S for diagnostic loading\n";return 5;}
    uint64_t signature=0;SIZE_T count=0;
    if (!ReadProcessMemory(process,reinterpret_cast<void*>(game_base+0x36835D0),&signature,sizeof(signature),&count) ||
        count!=sizeof(signature) || signature!=0x37353A38313A3132ULL) {
        CloseHandle(process);std::wcerr<<L"Unsupported executable; no plug-in loaded\n";return 5;
    }
    auto dll_path=path.wstring();size_t byte_count=(dll_path.size()+1)*sizeof(wchar_t);
    void* remote_path=VirtualAllocEx(process,nullptr,byte_count,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    if (!remote_path || !WriteProcessMemory(process,remote_path,dll_path.c_str(),byte_count,&count) || count!=byte_count) {
        if (remote_path) VirtualFreeEx(process,remote_path,0,MEM_RELEASE);
        CloseHandle(process);std::wcerr<<L"Cannot prepare diagnostic loading\n";return 6;
    }
    auto remote_loader=remote_loader_base+(reinterpret_cast<uintptr_t>(loader)-reinterpret_cast<uintptr_t>(local_loader_module));
    HANDLE thread=CreateRemoteThread(process,nullptr,0,reinterpret_cast<LPTHREAD_START_ROUTINE>(remote_loader),remote_path,0,nullptr);
    if (!thread) {VirtualFreeEx(process,remote_path,0,MEM_RELEASE);CloseHandle(process);return 7;}
    DWORD wait=WaitForSingleObject(thread,10000),result=0;
    if (wait==WAIT_OBJECT_0) {GetExitCodeThread(thread,&result);VirtualFreeEx(process,remote_path,0,MEM_RELEASE);}
    // If the loader hasn't finished, its argument must remain allocated until game exit.
    CloseHandle(thread);CloseHandle(process);
    if (wait!=WAIT_OBJECT_0) {std::wcerr<<L"Loading has not finished; check diagnostic log before retrying\n";return 8;}
    if (!result) {std::wcerr<<L"Windows could not load the diagnostic plug-in\n";return 9;}
    std::wcout<<L"Diagnostic plug-in loaded into XI S. Check itemization-probe.jsonl for activation.\n";
    return 0;
}
