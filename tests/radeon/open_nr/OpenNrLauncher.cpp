// SPDX-License-Identifier: GPL-3.0-only
// Native double-click entry point. Python/Tk and engine paths are configured by
// the local staging script; no console window or system installation required.
#include <windows.h>
#include <filesystem>
#include <string>

int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int) {
    wchar_t filename[32768]{};
    if(!GetModuleFileNameW(nullptr,filename,32768))return 1;
    const auto home=std::filesystem::path(filename).parent_path();
    const auto ini=home/L"launcher.ini";
    wchar_t python[32768]{};
    GetPrivateProfileStringW(L"Paths",L"Python",L"",python,32768,ini.c_str());
    const auto script=home/L"open_nr_gui.py";
    if(!std::filesystem::is_regular_file(python)||!std::filesystem::is_regular_file(script)){
        MessageBoxW(nullptr,L"실행 구성 파일을 찾지 못했습니다.\nlauncher.ini와 open_nr_gui.py를 실행 파일과 같은 폴더에 두세요.",L"Veyra OpenNR",MB_OK|MB_ICONERROR);return 1;
    }
    std::wstring command=L"\""+std::wstring(python)+L"\" -B \""+script.wstring()+L"\" --home \""+home.wstring()+L"\"";
    STARTUPINFOW si{};si.cb=sizeof(si);PROCESS_INFORMATION child{};
    if(!CreateProcessW(python,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,home.c_str(),&si,&child)){
        const auto detail=L"실행하지 못했습니다. Windows 오류: "+std::to_wstring(GetLastError());
        MessageBoxW(nullptr,detail.c_str(),L"Veyra OpenNR",MB_OK|MB_ICONERROR);return 1;
    }
    CloseHandle(child.hThread);CloseHandle(child.hProcess);return 0;
}
