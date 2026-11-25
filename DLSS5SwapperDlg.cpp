#include "pch.h"
#include <winhttp.h>
#include <urlmon.h>
#include <commctrl.h>
#include <vector>
#include <thread>
#include <memory>
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "urlmon.lib")
#pragma comment(lib, "comctl32.lib")
#include "DLSS5SwapperDlg.h"
#include <shellapi.h>
#include <shlobj.h>
#include <algorithm>
#include <set>
#include <cwctype>
#include <cmath>
#include <iterator>
#include <initializer_list>
#include <sstream>
#include <iomanip>
#include <cstdio>
#include <fstream>
#include <tlhelp32.h>
#include <dwmapi.h>
#include <afxdlgs.h>
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")

using namespace Gdiplus;

static bool NrParseBool(const std::wstring& raw, int fallback);
static std::wstring NrChoiceLabel(const NrUiSetting& s, int value);

namespace
{
    HRESULT CALLBACK DonationTaskDialogCallback(
        HWND, UINT notification, WPARAM, LPARAM lParam, LONG_PTR)
    {
        if (notification == TDN_HYPERLINK_CLICKED && lParam != 0) {
            const wchar_t* url = reinterpret_cast<const wchar_t*>(lParam);
            ShellExecuteW(nullptr, L"open", url, nullptr, nullptr, SW_SHOWNORMAL);
        }
        return S_OK;
    }

    constexpr float DW = 600.0f;
    constexpr float DH = 1240.0f;
    Color C(BYTE a, BYTE r, BYTE g, BYTE b) { return Color(a, r, g, b); }

    void Round(Graphics& g, RectF r, float radius, Color color)
    {
        GraphicsPath p;
        float d = radius * 2;
        p.AddArc(r.X, r.Y, d, d, 180, 90);
        p.AddArc(r.GetRight() - d, r.Y, d, d, 270, 90);
        p.AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0, 90);
        p.AddArc(r.X, r.GetBottom() - d, d, d, 90, 90);
        p.CloseFigure();
        SolidBrush b(color); g.FillPath(&b, &p);
    }
    void Outline(Graphics& g, RectF r, float radius, Color color, float w=1.2f)
    {
        GraphicsPath p;
        float d = radius * 2;
        p.AddArc(r.X, r.Y, d, d, 180, 90);
        p.AddArc(r.GetRight() - d, r.Y, d, d, 270, 90);
        p.AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0, 90);
        p.AddArc(r.X, r.GetBottom() - d, d, d, 90, 90);
        p.CloseFigure();
        Pen pen(color,w); g.DrawPath(&pen,&p);
    }
    void T(Graphics& g, const wchar_t* s, float x, float y, float size, Color color, bool bold=false)
    {
        // Empty labels are intentional for unlabeled controls. Never pass them
        // to GDI+ DrawString: an empty string can return InvalidParameter.
        if (s == nullptr || s[0] == L'\0') return;
        FontFamily ff(L"Microsoft YaHei UI");
        Font f(&ff,size * 1.30f,bold?FontStyleBold:FontStyleRegular,UnitPixel);
        SolidBrush b(color);
        g.DrawString(s,-1,&f,PointF(x,y),&b);
    }
    std::wstring Num(int n) { return std::to_wstring(n); }
    std::wstring Percent(int n) { return std::to_wstring(n) + L"%"; }

    std::wstring QuoteWindowsArg(const std::wstring& value)
    {
        std::wstring out=L"\"";
        unsigned slashes=0;
        for(wchar_t ch:value) {
            if(ch==L'\\') { ++slashes; continue; }
            if(ch==L'\"') { out.append(slashes*2+1,L'\\'); out.push_back(ch); slashes=0; continue; }
            out.append(slashes,L'\\'); slashes=0; out.push_back(ch);
        }
        out.append(slashes*2,L'\\'); out.push_back(L'\"');
        return out;
    }
    std::string WideToUtf8(const std::wstring& value)
    {
        if(value.empty()) return {};
        int n=WideCharToMultiByte(CP_UTF8,0,value.c_str(),(int)value.size(),nullptr,0,nullptr,nullptr);
        std::string out(n,'\0');
        WideCharToMultiByte(CP_UTF8,0,value.c_str(),(int)value.size(),out.data(),n,nullptr,nullptr);
        return out;
    }
    std::string JsonEscape(const std::wstring& value)
    {
        std::string u=WideToUtf8(value), out;
        for(unsigned char c:u) { if(c=='\\'||c=='\"') {out.push_back('\\');out.push_back((char)c);} else if(c=='\n') out+="\\n"; else if(c=='\r') out+="\\r"; else if(c<0x20) { char b[7]; sprintf_s(b,"\\u%04x",c); out+=b; } else out.push_back((char)c); }
        return out;
    }
    std::wstring ExactProcessPattern(const std::wstring& exe)
    {
        std::wstring out=L"^";
        const std::wstring special=L"\\^$.|?*+()[]{}/";
        for(wchar_t c:exe) { if(special.find(c)!=std::wstring::npos) out.push_back(L'\\'); out.push_back(c); }
        out.push_back(L'$'); return out;
    }

    bool ExeContainsWide(const std::wstring& exe, const wchar_t* value)
    {
        std::ifstream f(exe, std::ios::binary);
        if (!f) return false;
        std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        const size_t count = (wcslen(value) + 1) * sizeof(wchar_t);
        return bytes.find(std::string(reinterpret_cast<const char*>(value), count)) != std::string::npos;
    }
    bool ExeSupportsIniKey(const std::wstring& exe, const wchar_t* section, const wchar_t* key)
    {
        // Mirrors NRLiveConfig.Supported: both the wide section and the NUL-terminated key must exist.
        return ExeContainsWide(exe, section) && ExeContainsWide(exe, key);
    }
    bool ExeSupportsUpscaler(const std::wstring& exe)
    {
        std::ifstream f(exe, std::ios::binary);
        if (!f) return false;
        std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        const wchar_t marker[] = L"--upscaler\0";
        const char* needle = reinterpret_cast<const char*>(marker);
        // wchar_t is UTF-16 on Windows; include the explicit terminator.
        const size_t count = (wcslen(L"--upscaler") + 1) * sizeof(wchar_t);
        return bytes.find(std::string(needle, count)) != std::string::npos;
    }

    bool LoadProfileOverlayEnabled(const std::wstring& configDir, bool fallback)
    {
        std::wstring path = configDir + L"\\profiles\\Default.json";
        std::ifstream f(path, std::ios::binary);
        if (!f) return fallback;
        std::string json((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        const auto key=json.find("\"no_overlay\"");
        if(key==std::string::npos) return fallback;
        const auto colon=json.find(':',key); if(colon==std::string::npos)return fallback;
        const auto value=json.find_first_not_of(" \t\r\n",colon+1); if(value==std::string::npos)return fallback;
        if(json.compare(value,4,"true")==0)return false;
        if(json.compare(value,5,"false")==0)return true;
        return fallback;
    }
    int LoadProfileUpscaler(const std::wstring& configDir, int fallback)
    {
        std::wstring path = configDir + L"\\profiles\\Default.json";
        std::ifstream f(path, std::ios::binary);
        if (!f) return fallback;
        std::string json((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        const auto key = json.find("\"upscaler\"");
        if (key == std::string::npos) return fallback;
        const auto colon = json.find(':', key);
        const auto first = json.find('\"', colon == std::string::npos ? key : colon + 1);
        const auto last = first == std::string::npos ? first : json.find('\"', first + 1);
        if (first == std::string::npos || last == std::string::npos) return fallback;
        const auto value = json.substr(first + 1, last - first - 1);
        if (value == "fsr4") return 0;
        if (value == "fsr31") return 1;
        if (value == "none") return 2;
        return fallback;
    }
    std::wstring LoadProfileString(const std::wstring& configDir, const char* property, const std::wstring& fallback)
    {
        std::wstring path = configDir + L"\\profiles\\Default.json";
        std::ifstream f(path, std::ios::binary);
        if (!f) return fallback;
        std::string json((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        const std::string needle = std::string("\"") + property + "\"";
        const auto key = json.find(needle);
        if (key == std::string::npos) return fallback;
        const auto colon = json.find(':', key + needle.size());
        const auto first = json.find('"', colon == std::string::npos ? key : colon + 1);
        if (first == std::string::npos) return fallback;
        const auto last = json.find('"', first + 1);
        if (last == std::string::npos) return fallback;
        std::string value = json.substr(first + 1, last - first - 1);
        if (value.find('\\') != std::string::npos) return fallback;
        int n = MultiByteToWideChar(CP_UTF8, 0, value.data(), (int)value.size(), nullptr, 0);
        if (n <= 0) return fallback;
        std::wstring out(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, value.data(), (int)value.size(), out.data(), n);
        return out;
    }

    bool ParseHotkey(const std::wstring& text, UINT& modifiers, UINT& vk)
    {
        modifiers = 0; vk = 0;
        std::vector<std::wstring> parts;
        std::wstringstream ss(text); std::wstring part;
        while (std::getline(ss, part, L'+')) {
            size_t a = part.find_first_not_of(L" \t\r\n");
            if (a == std::wstring::npos) continue;
            size_t b = part.find_last_not_of(L" \t\r\n");
            part = part.substr(a, b - a + 1);
            std::transform(part.begin(), part.end(), part.begin(), [](wchar_t c) { return (wchar_t)towlower(c); });
            parts.push_back(part);
        }
        std::wstring key;
        for (const auto& x : parts) {
            if (x == L"ctrl" || x == L"control") modifiers |= MOD_CONTROL;
            else if (x == L"alt") modifiers |= MOD_ALT;
            else if (x == L"shift") modifiers |= MOD_SHIFT;
            else if (x == L"win" || x == L"windows") modifiers |= MOD_WIN;
            else { if (!key.empty()) return false; key = x; }
        }
        if (key.empty()) return false;
        if (key.size() == 1 && ((key[0] >= L'a' && key[0] <= L'z') || (key[0] >= L'0' && key[0] <= L'9'))) {
            vk = (UINT)towupper(key[0]); return true;
        }
        if (key.size() >= 2 && key[0] == L'f') {
            int f = _wtoi(key.c_str() + 1);
            if (f >= 1 && f <= 24 && key == L"f" + Num(f)) { vk = VK_F1 + f - 1; return true; }
        }
        static const std::map<std::wstring, UINT> names = {
            {L"home",VK_HOME},{L"end",VK_END},{L"insert",VK_INSERT},{L"delete",VK_DELETE},{L"del",VK_DELETE},
            {L"space",VK_SPACE},{L"spacebar",VK_SPACE},{L"tab",VK_TAB},{L"enter",VK_RETURN},{L"return",VK_RETURN},
            {L"esc",VK_ESCAPE},{L"escape",VK_ESCAPE},{L"pageup",VK_PRIOR},{L"pgup",VK_PRIOR},
            {L"pagedown",VK_NEXT},{L"pgdn",VK_NEXT},{L"up",VK_UP},{L"down",VK_DOWN},
            {L"left",VK_LEFT},{L"right",VK_RIGHT},{L"pause",VK_PAUSE},{L"printscreen",VK_SNAPSHOT}
        };
        auto it = names.find(key);
        if (it == names.end()) return false;
        vk = it->second; return true;
    }

    std::wstring KeyNameFromVk(UINT vk)
    {
        if(vk>='A'&&vk<='Z') return std::wstring(1,(wchar_t)towlower((wchar_t)vk));
        if(vk>='0'&&vk<='9') return std::wstring(1,(wchar_t)vk);
        if(vk>=VK_NUMPAD0&&vk<=VK_NUMPAD9) return std::wstring(1,(wchar_t)(L'0'+vk-VK_NUMPAD0));
        if(vk>=VK_F1&&vk<=VK_F24) return L"f"+Num((int)(vk-VK_F1+1));
        switch(vk) {
        case VK_HOME:return L"home"; case VK_END:return L"end"; case VK_INSERT:return L"insert"; case VK_DELETE:return L"delete";
        case VK_SPACE:return L"space"; case VK_TAB:return L"tab"; case VK_RETURN:return L"enter"; case VK_ESCAPE:return L"esc";
        case VK_PRIOR:return L"pageup"; case VK_NEXT:return L"pagedown"; case VK_UP:return L"up"; case VK_DOWN:return L"down";
        case VK_LEFT:return L"left"; case VK_RIGHT:return L"right"; case VK_PAUSE:return L"pause"; case VK_SNAPSHOT:return L"printscreen";
        default:return L"";
        }
    }

    std::wstring HotkeyDisplay(const std::wstring& value)
    {
        UINT mods=0,vk=0; if(!ParseHotkey(value,mods,vk)) return value;
        std::wstring out;
        if(mods&MOD_CONTROL) out+=L"Ctrl + "; if(mods&MOD_ALT) out+=L"Alt + ";
        if(mods&MOD_SHIFT) out+=L"Shift + "; if(mods&MOD_WIN) out+=L"Win + ";
        std::wstring key=KeyNameFromVk(vk); if(!key.empty()) key[0]=(wchar_t)towupper(key[0]);
        return out+key;
    }

    bool PressHotkey(const std::wstring& value)
    {
        UINT modifiers=0,vk=0; if(!ParseHotkey(value,modifiers,vk)) return false;
        std::vector<INPUT> inputs;
        auto keyInput=[&](WORD code,bool down) { INPUT in{}; in.type=INPUT_KEYBOARD; in.ki.wVk=code; if(!down) in.ki.dwFlags=KEYEVENTF_KEYUP; inputs.push_back(in); };
        if(modifiers&MOD_CONTROL) keyInput(VK_CONTROL,true);
        if(modifiers&MOD_ALT) keyInput(VK_MENU,true);
        if(modifiers&MOD_SHIFT) keyInput(VK_SHIFT,true);
        if(modifiers&MOD_WIN) keyInput(VK_LWIN,true);
        keyInput((WORD)vk,true); keyInput((WORD)vk,false);
        if(modifiers&MOD_WIN) keyInput(VK_LWIN,false);
        if(modifiers&MOD_SHIFT) keyInput(VK_SHIFT,false);
        if(modifiers&MOD_ALT) keyInput(VK_MENU,false);
        if(modifiers&MOD_CONTROL) keyInput(VK_CONTROL,false);
        return SendInput((UINT)inputs.size(),inputs.data(),sizeof(INPUT))==(UINT)inputs.size();
    }


#define WM_DLSS5_UPDATE_CHECK_RESULT (WM_APP + 61)

#define WM_DLSS5_DEPENDENCY_INSTALL_RESULT (WM_APP + 62)
#define WM_DLSS5_DEPENDENCY_INSTALL_PROGRESS (WM_APP + 63)

namespace {
class NrLiveDownloadCallback final : public IBindStatusCallback {
public:
    explicit NrLiveDownloadCallback(HWND hwnd) : hwnd_(hwnd) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IBindStatusCallback) {
            *ppv = static_cast<IBindStatusCallback*>(this); AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&refs_); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG n = (ULONG)InterlockedDecrement(&refs_); if (!n) delete this; return n;
    }
    HRESULT STDMETHODCALLTYPE OnStartBinding(DWORD, IBinding*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetPriority(LONG*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE OnLowResource(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnProgress(ULONG progress, ULONG maximum, ULONG, LPCWSTR) override {
        if (hwnd_ && IsWindow(hwnd_) && maximum) {
            int pct = (int)((progress * 90ULL) / maximum);
            PostMessageW(hwnd_, WM_DLSS5_DEPENDENCY_INSTALL_PROGRESS, (WPARAM)pct, 1);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnStopBinding(HRESULT, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetBindInfo(DWORD*, BINDINFO*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE OnDataAvailable(DWORD, DWORD, FORMATETC*, STGMEDIUM*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnObjectAvailable(REFIID, IUnknown*) override { return S_OK; }
private:
    volatile LONG refs_ = 1;
    HWND hwnd_ = nullptr;
};
}

namespace {
static std::wstring PsQuote(const std::wstring& value)
{
    std::wstring out = L"'";
    for (wchar_t ch : value) {
        if (ch == L'\'') out += L"''";
        else out += ch;
    }
    out += L"'";
    return out;
}

static std::wstring EncodePowerShellCommand(const std::wstring& script)
{
    // powershell.exe -EncodedCommand requires Base64 over UTF-16LE bytes.
    static const wchar_t alphabet[] = L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const unsigned char* bytes = reinterpret_cast<const unsigned char*>(script.data());
    const size_t byteCount = script.size() * sizeof(wchar_t);
    std::wstring encoded;
    encoded.reserve(((byteCount + 2) / 3) * 4);
    for (size_t i = 0; i < byteCount; i += 3) {
        const unsigned int b0 = bytes[i];
        const unsigned int b1 = (i + 1 < byteCount) ? bytes[i + 1] : 0;
        const unsigned int b2 = (i + 2 < byteCount) ? bytes[i + 2] : 0;
        const unsigned int triple = (b0 << 16) | (b1 << 8) | b2;
        encoded.push_back(alphabet[(triple >> 18) & 0x3F]);
        encoded.push_back(alphabet[(triple >> 12) & 0x3F]);
        encoded.push_back(i + 1 < byteCount ? alphabet[(triple >> 6) & 0x3F] : L'=');
        encoded.push_back(i + 2 < byteCount ? alphabet[triple & 0x3F] : L'=');
    }
    return encoded;
}

static bool RunPowerShellWait(const std::wstring& script, const std::wstring& workingDir)
{
    // Do not wrap the script in PowerShell quotes: that can make PowerShell
    // evaluate it as a string literal instead of executing it. EncodedCommand
    // safely transports Chinese text, quotes, paths and multiline scripts.
    const std::wstring encoded = EncodePowerShellCommand(script);
    std::wstring command = L"powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -EncodedCommand " + encoded;
    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cmd(command.begin(), command.end()); cmd.push_back(L'\0');
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
        nullptr, workingDir.c_str(), &si, &pi)) return false;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return code == 0;
}

static void AppendNrLiveInstallLog(const std::wstring& path, const std::wstring& message)
{
    // Always write UTF-8 explicitly. The previous wofstream used the active C
    // locale and could fail when Chinese text was encountered, leaving only timestamps.
    SYSTEMTIME st{}; GetLocalTime(&st);
    wchar_t stamp[64]{};
    swprintf_s(stamp, L"[%04u-%02u-%02u %02u:%02u:%02u] ",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    std::wstring line = stamp + message + L"\\r\\n";
    int bytes = WideCharToMultiByte(CP_UTF8, 0, line.data(), (int)line.size(),
        nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return;
    std::string utf8((size_t)bytes, '\\0');
    WideCharToMultiByte(CP_UTF8, 0, line.data(), (int)line.size(),
        &utf8[0], bytes, nullptr, nullptr);
    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
    CloseHandle(file);
}


// Resumable HTTP download. A valid .part is retained after transport interruption.
// If a mirror ignores Range (HTTP 200), the file is restarted safely from byte zero.
static bool NrLiveDownloadResumable(const std::wstring& url, const std::wstring& partPath,
    HWND target, std::wstring& error, ULONGLONG& finalSize)
{
    URL_COMPONENTS uc{}; uc.dwStructSize = sizeof(uc);
    wchar_t host[512]{}; wchar_t path[4096]{}; wchar_t extra[2048]{};
    uc.lpszHostName = host; uc.dwHostNameLength = _countof(host);
    uc.lpszUrlPath = path; uc.dwUrlPathLength = _countof(path);
    uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = _countof(extra);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) { error = L"URL 解析失败"; return false; }
    std::wstring requestPath(path, uc.dwUrlPathLength);
    if (uc.dwExtraInfoLength) requestPath.append(extra, uc.dwExtraInfoLength);

    WIN32_FILE_ATTRIBUTE_DATA fad{}; ULONGLONG offset = 0;
    if (GetFileAttributesExW(partPath.c_str(), GetFileExInfoStandard, &fad) &&
        !(fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        offset = (static_cast<ULONGLONG>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;

    HINTERNET session = WinHttpOpen(L"DLSS5Console/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) { error = L"WinHttpOpen 失败"; return false; }
    WinHttpSetTimeouts(session, 15000, 15000, 30000, 30000);
    HINTERNET connection = WinHttpConnect(session, host, uc.nPort, 0);
    if (!connection) { error = L"WinHttpConnect 失败"; WinHttpCloseHandle(session); return false; }
    DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest(connection, L"GET", requestPath.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request) { error = L"WinHttpOpenRequest 失败"; WinHttpCloseHandle(connection); WinHttpCloseHandle(session); return false; }

    std::wstring headers = L"Accept: */*\r\n";
    if (offset) headers += L"Range: bytes=" + std::to_wstring(offset) + L"-\r\n";
    BOOL ok = WinHttpSendRequest(request, headers.c_str(), (DWORD)-1L,
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(request, nullptr);
    DWORD status = 0, statusSize = sizeof(status);
    if (ok) ok = WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
    if (!ok || (status != 200 && status != 206)) {
        error = L"HTTP GET 请求失败；续传偏移=" + std::to_wstring(offset) +
            L"；HTTP 状态码=" + std::to_wstring(status) +
            L"；WinHTTP 错误=" + std::to_wstring(GetLastError());
        WinHttpCloseHandle(request); WinHttpCloseHandle(connection); WinHttpCloseHandle(session); return false;
    }

    ULONGLONG expectedTotal = 0;
    if (status == 206) {
        wchar_t range[256]{}; DWORD rangeSize = sizeof(range);
        if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_RANGE, WINHTTP_HEADER_NAME_BY_INDEX,
            range, &rangeSize, WINHTTP_NO_HEADER_INDEX)) {
            error = L"服务器返回 206，但缺少 Content-Range";
            WinHttpCloseHandle(request); WinHttpCloseHandle(connection); WinHttpCloseHandle(session); return false;
        }
        std::wstring cr(range);
        const std::wstring prefix = L"bytes " + std::to_wstring(offset) + L"-";
        if (cr.rfind(prefix, 0) != 0) {
            error = L"Content-Range 起始位置不匹配，拒绝拼接；请求偏移=" +
                std::to_wstring(offset) + L"；服务器返回=" + cr;
            WinHttpCloseHandle(request); WinHttpCloseHandle(connection); WinHttpCloseHandle(session); return false;
        }
        size_t slash = cr.find(L'/');
        if (slash != std::wstring::npos && slash + 1 < cr.size()) expectedTotal = _wcstoui64(cr.c_str() + slash + 1, nullptr, 10);
        error = L"HTTP 206；Range 偏移=" + std::to_wstring(offset) + L"；Content-Range=" + cr;
    } else {
        // A server/mirror that ignores Range must never be appended to the old partial file.
        offset = 0;
        DWORD len = 0, lenSize = sizeof(len);
        if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &len, &lenSize, WINHTTP_NO_HEADER_INDEX)) expectedTotal = len;
        error = L"HTTP 200；服务器未接受 Range 或从头下载；Content-Length=" + std::to_wstring(expectedTotal);
    }

    HANDLE file = CreateFileW(partPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
        offset ? OPEN_EXISTING : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE && offset) {
        // If the partial file disappeared between stat and open, restart cleanly.
        offset = 0; file = CreateFileW(partPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    if (file == INVALID_HANDLE_VALUE) {
        error = L"无法打开临时文件，错误=" + std::to_wstring(GetLastError());
        WinHttpCloseHandle(request); WinHttpCloseHandle(connection); WinHttpCloseHandle(session); return false;
    }
    LARGE_INTEGER pos{}; pos.QuadPart = offset;
    SetFilePointerEx(file, pos, nullptr, FILE_BEGIN);
    ULONGLONG received = offset;
    BYTE buffer[128 * 1024]; DWORD bytesRead = 0;
    bool transferOk = true;
    for (;;) {
        if (!WinHttpReadData(request, buffer, sizeof(buffer), &bytesRead)) {
            error = L"网络读取中断，已保留临时文件，错误=" + std::to_wstring(GetLastError());
            transferOk = false; break;
        }
        if (!bytesRead) break;
        DWORD written = 0;
        if (!WriteFile(file, buffer, bytesRead, &written, nullptr) || written != bytesRead) {
            error = L"写入临时文件失败，错误=" + std::to_wstring(GetLastError());
            transferOk = false; break;
        }
        received += written;
        if (target && IsWindow(target) && expectedTotal > 0) {
            ULONGLONG computedPct = (received * 90ULL) / expectedTotal;
            int pct = (int)(computedPct > 89 ? 89 : computedPct);
            PostMessageW(target, WM_DLSS5_DEPENDENCY_INSTALL_PROGRESS, (WPARAM)pct, 1);
        }
    }
    FlushFileBuffers(file); CloseHandle(file);
    WinHttpCloseHandle(request); WinHttpCloseHandle(connection); WinHttpCloseHandle(session);
    finalSize = received;
    if (!transferOk) {
        error += L"；已写入 .part 总大小=" + std::to_wstring(received);
        return false;
    }
    if (expectedTotal && received != expectedTotal) {
        error = L"下载长度不匹配；响应预期总大小=" + std::to_wstring(expectedTotal) +
            L"；实际 .part 总大小=" + std::to_wstring(received);
        return false;
    }
    error += L"；下载完成，总大小=" + std::to_wstring(received);
    return true;
}

static bool FindNrLiveExeRecursive(const std::wstring& root, std::wstring& foundExe, int depth = 0)
{
    if (depth > 8) return false;
    const std::wstring pattern = root + L"\\*";
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
            continue;
        const std::wstring full = root + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (FindNrLiveExeRecursive(full, foundExe, depth + 1)) { found = true; break; }
        } else if (_wcsicmp(fd.cFileName, L"NRLive.exe") == 0) {
            foundExe = full;
            found = true;
            break;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

static void InstallNrLiveWorker(HWND target, std::wstring appDir, std::wstring cacheDir)
{
    const std::wstring asset = L"https://github.com/a756598009-cmyk/DLSS5-console/releases/download/v1.0/nrlive.zip";
    const std::wstring routes[] = {
        L"https://gh-proxy.com/" + asset,
        L"https://ghfast.top/" + asset,
        L"https://ghproxy.net/" + asset,
        asset
    };
    const std::wstring zipPath = cacheDir + L"\\nrlive.zip";
    const std::wstring partPath = zipPath + L".part";
    const std::wstring stageDir = cacheDir + L"\\nrlive.installing";
    const std::wstring finalDir = cacheDir + L"\\nrlive\\nrlive";
    const std::wstring logPath = cacheDir + L"\\nrlive-install.log";
    AppendNrLiveInstallLog(logPath, L"开始安装 NRLive 依赖");
    SHCreateDirectoryExW(nullptr, cacheDir.c_str(), nullptr);

    // Recovery is idempotent: keep a valid final install, otherwise recover a
    // completed staging directory before attempting another network download.
    auto hasExe = [](const std::wstring& dir) -> bool {
        std::wstring found;
        return FindNrLiveExeRecursive(dir, found);
    };
    bool installed = hasExe(finalDir);
    if (!installed && hasExe(stageDir)) {
        if (GetFileAttributesW(finalDir.c_str()) != INVALID_FILE_ATTRIBUTES) {
            std::wstring cleanup = L"Remove-Item -LiteralPath " + PsQuote(finalDir) + L" -Recurse -Force -ErrorAction SilentlyContinue;";
            RunPowerShellWait(cleanup, appDir);
        }
        installed = MoveFileExW(stageDir.c_str(), finalDir.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE && hasExe(finalDir);
    }

    // Preserve a partial download for HTTP Range resume on the next attempt.
    // A complete staging directory was recovered above; remaining staging output is disposable.
    WIN32_FILE_ATTRIBUTE_DATA partialData{};
    if (GetFileAttributesExW(partPath.c_str(), GetFileExInfoStandard, &partialData)) {
        ULONGLONG partialBytes = (static_cast<ULONGLONG>(partialData.nFileSizeHigh) << 32) | partialData.nFileSizeLow;
        AppendNrLiveInstallLog(logPath, L"发现上次未完成下载，保留 .part，当前已有 " + std::to_wstring(partialBytes) + L" 字节，准备续传");
    }
    if (!installed && GetFileAttributesW(stageDir.c_str()) != INVALID_FILE_ATTRIBUTES) {
        std::wstring cleanupStage = L"Remove-Item -LiteralPath " + PsQuote(stageDir) +
            L" -Recurse -Force -ErrorAction SilentlyContinue";
        RunPowerShellWait(cleanupStage, appDir);
        AppendNrLiveInstallLog(logPath, L"清理上次遗留的不完整解压目录");
    }
    auto validZip = [&](const std::wstring& path) -> bool {
        // Validate the central directory and require NRLive.exe in the archive
        // before accepting the download. Also force every entry stream to be
        // read so truncated/corrupt compressed data is rejected.
        std::wstring check =
            L"$ErrorActionPreference='Stop';Add-Type -AssemblyName System.IO.Compression.FileSystem;$a=$null;"
            L"try{$a=[IO.Compression.ZipFile]::OpenRead(" + PsQuote(path) + L");"
            L"if($a.Entries.Count -le 0){throw 'ZIP 无文件'};"
            L"$exe=$a.Entries | Where-Object {$_.Name -ieq 'NRLive.exe' -and $_.Length -gt 0 -and !$_.FullName.EndsWith('/')} | Select-Object -First 1;"
            L"if($null -eq $exe){throw 'ZIP 内没有有效的 NRLive.exe 文件条目'};"
            L"Add-Content -LiteralPath " + PsQuote(logPath) + L" -Encoding UTF8 -Value ('ZIP 中 NRLive.exe 条目路径：' + $exe.FullName + '；大小：' + $exe.Length);"
            L"foreach($e in $a.Entries){if(!$e.FullName.EndsWith('/')){$st=$e.Open();try{$buf=New-Object byte[] 65536;while($st.Read($buf,0,$buf.Length) -gt 0){}}finally{$st.Dispose()}}};"
            L"$a.Dispose();exit 0}catch{if($a){$a.Dispose()};"
            L"$msg='ZIP 校验异常: '+$_.Exception.ToString();"
            L"Add-Content -LiteralPath " + PsQuote(logPath) + L" -Encoding UTF8 -Value $msg;"
            L"exit 1}";
        bool ok = RunPowerShellWait(check, appDir);
        AppendNrLiveInstallLog(logPath, ok ? L"ZIP 校验脚本返回成功" : L"ZIP 校验脚本返回失败；详细 PowerShell 异常已写入日志");
        return ok;
    };

    auto cleanupIncompleteArtifacts = [&](bool removeZipIfInvalid, bool removePartial = true) {
        // Keep a resumable .part after network failures; remove it only for a known corrupt artifact.
        if (removePartial) DeleteFileW(partPath.c_str());
        if (removeZipIfInvalid &&
            GetFileAttributesW(zipPath.c_str()) != INVALID_FILE_ATTRIBUTES &&
            !validZip(zipPath)) {
            DeleteFileW(zipPath.c_str());
            AppendNrLiveInstallLog(logPath, L"已删除校验失败的 nrlive.zip");
        }
        std::wstring cleanup = L"$ErrorActionPreference='SilentlyContinue';if(Test-Path -LiteralPath " +
            PsQuote(stageDir) + L"){Remove-Item -LiteralPath " + PsQuote(stageDir) + L" -Recurse -Force}";
        RunPowerShellWait(cleanup, appDir);
        // Never touch a working installation. Remove only a broken final folder.
        if (!hasExe(finalDir) && GetFileAttributesW(finalDir.c_str()) != INVALID_FILE_ATTRIBUTES) {
            std::wstring cleanupFinal = L"Remove-Item -LiteralPath " + PsQuote(finalDir) +
                L" -Recurse -Force -ErrorAction SilentlyContinue";
            RunPowerShellWait(cleanupFinal, appDir);
            AppendNrLiveInstallLog(logPath, L"已删除不完整的正式安装目录（未发现 NRLive.exe）");
        }
    };

    if (!installed && GetFileAttributesW(zipPath.c_str()) != INVALID_FILE_ATTRIBUTES && !validZip(zipPath)) {
        AppendNrLiveInstallLog(logPath, L"缓存 ZIP 校验失败，删除后重新下载");
        DeleteFileW(zipPath.c_str());
    }

    bool downloaded = installed || (GetFileAttributesW(zipPath.c_str()) != INVALID_FILE_ATTRIBUTES && validZip(zipPath));
    if (!installed && !downloaded) {
        for (size_t routeIndex = 0; routeIndex < _countof(routes); ++routeIndex) {
            const auto& url = routes[routeIndex];
            AppendNrLiveInstallLog(logPath, L"========== 开始线路 " + std::to_wstring(routeIndex + 1) + L"/4 ==========");
            AppendNrLiveInstallLog(logPath, L"请求 URL：" + url);
            if (target && IsWindow(target)) PostMessageW(target, WM_DLSS5_DEPENDENCY_INSTALL_PROGRESS, 0, 1);

            // The actual GET status is authoritative; do not gate on a separate HEAD request.
            std::wstring transferError;
            ULONGLONG actualSize = 0;
            if (!NrLiveDownloadResumable(url, partPath, target, transferError, actualSize)) {
                AppendNrLiveInstallLog(logPath, L"GET/续传失败：" + transferError + L"；保留 .part 供下次续传");
                // If this was an HTTP/protocol rejection rather than a transport
                // interruption, the next mirror may still work; keep bytes only
                // when the server can safely validate the requested range.
                continue;
            }
            AppendNrLiveInstallLog(logPath, L"GET 下载完成：" + transferError +
                L"；实际文件大小=" + std::to_wstring(actualSize) + L" 字节");

            if (actualSize < 22 || !validZip(partPath)) {
                AppendNrLiveInstallLog(logPath, L"ZIP 中央目录/数据校验失败，或压缩包内没有 NRLive.exe；删除损坏临时文件并切换线路");
                DeleteFileW(partPath.c_str());
                continue;
            }
            AppendNrLiveInstallLog(logPath, L"ZIP 中央目录、压缩数据及 NRLive.exe 验证通过");
            if (MoveFileExW(partPath.c_str(), zipPath.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                downloaded = true;
                break;
            }
            AppendNrLiveInstallLog(logPath, L"无法将验证通过的 ZIP 写入缓存目录");
            DeleteFileW(partPath.c_str());
        }
    }

    if (!installed && !downloaded) {
        AppendNrLiveInstallLog(logPath, L"四条下载线路均失败，清理不完整解压目录；保留 .part 供下次 HTTP Range 续传");
        cleanupIncompleteArtifacts(true, false);
    }

    if (!installed && downloaded) {
        if (target && IsWindow(target)) PostMessageW(target, WM_DLSS5_DEPENDENCY_INSTALL_PROGRESS, 92, 2);
        // Resolve NRLive.exe from its exact ZIP entry path before/after extraction.
        // Supports both root-level NRLive.exe and the observed nrlive/NRLive.exe layout.
        std::wstring script =
            L"$ErrorActionPreference='Stop';"
            L"$z=" + PsQuote(zipPath) + L";"
            L"$f=" + PsQuote(finalDir) + L";"
            L"Add-Type -AssemblyName System.IO.Compression.FileSystem;"
            L"try{"
            L"$a=[IO.Compression.ZipFile]::OpenRead($z);"
            L"try{$entry=$a.Entries | Where-Object {$_.Name -ieq 'NRLive.exe' -and $_.Length -gt 0 -and !$_.FullName.EndsWith('/')} | Select-Object -First 1;"
            L"if($null -eq $entry){throw 'ZIP 中找不到有效的 NRLive.exe 文件条目'};"
            L"$relative=$entry.FullName.Replace('/',[string][IO.Path]::DirectorySeparatorChar);"
            L"$expected=Join-Path $f $relative;$entryName=$entry.FullName;$entryLength=$entry.Length}finally{$a.Dispose()};"
            L"if(Test-Path -LiteralPath $f){Remove-Item -LiteralPath $f -Recurse -Force};"
            L"New-Item -ItemType Directory -Path $f -Force | Out-Null;"
            L"[IO.Compression.ZipFile]::ExtractToDirectory($z,$f);"
            L"if(!(Test-Path -LiteralPath $expected -PathType Leaf)){throw ('解压后未找到 ZIP 对应路径：' + $expected)};"
            L"$item=Get-Item -LiteralPath $expected;if($item.Length -le 0){throw '解压后的 NRLive.exe 文件大小为 0'};"
            L"Add-Content -LiteralPath " + PsQuote(logPath) + L" -Encoding UTF8 -Value ('ZIP 条目路径：' + $entryName + '；条目大小：' + $entryLength + '；解压文件：' + $expected + '；实际大小：' + $item.Length);"
            L"}catch{"
            L"$msg='直接解压失败: '+$_.Exception.ToString();"
            L"Add-Content -LiteralPath " + PsQuote(logPath) + L" -Encoding UTF8 -Value $msg;"
            L"exit 1}";
        AppendNrLiveInstallLog(logPath, L"ZIP 已验证，开始直接解压到缓存\\nrlive 目录");
        const bool extractOk = RunPowerShellWait(script, appDir);
        if (target && IsWindow(target)) PostMessageW(target, WM_DLSS5_DEPENDENCY_INSTALL_PROGRESS, 99, 3);
        installed = extractOk && hasExe(finalDir);
        if (installed) AppendNrLiveInstallLog(logPath, L"安装成功：已按 ZIP 内部真实路径定位 NRLive.exe，DLSS5 开关将直接调用此文件");
        else AppendNrLiveInstallLog(logPath, L"安装失败：ZIP 解压后整个 nrlive 目录树中都没有 NRLive.exe");
    }

    if (!installed) {
        cleanupIncompleteArtifacts(true, !(!downloaded && GetFileAttributesW(partPath.c_str()) != INVALID_FILE_ATTRIBUTES));
        AppendNrLiveInstallLog(logPath, L"本次安装结束：失败。网络未完成的 .part 保留供续传；损坏文件及不完整解压目录已清理");
    }
    if (target && IsWindow(target))
        PostMessageW(target, WM_DLSS5_DEPENDENCY_INSTALL_RESULT, installed ? 1 : 0, 0);
}

struct UpdateCheckResult {
    bool success = false;
    bool silent = false;
    std::wstring version;
    std::wstring notes;
};
static std::string JsonReadString(const std::string& json, const char* key)
{
    const std::string needle = std::string("\"") + key + "\"";
    size_t p = json.find(needle);
    if (p == std::string::npos) return {};
    p = json.find(':', p + needle.size());
    if (p == std::string::npos) return {};
    p = json.find('"', p + 1);
    if (p == std::string::npos) return {};
    ++p;
    std::string out;
    for (; p < json.size(); ++p) {
        char c = json[p];
        if (c == '"') break;
        if (c == '\\' && p + 1 < json.size()) {
            char e = json[++p];
            switch (e) {
            case 'n': out += "\r\n"; break;
            case 'r': break;
            case 't': out += "    "; break;
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            default: out += e; break;
            }
        } else out += c;
    }
    return out;
}

static std::wstring Utf8ToWide(const std::string& value)
{
    if (value.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, value.data(), (int)value.size(), nullptr, 0);
    if (n <= 0) return L"";
    std::wstring out((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), (int)value.size(), &out[0], n);
    return out;
}

static bool HttpGetText(const wchar_t* host, const wchar_t* path, std::string& body)
{
    HINTERNET session = WinHttpOpen(L"DLSS5Console/1.0",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return false;
    WinHttpSetTimeouts(session, 4000, 4000, 6000, 6000);
    HINTERNET connect = WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connect) { WinHttpCloseHandle(session); return false; }
    HINTERNET request = WinHttpOpenRequest(connect, L"GET", path,
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request) { WinHttpCloseHandle(connect); WinHttpCloseHandle(session); return false; }

    BOOL ok = WinHttpAddRequestHeaders(request,
        L"User-Agent: DLSS5Console/1.0\r\nAccept: application/vnd.github+json\r\n",
        (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD) &&
        WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr);

    DWORD status = 0, statusSize = sizeof(status);
    if (ok) WinHttpQueryHeaders(request,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);

    if (ok && status == 200) {
        DWORD available = 0;
        do {
            available = 0;
            if (!WinHttpQueryDataAvailable(request, &available) || !available) break;
            std::string chunk(available, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(request, &chunk[0], available, &read) || !read) break;
            chunk.resize(read);
            body += chunk;
        } while (available > 0 && body.size() < 2 * 1024 * 1024);
    }
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return ok && status == 200 && !body.empty();
}

static bool FetchLatestRelease(std::wstring& tag, std::wstring& notes)
{
    // Mainland China fallback order: GH-Proxy -> GHFast -> GHProxy.net -> official GitHub API.
    struct Route { const wchar_t* host; const wchar_t* path; };
    static const Route routes[] = {
        { L"gh-proxy.com", L"/https://api.github.com/repos/a756598009-cmyk/DLSS5-console/releases/latest" },
        { L"ghfast.top",   L"/https://api.github.com/repos/a756598009-cmyk/DLSS5-console/releases/latest" },
        { L"ghproxy.net",  L"/https://api.github.com/repos/a756598009-cmyk/DLSS5-console/releases/latest" },
        { L"api.github.com", L"/repos/a756598009-cmyk/DLSS5-console/releases/latest" }
    };
    for (const auto& route : routes) {
        std::string body;
        if (!HttpGetText(route.host, route.path, body)) continue;
        std::string tag8 = JsonReadString(body, "tag_name");
        std::string notes8 = JsonReadString(body, "body");
        if (tag8.empty()) continue; // invalid/HTML proxy response: try next route
        std::wstring candidateTag = Utf8ToWide(tag8);
        if (candidateTag.empty()) continue;
        tag = candidateTag;
        notes = Utf8ToWide(notes8);
        if (notes.empty()) notes = L"此版本没有填写更新说明。";
        return true; // stop at first valid release payload
    }
    return false;
}

static bool IsNewerVersion(const std::wstring& remote, const std::wstring& local)
{
    auto parse = [](const std::wstring& s) {
        std::vector<unsigned long long> v;
        size_t i = 0;
        while (i < s.size() && !(s[i] >= L'0' && s[i] <= L'9')) ++i;
        while (i < s.size()) {
            if (!(s[i] >= L'0' && s[i] <= L'9')) { ++i; continue; }
            unsigned long long x = 0;
            while (i < s.size() && s[i] >= L'0' && s[i] <= L'9') {
                x = x * 10 + (unsigned)(s[i] - L'0');
                ++i;
            }
            v.push_back(x);
        }
        return v;
    };
    auto a = parse(remote), b = parse(local);
    if (a.empty() || b.empty()) return remote != local;
    size_t n = (std::max)(a.size(), b.size());
    a.resize(n, 0); b.resize(n, 0);
    for (size_t i = 0; i < n; ++i) {
        if (a[i] != b[i]) return a[i] > b[i];
    }
    return false;
}
}


}

BEGIN_MESSAGE_MAP(CDlss5SwapperDlg, CDialog)
    ON_WM_PAINT()
    ON_WM_ERASEBKGND()
    ON_WM_LBUTTONDOWN()
    ON_WM_LBUTTONUP()
    ON_WM_MOUSEMOVE()
    ON_WM_MOUSEWHEEL()
    ON_WM_KEYDOWN()
    ON_WM_SYSKEYDOWN()
    ON_WM_CHAR()
    ON_WM_SIZE()
    ON_WM_SIZING()
    ON_WM_NCHITTEST()
    ON_WM_GETMINMAXINFO()
    ON_WM_DESTROY()
    ON_WM_TIMER()
    ON_MESSAGE(WM_HOTKEY, OnHotKey)
    ON_MESSAGE(WM_DLSS5_UPDATE_CHECK_RESULT, OnUpdateCheckResult)
    ON_MESSAGE(WM_DLSS5_DEPENDENCY_INSTALL_RESULT, OnDependencyInstallResult)
    ON_MESSAGE(WM_DLSS5_DEPENDENCY_INSTALL_PROGRESS, OnDependencyInstallProgress)
END_MESSAGE_MAP()

CDlss5SwapperDlg::CDlss5SwapperDlg(CWnd* pParent) : CDialog(IDD_DLSS5_SWAPPER_DIALOG,pParent) {}
CDlss5SwapperDlg::~CDlss5SwapperDlg() { if(m_gdiplusToken) GdiplusShutdown(m_gdiplusToken); }

BOOL CDlss5SwapperDlg::OnInitDialog()
{
    CDialog::OnInitDialog();
    HICON hDlss5Icon = AfxGetApp()->LoadIcon(IDR_MAINFRAME);
    if (hDlss5Icon)
    {
        SetIcon(hDlss5Icon, TRUE);
        SetIcon(hDlss5Icon, FALSE);
    }
    GdiplusStartupInput in; GdiplusStartup(&m_gdiplusToken,&in,nullptr);
    // Use a completely borderless popup. The purple rounded outline is the
    // actual outer silhouette; do not keep the native white sizing frame.
    // Resizing is provided by our own WM_NCHITTEST edge zones below.
    ModifyStyle(WS_CAPTION|WS_BORDER|WS_THICKFRAME|WS_SYSMENU|WS_MINIMIZEBOX|WS_MAXIMIZEBOX, WS_POPUP, SWP_FRAMECHANGED);
    ModifyStyleEx(WS_EX_CLIENTEDGE|WS_EX_WINDOWEDGE,0,SWP_FRAMECHANGED);
    const int screenW = GetSystemMetrics(SM_CXSCREEN);
    const int screenH = GetSystemMetrics(SM_CYSCREEN);
    // The visible purple outline is the actual window silhouette. Keep the
    // outer window at the design canvas aspect ratio (600:1240), so the
    // centered uniform-scale renderer cannot leave black side gutters.
    constexpr double designAspect = 600.0 / 1240.0;
    const int maxH = (std::min)(1450, (std::max)(900, screenH - 100));
    const int maxW = (std::max)(500, screenW - 100);
    int startH = maxH;
    int startW = static_cast<int>(std::lround(startH * designAspect));
    if (startW > maxW) {
        startW = maxW;
        startH = static_cast<int>(std::lround(startW / designAspect));
    }
    const int startX = (screenW - startW) / 2;
    const int startY = (screenH - startH) / 2;
    SetWindowPos(nullptr,startX,startY,startW,startH,SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED);
    ApplyRoundedWindowRegion();
    // Disable the DWM outline as well: the app-drawn purple stroke is the only
    // visible border. The OS must not add a second rectangular frame.
    const COLORREF noBorder = 0xFFFFFFFE; // DWMWA_COLOR_NONE
    DwmSetWindowAttribute(GetSafeHwnd(), 34, &noBorder, sizeof(noBorder)); // DWMWA_BORDER_COLOR
    const DWORD noDwmRounding = 1;
    DwmSetWindowAttribute(GetSafeHwnd(), 33, &noDwmRounding, sizeof(noDwmRounding)); // DWMWA_WINDOW_CORNER_PREFERENCE: DONOTROUND
    m_slider.fill(0.5f);
    m_scrollOffset = 0;
    UpdateScrollRange();
    FindNrLive();
    LoadSettings();
    UpdateScrollRange();
    RegisterHotkeys();
    SetTimer(10, 500, nullptr);
    StartUpdateCheck(true); // 启动后自动后台检查；无更新/网络失败不打扰用户
    return TRUE;
}
BOOL CDlss5SwapperDlg::OnEraseBkgnd(CDC* pDC)
{
    // The UI uses centered uniform scaling. Paint the letterboxed client area
    // with the same dark base color so the unused margins never show the
    // default white MFC dialog background while resizing or maximizing.
    if (pDC && GetSafeHwnd()) {
        CRect rc;
        GetClientRect(&rc);
        pDC->FillSolidRect(&rc, RGB(17, 15, 23));
    }
    return TRUE;
}

LRESULT CDlss5SwapperDlg::OnUpdateCheckResult(WPARAM, LPARAM lParam)
{
    std::unique_ptr<UpdateCheckResult> result(reinterpret_cast<UpdateCheckResult*>(lParam));
    m_updateChecking = false;
    SetStatus(L"已就绪");
    const std::wstring localVersion = L"v1.0";
    if (!result || !result->success) {
        if (!result || !result->silent) {
            MessageBoxW(L"暂时无法连接更新服务器，请检查网络后重试。", L"检查更新失败",
                MB_OK | MB_ICONWARNING);
        }
        return 0;
    }
    if (!IsNewerVersion(result->version, localVersion)) {
        if (!result->silent) {
            std::wstring msg = L"当前版本：";
            msg += localVersion;
            msg += L"\n已是最新版本。";
            MessageBoxW(msg.c_str(), L"检查更新", MB_OK | MB_ICONINFORMATION);
        }
        return 0;
    }
    std::wstring msg = L"发现新版本：";
    msg += result->version;
    msg += L"\n\n更新内容：\n";
    msg += result->notes;
    msg += L"\n\n点击“确定”前往获取新版本。";
    if (MessageBoxW(msg.c_str(), L"发现新版本",
        MB_OKCANCEL | MB_ICONINFORMATION) == IDOK) {
        ShellExecuteW(m_hWnd, L"open",
            L"https://pan.quark.cn/s/440c751e1165?pwd=kBUv",
            nullptr, nullptr, SW_SHOWNORMAL);
    }
    return 0;
}

void CDlss5SwapperDlg::StartUpdateCheck(bool silent)
{
    if (m_updateChecking || !GetSafeHwnd())
        return;

    m_updateChecking = true;
    if (!silent)
        SetStatus(L"正在检查更新…");

    const HWND target = GetSafeHwnd();
    std::thread([target, silent]() {
        auto* result = new UpdateCheckResult();
        result->silent = silent;
        result->success = FetchLatestRelease(result->version, result->notes);
        if (!::IsWindow(target) ||
            !::PostMessageW(target, WM_DLSS5_UPDATE_CHECK_RESULT, 0,
                reinterpret_cast<LPARAM>(result))) {
            delete result;
        }
    }).detach();
}

void CDlss5SwapperDlg::OnDestroy()
{
    KillTimer(10);
    UnregisterHotKey(GetSafeHwnd(),1);
    if(m_nrLiveProcess) { CloseHandle(m_nrLiveProcess); m_nrLiveProcess=nullptr; }
    CDialog::OnDestroy();
}
void CDlss5SwapperDlg::OnTimer(UINT_PTR nIDEvent)
{
    if(nIDEvent==10) {
        bool repaint = false;
        if (m_startCountdownActive) {
            repaint = true;
            if (GetTickCount64() >= m_startCountdownEnd)
                m_startCountdownActive = false;
        }
        if(m_nrLiveProcess) {
            DWORD wait=WaitForSingleObject(m_nrLiveProcess,0);
            if(wait==WAIT_OBJECT_0) {
                DWORD exitCode=0; GetExitCodeProcess(m_nrLiveProcess,&exitCode);
                CloseHandle(m_nrLiveProcess); m_nrLiveProcess=nullptr;
                m_startSwitchOn=false;
                m_startCountdownActive=false;
                SetStatus(L"NRLive 已退出，退出代码："+Num((int)exitCode));
            } else if(wait==WAIT_FAILED) {
                CloseHandle(m_nrLiveProcess); m_nrLiveProcess=nullptr;
                m_startSwitchOn=false;
                m_startCountdownActive=false;
                SetStatus(L"NRLive 进程状态不可用，已释放会话句柄");
            }
        }
        if (repaint) Invalidate(FALSE);
    }
    CDialog::OnTimer(nIDEvent);
}

LRESULT CDlss5SwapperDlg::OnHotKey(WPARAM id, LPARAM)
{
    if(id==1) StartNrLive();
    return 0;
}

std::wstring CDlss5SwapperDlg::GetAppDirectory() const
{
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir(exe);
    const auto slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) dir.resize(slash);
    return dir;
}

std::wstring CDlss5SwapperDlg::GetCacheDirectory() const
{
    // Keep downloaded runtime files in the user's dedicated local application
    // data cache, not beside the executable (which may be under Program Files).
    PWSTR localAppData = nullptr;
    std::wstring base;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &localAppData)) && localAppData) {
        base.assign(localAppData);
        CoTaskMemFree(localAppData);
    } else {
        wchar_t fallback[MAX_PATH]{};
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", fallback, MAX_PATH) > 0)
            base = fallback;
    }
    if (base.empty())
        base = GetAppDirectory(); // Last-resort fallback only if Windows gives no profile path.
    return base + L"\\DLSS5Console\\Cache";
}

bool CDlss5SwapperDlg::HasLocalNrLive() const
{
    std::wstring found;
    return FindNrLiveExeRecursive(GetCacheDirectory() + L"\\nrlive\\nrlive", found);
}

void CDlss5SwapperDlg::InstallDependencies()
{
    if (m_installingDependencies) return;
    m_installingDependencies = true;
    m_dependencyProgress = 0;
    m_dependencyStage = 1;
    SetStatus(L"正在安装 NRLive 依赖：依次尝试四条下载线路…");
    Invalidate(FALSE);
    UpdateWindow();

    const HWND target = GetSafeHwnd();
    const std::wstring appDir = GetAppDirectory();
    const std::wstring cacheDir = GetCacheDirectory();
    std::thread([target, appDir, cacheDir]() {
        InstallNrLiveWorker(target, appDir, cacheDir);
    }).detach();
}

LRESULT CDlss5SwapperDlg::OnDependencyInstallProgress(WPARAM wParam, LPARAM lParam)
{
    const int nextProgress = (int)wParam;
    const int nextStage = (int)lParam;
    // Ignore duplicate callbacks so a busy download cannot flood the UI queue.
    if (nextProgress == m_dependencyProgress && nextStage == m_dependencyStage)
        return 0;

    m_dependencyProgress = nextProgress;
    m_dependencyStage = nextStage;

    // Redraw only the progress panel, not the entire console. Convert its
    // design-canvas coordinates to client pixels using the same transform
    // as DrawInterface().
    CRect client;
    GetClientRect(&client);
    const double scale = (std::min)((double)client.Width() / DW, (double)client.Height() / DH);
    const double offsetX = (client.Width() - DW * scale) * 0.5;
    const double offsetY = (client.Height() - DH * scale) * 0.5;
    CRect dirty(
        (int)floor(offsetX + 34.0 * scale),
        (int)floor(offsetY + 274.0 * scale),
        (int)ceil(offsetX + 555.0 * scale),
        (int)ceil(offsetY + 319.0 * scale));
    InvalidateRect(&dirty, FALSE);
    return 0;
}

LRESULT CDlss5SwapperDlg::OnDependencyInstallResult(WPARAM wParam, LPARAM)
{
    m_installingDependencies = false;
    m_dependencyProgress = (wParam != 0) ? 100 : -1;
    m_dependencyStage = 3;
    if (wParam != 0 && HasLocalNrLive()) {
        FindNrLive();
        SetStatus(L"NRLive 依赖安装成功，已使用本地缓存");
        MessageBoxW(L"NRLive 依赖已成功安装到当前用户的本地缓存目录。", L"安装完成", MB_OK | MB_ICONINFORMATION);
    } else {
        SetStatus(L"依赖安装失败，请检查网络后重试");
        MessageBoxW(L"依赖安装失败。请查看本地缓存目录中的 nrlive-install.log，里面记录了下载线路、文件大小、ZIP 校验和解压错误。", L"安装依赖失败", MB_OK | MB_ICONWARNING);
    }
    Invalidate(FALSE);
    return 0;
}

void CDlss5SwapperDlg::FindNrLive()
{
    // Extract the archive as-is. NRLive.exe may be at the archive root or inside
    // one or more folders; use the actual extracted executable and its directory.
    const std::wstring installRoot = GetCacheDirectory() + L"\\nrlive\\nrlive";
    if (GetFileAttributesW(installRoot.c_str()) == INVALID_FILE_ATTRIBUTES)
        SHCreateDirectoryExW(nullptr, installRoot.c_str(), nullptr);
    std::wstring foundExe;
    if (FindNrLiveExeRecursive(installRoot, foundExe)) {
        m_nrLiveExe = foundExe;
        const size_t slash = foundExe.find_last_of(L"\\/");
        m_configDir = slash == std::wstring::npos ? installRoot : foundExe.substr(0, slash);
    } else {
        m_configDir = installRoot;
        m_nrLiveExe = installRoot + L"\\NRLive.exe";
    }
    m_configPath = m_configDir + L"\\scaleconfig.ini";
    m_upscalerSupported = ExeSupportsUpscaler(m_nrLiveExe);
}
std::wstring CDlss5SwapperDlg::IniRead(const wchar_t* sec,const wchar_t* key,const wchar_t* fallback)
{
    wchar_t b[256]{}; GetPrivateProfileStringW(sec,key,fallback,b,256,m_configPath.c_str()); return b;
}
void CDlss5SwapperDlg::IniWrite(const wchar_t* sec,const wchar_t* key,const std::wstring& value)
{
    WritePrivateProfileStringW(sec,key,value.c_str(),m_configPath.c_str());
}
void CDlss5SwapperDlg::LoadSettings()
{
    m_sharpness=_wtoi(IniRead(L"FSR",L"sharpness",L"65").c_str());
    m_frameGeneration=IniRead(L"FrameGeneration",L"enabled",L"0")!=L"0";
    m_frameLimiter=IniRead(L"FrameLimiter",L"enabled",L"0")!=L"0";
    m_fpsLimit=_wtoi(IniRead(L"FrameLimiter",L"fps",L"60").c_str());
    m_limiterMethod=_wtoi(IniRead(L"FrameLimiter",L"method",L"0").c_str());
    m_upscaler=LoadProfileUpscaler(m_configDir,m_upscaler);
    m_captureMode=IniRead(L"NRLive",L"capture_mode",L"wgc")==L"dxgi"?1:0;
    m_motionMode=IniRead(L"NRLive",L"motion",L"fast")==L"amdof"?1:0;
    m_targetMode=_wtoi(IniRead(L"NRLive",L"target_mode",L"0").c_str());
    m_targetMode=std::clamp(m_targetMode,0,2);
    m_targetText=IniRead(L"NRLive",L"target_text",L"");
    m_targetLabel=IniRead(L"NRLive",L"target_label",m_targetText.empty()?L"":m_targetText.c_str());
    m_delay=std::clamp(_wtoi(IniRead(L"NRLive",L"delay",L"5").c_str()),1,30);
    m_runAsAdmin=IniRead(L"NRLive",L"run_as_admin",L"1")!=L"0";
    m_scaleKey=LoadProfileString(m_configDir,"scale_hotkey",L"ctrl+alt+s");
    m_stopKey=LoadProfileString(m_configDir,"stop_key",L"ctrl+shift+a");
    m_overlayKey=LoadProfileString(m_configDir,"overlay_key",L"ctrl+home");
    m_bypassKeys=LoadProfileString(m_configDir,"bind_bypass",L"home,insert,end,pageup,pagedown");
    m_nrEnabled=NrParseBool(IniRead(L"DLSSNR",L"enabled",L"0"),0);
    LoadNrSettings();
    m_originalModel=IniRead(L"OpenNR",L"MochizukiOriginalModel",L"0")!=L"0";
    m_nrStyle=_wtoi(IniRead(L"OpenNR",L"MochizukiRenoStyle",L"0").c_str());
    m_nrStrength=_wtoi(IniRead(L"OpenNR",L"MochizukiRenoIntensity",L"100").c_str());
    m_nrPasses=_wtoi(IniRead(L"OpenNR",L"MochizukiRenoPasses",L"1").c_str());
    m_nrLocalTone=_wtoi(IniRead(L"OpenNR",L"MochizukiRenoLocalTone",L"100").c_str());
    m_nrLocalStructure=_wtoi(IniRead(L"OpenNR",L"MochizukiRenoLocalStructure",L"100").c_str());
    m_nrSkinStructure=_wtoi(IniRead(L"OpenNR",L"MochizukiRenoSkinStructure",L"100").c_str());
    m_nrAutoMask=IniRead(L"OpenNR",L"MochizukiRenoAutoMask",L"0")!=L"0";
    m_nrTemporal=IniRead(L"OpenNR",L"MochizukiTemporal",L"1")!=L"0";
    m_nrHistory=_wtoi(IniRead(L"OpenNR",L"MochizukiHistoryStrength",L"100").c_str());
    m_nrStyleGrade=IniRead(L"OpenNR",L"MochizukiStyleGrade",L"1")!=L"0";
    // Overlay visibility is stored in the profile (-nooverlay); FPS.enabled independently toggles the counter.
    m_overlayEnabled=LoadProfileOverlayEnabled(m_configDir,true);
    m_hudCounterEnabled=NrParseBool(IniRead(L"FPS",L"enabled",L"1"),1)!=0;
    m_hudNew = ExeSupportsIniKey(m_nrLiveExe, L"FPS", L"preset");
    m_hudPreset=m_hudNew?_wtoi(IniRead(L"FPS",L"preset",L"3").c_str()):0;
    m_hudLayout=_wtoi(IniRead(L"FPS",L"layout",L"0").c_str());
    m_hudPosition=_wtoi(IniRead(L"FPS",L"position",L"0").c_str());
    m_hudScale=_wtoi(IniRead(L"FPS",L"scale",L"100").c_str());
    m_hudFontSize=_wtoi(IniRead(L"FPS",L"font_size",L"24").c_str());
    m_hudBackgroundAlpha=_wtoi(IniRead(L"FPS",L"background_alpha",m_hudNew?L"80":L"50").c_str());
    m_hudBackground=IniRead(L"FPS",L"background",L"1")!=L"0";
    m_hudTextOutline=IniRead(L"FPS",L"text_outline",L"1")!=L"0";
    const wchar_t* keys[]={L"fps",L"title",L"fps_detail",L"frametime",L"frame_timing",L"lows",L"gpu_load",L"gpu_temp",L"vram",L"cpu_load",L"pipeline",L"resolution",L"gpu_clock",L"ram",L"gpu_times",L"latency",L"clock"};
    const bool defaults[]={true,true,true,true,true,true,true,true,true,true,true,true,false,false,false,false,false};
    for(int i=0;i<17;i++) m_hudItems[i]=NrParseBool(IniRead(L"FPS",keys[i],defaults[i]?L"1":L"0"),defaults[i]?1:0)!=0;
    m_hudTileMap.clear(); m_hudTileMap.push_back(-1); // FPS.enabled
    if(m_hudNew) {
        if(ExeSupportsIniKey(m_nrLiveExe,L"FPS",L"pipeline"))m_hudTileMap.push_back(10);
        if(ExeSupportsIniKey(m_nrLiveExe,L"FPS",L"gpu_load"))m_hudTileMap.push_back(6);
        if(ExeSupportsIniKey(m_nrLiveExe,L"FPS",L"gpu_temp"))m_hudTileMap.push_back(7);
        m_hudTileMap.push_back(0); m_hudTileMap.push_back(3); m_hudTileMap.push_back(4);
        if(ExeSupportsIniKey(m_nrLiveExe,L"FPS",L"lows"))m_hudTileMap.push_back(5);
        if(ExeSupportsIniKey(m_nrLiveExe,L"FPS",L"vram"))m_hudTileMap.push_back(8);
        if(ExeSupportsIniKey(m_nrLiveExe,L"FPS",L"cpu_load"))m_hudTileMap.push_back(9);
        if(ExeSupportsIniKey(m_nrLiveExe,L"FPS",L"gpu_times"))m_hudTileMap.push_back(14);
        m_hudTileMap.push_back(11);
    } else { m_hudTileMap.insert(m_hudTileMap.end(),{0,3,4,11}); }
    m_hudPosition=std::clamp(m_hudPosition,0,m_hudNew?5:3);
    m_hudPreset=std::clamp(m_hudPreset,0,4);
    m_hudScale=std::clamp(m_hudScale,50,250); m_hudFontSize=std::clamp(m_hudFontSize,12,48); m_hudBackgroundAlpha=std::clamp(m_hudBackgroundAlpha,0,100);
    m_sharpness=std::clamp(m_sharpness,0,100); m_fpsLimit=std::clamp(m_fpsLimit,15,500);
    m_limiterMethod=std::clamp(m_limiterMethod,0,1); m_upscaler=std::clamp(m_upscaler,0,2);
    m_nrStrength=std::clamp(m_nrStrength,0,200); m_nrPasses=std::clamp(m_nrPasses,1,4);

    // Repair invalid legacy/profile hotkeys before the launcher is used.
    // A malformed shortcut must never leave the main Start action unusable.
    std::wstring hotkeyReason;
    if(!ValidateHotkeySettings(hotkeyReason)) {
        m_scaleKey=L"ctrl+alt+s";
        m_stopKey=L"ctrl+shift+a";
        m_overlayKey=L"ctrl+home";
        m_bypassKeys=L"home,insert,end,pageup,pagedown";
        SaveProfile();
        m_hotkeyStatus=L"检测到无效快捷键配置，已恢复默认快捷键";
    }
}
void CDlss5SwapperDlg::SaveSettings()
{
    IniWrite(L"FSR",L"sharpness",Num(m_sharpness));
    IniWrite(L"FrameGeneration",L"enabled",m_frameGeneration?L"1":L"0");
    IniWrite(L"FrameLimiter",L"enabled",m_frameLimiter?L"1":L"0");
    IniWrite(L"FrameLimiter",L"fps",Num(m_fpsLimit));
    IniWrite(L"FrameLimiter",L"method",Num(m_limiterMethod));
    IniWrite(L"DLSSNR",L"enabled",m_nrEnabled?L"1":L"0");
    SaveNrSettings();
    // FPS.enabled is the FPS counter tile, not the page-level overlay switch.
    IniWrite(L"FPS",L"enabled",m_hudCounterEnabled?L"1":L"0");
    if(m_hudNew) IniWrite(L"FPS",L"preset",Num(m_hudPreset));
    IniWrite(L"FPS",L"position",Num(m_hudPosition));
    IniWrite(L"FPS",L"background",m_hudBackground?L"1":L"0");
    IniWrite(L"FPS",L"text_outline",m_hudTextOutline?L"1":L"0");
    IniWrite(L"FPS",L"background_alpha",Num(m_hudBackgroundAlpha));
    if(m_hudNew) IniWrite(L"FPS",L"scale",Num(m_hudScale)); else IniWrite(L"FPS",L"font_size",Num(m_hudFontSize));
    const wchar_t* keys[]={L"fps",L"title",L"fps_detail",L"frametime",L"frame_timing",L"lows",L"gpu_load",L"gpu_temp",L"vram",L"cpu_load",L"pipeline",L"resolution",L"gpu_clock",L"ram",L"gpu_times",L"latency",L"clock"};
    for(int i=0;i<17;i++) {
        // The legacy counter has only the four non-pending rows; never write newer HUD keys into it.
        if(!m_hudNew && i!=0 && i!=3 && i!=4 && i!=11) continue;
        IniWrite(L"FPS",keys[i],m_hudItems[i]?L"1":L"0");
    }
    if(m_hudNew) IniWrite(L"FPS",L"layout",Num(m_hudLayout));
    IniWrite(L"NRLive",L"capture_mode",m_captureMode==1?L"dxgi":L"wgc");
    IniWrite(L"NRLive",L"motion",m_motionMode==1?L"amdof":L"fast");
    IniWrite(L"NRLive",L"target_mode",Num(m_targetMode));
    IniWrite(L"NRLive",L"target_text",m_targetText);
    IniWrite(L"NRLive",L"target_label",m_targetLabel);
    IniWrite(L"NRLive",L"delay",Num(m_delay));
    IniWrite(L"NRLive",L"run_as_admin",m_runAsAdmin?L"1":L"0");
    SaveProfile();
    RegisterHotkeys();
    SetStatus(L"已保存："+m_configPath);
}
void CDlss5SwapperDlg::SaveProfile()
{
    std::wstring profileDir=m_configDir+L"\\profiles";
    CreateDirectoryW(profileDir.c_str(),nullptr);
    std::wstring path=profileDir+L"\\Default.json";
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || f == nullptr) return;
    std::string target=m_targetMode==0?"picker":m_targetMode==1?"pid":"pname";
    std::string targetText=JsonEscape(m_targetText);
    std::string json="{\n"
        "  \"target_mode\": \""+target+"\",\n"
        "  \"target_text\": \""+targetText+"\",\n"
        "  \"delay\": "+std::to_string(m_delay)+",\n"
        "  \"no_overlay\": "+std::string(m_overlayEnabled?"false":"true")+",\n"
        "  \"motion\": \""+std::string(m_motionMode==1?"amdof":"fast")+"\",\n"
        "  \"capture_mode\": \""+std::string(m_captureMode==1?"dxgi":"wgc")+"\",\n"
        "  \"upscaler\": \""+std::string(m_upscaler==0?"fsr4":m_upscaler==1?"fsr31":"none")+"\",\n"
        "  \"schema_version\": 1,\n"
        "  \"run_as_admin\": "+std::string(m_runAsAdmin?"true":"false")+",\n"
        "  \"scale_hotkey\": \""+JsonEscape(m_scaleKey)+"\",\n"
        "  \"stop_key\": \""+JsonEscape(m_stopKey)+"\",\n"
        "  \"overlay_key\": \""+JsonEscape(m_overlayKey)+"\",\n"
        "  \"bind_bypass\": \""+JsonEscape(m_bypassKeys)+"\"\n"
        "}\n";
    fwrite(json.data(),1,json.size(),f); fclose(f);
}
void CDlss5SwapperDlg::RegisterHotkeys()
{
    UnregisterHotKey(GetSafeHwnd(),1);
    UINT mods=0,vk=0;
    m_hotkeyStatus.clear();
    std::wstring invalid;
    if(!ValidateHotkeySettings(invalid)) { m_hotkeyStatus=invalid; return; }
    if(GetFileAttributesW(m_nrLiveExe.c_str())==INVALID_FILE_ATTRIBUTES) return;
    // The original launcher registers only ScaleHotkey. StopKey is pressed by the launcher
    // only as a fallback when it cannot terminate an elevated NRLive process; OverlayKey
    // is registered/handled by NRLive itself. Registering either here would steal the key.
    if(ParseHotkey(m_scaleKey,mods,vk) && !RegisterHotKey(GetSafeHwnd(),1,mods|MOD_NOREPEAT,vk))
        m_hotkeyStatus=L"缩放快捷键注册失败，可能已被其他程序占用";
}
bool CDlss5SwapperDlg::BrowseCaptureExecutable()
{
    CFileDialog dlg(TRUE,L"exe",nullptr,OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_HIDEREADONLY,
                    L"可执行文件 (*.exe)|*.exe|所有文件 (*.*)|*.*||",this);
    dlg.m_ofn.lpstrTitle=L"选择要捕获的游戏程序（可选择尚未启动的游戏）";
    if(dlg.DoModal()!=IDOK) return false;
    std::wstring exe=dlg.GetFileName().GetString();
    if(exe.empty()) return false;
    m_targetMode=2;
    m_targetText=ExactProcessPattern(exe);
    m_targetLabel=exe+L"（等待该程序启动）";
    m_targetWaitNotice=true;
    SaveSettings();
    SetStatus(L"已选择程序：NRLive 将按进程名等待目标启动；等待超时由 NRLive 运行时处理");
    return true;
}

void CDlss5SwapperDlg::RefreshCaptureTargets()
{
    m_captureTargets.clear();
    m_targetPage=0;
    struct EnumContext { std::vector<CaptureTargetChoice>* items; DWORD self; } ctx{&m_captureTargets,GetCurrentProcessId()};
    EnumWindows([](HWND hwnd, LPARAM lp)->BOOL {
        auto* c=reinterpret_cast<EnumContext*>(lp);
        if(!::IsWindowVisible(hwnd) || ::GetWindow(hwnd,GW_OWNER)!=nullptr) return TRUE;
        // DWM-cloaked windows are not user-visible capture targets.
        DWORD cloaked=0; HRESULT dwm=DwmGetWindowAttribute(hwnd,DWMWA_CLOAKED,&cloaked,sizeof(cloaked));
        if(SUCCEEDED(dwm) && cloaked!=0) return TRUE;
        LONG_PTR ex=GetWindowLongPtrW(hwnd,GWL_EXSTYLE);
        if((ex & WS_EX_TOOLWINDOW)!=0) return TRUE;
        int len=::GetWindowTextLengthW(hwnd); if(len<=0) return TRUE;
        std::wstring title((size_t)len+1,L'\0'); ::GetWindowTextW(hwnd,title.data(),len+1); title.resize(wcsnlen_s(title.c_str(),title.size()));
        DWORD pid=0; GetWindowThreadProcessId(hwnd,&pid); if(!pid || pid==c->self) return TRUE;
        HANDLE hp=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid); if(!hp) return TRUE;
        wchar_t path[1024]{}; DWORD cap=1024; std::wstring exe=L"";
        if(QueryFullProcessImageNameW(hp,0,path,&cap)) { exe=path; size_t pos=exe.find_last_of(L"\\/"); if(pos!=std::wstring::npos) exe=exe.substr(pos+1); }
        CloseHandle(hp); if(exe.empty()) return TRUE;
        std::wstring label=title+L"  ["+exe+L" / PID "+std::to_wstring(pid)+L"]";
        c->items->push_back({label,exe,pid}); return TRUE;
    },reinterpret_cast<LPARAM>(&ctx));
    std::sort(m_captureTargets.begin(),m_captureTargets.end(),[](const auto& a,const auto& b){return a.label<b.label;});
    if(m_targetMode==2) {
        m_captureTargets.clear();
        HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
        if(snap!=INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W pe{}; pe.dwSize=sizeof(pe);
            if(Process32FirstW(snap,&pe)) do {
                if(pe.th32ProcessID==ctx.self || !pe.szExeFile[0]) continue;
                std::wstring exe=pe.szExeFile;
                auto it=std::find_if(m_captureTargets.begin(),m_captureTargets.end(),[&](const auto& x){return _wcsicmp(x.processName.c_str(),exe.c_str())==0;});
                if(it==m_captureTargets.end()) m_captureTargets.push_back({exe+L"  [PID "+std::to_wstring(pe.th32ProcessID)+L"]",exe,pe.th32ProcessID});
            } while(Process32NextW(snap,&pe));
            CloseHandle(snap);
        }
        std::sort(m_captureTargets.begin(),m_captureTargets.end(),[](const auto& a,const auto& b){return _wcsicmp(a.processName.c_str(),b.processName.c_str())<0;});
    }
}

void CDlss5SwapperDlg::DrawCaptureTargetPopup(Graphics& g)
{
    if(!m_targetPopup || m_targetMode==0) return;
    const int x=309,y=323,w=239,row=30,maxRows=8;
    int start=m_targetPage*maxRows;
    int count=start<(int)m_captureTargets.size()?(int)std::min<size_t>(maxRows,m_captureTargets.size()-start):0;
    int h=30+(std::max)(1,count)*row+28;
    Round(g,RectF((REAL)x,(REAL)y,(REAL)w,(REAL)h),7,C(255,35,29,43)); Outline(g,RectF((REAL)x,(REAL)y,(REAL)w,(REAL)h),7,C(255,111,73,139));
    T(g,L"刷新列表",x+12,y+8,10,C(255,191,150,224),true);
    if(m_targetMode==2) T(g,L"浏览 EXE（可选未启动游戏）",x+104,y+8,9,C(255,191,150,224),true);
    if(!count) T(g,L"没有发现可选窗口/进程",x+12,y+38,9,C(255,185,174,194));
    for(int i=0;i<count;i++) {
        int yy=y+30+i*row; if(i%2==0) Round(g,RectF((REAL)(x+4),(REAL)yy,(REAL)(w-8),(REAL)row),3,C(255,43,36,52));
        std::wstring label=m_captureTargets[start+i].label; if(label.size()>34) label=label.substr(0,31)+L"...";
        T(g,label.c_str(),x+10,yy+8,8,C(255,222,214,231));
    }
    T(g,(m_targetPage>0?L"上一页":L""),x+10,y+h-18,8,C(255,164,151,177));
    T(g,(start+count<(int)m_captureTargets.size()?L"下一页":L""),x+w-55,y+h-18,8,C(255,191,150,224));
}

bool CDlss5SwapperDlg::HandleCaptureTargetClick(CPoint p)
{
    if(m_targetPopup) {
        const int x=309,y=323,w=239,row=30,maxRows=8;
        int start=m_targetPage*maxRows;
        int count=start<(int)m_captureTargets.size()?(int)std::min<size_t>(maxRows,m_captureTargets.size()-start):0;
        int h=30+(std::max)(1,count)*row+28;
        CRect popup(x,y,x+w,y+h);
        if(popup.PtInRect(p)) {
            if(p.y<y+30) {
                if(m_targetMode==2 && p.x>=x+98) { BrowseCaptureExecutable(); m_targetPopup=false; Invalidate(FALSE); return true; }
                RefreshCaptureTargets(); Invalidate(FALSE); return true;
            }
            if(p.y>=y+h-28) { if(p.x>x+w/2 && start+count<(int)m_captureTargets.size()) ++m_targetPage; else if(p.x<x+w/2 && m_targetPage>0) --m_targetPage; Invalidate(FALSE); return true; }
            int index=(p.y-y-30)/row;
            if(index>=0 && index<count) {
                const auto& item=m_captureTargets[start+index];
                m_targetText=m_targetMode==1?std::to_wstring(item.pid):ExactProcessPattern(item.processName);
                m_targetLabel=item.label;
                m_targetWaitNotice=false;
                SaveSettings(); m_targetPopup=false; Invalidate(FALSE); return true;
            }
            return true;
        }
        m_targetPopup=false;
    }
    if(m_targetItemRect.PtInRect(p)) {
        if(m_targetMode==0) { SetStatus(L"当前为倒计时捕获前台窗口模式，无需选择目标"); return true; }
        RefreshCaptureTargets(); m_targetPopup=true; Invalidate(FALSE); return true;
    }
    return false;
}

void CDlss5SwapperDlg::StartNrLive()
{
    if (!HasLocalNrLive()) {
        m_startSwitchOn = false;
        m_startCountdownActive = false;
        SetStatus(L"尚未安装 NRLive 依赖，请先点击「安装依赖」");
        MessageBoxW(L"尚未安装 NRLive 依赖。请先点击主界面的「安装依赖」，下载并解压完成后再开启 DLSS5。", L"需要安装依赖", MB_OK | MB_ICONINFORMATION);
        Invalidate(FALSE);
        return;
    }
    FindNrLive();
    if(m_nrLiveProcess && WaitForSingleObject(m_nrLiveProcess,0)==WAIT_TIMEOUT) {
        if(TerminateProcess(m_nrLiveProcess,0)) {
            CloseHandle(m_nrLiveProcess); m_nrLiveProcess=nullptr;
            m_startSwitchOn=false; m_startCountdownActive=false;
            SetStatus(L"NRLive 已停止");
        } else if(PressHotkey(m_stopKey)) {
            m_startSwitchOn=false; m_startCountdownActive=false;
            SetStatus(L"无法直接结束提升权限的 NRLive，已发送停止快捷键："+HotkeyDisplay(m_stopKey));
        } else {
            SetStatus(L"停止 NRLive 失败：进程访问被拒绝，停止快捷键也未能发送");
        }
        Invalidate(FALSE);
        return;
    }
    if(m_nrLiveProcess) { CloseHandle(m_nrLiveProcess); m_nrLiveProcess=nullptr; }
    // Do not let a stale/invalid shortcut profile block the primary NRLive launch.
    // Restore known-good defaults, persist them, and continue the launch sequence.
    std::wstring hotkeyProblem;
    if(!ValidateHotkeySettings(hotkeyProblem)) {
        m_activeHotkey=0;
        m_scaleKey=L"ctrl+alt+s";
        m_stopKey=L"ctrl+shift+a";
        m_overlayKey=L"ctrl+home";
        m_bypassKeys=L"home,insert,end,pageup,pagedown";
        SaveProfile();
        RegisterHotkeys();
        m_hotkeyStatus.clear();
        m_status=L"快捷键配置无效，已恢复默认值；正在启动 NRLive";
        Invalidate(FALSE);
    }
    SaveSettings();
    if(GetFileAttributesW(m_nrLiveExe.c_str())==INVALID_FILE_ATTRIBUTES) { SetStatus(L"未找到 NRLive.exe：请将 NRLive 放入程序目录的 nrlive 文件夹"); return; }
    if(m_targetMode==1 && (m_targetText.empty() || _wtoi(m_targetText.c_str())<=0)) { SetStatus(L"请先选择一个有效的窗口目标"); m_targetPopup=true; RefreshCaptureTargets(); Invalidate(FALSE); return; }
    if(m_targetMode==1) {
        DWORD pid=(DWORD)_wtoi(m_targetText.c_str());
        HANDLE hp=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
        if(!hp) { SetStatus(L"所选窗口进程已退出或无权访问，请刷新窗口列表"); m_targetPopup=true; RefreshCaptureTargets(); Invalidate(FALSE); return; }
        CloseHandle(hp);
    }
    if(m_targetMode==2 && m_targetText.empty()) { SetStatus(L"请先选择一个正在运行的进程"); m_targetPopup=true; RefreshCaptureTargets(); Invalidate(FALSE); return; }
    std::wstring args;
    if(m_targetMode==0) args=L"-picker "+Num(m_delay);
    else if(m_targetMode==1) args=L"-pid "+m_targetText+L" -delay "+Num(m_delay);
    else args=L"-pname "+QuoteWindowsArg(m_targetText)+L" -delay "+Num(m_delay);
    args += L" --mv ";
    args += (m_motionMode==1 ? L"amdof" : L"fast");
    args += L" --capture ";
    args += (m_captureMode==1 ? L"dxgi" : L"wgc");
    args += L" --key "; args += QuoteWindowsArg(m_stopKey);
    args += L" --overlaykey "; args += QuoteWindowsArg(m_overlayKey);
    args += L" --bindbypass "; args += QuoteWindowsArg(m_bypassKeys);
    if(!m_overlayEnabled) args += L" -nooverlay";
    if(m_upscalerSupported) {
        args += L" --upscaler ";
        args += (m_upscaler==0 ? L"fsr4" : (m_upscaler==1 ? L"fsr31" : L"none"));
    }
    SHELLEXECUTEINFOW sei{}; sei.cbSize=sizeof(sei); sei.fMask=SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb=m_runAsAdmin?L"runas":L"open"; sei.lpFile=m_nrLiveExe.c_str(); sei.lpParameters=args.c_str();
    sei.lpDirectory=m_configDir.c_str(); sei.nShow=SW_SHOWNORMAL;
    if(!ShellExecuteExW(&sei)) SetStatus(L"启动 NRLive 失败，错误码："+Num((int)GetLastError()));
    else {
        m_startSwitchOn=true;
        m_startCountdownActive=(m_delay>0);
        m_startCountdownEnd=GetTickCount64()+(ULONGLONG)m_delay*1000ULL;
        if(m_targetMode==0) SetStatus(L"NRLive 已启动：倒计时结束后捕获前台窗口");
        else if(m_targetMode==1) SetStatus(L"NRLive 已启动：正在捕获指定窗口 PID");
        else SetStatus(L"NRLive 已启动：正在等待指定进程出现；可用停止热键取消");
        m_nrLiveProcess=sei.hProcess;
        Invalidate(FALSE);
    }
}

void CDlss5SwapperDlg::SetStatus(const std::wstring& s)
{
    // Copy first because callers may pass m_hotkeyStatus itself.
    const std::wstring message=s;
    m_hotkeyStatus.clear();
    m_status=message;
    Invalidate(FALSE);
}

CPoint CDlss5SwapperDlg::ToDesignPoint(CPoint p,int cx,int cy) const
{
    // Keep the original 600x1240 design proportions while the native window
    // is freely resizable. Uniform scaling avoids stretched controls and text.
    if (cx <= 0 || cy <= 0) return p;
    const double scale = (std::min)((double)cx / DW, (double)cy / DH);
    const double offsetX = (cx - DW * scale) * 0.5;
    const double offsetY = (cy - DH * scale) * 0.5;
    return CPoint((int)std::lround((p.x - offsetX) / scale),
                  (int)std::lround((p.y - offsetY) / scale));
}
void CDlss5SwapperDlg::DrawButton(Graphics& g,const RectF& r,const wchar_t* label,bool primary)
{
    Round(g,r,7,primary?C(255,65,31,83):C(255,31,29,40));
    Outline(g,r,7,primary?C(255,104,56,135):C(255,57,48,61));
    T(g,label,r.X+14,r.Y+r.Height/2-8,13,C(255,226,216,235),true);
}
void CDlss5SwapperDlg::DrawToggle(Graphics& g,float x,float y,const wchar_t* label,bool value)
{
    T(g,label,x,y+2,13,C(255,207,202,214));
    Round(g,RectF(490,y,48,25),13,value?C(255,116,54,153):C(255,54,53,62));
    SolidBrush b(value?C(255,238,230,243):C(255,170,165,176));
    g.FillEllipse(&b,value?RectF(514,y+4,17,17):RectF(495,y+4,17,17));
}
void CDlss5SwapperDlg::DrawCombo(Graphics& g,float x,float y,float width,const wchar_t* label,const std::vector<std::wstring>& opts,int selected,int comboId)
{
    T(g,label,x,y+7,12,C(255,165,158,176));
    const float fieldX = x + 110.0f;
    const float fieldW = width - 110.0f;
    Round(g,RectF(fieldX,y,fieldW,38),7,C(255,34,31,42));
    Outline(g,RectF(fieldX,y,fieldW,38),7,C(255,77,61,86));
    const int safe = opts.empty() ? 0 : (std::clamp)(selected,0,(int)opts.size()-1);
    if(!opts.empty()) {
        std::wstring shown=opts[safe];
        FontFamily ff(L"Microsoft YaHei UI"); Font font(&ff,13.0f*1.30f,FontStyleRegular,UnitPixel);
        RectF measured; const REAL maxW=fieldW-43.0f;
        g.MeasureString(shown.c_str(),-1,&font,PointF(0,0),&measured);
        while(measured.Width>maxW && shown.size()>1) { shown.pop_back(); if(!shown.empty()) shown.back()=L'…'; g.MeasureString(shown.c_str(),-1,&font,PointF(0,0),&measured); }
        T(g,shown.c_str(),fieldX+12,y+9,13,C(255,225,219,232));
    }
    Pen p(C(255,186,174,194),1.5f);
    g.DrawLine(&p,x+width-25,y+15,x+width-19,y+21);
    g.DrawLine(&p,x+width-19,y+21,x+width-13,y+15);
    if(comboId>=0 && comboId<(int)m_comboRects.size()) {
        // Settings controls live on a fixed page and must never inherit the
        // main-page content scroll offset. Only main-page combos below the
        // content origin use the scrolled screen coordinate.
        const bool scrollableMainCombo = !m_settingsOpen && y >= 515.0f;
        const int screenY = (int)y - (scrollableMainCombo ? m_scrollOffset : 0);
        m_comboRects[comboId] = CRect((int)fieldX,screenY,(int)(fieldX+fieldW),screenY+38);
        m_comboOptions[comboId] = opts;
        m_comboSelections[comboId] = safe;
    }
}

void CDlss5SwapperDlg::InvalidateDesignRect(const CRect& designRect)
{
    if(!GetSafeHwnd()) return;
    CRect client; GetClientRect(&client);
    const double scale=(std::min)((double)client.Width()/DW,(double)client.Height()/DH);
    const double offsetX=(client.Width()-DW*scale)*0.5;
    const double offsetY=(client.Height()-DH*scale)*0.5;
    CRect r((int)std::floor(offsetX+designRect.left*scale)-2, (int)std::floor(offsetY+designRect.top*scale)-2,
            (int)std::ceil(offsetX+designRect.right*scale)+2, (int)std::ceil(offsetY+designRect.bottom*scale)+2);
    r.IntersectRect(r,client);
    if(!r.IsRectEmpty()) InvalidateRect(&r,FALSE);
}

void CDlss5SwapperDlg::InvalidateComboRegion(int comboId, bool includePopup)
{
    if(comboId<0 || comboId>=(int)m_comboRects.size()) return;
    if(comboId==6) { InvalidateDesignRect(CRect(22,515,582,1178)); return; }
    CRect r=m_comboRects[comboId];
    if(includePopup && !m_comboOptions[comboId].empty()) {
        const int itemH=36; int popupY=r.bottom+3;
        int maxBottom=(r.top<515)?1178:1172;
        int totalH=(int)m_comboOptions[comboId].size()*itemH;
        if(popupY+totalH>maxBottom && r.top-totalH-3>=515) popupY=r.top-totalH-3;
        const int popupWidth=(comboId==0) ? 300 : r.Width();
        // CRect coordinates are LONG on Windows; keep both operands the same type.
        const LONG popupTop = static_cast<LONG>(popupY);
        const LONG popupBottom = static_cast<LONG>(popupY + totalH);
        const LONG popupRight = static_cast<LONG>(r.left + popupWidth);
        r.top = (std::min)(r.top, popupTop);
        r.bottom = (std::max)(r.bottom, popupBottom);
        r.right = (std::max)(r.right, popupRight);
    }
    // Main-page controls are painted under the fixed -140px page translation.
    // Their hit rectangles stay in pre-translation content coordinates, so the
    // invalidation rectangle must use the same visible-screen coordinates as
    // DrawOpenComboPopup. Without this, Windows repaints the area 140px too low:
    // the dropdown appears for a frame and then gets clipped/erased.
    if(!m_settingsOpen && comboId!=0) {
        r.OffsetRect(0,-140);
    }
    r.InflateRect(4,4); InvalidateDesignRect(r);
}

void CDlss5SwapperDlg::DrawOpenComboPopup(Graphics& g)
{
    if(m_openCombo<0 || m_openCombo>=(int)m_comboRects.size()) return;
    const auto& opts=m_comboOptions[m_openCombo];
    if(opts.empty()) return;
    CRect anchor=m_comboRects[m_openCombo];
    const int itemH=36;
    int popupY=anchor.bottom+3;
    int maxBottom=(anchor.top<515)?1178:1172;
    int totalH=(int)opts.size()*itemH;
    if(popupY+totalH>maxBottom && anchor.top-totalH-3>=515) popupY=anchor.top-totalH-3;
    const int left=anchor.left;
    // Capture-target choices are long Chinese labels. Give only this popup
    // additional width; keep the closed combo aligned with its original row.
    const int width=(m_openCombo==0) ? 300 : anchor.Width();
    Round(g,RectF((REAL)left,(REAL)popupY,(REAL)width,(REAL)totalH),7,C(255,38,32,47));
    Outline(g,RectF((REAL)left,(REAL)popupY,(REAL)width,(REAL)totalH),7,C(255,132,73,166),1.3f);
    for(int i=0;i<(int)opts.size();++i) {
        const int iy=popupY+i*itemH;
        if(i==m_comboSelections[m_openCombo])
            Round(g,RectF((REAL)(left+3),(REAL)(iy+2),(REAL)(width-6),(REAL)(itemH-4)),5,C(255,77,43,98));
        T(g,opts[i].c_str(),(REAL)(left+13),(REAL)(iy+9),13,C(255,232,222,239),i==m_comboSelections[m_openCombo]);
    }
}

void CDlss5SwapperDlg::OpenCombo(int comboId)
{
    if(comboId<0 || comboId>=(int)m_comboRects.size() || m_comboOptions[comboId].empty()) return;
    int old=m_openCombo;
    m_openCombo = (m_openCombo==comboId) ? -1 : comboId;
    if(old>=0) InvalidateComboRegion(old,true);
    if(m_openCombo>=0) InvalidateComboRegion(m_openCombo,true);
}

bool CDlss5SwapperDlg::HandleComboPopupClick(CPoint p)
{
    if(m_openCombo<0) return false;
    const int id=m_openCombo;
    const CRect anchor=m_comboRects[id];
    const auto& opts=m_comboOptions[id];
    const int itemH=36;
    int popupY=anchor.bottom+3;
    int maxBottom=(anchor.top<515)?1178:1172;
    int totalH=(int)opts.size()*itemH;
    if(popupY+totalH>maxBottom && anchor.top-totalH-3>=515) popupY=anchor.top-totalH-3;
    const int popupWidth=(id==0) ? 300 : anchor.Width();
    CRect popup(anchor.left,popupY,anchor.left+popupWidth,popupY+totalH);
    if(popup.PtInRect(p)) {
        int selected=(p.y-popupY)/itemH;
        if(selected>=0 && selected<(int)opts.size()) {
            switch(id) {
            case 0: m_targetMode=selected; m_targetText.clear(); m_targetLabel.clear(); m_targetPopup=false; m_targetPage=0; RefreshCaptureTargets(); break;
            case 1: m_captureMode=selected; break;
            case 2: m_motionMode=selected; break;
            case 3: m_upscaler=selected; break;
            case 4: m_limiterMethod=selected; break;
            case 5: m_nrStyle=selected; break;
            case 6:
                m_hudPreset=selected;
                if(selected>0) {
                    const int on[17][4]={{1,1,1,1},{0,0,1,1},{0,0,1,1},{0,1,1,1},{0,0,1,1},{0,0,1,1},{0,1,1,1},{0,1,1,1},{0,0,1,1},{0,1,1,1},{0,1,1,1},{0,0,1,1},{0,0,0,1},{0,0,0,1},{0,0,0,1},{0,0,0,1},{0,0,0,1}};
                    for(int i=0;i<17;i++) m_hudItems[i]=on[i][selected-1]!=0;
                    m_hudLayout=selected==2?1:0;
                }
                break;
            case 7: m_hudPosition=selected; break;
            case 8: {
                static const int delays[] = {3,5,10,15,20,30};
                if(selected>=0 && selected<6) m_delay=delays[selected];
                break;
            }
            }
            SaveSettings();
        }
        m_openCombo=-1;
        if(id==0) InvalidateDesignRect(CRect(22,82,578,392));
        else if(id==6) InvalidateComboRegion(id,false);
        else InvalidateComboRegion(id,false);
        return true;
    }
    // Clicking outside closes only the old popup area; avoid repainting the full window.
    m_openCombo=-1;
    InvalidateComboRegion(id,true);
    return false;
}
void CDlss5SwapperDlg::DrawSlider(Graphics& g,int id,float x,float y,float width,const wchar_t* label,int value,int minv,int maxv,const wchar_t* suffix)
{
    T(g,label,x,y,13,C(255,204,198,211));
    std::wstring val=std::to_wstring(value)+suffix;
    T(g,val.c_str(),x+width-54,y,12,C(255,229,220,237),true);
    Round(g,RectF(x,y+27,width,8),4,C(255,52,49,58));
    float f = static_cast<float>(value - minv) / static_cast<float>(maxv - minv);
    Round(g,RectF(x,y+27,width*std::clamp(f,0.0f,1.0f),8),4,C(255,130,63,169));
    SolidBrush b(C(255,177,93,210)); g.FillEllipse(&b,RectF(x+width*f-7,y+20,14,22));
    m_sliderRects[id]=CRect((int)x,(int)(y+13-m_scrollOffset),(int)(x+width),(int)(y+50-m_scrollOffset));
}
static bool IsNrChoiceEnabled(const std::vector<NrUiSetting>& settings, const std::vector<int>& values, size_t index)
{
    if(index>=settings.size() || index>=values.size()) return false;
    const auto& s=settings[index];
    if(s.needsEncoding) {
        for(size_t j=0;j<settings.size() && j<values.size();++j)
            if(settings[j].key==L"MochizukiRenoSourceEncoding" && values[j]==0) return false;
    }
    if(s.needsFast) {
        for(size_t j=0;j<settings.size() && j<values.size();++j)
            if(settings[j].key==L"MochizukiRenoPassesMode" && values[j]!=1) return false;
    }
    if(s.key==L"MochizukiInt4") {
        for(size_t j=0;j<settings.size() && j<values.size();++j)
            if(settings[j].key==L"MochizukiOriginalModel" && values[j]==0) return false;
    }
    if(s.group==L"调整 NR 的外观" && s.key!=L"MochizukiRenoLookMode") {
        for(size_t j=0;j<settings.size() && j<values.size();++j)
            if(settings[j].key==L"MochizukiRenoLookMode" && values[j]==0) return false;
    }
    if(s.group==L"预处理" && s.key!=L"MochizukiPreprocess") {
        for(size_t j=0;j<settings.size() && j<values.size();++j)
            if(settings[j].key==L"MochizukiPreprocess" && values[j]==0) return false;
    }
    return true;
}

void CDlss5SwapperDlg::DrawNrChoicePopup(Graphics& g)
{
    m_nrChoicePopupRect=CRect(0,0,0,0);
    if(m_nrOpenChoice<0 || m_nrOpenChoice>=(int)m_nrSettings.size() || m_nrOpenChoice>=(int)m_nrHitRects.size()) return;
    const auto& setting=m_nrSettings[m_nrOpenChoice];
    if(setting.kind!=L"choice" || setting.choices<=0 || m_nrHitRects[m_nrOpenChoice].IsRectEmpty() ||
       !IsNrChoiceEnabled(m_nrSettings,m_nrValues,(size_t)m_nrOpenChoice)) {
        m_nrOpenChoice=-1;
        return;
    }
    const int itemH=30;
    const int totalH=setting.choices*itemH;
    const CRect anchor=m_nrHitRects[m_nrOpenChoice];
    // NR content is scrolled and the page itself is translated by -140px.
    // Keep the popup inside the scroll viewport in content coordinates.
    const int viewTop=515+m_scrollOffset;
    const int viewBottom=1175+m_scrollOffset;
    const int availableH=(std::max)(0,viewBottom-viewTop);
    const int popupH=(std::min)(totalH,availableH);
    if(popupH<itemH) { m_nrOpenChoice=-1; return; }
    int top=anchor.bottom+2;
    if(top+popupH>viewBottom) top=anchor.top-popupH-2;
    top=(std::clamp)(top,viewTop,viewBottom-popupH);
    m_nrChoicePopupRect=CRect(400,top,540,top+popupH);
    Round(g,RectF(400,(REAL)top,140,(REAL)popupH),6,C(255,38,32,47));
    Outline(g,RectF(400,(REAL)top,140,(REAL)popupH),6,C(255,132,73,166),1.2f);
    const int visibleCount=(popupH+itemH-1)/itemH;
    for(int n=0;n<setting.choices && n<visibleCount;++n) {
        int iy=top+n*itemH;
        if(iy+itemH>top+popupH) break;
        if(n==m_nrValues[m_nrOpenChoice]) Round(g,RectF(403,(REAL)(iy+1),134,28),4,C(255,77,43,98));
        const std::wstring label=NrChoiceLabel(setting,n);
        T(g,label.c_str(),410,(REAL)(iy+8),10,C(255,232,222,239),n==m_nrValues[m_nrOpenChoice]);
    }
}

bool CDlss5SwapperDlg::HandleNrChoicePopupClick(CPoint p)
{
    if(m_nrOpenChoice<0) return false;
    const int index=m_nrOpenChoice;
    const auto& setting=m_nrSettings[index];
    if(!IsNrChoiceEnabled(m_nrSettings,m_nrValues,(size_t)index) || setting.choices<=0) {
        m_nrOpenChoice=-1;
        Invalidate(FALSE);
        return true;
    }
    const CRect popup=m_nrChoicePopupRect;
    if(popup.PtInRect(p)) {
        int selected=(p.y-popup.top)/30;
        if(selected>=0 && selected<setting.choices) {
            m_nrValues[index]=selected;
            SaveSettings();
        }
        m_nrOpenChoice=-1;
        Invalidate(FALSE);
        return true;
    }
    m_nrOpenChoice=-1;
    Invalidate(FALSE);
    return false;
}

void CDlss5SwapperDlg::DrawTabs(Graphics& g)
{
    const wchar_t* names[]={L"超分和帧生成",L"神经渲染",L"帧率监控"};
    for(int i=0;i<3;i++) {
        m_tabRects[i]=CRect(34+i*178,480,34+i*178+170,523);
        RectF r((REAL)m_tabRects[i].left,(REAL)m_tabRects[i].top,(REAL)m_tabRects[i].Width(),(REAL)m_tabRects[i].Height());
        Round(g,r,7,m_tab==i?C(255,59,31,76):C(255,29,27,36));
        if(m_tab==i) Outline(g,r,7,C(255,122,66,157));
        T(g,names[i],r.X+42,r.Y+13,13,m_tab==i?C(255,231,212,241):C(255,178,172,185),true);
    }
}

void CDlss5SwapperDlg::DrawPictureTab(Graphics& g)
{
    T(g,L"超分和帧生成",35,525,15,C(255,224,218,232),true);
    if(m_upscalerSupported) DrawCombo(g,35,555,520,L"超分辨率",{L"FSR 4",L"FSR 3.1",L"原生分辨率"},m_upscaler,3);
    else T(g,L"当前 NRLive 版本不支持 --upscaler，已隐藏超分辨率选择",35,562,11,C(255,160,150,165));
    DrawSlider(g,0,35,615,470,L"锐度",m_sharpness,0,100);
    DrawToggle(g,35,680,L"帧生成（在游戏每绘制的两帧之间生成一帧）",m_frameGeneration);
    Round(g,RectF(35,720,520,115),9,C(255,27,25,34)); Outline(g,RectF(35,720,520,115),9,C(255,62,51,68));
    DrawToggle(g,52,735,L"帧率限制",m_frameLimiter);
    DrawSlider(g,1,52,780,460,L"限制为",m_fpsLimit,15,500,L" fps");
    DrawButton(g,RectF(35,855,520,45),m_morePicture?L"收起更多设置  ▲":L"更多设置  ▼");
    if(m_morePicture) {
        T(g,L"高级设置",40,925,13,C(255,224,218,232),true);
        DrawCombo(g,35,955,520,L"限帧方式",{L"默认",L"平滑"},m_limiterMethod,4);
        T(g,L"此项对应 scaleconfig.ini 中 [FrameLimiter] method",45,1020,10,C(255,145,140,155));
    }
}
static int NrScaleForKey(const std::wstring& key)
{
    if(key==L"MochizukiRenoProxyAnchor") return 10;
    if(key==L"MochizukiRenoPaperWhiteScale") return 10000;
    if(key==L"MochizukiRenoPQCalibration") return 1000;
    static const std::set<std::wstring> scale100 = {
        L"MochizukiRenoIntensity",L"MochizukiRenoLocalTone",L"MochizukiRenoLocalStructure",
        L"MochizukiRenoSkinStructure",L"MochizukiHistoryStrength",L"MochizukiRenoColorStrength",
        L"MochizukiRenoTransferStrength",L"MochizukiRenoChromaClamp",L"MochizukiWhitePoint",
        L"MochizukiRenoLookStrength",L"MochizukiRenoLookBrighten",L"MochizukiRenoLookDarken",
        L"MochizukiRenoLookColour",L"MochizukiRenoLookHue",L"MochizukiRenoLookShadows",
        L"MochizukiRenoLookMidtones",L"MochizukiRenoLookHighlights",L"MochizukiRenoLookMaxBrighten",
        L"MochizukiRenoLookMaxDarken",L"MochizukiRenoLookMaxColour",L"MochizukiRenoLookTone",
        L"MochizukiRenoLookDetail",L"MochizukiRenoLookDetailRadius",L"MochizukiRenoLookHalo",
        L"MochizukiRenoLookHaloBrightSide",L"MochizukiPreprocessBias",L"MochizukiPreprocessContrast",
        L"MochizukiPreprocessSaturation",L"MochizukiRenoFastExtrapolationPasses",
        L"MochizukiRenoFastExtrapolationFine",L"MochizukiRenoFastFeedbackPasses",
        L"MochizukiRenoFastFeedbackFine",L"MochizukiRenoFastHybridPasses",
        L"MochizukiRenoFastHybridFine",L"MochizukiRenoFastEditStrength"
    };
    return scale100.count(key) ? 100 : 1;
}

static bool NrParseBool(const std::wstring& raw, int fallback)
{
    std::wstring v=raw; std::transform(v.begin(),v.end(),v.begin(),[](wchar_t c){return (wchar_t)towlower(c);});
    if(v==L"1"||v==L"true"||v==L"on"||v==L"yes") return true;
    if(v==L"0"||v==L"false"||v==L"off"||v==L"no") return false;
    return fallback!=0;
}

static std::wstring NrChoiceLabel(const NrUiSetting& s, int value)
{
    const int n=std::clamp(value,0,(std::max)(0,s.choices-1));
    if(s.section==L"DLSSNR" && s.key==L"runtime" && n==0) return L"自动";
    if(s.section==L"DLSSNR" && s.key==L"runtime" && n==1) return L"OpenNR (mochizuki)";
    if(s.section==L"DLSSNR" && s.key==L"runtime" && n==2) return L"NRLive 的 0.0.6 桥接";
    if(s.section==L"DLSSNR" && s.key==L"model" && n==0) return L"原版";
    if(s.section==L"DLSSNR" && s.key==L"model" && n==1) return L"新版，更快";
    if(s.section==L"DLSSNR" && s.key==L"style" && n==0) return L"中性";
    if(s.section==L"DLSSNR" && s.key==L"style" && n==1) return L"自然";
    if(s.section==L"DLSSNR" && s.key==L"style" && n==2) return L"电影感";
    if(s.section==L"OpenNR" && s.key==L"MochizukiRenoStyle" && n==0) return L"标准";
    if(s.section==L"OpenNR" && s.key==L"MochizukiRenoStyle" && n==1) return L"自然";
    if(s.section==L"OpenNR" && s.key==L"MochizukiRenoStyle" && n==2) return L"电影感";
    if(s.section==L"OpenNR" && s.key==L"MochizukiRenoTransferMode" && n==0) return L"有界比例";
    if(s.section==L"OpenNR" && s.key==L"MochizukiRenoTransferMode" && n==1) return L"一致";
    if(s.section==L"OpenNR" && s.key==L"MochizukiRenoSdrBlackRestore" && n==0) return L"自动";
    if(s.section==L"OpenNR" && s.key==L"MochizukiRenoSdrBlackRestore" && n==1) return L"每个块";
    if(s.section==L"OpenNR" && s.key==L"MochizukiRenoSdrBlackRestore" && n==2) return L"平滑";
    if(s.section==L"OpenNR" && s.key==L"MochizukiRenoCodecMode" && n==0) return L"经典";
    if(s.section==L"OpenNR" && s.key==L"MochizukiRenoCodecMode" && n==1) return L"锚定";
    if(s.section==L"OpenNR" && s.key==L"MochizukiRenoCodecMode" && n==2) return L"自动";
    if(s.section==L"OpenNR" && s.key==L"MochizukiRenoCodecMode" && n==3) return L"显示器";
    if(s.section==L"OpenNR" && s.key==L"MochizukiRenoSourceEncoding" && n==0) return L"按格式";
    if(s.section==L"OpenNR" && s.key==L"MochizukiRenoSourceEncoding" && n==1) return L"SDR";
    if(s.section==L"OpenNR" && s.key==L"MochizukiRenoSourceEncoding" && n==2) return L"线性";
    if(s.section==L"OpenNR" && s.key==L"MochizukiRenoSourceEncoding" && n==3) return L"PQ";
    if(s.section==L"OpenNR" && s.key==L"MochizukiRenoFeedMode" && n==0) return L"自动";
    if(s.section==L"OpenNR" && s.key==L"MochizukiRenoFeedMode" && n==1) return L"内容测光";
    if(s.section==L"OpenNR" && s.key==L"MochizukiRenoFeedMode" && n==2) return L"游戏曝光";
    if(s.section==L"OpenNR" && s.key==L"MochizukiPreprocessExposure" && n==0) return L"关";
    if(s.section==L"OpenNR" && s.key==L"MochizukiPreprocessExposure" && n==1) return L"自动";
    if(s.section==L"OpenNR" && s.key==L"MochizukiPreprocessExposure" && n==2) return L"固定(仅偏置)";
    if(s.section==L"OpenNR" && s.key==L"MochizukiPreprocessCurve" && n==0) return L"无";
    if(s.section==L"OpenNR" && s.key==L"MochizukiPreprocessCurve" && n==1) return L"中性";
    if(s.section==L"OpenNR" && s.key==L"MochizukiPreprocessCurve" && n==2) return L"Reinhard";
    if(s.section==L"OpenNR" && s.key==L"MochizukiPreprocessCurve" && n==3) return L"电影感";
    if(s.section==L"OpenNR" && s.key==L"MochizukiPreprocessCurve" && n==4) return L"GT";
    if(s.section==L"OpenNR" && s.key==L"MochizukiPreprocessCurve" && n==5) return L"ACES";
    if(s.section==L"OpenNR" && s.key==L"MochizukiPreprocessCurve" && n==6) return L"AgX";
    if(s.section==L"OpenNR" && s.key==L"MochizukiHistoryPrecision" && n==0) return L"16-bit";
    if(s.section==L"OpenNR" && s.key==L"MochizukiHistoryPrecision" && n==1) return L"32-bit";
    if(s.section==L"OpenNR" && s.key==L"MochizukiRenoPassesMode" && n==0) return L"旧版";
    if(s.section==L"OpenNR" && s.key==L"MochizukiRenoPassesMode" && n==1) return L"快速";
    if(s.section==L"OpenNR" && s.key==L"MochizukiRenoFastMethod" && n==0) return L"外推";
    if(s.section==L"OpenNR" && s.key==L"MochizukiRenoFastMethod" && n==1) return L"反馈";
    if(s.section==L"OpenNR" && s.key==L"MochizukiRenoFastMethod" && n==2) return L"混合";
    if(s.section==L"OpenNR" && s.key==L"MochizukiRenoFastMethod" && n==3) return L"编辑强度";
    return L"选项 "+std::to_wstring(n+1);
}

void CDlss5SwapperDlg::InitNrSettings()
{
    if(!m_nrSettings.empty()) return;
    auto add=[&](const wchar_t* sec,const wchar_t* key,const wchar_t* label,const wchar_t* kind,int def,int lo,int hi,int choices,bool adv,const wchar_t* group,bool pending=false,bool bridge=false,bool enc=false,bool fast=false){
        NrUiSetting s; s.section=sec;s.key=key;s.label=label;s.kind=kind;s.def=def;s.min=lo;s.max=hi;s.choices=choices;s.advanced=adv;s.group=group;s.pending=pending;s.bridge=bridge;s.needsEncoding=enc;s.needsFast=fast;s.scale=(sec==std::wstring(L"OpenNR")?NrScaleForKey(s.key):1);m_nrSettings.push_back(s);
    };
    // Core OpenNR controls (same keys, defaults and ranges as NrLiveConfig.Settings).
    add(L"OpenNR",L"MochizukiOriginalModel",L"原版模型 (310.8)",L"toggle",0,0,1,0,false,L"基础",true);
    add(L"OpenNR",L"MochizukiRenoStyle",L"风格",L"choice",0,0,2,3,false,L"基础",true);
    add(L"OpenNR",L"MochizukiRenoIntensity",L"强度",L"range",100,0,200,0,false,L"基础",true);
    add(L"OpenNR",L"MochizukiRenoPasses",L"处理次数",L"range",1,1,4,0,false,L"基础",true);
    add(L"OpenNR",L"MochizukiRenoPassesMode",L"通道模式",L"choice",0,0,1,2,true,L"快速通道",true,false,false,false);
    add(L"OpenNR",L"MochizukiRenoLocalTone",L"局部色调",L"range",100,0,200,0,true,L"图像结构",true);
    add(L"OpenNR",L"MochizukiRenoLocalStructure",L"局部结构",L"range",100,0,200,0,true,L"图像结构",true);
    add(L"OpenNR",L"MochizukiRenoSkinStructure",L"皮肤结构",L"range",100,0,100,0,true,L"图像结构",true);
    add(L"OpenNR",L"MochizukiRenoAutoMask",L"角色遮罩",L"toggle",0,0,1,0,true,L"图像结构",true);
    add(L"OpenNR",L"MochizukiRenoSkinIndependent",L"皮肤与结构分开处理",L"toggle",1,0,1,0,true,L"图像结构",true);
    add(L"OpenNR",L"MochizukiTemporal",L"时间历史",L"toggle",1,0,1,0,true,L"时间与历史",true);
    add(L"OpenNR",L"MochizukiHistoryStrength",L"历史强度",L"range",100,0,100,0,true,L"时间与历史",true);
    add(L"OpenNR",L"MochizukiStyleGrade",L"风格调色",L"toggle",1,0,1,0,true,L"时间与历史",true);
    add(L"OpenNR",L"MochizukiHistoryPrecision",L"历史精度",L"choice",0,0,1,2,true,L"时间与历史",true);
    add(L"OpenNR",L"MochizukiNoiseEveryFrame",L"无运动矢量时变化噪点",L"toggle",0,0,1,0,true,L"时间与历史",true);
    add(L"OpenNR",L"MochizukiAco",L"ACO 着色器编译器（下次启动起生效）",L"toggle",1,0,1,0,true,L"时间与历史",true);
    add(L"OpenNR",L"MochizukiInt4",L"int4 混合网络(原始模型)",L"toggle",0,0,1,0,true,L"时间与历史",true);
    // Composition and Look controls.
    add(L"OpenNR",L"MochizukiRenoColorStrength",L"NR 色彩",L"range",100,0,100,0,true,L"合成",true);
    add(L"OpenNR",L"MochizukiRenoTransferStrength",L"保留的亮度变化",L"range",100,0,100,0,true,L"合成",true,false,true);
    add(L"OpenNR",L"MochizukiRenoChromaClamp",L"最大色彩变化",L"range",100,25,200,0,true,L"合成",true,false,true);
    add(L"OpenNR",L"MochizukiRenoTransferMode",L"传输模式",L"choice",0,0,1,2,true,L"合成",true,false,true);
    add(L"OpenNR",L"MochizukiRenoSdrBlackRestore",L"SDR 黑位恢复",L"choice",0,0,2,3,true,L"合成",true);
    add(L"OpenNR",L"MochizukiRenoCodecMode",L"编解码 (HDR)",L"choice",2,0,3,4,true,L"合成",true,false,true);
    add(L"OpenNR",L"MochizukiRenoLookMode",L"调整 NR 的外观",L"toggle",0,0,1,0,true,L"调整 NR 的外观",true);
    add(L"OpenNR",L"MochizukiRenoLookStrength",L"编辑强度",L"range",100,0,200,0,true,L"调整 NR 的外观",true);
    add(L"OpenNR",L"MochizukiRenoLookBrighten",L"提亮",L"range",100,0,200,0,true,L"调整 NR 的外观",true);
    add(L"OpenNR",L"MochizukiRenoLookDarken",L"压暗",L"range",100,0,200,0,true,L"调整 NR 的外观",true);
    add(L"OpenNR",L"MochizukiRenoLookColour",L"色彩",L"range",100,0,200,0,true,L"调整 NR 的外观",true);
    add(L"OpenNR",L"MochizukiRenoLookHue",L"色相偏移",L"range",100,0,200,0,true,L"调整 NR 的外观",true);
    add(L"OpenNR",L"MochizukiRenoLookShadows",L"阴影",L"range",100,0,200,0,true,L"调整 NR 的外观",true);
    add(L"OpenNR",L"MochizukiRenoLookMidtones",L"中间调",L"range",100,0,200,0,true,L"调整 NR 的外观",true);
    add(L"OpenNR",L"MochizukiRenoLookHighlights",L"高光",L"range",100,0,200,0,true,L"调整 NR 的外观",true);
    add(L"OpenNR",L"MochizukiRenoLookMaxBrighten",L"最大提亮 (0: 不限)",L"range",0,0,400,0,true,L"调整 NR 的外观",true);
    add(L"OpenNR",L"MochizukiRenoLookMaxDarken",L"最大压暗 (0: 不限)",L"range",0,0,400,0,true,L"调整 NR 的外观",true);
    add(L"OpenNR",L"MochizukiRenoLookMaxColour",L"最大色彩变化 (0: 不限)",L"range",0,0,200,0,true,L"调整 NR 的外观",true);
    add(L"OpenNR",L"MochizukiRenoLookTone",L"大尺度色调",L"range",100,0,200,0,true,L"调整 NR 的外观",true);
    add(L"OpenNR",L"MochizukiRenoLookDetail",L"精细细节",L"range",100,0,200,0,true,L"调整 NR 的外观",true);
    add(L"OpenNR",L"MochizukiRenoLookDetailRadius",L"细节半径",L"range",100,25,400,0,true,L"调整 NR 的外观",true);
    add(L"OpenNR",L"MochizukiRenoLookHalo",L"光晕抑制",L"range",0,0,100,0,true,L"调整 NR 的外观",true);
    add(L"OpenNR",L"MochizukiRenoLookHaloBrightSide",L"暗物体旁的光晕",L"range",0,0,100,0,true,L"调整 NR 的外观",true);
    // Source encoding and HDR transfer calibration.
    add(L"OpenNR",L"MochizukiRenoSourceEncoding",L"源编码",L"choice",0,0,3,4,true,L"编码",true);
    add(L"OpenNR",L"MochizukiWhitePoint",L"白点 (Linear，0：从帧中测得)",L"range",0,0,1600,0,true,L"编码",true,false,true);
    add(L"OpenNR",L"MochizukiRenoLinearUnitNits",L"线性单位",L"range",0,0,10000,0,true,L"编码",true,false,true);
    add(L"OpenNR",L"MochizukiRenoProxyAnchor",L"代理锚点",L"range",40,5,320,0,true,L"编码",true,false,true);
    add(L"OpenNR",L"MochizukiRenoPaperWhiteScale",L"纸白缩放",L"range",25375,50,160000,0,true,L"编码",true,false,true);
    add(L"OpenNR",L"MochizukiRenoDiffuseWhiteNits",L"漫反射白",L"range",203,80,1000,0,true,L"编码",true,false,true);
    add(L"OpenNR",L"MochizukiRenoPQCalibration",L"PQ 校准",L"range",1000,5,16000,0,true,L"编码",true,false,true);
    add(L"OpenNR",L"MochizukiRenoFeedMode",L"输入",L"choice",0,0,2,3,true,L"编码",true,false,true);
    // Preprocess.
    add(L"OpenNR",L"MochizukiPreprocess",L"预处理",L"toggle",0,0,1,0,true,L"预处理",true);
    add(L"OpenNR",L"MochizukiPreprocessExposure",L"曝光",L"choice",1,0,2,3,true,L"预处理",true);
    add(L"OpenNR",L"MochizukiPreprocessBias",L"曝光偏置",L"range",0,-300,300,0,true,L"预处理",true);
    add(L"OpenNR",L"MochizukiPreprocessCurve",L"曲线",L"choice",3,0,6,7,true,L"预处理",true);
    add(L"OpenNR",L"MochizukiPreprocessContrast",L"对比度",L"range",100,50,200,0,true,L"预处理",true);
    add(L"OpenNR",L"MochizukiPreprocessSaturation",L"饱和度",L"range",100,5,200,0,true,L"预处理",true);
    add(L"OpenNR",L"MochizukiPreprocessInverse",L"通过曲线撤回改动",L"toggle",1,0,1,0,true,L"预处理",true);
    // Fast processing modes.
    add(L"OpenNR",L"MochizukiRenoFastMethod",L"快速方式",L"choice",0,0,3,4,true,L"快速通道",true,false,false,true);
    add(L"OpenNR",L"MochizukiRenoFastExtrapolationPasses",L"外推通道数",L"range",300,100,800,0,true,L"快速通道",true,false,false,true);
    add(L"OpenNR",L"MochizukiRenoFastExtrapolationHaloLimit",L"外推光晕上限",L"toggle",0,0,1,0,true,L"快速通道",true,false,false,true);
    add(L"OpenNR",L"MochizukiRenoFastExtrapolationFine",L"外推精细细节",L"range",0,0,200,0,true,L"快速通道",true,false,false,true);
    add(L"OpenNR",L"MochizukiRenoFastFeedbackPasses",L"反馈通道数",L"range",300,100,800,0,true,L"快速通道",true,false,false,true);
    add(L"OpenNR",L"MochizukiRenoFastFeedbackHaloLimit",L"反馈光晕上限",L"toggle",0,0,1,0,true,L"快速通道",true,false,false,true);
    add(L"OpenNR",L"MochizukiRenoFastFeedbackFine",L"反馈精细细节",L"range",100,0,200,0,true,L"快速通道",true,false,false,true);
    add(L"OpenNR",L"MochizukiRenoFastHybridPasses",L"混合通道数",L"range",300,100,800,0,true,L"快速通道",true,false,false,true);
    add(L"OpenNR",L"MochizukiRenoFastHybridHaloLimit",L"混合光晕上限",L"toggle",0,0,1,0,true,L"快速通道",true,false,false,true);
    add(L"OpenNR",L"MochizukiRenoFastHybridFine",L"混合精细细节",L"range",100,0,200,0,true,L"快速通道",true,false,false,true);
    add(L"OpenNR",L"MochizukiRenoFastEditStrength",L"编辑强度",L"range",200,0,400,0,true,L"快速通道",true,false,false,true);
    // NRLive 0.0.6 Bridge options; shown only when the OpenNR runtime DLL is absent.
    add(L"DLSSNR",L"model",L"模型",L"choice",0,0,1,2,false,L"Bridge 基础",false,true);
    add(L"DLSSNR",L"style",L"风格",L"choice",0,0,2,3,false,L"Bridge 基础",false,true);
    add(L"DLSSNR",L"scale",L"NR 运行分辨率",L"range",100,25,100,0,false,L"Bridge 基础",false,true);
    add(L"DLSSNR",L"intensity",L"强度",L"range",100,0,200,0,false,L"Bridge 基础",false,true);
    add(L"DLSSNR",L"passes",L"处理次数",L"range",1,1,3,0,true,L"Bridge 高级",false,true);
    add(L"DLSSNR",L"structure",L"结构",L"range",100,0,200,0,true,L"Bridge 高级",false,true);
    add(L"DLSSNR",L"color_strength",L"NR 色彩",L"range",0,0,100,0,true,L"Bridge 高级",false,true);
    add(L"DLSSNR",L"local_tone",L"局部色调",L"range",100,0,200,0,true,L"Bridge 高级",false,true);
    add(L"DLSSNR",L"max_ratio",L"最大比例",L"range",200,100,800,0,true,L"Bridge 高级",false,true);
    add(L"DLSSNR",L"skin_structure",L"皮肤结构",L"range",-100,-100,200,0,true,L"Bridge 高级",false,true);
    add(L"DLSSNR",L"history_strength",L"历史强度",L"range",20,0,100,0,true,L"Bridge 高级",false,true);
    add(L"DLSSNR",L"automatic_skin_mask",L"自动识别皮肤",L"toggle",1,0,1,0,true,L"Bridge 高级",false,true);
    add(L"DLSSNR",L"stabilizer",L"稳定器",L"toggle",1,0,1,0,true,L"Bridge 高级",false,true);
    m_nrValues.resize(m_nrSettings.size());m_nrSupported.resize(m_nrSettings.size());m_nrHitRects.resize(m_nrSettings.size());m_nrSliderRects.resize(m_nrSettings.size());
    for(size_t i=0;i<m_nrSettings.size();++i){m_nrValues[i]=m_nrSettings[i].def;m_nrSupported[i]=IsNrSettingSupported(m_nrSettings[i]);}
}

bool CDlss5SwapperDlg::IsNrSettingSupported(const NrUiSetting& s) const
{
    if(!s.pending) return true;
    static std::wstring cachedExe; static std::string b;
    if(cachedExe!=m_nrLiveExe){cachedExe=m_nrLiveExe;b.clear();std::ifstream f(m_nrLiveExe,std::ios::binary);if(f)b.assign(std::istreambuf_iterator<char>(f),{});}
    if(b.empty())return false;
    auto has=[&](const std::wstring& w,bool wide){std::string n;for(wchar_t c:w){n.push_back((char)c);if(wide)n.push_back('\0');}n.push_back('\0');if(wide)n.push_back('\0');return b.find(n)!=std::string::npos;};
    return has(s.section,true) && (has(s.key,true)||(s.section==L"OpenNR"&&has(s.key,false)));
}

void CDlss5SwapperDlg::LoadNrSettings()
{
    InitNrSettings();
    for(size_t i=0;i<m_nrSettings.size();++i){
        const auto& s=m_nrSettings[i]; wchar_t sec[128],key[128]; wcscpy_s(sec,s.section.c_str()); wcscpy_s(key,s.key.c_str());
        std::wstring raw=IniRead(sec,key,L"");
        if(raw.empty()){m_nrValues[i]=s.def;continue;}
        if(s.kind==L"toggle") { m_nrValues[i]=NrParseBool(raw,s.def)?1:0; continue; }
        if(s.key==L"MochizukiHistoryPrecision") { m_nrValues[i]=(raw==L"32")?1:0; continue; }
        wchar_t* tail=nullptr; double d=wcstod(raw.c_str(),&tail);
        if(tail==raw.c_str()) { m_nrValues[i]=s.def; continue; }
        int v=(int)std::lround(d*s.scale);
        m_nrValues[i]=s.kind==L"choice"?std::clamp(v,0,(std::max)(0,s.choices-1)):std::clamp(v,s.min,s.max);
    }
    const std::wstring dll1=m_configDir+L"\\opennr\\MochizukiNrRuntime.dll";
    const std::wstring dll2=m_configDir+L"\\MochizukiNrRuntime.dll";
    const bool hasOpenDll=GetFileAttributesW(dll1.c_str())!=INVALID_FILE_ATTRIBUTES || GetFileAttributesW(dll2.c_str())!=INVALID_FILE_ATTRIBUTES;
    // Match NrLiveConfig.Tags(): a runtime override is honored only when this NRLive build reads it.
    std::string exeBytes; { std::ifstream f(m_nrLiveExe,std::ios::binary); if(f) exeBytes.assign(std::istreambuf_iterator<char>(f),{}); }
    auto containsWide=[&](const wchar_t* text){ std::string needle; for(const wchar_t* q=text;*q;++q){needle.push_back((char)*q);needle.push_back('\0');} needle.push_back('\0');needle.push_back('\0'); return !needle.empty()&&exeBytes.find(needle)!=std::string::npos; };
    m_nrRuntimeChoiceSupported=containsWide(L"DLSSNR") && (containsWide(L"runtime"));
    const std::wstring runtimeChoice=IniRead(L"DLSSNR",L"runtime",L"auto");
    if(m_nrRuntimeChoiceSupported && runtimeChoice==L"opennr") m_nrOpenRuntime=true;
    else {
        if(m_nrRuntimeChoiceSupported && runtimeChoice==L"bridge") IniWrite(L"DLSSNR",L"runtime",L"auto");
        m_nrOpenRuntime=hasOpenDll;
    }
}

void CDlss5SwapperDlg::SaveNrSettings()
{
    for(size_t i=0;i<m_nrSettings.size();++i){
        const auto& s=m_nrSettings[i];
        if(s.bridge == m_nrOpenRuntime) continue;
        if(s.pending && (i>=m_nrSupported.size() || !m_nrSupported[i])) continue;
        wchar_t sec[128],key[128]; wcscpy_s(sec,s.section.c_str()); wcscpy_s(key,s.key.c_str());
        std::wstring value;
        if(s.key==L"MochizukiHistoryPrecision") value=m_nrValues[i]==0?L"16":L"32";
        else if(s.scale>1){std::wostringstream os;os<<std::fixed<<std::setprecision(6)<<(m_nrValues[i]/(double)s.scale);value=os.str();while(value.find(L'.')!=std::wstring::npos&&value.back()==L'0')value.pop_back();if(!value.empty()&&value.back()==L'.')value.pop_back();}
        else value=Num(m_nrValues[i]);
        IniWrite(sec,key,value);
    }
}

void CDlss5SwapperDlg::UpdateNrSliderAt(CPoint p)
{
    if(m_activeNrSlider<0||m_activeNrSlider>=(int)m_nrSettings.size())return;
    auto& s=m_nrSettings[m_activeNrSlider];CRect r=m_nrSliderRects[m_activeNrSlider];if(r.Width()<=0)return;
    double f=std::clamp((double)(p.x-r.left)/(double)r.Width(),0.0,1.0);m_nrValues[m_activeNrSlider]=s.min+(int)std::round(f*(s.max-s.min));Invalidate(FALSE);
}

void CDlss5SwapperDlg::DrawNrTab(Graphics& g)
{
    InitNrSettings();
    std::fill(m_nrHitRects.begin(),m_nrHitRects.end(),CRect(0,0,0,0));
    std::fill(m_nrSliderRects.begin(),m_nrSliderRects.end(),CRect(0,0,0,0));
    m_nrMoreRect=CRect(0,0,0,0);
    m_nrGroupRects.clear();
    T(g,L"神经渲染 / NRLive 配置",35,525,15,C(255,224,218,232),true);
    DrawToggle(g,35,560,L"启用 DLSSNR 神经渲染",m_nrEnabled);
    const int left=42, width=505; int y=610; std::wstring lastGroup; bool groupExpanded=true;
    for(size_t i=0;i<m_nrSettings.size();++i){
        const auto& s=m_nrSettings[i];
        if(s.bridge != !m_nrOpenRuntime) continue;
        if(s.pending && !m_nrSupported[i]) continue;
        if(s.advanced && !m_moreNr) continue;
        if(s.group!=lastGroup){
            lastGroup=s.group;
            if(!m_nrGroupExpanded.count(lastGroup)) m_nrGroupExpanded[lastGroup]=(lastGroup==L"基础"||lastGroup==L"Bridge 基础");
            groupExpanded=m_nrGroupExpanded[lastGroup];
            Round(g,RectF(35,(REAL)y,520,30),6,C(255,31,27,39));
            Outline(g,RectF(35,(REAL)y,520,30),6,C(255,70,54,82));
            T(g,(groupExpanded?L"▼  ":L"▶  "),47,(REAL)(y+8),10,C(255,183,151,211),true);
            T(g,s.group.c_str(),68,(REAL)(y+7),11,C(255,205,184,220),true);
            m_nrGroupRects.push_back({CRect(35,y,555,y+30),lastGroup});
            y+=36;
        }
        if(!groupExpanded) { m_nrHitRects[i]=CRect(0,0,0,0); m_nrSliderRects[i]=CRect(0,0,0,0); continue; }
        const bool settingEnabled=IsNrChoiceEnabled(m_nrSettings,m_nrValues,i);
        m_nrHitRects[i]=CRect(0,0,0,0);m_nrSliderRects[i]=CRect(0,0,0,0);
        if(s.kind==L"toggle"){
            T(g,s.label.c_str(),(REAL)left,(REAL)(y+8),11,settingEnabled?C(255,210,203,218):C(255,112,108,120));
            Round(g,RectF(485,(REAL)(y+5),54,25),12,settingEnabled?(m_nrValues[i]?C(255,130,55,160):C(255,54,49,61)):C(255,42,40,46));
            SolidBrush knob(C(255,245,235,248));g.FillEllipse(&knob,m_nrValues[i]?RectF(515,(REAL)(y+9),17,17):RectF(490,(REAL)(y+9),17,17));
            if(settingEnabled)m_nrHitRects[i]=CRect(35,y,545,y+36);y+=42;
        }else if(s.kind==L"range"){
            T(g,s.label.c_str(),(REAL)left,(REAL)(y+1),11,settingEnabled?C(255,210,203,218):C(255,112,108,120));
            std::wstring val;
            if(s.scale>1){std::wostringstream os;os<<std::fixed<<std::setprecision(2)<<(m_nrValues[i]/(double)s.scale);val=os.str();while(val.find(L'.')!=std::wstring::npos&&val.back()==L'0')val.pop_back();if(val.back()==L'.')val.pop_back();}
            else val=Num(m_nrValues[i]);
            const bool stopUnit=(s.key==L"MochizukiRenoLookMaxBrighten"||s.key==L"MochizukiRenoLookMaxDarken"||s.key==L"MochizukiRenoChromaClamp");
            const bool nitUnit=(s.key.find(L"Nits")!=std::wstring::npos||s.key==L"MochizukiRenoProxyAnchor");
            if(stopUnit) val+=L" stops";
            else if(nitUnit) val+=L" nits";
            else if(s.key==L"MochizukiPreprocessBias") val+=L" EV";
            else if(s.key==L"MochizukiRenoProxyAnchor"||s.key==L"MochizukiRenoDiffuseWhiteNits"||s.key==L"MochizukiRenoLinearUnitNits") val+=L" nits";
            else if(s.key==L"MochizukiRenoPaperWhiteScale") val+=L" %";
            else if(s.key==L"MochizukiRenoIntensity"||s.key==L"MochizukiRenoLocalTone"||s.key.find(L"Strength")!=std::wstring::npos||s.key.find(L"Structure")!=std::wstring::npos||s.key.find(L"Look")!=std::wstring::npos||s.key==L"MochizukiPreprocessContrast"||s.key==L"MochizukiPreprocessSaturation"||s.key==L"MochizukiRenoFastExtrapolationFine"||s.key==L"MochizukiRenoFastFeedbackFine"||s.key==L"MochizukiRenoFastHybridFine"||s.section==L"DLSSNR") val+=L"%";
            T(g,val.c_str(),490,(REAL)(y+1),10,C(255,229,220,237),true);
            Round(g,RectF(left,(REAL)(y+25),width,7),3,C(255,52,49,58));
            float f=(float)(m_nrValues[i]-s.min)/(float)(s.max-s.min);Round(g,RectF(left,(REAL)(y+25),width*std::clamp(f,0.0f,1.0f),7),3,settingEnabled?C(255,130,63,169):C(255,76,70,82));
            SolidBrush knob(settingEnabled?C(255,177,93,210):C(255,91,85,98));g.FillEllipse(&knob,RectF(left+width*f-6,(REAL)(y+20),12,17));
            if(settingEnabled)m_nrSliderRects[i]=CRect(left,y+17,left+width,y+43);y+=52;
        }else{
            T(g,s.label.c_str(),(REAL)left,(REAL)(y+8),11,settingEnabled?C(255,210,203,218):C(255,112,108,120));
            std::wstring value=NrChoiceLabel(s,m_nrValues[i]);
            Round(g,RectF(400,(REAL)(y+2),140,30),6,settingEnabled?C(255,39,34,47):C(255,31,30,35));Outline(g,RectF(400,(REAL)(y+2),140,30),6,settingEnabled?C(255,91,67,111):C(255,62,58,68));
            T(g,value.c_str(),410,(REAL)(y+10),10,settingEnabled?C(255,228,218,236):C(255,112,108,120));if(settingEnabled)m_nrHitRects[i]=CRect(395,y,545,y+36);y+=42;
        }
    }
    if(!m_moreNr){DrawButton(g,RectF(35,(REAL)(y+8),520,42),L"展开全部神经渲染高级设置  ▼");}
    else {DrawButton(g,RectF(35,(REAL)(y+8),520,42),L"收起高级设置  ▲");}
    m_nrMoreRect=CRect(35,y+8,555,y+50);
}

void CDlss5SwapperDlg::DrawOverlayTab(Graphics& g)
{
    T(g,L"帧率监控 / HUD",35,525,15,C(255,224,218,232),true);
    DrawToggle(g,35,560,L"启用 NRLive 帧率监控",m_overlayEnabled);
    if(m_hudNew) {
        DrawCombo(g,35,605,520,L"HUD 预设",{L"自定义",L"极简",L"紧凑",L"标准",L"完整"},m_hudPreset,6);
        DrawCombo(g,35,650,520,L"计数器位置",{L"左上",L"右上",L"左下",L"右下",L"顶部居中",L"底部居中"},m_hudPosition,7);
    } else {
        T(g,L"旧版 HUD（当前 NRLive 不支持新版预设）",35,610,11,C(255,180,170,190));
        DrawCombo(g,35,640,520,L"计数器位置",{L"左上",L"右上",L"左下",L"右下"},m_hudPosition,7);
    }
    T(g,L"屏幕上显示的项目（点击切换）",35,700,12,C(255,171,164,181));
    const wchar_t* names[]={L"每秒帧数",L"标题",L"FPS 详细信息",L"帧时间",L"帧时间图表",L"1% 和 0.1% 低帧",L"GPU 占用",L"GPU 温度",L"VRAM",L"CPU 占用",L"FSR、NR 和 FG 状态",L"分辨率",L"GPU 频率",L"系统内存",L"各处理阶段 GPU 时间",L"延迟",L"时钟"};
    // Match NrLivePage.Rows.cs: the new HUD exposes eleven tiles; the six additional
    // preset-managed keys (title/fps_detail/gpu_clock/ram/latency/clock) are not manual tiles.
    const int itemCount=(int)m_hudTileMap.size();
    const int tileY=730;
    for(int i=0;i<itemCount;i++) {
        const int settingIndex=m_hudTileMap[i]; const bool on=settingIndex<0?m_hudCounterEnabled:m_hudItems[settingIndex];
        const wchar_t* label=settingIndex<0?L"帧数计数器":names[settingIndex];
        int col=i%2,row=i/2; float x=35.0f+static_cast<float>(col)*265.0f,y=static_cast<float>(tileY+row*42);
        Round(g,RectF(x,y,250,35),6,on?C(255,49,31,61):C(255,28,27,34));
        Outline(g,RectF(x,y,250,35),6,on?C(255,112,67,143):C(255,62,54,64));
        T(g,label,x+9,y+10,10,C(255,214,207,218));
        Round(g,RectF(x+207,y+7,32,21),11,on?C(255,116,54,153):C(255,57,55,62));
        SolidBrush b(C(255,245,235,238)); g.FillEllipse(&b,on?RectF(x+221,y+9,16,16):RectF(x+209,y+9,16,16));
    }
    const int afterTiles=tileY+((itemCount+1)/2)*42+8;
    Round(g,RectF(35,(REAL)afterTiles,520,48),7,C(255,19,18,24)); Outline(g,RectF(35,(REAL)afterTiles,520,48),7,C(255,63,52,67));
    T(g,L"HUD 预览",48,(REAL)(afterTiles+17),11,C(255,164,154,170));
    // This is a configuration preview, so keep it visible even when the master
    // NRLive overlay switch is currently off. The runtime overlay itself still
    // obeys m_overlayEnabled; only this sample strip is independent.
    struct HudPreviewPart { std::wstring text; Color color; };
    std::vector<HudPreviewPart> previewParts;
    auto addPreview=[&](int i,const wchar_t* text,Color color=C(255,224,216,235)) {
        if(m_hudItems[i]) previewParts.push_back({text,color});
    };
    if(m_hudNew) {
        std::wstring pipe;
        if(m_upscaler==0) pipe=L"FSR4"; else if(m_upscaler==1) pipe=L"FSR3.1";
        if(m_nrEnabled) { if(!pipe.empty()) pipe+=L" + "; pipe+=L"NR"; }
        if(m_frameGeneration) { if(!pipe.empty()) pipe+=L" + "; pipe+=L"FG"; }
        if(pipe.empty()) pipe=L"NR";
        addPreview(10,pipe.c_str(),C(255,235,104,112));
        addPreview(6,L"GPU 97%",C(255,244,191,105));
        addPreview(7,L"64°C",C(255,244,191,105));
    }
    if(m_hudCounterEnabled) previewParts.push_back({L"142 FPS",C(255,106,220,151)});
    addPreview(3,L"6.9 ms",C(255,224,216,235));
    if(m_hudNew) {
        addPreview(5,L"1% 118",C(255,224,216,235));
        addPreview(8,L"VRAM 9.1 GB",C(255,224,216,235));
        addPreview(9,L"CPU 34%",C(255,224,216,235));
        addPreview(14,L"NR 2.1 ms",C(255,224,216,235));
    }
    addPreview(11,L"1440p → 4K",C(255,224,216,235));
    FontFamily previewFamily(L"Microsoft YaHei UI");
    Font previewFont(&previewFamily,9.0f*1.30f,FontStyleRegular,UnitPixel);
    REAL previewX=130.0f;
    const REAL previewRight=35.0f+520.0f-12.0f;
    for(size_t pi=0;pi<previewParts.size();++pi) {
        RectF measured;
        g.MeasureString(previewParts[pi].text.c_str(),-1,&previewFont,PointF(0,0),&measured);
        const REAL separatorWidth=(pi==0)?0.0f:18.0f;
        if(previewX+separatorWidth+measured.Width>previewRight) break;
        if(pi>0) {
            T(g,L"|",previewX+5.0f,(REAL)(afterTiles+17),9,C(255,105,91,116));
            previewX+=separatorWidth;
        }
        T(g,previewParts[pi].text.c_str(),previewX,(REAL)(afterTiles+17),9,previewParts[pi].color,true);
        previewX+=measured.Width+8.0f;
    }
    if(previewParts.empty()) T(g,L"未选择显示项目",130,(REAL)(afterTiles+17),9,C(255,130,122,142));
    DrawButton(g,RectF(35,(REAL)(afterTiles+58),520,42),m_moreOverlay?L"收起 HUD 外观设置  ▲":L"HUD 外观设置  ▼");
    if(m_moreOverlay) {
        int y=afterTiles+112;
        DrawToggle(g,35,(REAL)y,L"显示背景",m_hudBackground); y+=42;
        DrawToggle(g,35,(REAL)y,L"文字描边",m_hudTextOutline); y+=48;
        if(m_hudNew) DrawSlider(g,8,45,(REAL)y,460,L"HUD 大小",m_hudScale,50,250,L"%");
        else DrawSlider(g,9,45,(REAL)y,460,L"文字大小",m_hudFontSize,12,48,L" px");
        y+=60;
        DrawSlider(g,10,45,(REAL)y,460,L"背景不透明度",m_hudBackgroundAlpha,0,100,L"%");
    }
}

int CDlss5SwapperDlg::GetCurrentContentBottom() const
{
    // Content coordinates are in the 600x1240 design canvas.
    // The viewport is y=515..1175. These bottoms grow when advanced
    // sections are opened, allowing the page to scroll instead of clipping.
    if (m_tab == 0) return m_morePicture ? 1215 : 970;
    if (m_tab == 1) {
        int y=610; std::wstring group; bool expanded=true;
        for(size_t i=0;i<m_nrSettings.size();++i){
            const auto& s=m_nrSettings[i]; if(s.bridge != !m_nrOpenRuntime)continue;
            if(s.pending&&!m_nrSupported[i])continue; if(s.advanced&&!m_moreNr)continue;
            if(s.group!=group){group=s.group;auto it=m_nrGroupExpanded.find(group);expanded=(it!=m_nrGroupExpanded.end()?it->second:(group==L"基础"||group==L"Bridge 基础"));y+=36;}
            if(expanded)y+=(s.kind==L"range"?52:42);
        }
        return y+70;
    }
    return m_moreOverlay ? (m_hudNew ? 1540 : 1260) : (m_hudNew ? 1390 : 1130);
}

void CDlss5SwapperDlg::UpdateScrollRange()
{
    constexpr int viewportTop = 515;
    constexpr int viewportBottom = 1175;
    const int contentBottom = GetCurrentContentBottom();
    m_scrollMax = (std::max)(0, contentBottom - viewportBottom);
    m_scrollOffset = (std::clamp)(m_scrollOffset, 0, m_scrollMax);
}

void CDlss5SwapperDlg::DrawScrollBar(Graphics& g)
{
    if (m_scrollMax <= 0) {
        m_scrollThumb = CRect(0, 0, 0, 0);
        return;
    }

    constexpr float trackX = 565.0f;
    constexpr float trackY = 522.0f;
    constexpr float trackH = 642.0f;
    constexpr float viewportH = 660.0f;
    const float contentH = viewportH + static_cast<float>(m_scrollMax);
    const float thumbH = (std::max)(42.0f, trackH * viewportH / contentH);
    const float travel = trackH - thumbH;
    const float ratio = m_scrollMax > 0 ? static_cast<float>(m_scrollOffset) / m_scrollMax : 0.0f;
    const float thumbY = trackY + travel * ratio;

    Round(g, RectF(trackX, trackY, 5.0f, trackH), 2.5f, C(255, 43, 39, 51));
    Round(g, RectF(trackX, thumbY, 5.0f, thumbH), 2.5f, C(255, 153, 82, 190));
    m_scrollThumb = CRect(static_cast<int>(trackX - 7), static_cast<int>(thumbY),
                          static_cast<int>(trackX + 12), static_cast<int>(thumbY + thumbH));
}

void CDlss5SwapperDlg::DrawInterface(Graphics& g,int cx,int cy)
{
    m_sliderRects.fill(CRect(0,0,0,0));
    m_comboRects.fill(CRect(0,0,0,0));
    auto st=g.Save();
    // Uniform scale + centered letterboxing: never stretch the original UI
    // horizontally or vertically when the user changes the window aspect ratio.
    const REAL uiScale=(REAL)(std::min)((double)cx/DW,(double)cy/DH);
    const REAL offsetX=((REAL)cx-DW*uiScale)*0.5f;
    const REAL offsetY=((REAL)cy-DH*uiScale)*0.5f;
    Matrix uiTransform(uiScale,0.0f,0.0f,uiScale,offsetX,offsetY);
    g.SetTransform(&uiTransform);
    Round(g,RectF(4,4,592,1232),22,C(255,17,15,23));
    Outline(g,RectF(4,4,592,1232),22,C(255,107,62,134),1.5f);
    // Keep the console title visible. The large master switch is rendered
    // inside the first control panel below, matching the requested layout.
    T(g,L"DLSS5控制台 v1.0",30,22,16,C(255,231,224,240),true);
    m_startToggleRect = CRect(164,101,274,149);
    std::wstring startHint;
    if (m_startSwitchOn) {
        if (m_startCountdownActive) {
            ULONGLONG now = GetTickCount64();
            int remain = now < m_startCountdownEnd
                ? (int)((m_startCountdownEnd - now + 999) / 1000) : 0;
            startHint = remain > 0 ? (L"启动倒计时 " + Num(remain) + L" 秒") : L"正在运行";
        } else {
            startHint = L"正在运行";
        }
    } else {
        startHint = L"启动倒计时 " + Num(m_delay) + L" 秒";
    }
    m_bilibiliRect = CRect(350,20,414,48);
    m_qqGroupRect = CRect(420,20,478,48);
    m_minimizeRect = CRect(492,20,522,48);
    m_maximizeRect = CRect(528,20,558,48);
    m_closeRect = CRect(564,20,594,48);
    // Compact community shortcuts in the title bar, immediately left of the window controls.
    Round(g,RectF(350,20,64,28),7,C(255,48,30,62));
    Outline(g,RectF(350,20,64,28),7,C(255,126,65,157),1.0f);
    T(g,L"bilibili",358,24,13,C(255,244,224,249),true);
    Round(g,RectF(420,20,58,28),7,C(255,48,30,62));
    Outline(g,RectF(420,20,58,28),7,C(255,126,65,157),1.0f);
    T(g,L"QQ群",429,24,13,C(255,244,224,249),true);
    Round(g,RectF(492,20,30,28),5,C(255,44,36,57));
    Round(g,RectF(528,20,30,28),5,C(255,44,36,57));
    Round(g,RectF(564,20,30,28),5,C(255,44,36,57));
    Pen titlePen(C(255,193,179,203),1.6f);
    g.DrawLine(&titlePen,500,35,514,35); // minimize
    if(IsZoomed()) {
        g.DrawRectangle(&titlePen,536,27,13,11);
        g.DrawRectangle(&titlePen,539,24,13,11);
    } else {
        g.DrawRectangle(&titlePen,536,26,14,13);
    }
    g.DrawLine(&titlePen,573,28,585,40); g.DrawLine(&titlePen,585,28,573,40);

    // Session / hotkeys area. The three recordable chords and Bind Bypass are editable.
    // First panel: reserve a dedicated 64px header band for the DLSS5 master
    // control, with the capture/hotkey controls occupying the lower body.
    Round(g,RectF(22,82,556,250),10,C(255,25,22,31));
    Outline(g,RectF(22,82,556,250),10,C(255,57,47,63));

    // Always-visible master control row. This row is outside the settings
    // content area, so DLSS5 and its switch remain visible on both pages.
    Round(g,RectF(29,89,542,60),8,C(255,29,25,35));
    T(g,L"DLSS5",40,103,14,C(255,235,227,243),true);
    Round(g,RectF(164,101,82,42),21,
          m_startSwitchOn?C(255,119,55,160):C(255,57,52,66));
    Outline(g,RectF(164,101,82,42),21,
            m_startSwitchOn?C(255,169,103,207):C(255,87,78,96),1.0f);
    SolidBrush switchKnob(m_startSwitchOn?C(255,245,238,250):C(255,177,170,184));
    g.FillEllipse(&switchKnob,m_startSwitchOn?RectF(207,106,32,32):RectF(171,106,32,32));
    T(g,startHint.c_str(),260,111,10,C(255,193,181,205),true);

    m_settingsButtonRect=CRect(444,100,558,145);
    Round(g,RectF(444,100,114,42),7,C(255,76,42,96));
    Outline(g,RectF(444,100,114,42),7,C(255,157,94,190),1.1f);
    T(g,m_settingsOpen?L"返回主界面":L"设置",458,110,12,C(255,245,237,250),true);

    if (m_settingsOpen) {
        // Full-page Settings view: replace the feature tabs and their content.
        Round(g,RectF(22,156,556,1016),10,C(255,25,22,31));
        Outline(g,RectF(22,156,556,1016),10,C(255,57,47,63));
        T(g,L"设置",40,169,18,C(255,235,227,243),true);

        // Capture/launch controls. Capture source and motion vector remain in
        // their own separate section below, matching the earlier layout.
        Round(g,RectF(35,208,530,185),9,C(255,29,25,35));
        Outline(g,RectF(35,208,530,185),9,C(255,57,47,63));
        T(g,L"捕获与启动",52,222,13,C(255,225,218,233),true);

        T(g,L"捕获对象",52,267,11,C(255,166,158,176));
        DrawCombo(g,52,282,245,L"",{L"倒计时后捕获前台窗口",L"选择指定窗口",L"选择指定进程"},m_targetMode,0);
        m_targetItemRect=CRect(309,282,548,320);
        Round(g,RectF(309,282,239,38),6,C(255,34,29,38));
        Outline(g,RectF(309,282,239,38),6,C(255,67,54,76));
        std::wstring targetLabel=m_targetMode==0?L"无需选择目标窗口":(m_targetText.empty()?L"点击选择目标并刷新列表":(m_targetLabel.empty()?m_targetText:m_targetLabel));
        T(g,targetLabel.c_str(),320,294,9,C(255,215,205,224));

        // Administrator toggle and startup countdown share one row.
        // Keep the countdown group left of the fixed right-side toggle switch.
        DrawToggle(g,52,333,L"以管理员身份运行",m_runAsAdmin);
        DrawCombo(g,288,330,260,L"",{L"3 秒",L"5 秒",L"10 秒",L"15 秒",L"20 秒",L"30 秒"},
            m_delay==3?0:m_delay==5?1:m_delay==10?2:m_delay==15?3:m_delay==20?4:5,8);
        T(g,L"启动倒计时",268,339,10,C(255,180,161,194));

        // Hotkeys section.
        Round(g,RectF(35,408,530,245),9,C(255,29,25,35));
        Outline(g,RectF(35,408,530,245),9,C(255,57,47,63));
        T(g,L"快捷键",52,422,13,C(255,225,218,233),true);
        Round(g,RectF(52,458,496,34),6,C(255,34,29,38));
        const std::wstring& statusLine=m_hotkeyStatus.empty()?m_status:m_hotkeyStatus;
        T(g,statusLine.c_str(),64,469,9,C(255,178,169,185));

        const wchar_t* hkLabels[]={L"缩放快捷键",L"停止快捷键",L"HUD 快捷键"};
        const std::wstring hkValues[]={m_scaleKey,m_stopKey,m_overlayKey};
        for(int i=0;i<3;i++) {
            int x=52+i*168; m_hotkeyRects[i]=CRect(x,511,x+156,561);
            bool active=m_activeHotkey==i+1;
            Round(g,RectF((REAL)x,511,156,50),6,active?C(255,70,43,86):C(255,34,29,38));
            Outline(g,RectF((REAL)x,511,156,50),6,active?C(255,190,115,225):C(255,67,54,76));
            T(g,hkLabels[i],x+10,519,8,C(255,166,158,176));
            std::wstring shown=active?L"按下组合键…":HotkeyDisplay(hkValues[i]);
            T(g,shown.c_str(),x+10,538,9,active?C(255,240,205,255):C(255,225,215,235),true);
        }

        m_bypassRect=CRect(52,581,548,627);
        Round(g,RectF(52,581,496,46),6,m_activeHotkey==4?C(255,70,43,86):C(255,34,29,38));
        Outline(g,RectF(52,581,496,46),6,m_activeHotkey==4?C(255,190,115,225):C(255,67,54,76));
        T(g,L"Bind Bypass",64,589,9,C(255,166,158,176),true);
        std::wstring bypassShown=m_activeHotkey==4?m_bypassKeys+L"  | 输入逗号分隔按键，Enter 保存，Esc 取消":m_bypassKeys;
        if(bypassShown.size()>67) bypassShown=bypassShown.substr(0,64)+L"...";
        T(g,bypassShown.c_str(),64,607,8,C(255,225,215,235));

        // Separate Capture Settings section, below the hotkeys panel.
        Round(g,RectF(35,668,530,126),9,C(255,29,25,35));
        Outline(g,RectF(35,668,530,126),9,C(255,57,47,63));
        T(g,L"捕获设置",52,682,13,C(255,225,218,233),true);
        DrawCombo(g,52,709,245,L"捕获来源",{L"WGC",L"DXGI"},m_captureMode,1);
        DrawCombo(g,309,709,239,L"运动矢量",{L"FastMV",L"AMD 光流"},m_motionMode,2);

        T(g,L"修改后自动保存到 scaleconfig.ini",52,817,9,C(255,150,142,164));
        // The Settings branch returns before the normal main-page popup pass.
        // Draw the popup last here, otherwise opening a selector only flashes.
        DrawOpenComboPopup(g);
        DrawCaptureTargetPopup(g);
        // Footer remains visible. Settings view intentionally has no feature tabs.
        Round(g,RectF(22,1185,556,38),8,C(255,31,23,43));
        T(g,L"设置保存至 scaleconfig.ini；启动时调用已安装的 NRLive.exe",34,1197,10,C(255,158,151,168));
        g.Restore(st);
        return;
    } else {
        // Main page: keep only the DLSS5 master switch and short launch guidance.
        T(g,L"点击右侧「设置」配置捕获方式、快捷键和启动选项",38,181,10,C(255,166,158,176));
        const std::wstring countdownText = L"当前启动倒计时：" + Num(m_delay) + L" 秒";
        T(g,countdownText.c_str(),38,221,11,C(255,215,205,224),true);
        T(g,L"快捷键与捕获参数保存在配置文件中",38,255,9,C(255,150,142,164));

        // Dependency manager sits directly below the Settings button.
        // Match the Settings button dimensions and center all dependency labels.
        constexpr REAL depX = 444.0f;
        constexpr REAL depW = 114.0f;
        constexpr REAL depH = 42.0f;
        m_installDependencyRect = CRect(444,164,558,206);
        Round(g,RectF(depX,164,depW,depH),7,C(255,76,42,96));
        Outline(g,RectF(depX,164,depW,depH),7,C(255,157,94,190),1.0f);
        const wchar_t* installLabel = m_installingDependencies?L"正在安装…":L"安装依赖";
        FontFamily depFontFamily(L"Microsoft YaHei UI");
        Font depFont(&depFontFamily,11.0f*1.30f,FontStyleBold,UnitPixel);
        RectF installMeasure;
        g.MeasureString(installLabel,-1,&depFont,PointF(0,0),&installMeasure);
        T(g,installLabel,depX+(depW-installMeasure.Width)/2.0f,
          164+(depH-installMeasure.Height)/2.0f,11,C(255,245,237,250),true);

        m_localCacheRect = CRect(444,216,558,258);
        Round(g,RectF(depX,216,depW,depH),7,C(255,34,29,42));
        Outline(g,RectF(depX,216,depW,depH),7,C(255,105,76,125),1.0f);
        const wchar_t* cacheLabel = L"本地缓存";
        RectF cacheMeasure;
        g.MeasureString(cacheLabel,-1,&depFont,PointF(0,0),&cacheMeasure);
        T(g,cacheLabel,depX+(depW-cacheMeasure.Width)/2.0f,
          216+(depH-cacheMeasure.Height)/2.0f,11,C(255,225,215,235),true);

        const bool localReady = HasLocalNrLive();
        const wchar_t* statusLabel = localReady?L"依赖已就绪":L"尚未安装";
        RectF statusMeasure;
        Font statusFont(&depFontFamily,9.0f*1.30f,FontStyleRegular,UnitPixel);
        g.MeasureString(statusLabel,-1,&statusFont,PointF(0,0),&statusMeasure);
        T(g,statusLabel,depX+(depW-statusMeasure.Width)/2.0f,263,9,
            localReady?C(255,133,205,158):C(255,190,150,160));
        // Installation progress is displayed in the wide reserved panel below the status.
        if (m_installingDependencies) {
            const REAL px = 39.0f, py = 279.0f, pw = 510.0f, ph = 36.0f;
            Round(g, RectF(px,py,pw,ph), 4, C(255,31,25,37));
            Outline(g, RectF(px,py,pw,ph), 4, C(255,139,63,172), 1.0f);
            const REAL trackX = px + 10.0f, trackY = py + 22.0f, trackW = pw - 20.0f;
            Round(g, RectF(trackX,trackY,trackW,5.0f), 3, C(255,65,57,72));
            int clampedProgress = m_dependencyProgress;
            if (clampedProgress < 0) clampedProgress = 0;
            if (clampedProgress > 100) clampedProgress = 100;
            const REAL fillW = trackW * (static_cast<REAL>(clampedProgress) / 100.0f);
            if (fillW > 0.5f) Round(g, RectF(trackX,trackY,fillW,5.0f), 3, C(255,166,75,202));
            std::wstring progressLabel = m_dependencyStage == 1 ? L"正在下载依赖…" :
                (m_dependencyStage == 2 ? L"正在解压并安装…" : L"正在验证安装…");
            if (m_dependencyProgress >= 0) progressLabel += L" " + Num(m_dependencyProgress) + L"%";
            T(g, progressLabel.c_str(), px+10.0f, py+3.0f, 8, C(255,222,211,232));
        }
    }

    // NRLive is controlled by the always-visible DLSS5 master switch above.

    // Capture source and motion-vector controls are shown only on the Settings page above.

    // Main feature card is moved upward to balance the shorter header card.
    // The tabs and their content share a -140px layout translation.
    Round(g,RectF(22,375,556,800),10,C(255,22,20,28));
    Outline(g,RectF(22,375,556,800),10,C(255,58,47,65));

    UpdateScrollRange();
    GraphicsState pageState = g.Save();
    g.TranslateTransform(0.0f, -140.0f);
    DrawTabs(g);
    // Reserve the right gutter for the fixed scrollbar; page controls must not paint beneath it.
    g.SetClip(RectF(25.0f, 515.0f, 533.0f, 660.0f));
    g.TranslateTransform(0.0f, -static_cast<REAL>(m_scrollOffset));
    if(m_tab==0) DrawPictureTab(g);
    else if(m_tab==1) DrawNrTab(g);
    else DrawOverlayTab(g);
    g.Restore(pageState);

    // Scrollbar and dropdowns follow the translated main-page layout.
    GraphicsState overlayState = g.Save();
    g.TranslateTransform(0.0f, -140.0f);
    DrawScrollBar(g);
    DrawOpenComboPopup(g);
    DrawCaptureTargetPopup(g);
    g.TranslateTransform(0.0f, -static_cast<REAL>(m_scrollOffset));
    if(m_tab==1) DrawNrChoicePopup(g);
    g.Restore(overlayState);

    // Footer
    Round(g,RectF(22,1185,556,38),8,C(255,31,23,43));
    T(g,L"设置保存至 scaleconfig.ini；启动时调用已安装的 NRLive.exe",34,1197,10,C(255,158,151,168));

    // Bottom-right hamburger menu. It opens upward so it stays inside the
    // window and does not cover the main controls.
    m_menuButtonRect = CRect(540,1187,578,1223);
    Round(g,RectF(540,1187,38,36),7,C(255,38,29,48));
    Outline(g,RectF(540,1187,38,36),7,C(255,112,66,139),1.0f);
    Pen menuPen(C(255,225,211,234),1.8f);
    g.DrawLine(&menuPen,550,1198,568,1198);
    g.DrawLine(&menuPen,550,1205,568,1205);
    g.DrawLine(&menuPen,550,1212,568,1212);

    if (m_menuOpen) {
        const float mx=410.0f, my=1008.0f, mw=168.0f, rowH=42.0f;
        Round(g,RectF(mx,my,mw,rowH*4),10,C(255,31,26,39));
        Outline(g,RectF(mx,my,mw,rowH*4),10,C(255,111,70,135),1.2f);
        const wchar_t* menuLabels[4]={L"检查更新",L"常见问题",L"关于",L"捐款"};
        for(int i=0;i<4;i++) {
            m_menuItemRects[i]=CRect((int)mx,(int)(my+i*rowH),(int)(mx+mw),(int)(my+(i+1)*rowH));
            if(i>0) {
                Pen sep(C(255,62,51,70),1.0f);
                g.DrawLine(&sep,mx+12,my+i*rowH,mx+mw-12,my+i*rowH);
            }
            T(g,menuLabels[i],mx+42,my+10+i*rowH,13,C(255,232,223,239),false);
        }
    } else {
        for(auto& r : m_menuItemRects) r=CRect(0,0,0,0);
    }
    g.Restore(st);
}



void CDlss5SwapperDlg::OnPaint()
{
    CPaintDC dc(this); CRect rc; GetClientRect(&rc);
    Graphics g(dc.GetSafeHdc());
    // Clear the entire client surface before drawing the centered UI canvas.
    // This removes the white letterbox margins at non-4:~9 window ratios.
    g.Clear(Color(255, 17, 15, 23));
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    DrawInterface(g,rc.Width(),rc.Height());
}
void CDlss5SwapperDlg::ApplyRoundedWindowRegion()
{
    if (!GetSafeHwnd() || IsIconic()) return;
    CRect windowRect;
    GetWindowRect(&windowRect);
    const int width = windowRect.Width();
    const int height = windowRect.Height();
    if (width <= 0 || height <= 0) return;

    // Clip the entire top-level window (including the resize frame) to one
    // continuous rounded silhouette. 56px gives a 28px corner radius, matching
    // the 22px design-canvas radius at the normal UI scale.
    HRGN rounded = CreateRoundRectRgn(0, 0, width + 1, height + 1, 56, 56);
    if (!rounded) return;
    if (!SetWindowRgn(rounded, TRUE)) DeleteObject(rounded);
}

void CDlss5SwapperDlg::OnSize(UINT nType,int cx,int cy)
{
    CDialog::OnSize(nType,cx,cy);
    if (!GetSafeHwnd()) return;
    if (nType != SIZE_MINIMIZED) ApplyRoundedWindowRegion();
    Invalidate(FALSE);
}

void CDlss5SwapperDlg::OnSizing(UINT fwSide, LPRECT pRect)
{
    CDialog::OnSizing(fwSide, pRect);
    if (!pRect || IsZoomed()) return;

    // Free resizing with a locked canvas ratio: the user can resize from all
    // eight edges/corners, but the purple outline remains flush with the
    // window silhouette instead of exposing letterboxed side strips.
    constexpr double aspect = 600.0 / 1240.0;
    const int minW = 500;
    const int minH = static_cast<int>(std::ceil(minW / aspect));
    int w = pRect->right - pRect->left;
    int h = pRect->bottom - pRect->top;

    switch (fwSide) {
    case WMSZ_LEFT:
    case WMSZ_RIGHT:
        h = static_cast<int>(std::lround(w / aspect));
        h = (std::max)(h, minH);
        if (fwSide == WMSZ_LEFT) pRect->top = pRect->bottom - h;
        else pRect->bottom = pRect->top + h;
        break;
    case WMSZ_TOP:
    case WMSZ_BOTTOM:
        w = static_cast<int>(std::lround(h * aspect));
        w = (std::max)(w, minW);
        if (fwSide == WMSZ_TOP) pRect->left = pRect->right - w;
        else pRect->right = pRect->left + w;
        break;
    default: // corners: use width as the driving dimension
        h = static_cast<int>(std::lround(w / aspect));
        h = (std::max)(h, minH);
        if (fwSide == WMSZ_TOPLEFT || fwSide == WMSZ_TOPRIGHT)
            pRect->top = pRect->bottom - h;
        else
            pRect->bottom = pRect->top + h;
        break;
    }
}
LRESULT CDlss5SwapperDlg::OnNcHitTest(CPoint screenPoint)
{
    // Borderless rounded window: emulate the eight native resize grips
    // without reintroducing WS_THICKFRAME (which draws the unwanted white box).
    if (IsZoomed()) return HTCLIENT;
    CPoint p = screenPoint;
    ScreenToClient(&p);
    CRect rc; GetClientRect(&rc);
    const int grip = 9;
    const bool left = p.x >= 0 && p.x < grip;
    const bool right = p.x < rc.right && p.x >= rc.right - grip;
    const bool top = p.y >= 0 && p.y < grip;
    const bool bottom = p.y < rc.bottom && p.y >= rc.bottom - grip;
    if (top && left) return HTTOPLEFT;
    if (top && right) return HTTOPRIGHT;
    if (bottom && left) return HTBOTTOMLEFT;
    if (bottom && right) return HTBOTTOMRIGHT;
    if (left) return HTLEFT;
    if (right) return HTRIGHT;
    if (top) return HTTOP;
    if (bottom) return HTBOTTOM;
    return HTCLIENT;
}
void CDlss5SwapperDlg::OnGetMinMaxInfo(MINMAXINFO* lpMMI)
{
    CDialog::OnGetMinMaxInfo(lpMMI);
    if(!lpMMI) return;
    // Keep enough room for the header, three tabs, page viewport and footer.
    lpMMI->ptMinTrackSize.x = 500;
    lpMMI->ptMinTrackSize.y = 1034; // 500px wide at the 600:1240 canvas ratio
}
void CDlss5SwapperDlg::UpdateSliderAt(CPoint p)
{
    int id=m_activeSlider;
    if(id<0||id>=12) return;
    CRect r=m_sliderRects[id];
    if(r.Width()<=0) return;
    float f=std::clamp((float)(p.x-r.left)/(float)r.Width(),0.0f,1.0f);
    switch(id) {
    case 0:m_sharpness=(int)(f*100);break; case 1:m_fpsLimit=15+(int)(f*485);break;
    case 2:m_nrStrength=(int)(f*200);break; case 3:m_nrPasses=1+(int)(f*3);break;
    case 4:m_nrLocalTone=(int)(f*200);break; case 5:m_nrLocalStructure=(int)(f*200);break;
    case 6:m_nrSkinStructure=(int)(f*100);break; case 7:m_nrHistory=(int)(f*100);break;
    case 8:m_hudScale=50+(int)(f*200);break; case 9:m_hudFontSize=12+(int)(f*36);break; case 10:m_hudBackgroundAlpha=(int)(f*100);break; default:break;
    }
    Invalidate(FALSE);
}
// Choice controls are handled exclusively by OpenCombo/HandleComboPopupClick.
// The former CycleCombo implementation was obsolete and has been removed so
// these selectors cannot regress to click-to-cycle behavior.
void CDlss5SwapperDlg::ToggleAt(CPoint p)
{
    // Only persist and repaint when the click actually hits a toggle control.
    bool changed = false;

    // Administrator mode: the complete label/switch row is clickable.
    if(m_settingsOpen && p.y>=330&&p.y<=370&&p.x>=30&&p.x<=565) {
        m_runAsAdmin=!m_runAsAdmin;
        SaveProfile();
        Invalidate(FALSE);
        return;
    }

    if(m_tab==0) {
        if(p.y>=680&&p.y<=710) {
            m_frameGeneration=!m_frameGeneration;
            changed=true;
        }
        else if(p.y>=735&&p.y<=765) {
            m_frameLimiter=!m_frameLimiter;
            changed=true;
        }
    }
    else if(m_tab==1) {
        if(p.y>=560&&p.y<=590) {
            m_nrEnabled=!m_nrEnabled;
            changed=true;
        }

    }
    else {
        if(p.y>=560&&p.y<=590) {
            m_overlayEnabled=!m_overlayEnabled;
            changed=true;
        }
        else if(m_tab==2 && p.y>=730 && p.y< ((int)(730+((m_hudTileMap.size()+1)/2)*42))) {
            int row=(p.y-730)/42, col=(p.x-35)/265, i=row*2+col;
            int count=(int)m_hudTileMap.size();
            if(col>=0&&col<2&&i>=0&&i<count) { int settingIndex=m_hudTileMap[i]; if(settingIndex<0)m_hudCounterEnabled=!m_hudCounterEnabled; else {m_hudItems[settingIndex]=!m_hudItems[settingIndex]; if(m_hudNew)m_hudPreset=0;} changed=true; }
        }
        else if(m_tab==2 && m_moreOverlay) {
            int base=730+(((int)m_hudTileMap.size())+1)/2*42+8+112;
            if(p.y>=base&&p.y<=base+34){m_hudBackground=!m_hudBackground;changed=true;}
            else if(p.y>=base+42&&p.y<=base+76){m_hudTextOutline=!m_hudTextOutline;changed=true;}
        }
    }

    if(changed) {
        SaveSettings();
        Invalidate(FALSE);
    }
}

void CDlss5SwapperDlg::HitTest(CPoint p)
{
    if(m_bilibiliRect.PtInRect(p)) { ShellExecuteW(m_hWnd,L"open",L"https://space.bilibili.com/173069391?spm_id_from=333.1007.0.0",nullptr,nullptr,SW_SHOWNORMAL); return; }
    if(m_qqGroupRect.PtInRect(p)) { ShellExecuteW(m_hWnd,L"open",L"https://qm.qq.com/q/KBkPCd5hgQ",nullptr,nullptr,SW_SHOWNORMAL); return; }
    if(m_closeRect.PtInRect(p)) { EndDialog(IDOK); return; }
    if(m_minimizeRect.PtInRect(p)) { ShowWindow(SW_MINIMIZE); return; }
    if(m_maximizeRect.PtInRect(p)) { ShowWindow(IsZoomed()?SW_RESTORE:SW_MAXIMIZE); Invalidate(FALSE); return; }
    // Main-page tabs are handled before content scroll normalization in OnLButtonDown.
    if(p.y<80) { SendMessage(WM_NCLBUTTONDOWN,HTCAPTION,0); return; }
    if(m_startToggleRect.PtInRect(p)) { StartNrLive(); return; }
    if(!m_settingsOpen && m_installDependencyRect.PtInRect(p)) { InstallDependencies(); return; }
    if(!m_settingsOpen && m_localCacheRect.PtInRect(p)) {
        const std::wstring cache = GetCacheDirectory();
        CreateDirectoryW(cache.c_str(), nullptr);
        ShellExecuteW(m_hWnd, L"open", cache.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    if(m_settingsButtonRect.PtInRect(p)) {
        // Close every Settings-only popup/editor when changing pages. The target
        // popup is also drawn by the main-page overlay pass, so leaving it open
        // would display an orphaned popup whose click handler is Settings-only.
        const bool leavingSettings = m_settingsOpen;
        m_settingsOpen = !m_settingsOpen;
        m_openCombo = -1;
        m_targetPopup = false;
        m_targetPage = 0;
        if(leavingSettings) {
            if(m_activeHotkey == 4) m_bypassKeys = m_bypassBeforeEdit;
            m_bypassBeforeEdit.clear();
            m_activeHotkey = 0;
        }
        Invalidate(FALSE);
        return;
    }


    if(p.y>=855&&p.y<=905&&m_tab==0) { m_morePicture=!m_morePicture; UpdateScrollRange(); Invalidate(FALSE); return; }
    if(m_tab==2) {
        const int afterTiles=730+(((int)m_hudTileMap.size()+1)/2)*42+8;
        if(p.y>=afterTiles+58&&p.y<=afterTiles+100&&p.x>=35&&p.x<=555) { m_moreOverlay=!m_moreOverlay; UpdateScrollRange(); Invalidate(FALSE); return; }
    }
    ToggleAt(p);
}
void CDlss5SwapperDlg::OnLButtonDown(UINT nFlags,CPoint point)
{
    CRect rc; GetClientRect(&rc); CPoint p=ToDesignPoint(point,rc.Width(),rc.Height());

    if (m_menuOpen) {
        bool hitMenuItem=false;
        for(int i=0;i<4;i++) {
            if(m_menuItemRects[i].PtInRect(p)) {
                m_menuOpen=false;
                Invalidate(FALSE);
                hitMenuItem=true;
                switch(i) {
                case 0: {
                    if (m_updateChecking) {
                        MessageBoxW(L"正在检查更新，请稍候。", L"检查更新", MB_OK | MB_ICONINFORMATION);
                        break;
                    }
                    StartUpdateCheck(false);
                    break;
                }
                case 1:
                    MessageBoxW(
                        L"专为视频、窗口、检测严的游戏打造 DLSS5 使用工具，支持超分辨率、帧生成、DLSS5 神经渲染、帧率监控等。\n\n"
                        L"1. 望月后端 Python 环境\n"
                        L"使用 9000、7000、6000 系显卡的望月后端需要安装 Python 3.13（Python 3.13 直接百度里面下载，全部下一步安装就好了）。\n\n"
                        L"2. 6000 系显卡 RDNA2\n"
                        L"RDNA2 显卡需要安装 AMD HIP 7.2 运行时才能正常工作，可以从 AMD 官网下载：\n"
                        L"https://www.amd.com/en/developer/resources/rocm-hub/hip-sdk.html",
                        L"常见问题",MB_OK|MB_ICONINFORMATION);
                    break;
                case 2:
                    MessageBoxW(
                        L"原作者\n"
                        L"NRLive 由 Caliel 制作（GPL-3.0）\nhttps://github.com/Caliel666/NRLive\n"
                        L"mochizuki0323/DLSSNR-AMD（MIT）\nhttps://gitlab.com/mochizuki0323/DLSSNR-AMD\n"
                        L"OpenNR-Installer 由MatheusFerreiraS （MIT）\nhttps://github.com/zmodelerlover/OpenNR-Installer?tab=readme-ov-file\n"
                        L"DLSS5-控制台\nhttps://github.com/a756598009-cmyk/DLSS5-console",
                        L"关于",MB_OK|MB_ICONINFORMATION);
                    break;
                case 3:
                {
                    const wchar_t* donationText =
                        L"本软件完全免费，而且已经开源，后续会一直更新下去。"
                        L"如果你遇到收费，那就是你被骗了，请立即退款。"
                        L"本软件完全由我一个人维护，如果你有能力的话，欢迎来我的 Bilibili 主页支持支持我，给我充充电。"
                        L"有 Bilibili 大会员的兄弟姐妹们，也麻烦大家点点发财的小手，每月的免费充电券可以给我充充电，谢谢各位了！"
                        L"\n\n"
                        L"This software is completely free and open source, and I will continue maintaining and updating it."
                        L"If someone asks you to pay for it, you are being scammed. Please request an immediate refund."
                        L"I maintain this project on my own. If you can, please support me on Bilibili by sending a charge."
                        L"If you are a Bilibili Premium member, you can also use your monthly free charge voucher to support me. Thank you all!";
                    const wchar_t* donationContent =
                        L"<a href=\"https://space.bilibili.com/173069391?spm_id_from=333.1007.0.0\">"
                        L"https://space.bilibili.com/173069391?spm_id_from=333.1007.0.0</a>"
                        L"\n\n"
                        L"<a href=\"https://space.bilibili.com/173069391?spm_id_from=333.1007.0.0\">"
                        L"点击打开 Bilibili 主页 / Click to open my Bilibili profile</a>";
                    TASKDIALOGCONFIG config = {};
                    config.cbSize = sizeof(config);
                    config.hwndParent = m_hWnd;
                    config.dwFlags = TDF_ENABLE_HYPERLINKS | TDF_SIZE_TO_CONTENT;
                    config.dwCommonButtons = TDCBF_OK_BUTTON;
                    config.pszWindowTitle = L"支持作者 / Support the Developer";
                    config.pszMainInstruction = L"感谢你的支持！ / Thank you for your support!";
                    std::wstring fullDonationText = donationText;
                    fullDonationText += L"\n\n";
                    fullDonationText += donationContent;
                    config.pszContent = fullDonationText.c_str();
                    config.pfCallback = DonationTaskDialogCallback;
                    TaskDialogIndirect(&config, nullptr, nullptr, nullptr);
                    break;
                }
                }
                return;
            }
        }
        if(!hitMenuItem && !m_menuButtonRect.PtInRect(p)) {
            m_menuOpen=false;
            Invalidate(FALSE);
        }
    }
    if(m_menuButtonRect.PtInRect(p)) {
        m_menuOpen=!m_menuOpen;
        Invalidate(FALSE);
        return;
    }
    if(m_bilibiliRect.PtInRect(p)) { ShellExecuteW(m_hWnd,L"open",L"https://space.bilibili.com/173069391?spm_id_from=333.1007.0.0",nullptr,nullptr,SW_SHOWNORMAL); return; }
    if(m_qqGroupRect.PtInRect(p)) { ShellExecuteW(m_hWnd,L"open",L"https://qm.qq.com/q/KBkPCd5hgQ",nullptr,nullptr,SW_SHOWNORMAL); return; }
    if(m_closeRect.PtInRect(p)) { EndDialog(IDOK); return; }
    if(m_minimizeRect.PtInRect(p)) { ShowWindow(SW_MINIMIZE); return; }
    if(m_maximizeRect.PtInRect(p)) { ShowWindow(IsZoomed()?SW_RESTORE:SW_MAXIMIZE); Invalidate(FALSE); return; }
    // The top DLSS5 switch must be tested before the draggable title region.
    if(m_startToggleRect.PtInRect(p)) { StartNrLive(); return; }
    if(!m_settingsOpen && m_installDependencyRect.PtInRect(p)) { InstallDependencies(); return; }
    if(!m_settingsOpen && m_localCacheRect.PtInRect(p)) {
        const std::wstring cache = GetCacheDirectory();
        CreateDirectoryW(cache.c_str(), nullptr);
        ShellExecuteW(m_hWnd, L"open", cache.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    if(m_settingsButtonRect.PtInRect(p)) {
        // Close every Settings-only popup/editor when changing pages. The target
        // popup is also drawn by the main-page overlay pass, so leaving it open
        // would display an orphaned popup whose click handler is Settings-only.
        const bool leavingSettings = m_settingsOpen;
        m_settingsOpen = !m_settingsOpen;
        m_openCombo = -1;
        m_targetPopup = false;
        m_targetPage = 0;
        if(leavingSettings) {
            if(m_activeHotkey == 4) m_bypassKeys = m_bypassBeforeEdit;
            m_bypassBeforeEdit.clear();
            m_activeHotkey = 0;
        }
        Invalidate(FALSE);
        return;
    }
    if(p.y<80) { SendMessage(WM_NCLBUTTONDOWN,HTCAPTION,0); return; }

    // An open popup is topmost: consume option clicks before hotkey/capture
    // hit regions, otherwise the click falls through to controls underneath.
    // Main-page combos are painted under the fixed -140px page translation;
    // their anchor/popup rectangles are stored in pre-translation screen
    // coordinates (with the scroll offset already removed by DrawCombo).
    // Normalize only main-page popup clicks before hit-testing. Settings
    // combos (IDs 0-2) remain in the fixed page coordinate space.
    if(m_openCombo>=0) {
        CPoint comboClick = p;
        if(!m_settingsOpen && m_openCombo>=3) comboClick.y += 140;
        if(HandleComboPopupClick(comboClick)) return;
    }

    // These hit rectangles belong exclusively to the Settings page. They
    // retain their last drawn positions while the main page is visible, so
    // never let them intercept feature-tab clicks.
    if(m_settingsOpen) {
        if(HandleCaptureTargetClick(p)) return;
        for(int i=0;i<3;i++) if(m_hotkeyRects[i].PtInRect(p)) {
            m_activeHotkey=i+1; m_hotkeyStatus=L"请按下新的快捷键组合；Esc 取消"; SetFocus(); Invalidate(FALSE); return;
        }
        if(m_bypassRect.PtInRect(p)) { m_bypassBeforeEdit=m_bypassKeys; m_activeHotkey=4; m_hotkeyStatus=L"编辑 Bind Bypass：使用逗号分隔按键，Enter 保存"; SetFocus(); Invalidate(FALSE); return; }
    }

    // Settings page owns its combo boxes; handle them before the full-page
    // early return so capture-object/source/motion selectors remain clickable.
    if(m_settingsOpen) {
        for(int i=0;i<(int)m_comboRects.size();++i) {
            if(m_comboRects[i].PtInRect(p)) { OpenCombo(i); return; }
        }
        // Countdown is a real combo (combo ID 8). Do not cycle its value
        // from a broad label-area click; only HandleComboPopupClick may commit
        // a new delay after the user selects an explicit option.
        ToggleAt(p);
        return;
    }

    // Main-page tabs and content are drawn under a -140px Y translation.
    // The visible tabs start at y=340 (design y=480), so normalize from 340,
    // not 375. Hit the tabs BEFORE applying page scroll offset: tabs are fixed
    // and must never move with the scrollable content.
    if(!m_settingsOpen && p.y>=340) p.y+=140;
    if(!m_settingsOpen) {
        for(int i=0;i<3;i++) {
            if(m_tabRects[i].PtInRect(p)) {
                m_tab=i;
                m_nrOpenChoice=-1;
                m_scrollOffset=0;
                UpdateScrollRange();
                m_openCombo=-1;
                Invalidate(FALSE);
                return;
            }
        }
    }

    // Test the real drawn combo rectangles (including the fixed top capture controls).
    for(int i=0;i<(int)m_comboRects.size();++i) {
        if(m_comboRects[i].PtInRect(p)) { OpenCombo(i); return; }
    }

    // Scrollbar thumb can be dragged; clicking the track pages the content.
    if(m_scrollMax > 0 && p.x >= 558 && p.x <= 582 && p.y >= 515 && p.y <= 1175) {
        if(m_scrollThumb.PtInRect(p)) {
            m_draggingScroll = true;
            m_scrollDragStartY = p.y;
            m_scrollStartOffset = m_scrollOffset;
            SetCapture();
        } else {
            const int delta = p.y < m_scrollThumb.top ? -520 : 520;
            m_scrollOffset = (std::clamp)(m_scrollOffset + delta, 0, m_scrollMax);
            Invalidate(FALSE);
        }
        return;
    }

    if(m_tab==1) {
        // NR hit/group/slider rectangles are stored in unscrolled content
        // coordinates, unlike the standard combo/slider rectangles.
        CPoint nrp=p;
        nrp.y+=m_scrollOffset;
        // An open popup is visually above every NR control; it must receive
        // the click before group headers, toggles, sliders, or underlying rows.
        if(m_nrOpenChoice>=0 && HandleNrChoicePopupClick(nrp)) return;
        if(nrp.y>=560&&nrp.y<=590&&nrp.x>=30&&nrp.x<=555){m_nrEnabled=!m_nrEnabled;SaveSettings();Invalidate(FALSE);return;}
        for(const auto& group:m_nrGroupRects) if(group.first.PtInRect(nrp)){m_nrGroupExpanded[group.second]=!m_nrGroupExpanded[group.second];UpdateScrollRange();Invalidate(FALSE);return;}
        for(size_t i=0;i<m_nrSettings.size()&&i<m_nrHitRects.size();++i){CRect r=m_nrHitRects[i];if(r.PtInRect(nrp)){auto& st=m_nrSettings[i];if(st.kind==L"toggle"){m_nrValues[i]=!m_nrValues[i];SaveSettings();Invalidate(FALSE);}else if(st.kind==L"choice"){m_nrOpenChoice=(m_nrOpenChoice==(int)i)?-1:(int)i;Invalidate(FALSE);}return;}}
        if(m_nrMoreRect.PtInRect(nrp)){m_moreNr=!m_moreNr;UpdateScrollRange();Invalidate(FALSE);return;}
        for(size_t i=0;i<m_nrSliderRects.size();++i)if(m_nrSliderRects[i].PtInRect(nrp)){m_activeNrSlider=(int)i;m_draggingSlider=true;SetCapture();UpdateNrSliderAt(nrp);return;}
    }
    for(int i=0;i<12;i++) if(m_sliderRects[i].PtInRect(p)) { m_activeSlider=i; m_draggingSlider=true; SetCapture(); UpdateSliderAt(p); return; }

    // Remaining ToggleAt/HitTest regions use original content coordinates.
    // All clicks reaching this point are in the scrollable page, so restore
    // the content coordinate unconditionally rather than testing y>=515.
    CPoint hit = p;
    hit.y += m_scrollOffset;
    HitTest(hit);
    // All interactive regions are handled by custom hit-testing above.
    // Do not invoke the default dialog handler for clicks on decorative/empty areas.
}
void CDlss5SwapperDlg::OnMouseMove(UINT nFlags,CPoint point)
{
    CRect rc; GetClientRect(&rc);
    CPoint p = ToDesignPoint(point, rc.Width(), rc.Height());
    // Mouse capture can continue outside the content viewport. Apply the same
    // fixed page translation for the entire drag so scrollbar deltas stay
    // continuous when the pointer crosses the viewport edge.
    if(!m_settingsOpen) p.y+=140;

    if(m_draggingScroll && m_scrollMax > 0) {
        constexpr int trackY = 522;
        constexpr int trackH = 642;
        constexpr int viewportH = 660;
        const float contentH = static_cast<float>(viewportH + m_scrollMax);
        const float thumbH = (std::max)(42.0f, trackH * viewportH / contentH);
        const float travel = trackH - thumbH;
        const int deltaY = p.y - m_scrollDragStartY;
        const int newTravel = (std::max)(1, static_cast<int>(travel));
        m_scrollOffset = (std::clamp)(m_scrollStartOffset + static_cast<int>(
            static_cast<float>(deltaY) * m_scrollMax / newTravel), 0, m_scrollMax);
        Invalidate(FALSE);
    }
    else if(m_draggingSlider) {
        if(m_activeNrSlider>=0) {
            p.y+=m_scrollOffset;
            UpdateNrSliderAt(p);
        } else {
            // Standard sliders store their hit rectangles in screen-content
            // coordinates (already reduced by m_scrollOffset).
            UpdateSliderAt(p);
        }
    }
    CDialog::OnMouseMove(nFlags,point);
}

BOOL CDlss5SwapperDlg::OnMouseWheel(UINT nFlags, short zDelta, CPoint screenPoint)
{
    ScreenToClient(&screenPoint);
    CRect rc; GetClientRect(&rc);
    CPoint p = ToDesignPoint(screenPoint, rc.Width(), rc.Height());
    // Only the scrollable content viewport is translated; fixed tabs/header
    // remain in their own coordinate space.
    if(!m_settingsOpen && p.y>=375) p.y+=140;

    // An open NR choice popup owns the wheel while the pointer is over the
    // popup or its anchor. NR popup/hit rectangles are stored in scrolled
    // content coordinates; convert the pointer into that same coordinate space.
    if (!m_settingsOpen && m_tab == 1 && m_nrOpenChoice >= 0 &&
        m_nrOpenChoice < (int)m_nrHitRects.size()) {
        CPoint nrPoint = p;
        nrPoint.y += m_scrollOffset;
        const CRect nrAnchor = m_nrHitRects[m_nrOpenChoice];
        if (m_nrChoicePopupRect.PtInRect(nrPoint) || nrAnchor.PtInRect(nrPoint))
            return TRUE;
    }

    // An open custom combo owns the wheel while the pointer is over its popup.
    // Its option list is short and fully visible, so consume the wheel without
    // scrolling the page or triggering a full-window repaint.
    if (m_openCombo >= 0) {
        const CRect anchor = m_comboRects[m_openCombo];
        const auto& opts = m_comboOptions[m_openCombo];
        const int itemH = 36;
        int popupY = anchor.bottom + 3;
        const int totalH = static_cast<int>(opts.size()) * itemH;
        const int maxBottom = (anchor.top < 515) ? 1178 : 1172;
        if (popupY + totalH > maxBottom && anchor.top - totalH - 3 >= 515)
            popupY = anchor.top - totalH - 3;
        const int popupWidth=(m_openCombo==0) ? 300 : anchor.Width();
        CRect popup(anchor.left, popupY, anchor.left+popupWidth, popupY + totalH);
        if (popup.PtInRect(p) || anchor.PtInRect(p)) return TRUE;
    }

    if (!m_settingsOpen && p.y >= 515 && p.y <= 1175 && m_scrollMax > 0) {
        const int oldOffset = m_scrollOffset;
        const int step = (zDelta > 0) ? -90 : 90;
        m_scrollOffset = (std::clamp)(m_scrollOffset + step, 0, m_scrollMax);
        if (m_scrollOffset != oldOffset) {
            // Invalidate only the scrollable page and scrollbar gutter.
            // Header, capture settings, tabs and footer remain untouched.
            InvalidateDesignRect(CRect(22, 515, 582, 1178));
        }
        return TRUE;
    }
    return CDialog::OnMouseWheel(nFlags, zDelta, screenPoint);
}
void CDlss5SwapperDlg::OnLButtonUp(UINT nFlags,CPoint point)
{
    if(m_draggingSlider) { m_draggingSlider=false; m_activeSlider=-1; m_activeNrSlider=-1; ReleaseCapture(); SaveSettings(); }
    if(m_draggingScroll) { m_draggingScroll=false; ReleaseCapture(); }
    CDialog::OnLButtonUp(nFlags,point);
}
void CDlss5SwapperDlg::OnKeyDown(UINT key,UINT rep,UINT flags)
{
    if(m_activeHotkey>=1 && m_activeHotkey<=3) {
        if(key==VK_ESCAPE) { m_activeHotkey=0; m_hotkeyStatus=L"已取消快捷键录制"; Invalidate(FALSE); return; }
        // Windows may report either generic or left/right modifier virtual keys.
        if(key==VK_CONTROL||key==VK_LCONTROL||key==VK_RCONTROL||
           key==VK_MENU||key==VK_LMENU||key==VK_RMENU||
           key==VK_SHIFT||key==VK_LSHIFT||key==VK_RSHIFT||
           key==VK_LWIN||key==VK_RWIN) { Invalidate(FALSE); return; }
        std::wstring base=KeyNameFromVk(key);
        if(base.empty()) { m_hotkeyStatus=L"不支持该按键，请使用字母、数字、F1-F24 或常用功能键"; Invalidate(FALSE); return; }
        std::wstring chord;
        if((GetAsyncKeyState(VK_CONTROL)&0x8000)||(GetAsyncKeyState(VK_LCONTROL)&0x8000)||(GetAsyncKeyState(VK_RCONTROL)&0x8000)) chord+=L"ctrl+";
        if((GetAsyncKeyState(VK_MENU)&0x8000)||(GetAsyncKeyState(VK_LMENU)&0x8000)||(GetAsyncKeyState(VK_RMENU)&0x8000)) chord+=L"alt+";
        if((GetAsyncKeyState(VK_SHIFT)&0x8000)||(GetAsyncKeyState(VK_LSHIFT)&0x8000)||(GetAsyncKeyState(VK_RSHIFT)&0x8000)) chord+=L"shift+";
        if((GetAsyncKeyState(VK_LWIN)&0x8000)||(GetAsyncKeyState(VK_RWIN)&0x8000)) chord+=L"win+";
        chord+=base;
        CommitHotkeyCapture(chord); return;
    }
    if(m_activeHotkey==4) {
        if(key==VK_ESCAPE) { m_bypassKeys=m_bypassBeforeEdit; m_activeHotkey=0; m_hotkeyStatus=L"已取消 Bind Bypass 编辑"; Invalidate(FALSE); return; }
        if(key==VK_RETURN) {
            std::wstring reason;
            if(ValidateHotkeySettings(reason)) { m_activeHotkey=0; m_bypassBeforeEdit.clear(); SaveProfile(); RegisterHotkeys(); if(m_hotkeyStatus.empty()) m_hotkeyStatus=L"Bind Bypass 已保存"; }
            else m_hotkeyStatus=reason;
            Invalidate(FALSE); return;
        }
        if((GetKeyState(VK_CONTROL)&0x8000) && key=='A') { m_bypassKeys.clear(); Invalidate(FALSE); return; }
        return; // let OnChar handle text input; do not trigger scroll shortcuts while editing
    }
    if(key==VK_ESCAPE) { m_captureHotkey=false; }
    // Keyboard page navigation belongs to the main content page only.
    // The Settings page is fixed-layout and must not mutate the hidden
    // main-page scroll position while it is active.
    else if(!m_settingsOpen && key==VK_NEXT) { m_scrollOffset=(std::min)(m_scrollMax,m_scrollOffset+520); Invalidate(FALSE); }
    else if(!m_settingsOpen && key==VK_PRIOR) { m_scrollOffset=(std::max)(0,m_scrollOffset-520); Invalidate(FALSE); }
    else if(!m_settingsOpen && key==VK_HOME) { m_scrollOffset=0; Invalidate(FALSE); }
    else if(!m_settingsOpen && key==VK_END) { m_scrollOffset=m_scrollMax; Invalidate(FALSE); }
    CDialog::OnKeyDown(key,rep,flags);
}

void CDlss5SwapperDlg::OnSysKeyDown(UINT key, UINT rep, UINT flags)
{
    if(m_activeHotkey>=1 && m_activeHotkey<=3) { OnKeyDown(key,rep,flags); return; }
    CDialog::OnSysKeyDown(key,rep,flags);
}

void CDlss5SwapperDlg::OnChar(UINT ch, UINT rep, UINT flags)
{
    if(m_activeHotkey==4) {
        if(ch==VK_BACK) { if(!m_bypassKeys.empty()) m_bypassKeys.pop_back(); Invalidate(FALSE); return; }
        if(ch==VK_RETURN) return;
        wchar_t c=(wchar_t)ch;
        if((c>=L'a'&&c<=L'z')||(c>=L'A'&&c<=L'Z')||(c>=L'0'&&c<=L'9')||c==L','||c==L'+'||c==L' '||c==L'_') {
            if(m_bypassKeys.size()<180) m_bypassKeys.push_back((wchar_t)towlower(c));
            Invalidate(FALSE); return;
        }
        return;
    }
    CDialog::OnChar(ch,rep,flags);
}

void CDlss5SwapperDlg::CommitHotkeyCapture(const std::wstring& value)
{
    UINT mods=0,vk=0; if(!ParseHotkey(value,mods,vk)) { m_hotkeyStatus=L"快捷键格式无效（录入值："+value+L"），请重新按下组合键"; Invalidate(FALSE); return; }
    std::wstring* target=m_activeHotkey==1?&m_scaleKey:m_activeHotkey==2?&m_stopKey:&m_overlayKey;
    std::wstring old=*target; *target=value;
    std::wstring reason;
    if(!ValidateHotkeySettings(reason)) { *target=old; m_hotkeyStatus=reason; Invalidate(FALSE); return; }
    m_activeHotkey=0; SaveProfile(); RegisterHotkeys();
    if(!m_hotkeyStatus.empty()) SetStatus(m_hotkeyStatus); else SetStatus(L"快捷键已更新并保存");
    Invalidate(FALSE);
}

bool CDlss5SwapperDlg::ValidateHotkeySettings(std::wstring& reason) const
{
    UINT am=0,av=0,bm=0,bv=0,cm=0,cv=0;
    if(!ParseHotkey(m_scaleKey,am,av)||!ParseHotkey(m_stopKey,bm,bv)||!ParseHotkey(m_overlayKey,cm,cv)) { reason=L"快捷键格式无效"; return false; }
    if((am==bm&&av==bv)||(am==cm&&av==cv)||(bm==cm&&bv==cv)) { reason=L"三个快捷键不能重复，请重新录制"; return false; }
    std::wstringstream ss(m_bypassKeys); std::wstring token; int count=0;
    while(std::getline(ss,token,L',')) {
        size_t a=token.find_first_not_of(L" \t"),b=token.find_last_not_of(L" \t");
        if(a==std::wstring::npos) continue;
        token=token.substr(a,b-a+1); UINT m=0,v=0;
        if(!ParseHotkey(token,m,v)) { reason=L"Bind Bypass 包含不支持的按键："+token; return false; }
        ++count;
    }
    if(count==0) { reason=L"Bind Bypass 至少需要一个有效按键"; return false; }
    reason.clear(); return true;
}
