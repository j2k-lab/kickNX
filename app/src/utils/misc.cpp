#include "utils/misc.hpp"
#include <borealis/core/logger.hpp>
#include <borealis/core/i18n.hpp>
#include <fmt/chrono.h>
#include <cctype>
#include <random>
#include <sstream>
#include <iomanip>

using namespace brls::literals;  // for _i18n

#if defined(__APPLE__) || defined(__linux__)
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#endif

#if defined(_WIN32) && !defined(_WINRT_)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>

LONG WINAPI createMiniDump(_EXCEPTION_POINTERS* pep) {
    MEMORY_BASIC_INFORMATION memInfo;
    PEXCEPTION_RECORD xcpt = pep->ExceptionRecord;
    HANDLE hProcess = ::GetCurrentProcess();
    HANDLE hThread = ::GetCurrentThread();
    brls::Logger::error("Exception code {:#x}", xcpt->ExceptionCode);

    if (::VirtualQuery(xcpt->ExceptionAddress, &memInfo, sizeof(memInfo))) {
        char modulePath[MAX_PATH] = "Unknown Module";
        HMODULE hModule = (HMODULE)memInfo.AllocationBase;
        if (!memInfo.AllocationBase) hModule = GetModuleHandleA(nullptr);
        ::GetModuleFileNameA(hModule, modulePath, sizeof(modulePath));
        brls::Logger::error("Fault address {} {}", fmt::ptr(xcpt->ExceptionAddress), modulePath);
    }

    // Get handles to kernel32 and dbghelp
    HMODULE dbghelp = ::LoadLibraryA("dbghelp.dll");
    if (!dbghelp) return EXCEPTION_CONTINUE_SEARCH;

    auto fnMiniDumpWriteDump = (WINBOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
        CONST PMINIDUMP_EXCEPTION_INFORMATION, CONST PMINIDUMP_USER_STREAM_INFORMATION,
        CONST PMINIDUMP_CALLBACK_INFORMATION))::GetProcAddress(dbghelp, "MiniDumpWriteDump");
    auto fnSymInitialize = (WINBOOL(WINAPI*)(HANDLE, PCSTR, WINBOOL))::GetProcAddress(dbghelp, "SymInitialize");
    auto fnSymCleanup = (WINBOOL(WINAPI*)(HANDLE))::GetProcAddress(dbghelp, "SymCleanup");
    auto fnStackWalk64 = (WINBOOL(WINAPI*)(DWORD, HANDLE, HANDLE, LPSTACKFRAME64, PVOID, PREAD_PROCESS_MEMORY_ROUTINE64,
        PFUNCTION_TABLE_ACCESS_ROUTINE64, PGET_MODULE_BASE_ROUTINE64,
        PTRANSLATE_ADDRESS_ROUTINE64))::GetProcAddress(dbghelp, "StackWalk64");
    auto fnSymGetSymFromAddr64 = (WINBOOL(WINAPI*)(HANDLE, DWORD64, PDWORD64, PIMAGEHLP_SYMBOL64))::GetProcAddress(
        dbghelp, "SymGetSymFromAddr64");
    auto fnSymGetModuleInfo64 =
        (WINBOOL(WINAPI*)(HANDLE, DWORD64, PIMAGEHLP_MODULE64))::GetProcAddress(dbghelp, "SymGetModuleInfo64");
    auto fnSymFTA64 = (PFUNCTION_TABLE_ACCESS_ROUTINE64)::GetProcAddress(dbghelp, "SymFunctionTableAccess64");
    auto fnSymGMB64 = (PGET_MODULE_BASE_ROUTINE64)::GetProcAddress(dbghelp, "SymGetModuleBase64");

    STACKFRAME64 s;
    DWORD imageType;
    CONTEXT ctx = *pep->ContextRecord;

    ZeroMemory(&s, sizeof(s));
    s.AddrPC.Mode = AddrModeFlat;
    s.AddrFrame.Mode = AddrModeFlat;
    s.AddrStack.Mode = AddrModeFlat;
#ifdef _M_IX86
    imageType = IMAGE_FILE_MACHINE_I386;
    s.AddrPC.Offset = ctx.Eip;
    s.AddrFrame.Offset = ctx.Ebp;
    s.AddrStack.Offset = ctx.Esp;
#elif _M_X64
    imageType = IMAGE_FILE_MACHINE_AMD64;
    s.AddrPC.Offset = ctx.Rip;
    s.AddrFrame.Offset = ctx.Rsp;
    s.AddrStack.Offset = ctx.Rsp;
#elif _M_ARM64
    imageType = IMAGE_FILE_MACHINE_ARM64;
    s.AddrPC.Offset = ctx.Pc;
    s.AddrFrame.Offset = ctx.Fp;
    s.AddrStack.Offset = ctx.Sp;
#else
#error "Platform not supported!"
#endif

    std::vector<char> buf(sizeof(IMAGEHLP_SYMBOL64) + MAX_SYM_NAME);
    PIMAGEHLP_SYMBOL64 symbol = reinterpret_cast<PIMAGEHLP_SYMBOL64>(buf.data());
    symbol->SizeOfStruct = sizeof(IMAGEHLP_SYMBOL64);
    symbol->MaxNameLength = MAX_SYM_NAME;
    int n = 0;

    if (!fnSymInitialize(hProcess, nullptr, TRUE)) {
        brls::Logger::warning("SymInitialize failed {}", ::GetLastError());
    }
    while (fnStackWalk64(imageType, hProcess, hThread, &s, &ctx, nullptr, fnSymFTA64, fnSymGMB64, nullptr)) {
        IMAGEHLP_MODULE64 mi = {sizeof(IMAGEHLP_MODULE64)};
        DWORD64 funcOffset = 0;

        if (!fnSymGetModuleInfo64(hProcess, s.AddrPC.Offset, &mi)) {
            strcpy(mi.ImageName, "???");
        }
        if (fnSymGetSymFromAddr64(hProcess, s.AddrPC.Offset, &funcOffset, symbol)) {
            brls::Logger::error("[{}]: {}!{} + {:#x}", n, mi.ImageName, symbol->Name, funcOffset);
        } else {
            brls::Logger::error("[{}]: {} + {:#x}", n, mi.ImageName, s.AddrPC.Offset);
        }
        n++;
    }
    fnSymCleanup(hProcess);

    SYSTEMTIME lt;
    CHAR tempPath[MAX_PATH];
    DWORD cbTemp = ::GetTempPathA(sizeof(tempPath), tempPath);
    ::GetLocalTime(&lt);

    snprintf(tempPath + cbTemp, sizeof(tempPath) - cbTemp, "switchfin-%04d%02d%02d-%02d%02d%02d.dmp", lt.wYear,
        lt.wMonth, lt.wDay, lt.wHour, lt.wMinute, lt.wSecond);
    HANDLE hDump = ::CreateFileA(tempPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    MINIDUMP_EXCEPTION_INFORMATION info;
    info.ThreadId = ::GetCurrentThreadId();
    info.ExceptionPointers = pep;
    info.ClientPointers = TRUE;
    fnMiniDumpWriteDump(hProcess, ::GetCurrentProcessId(), hDump, MiniDumpNormal, &info, nullptr, nullptr);
    ::CloseHandle(hDump);

    return EXCEPTION_CONTINUE_SEARCH;
}

void misc::initCrashDump() { SetUnhandledExceptionFilter(createMiniDump); }

#else

void misc::initCrashDump() {}

#endif

inline std::string pre0(size_t num, size_t length) {
    std::string str = std::to_string(num);
    if (length <= str.length()) {
        return str;
    }
    return std::string(length - str.length(), '0') + str;
}

std::string misc::sec2Time(int64_t t) {
    size_t hour = t / 3600;
    size_t minute = t / 60 % 60;
    size_t sec = t % 60;
    if (hour == 0) {
        return pre0(minute, 2) + ":" + pre0(sec, 2);
    }
    return pre0(hour, 2) + ":" + pre0(minute, 2) + ":" + pre0(sec, 2);
}

std::string misc::formatSize(uint64_t s) {
    if (s == 0) return "-";
    if (s < (1 << 20)) return fmt::format("{}KB", s / 1024);
    if (s < (1 << 30)) return fmt::format("{:.2f}MB", (s >> 10) / 1024.0f);
    return fmt::format("{:.2f}GB", (s >> 20) / 1024.0f);
}

std::string misc::randHex(const int len) {
    std::stringstream ss;
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 255);

    for (int i = 0; i < len; i++) {
        ss << std::hex << std::setw(2) << std::setfill('0') << dis(gen);
    }
    return ss.str();
}

std::string misc::hexEncode(const unsigned char* data, size_t len) {
    std::stringstream ss;
    for (size_t i = 0; i < len; i++) {
        ss << std::hex << std::setw(2) << std::setfill('0') << (int)data[i];
    }
    return ss.str();
}

void misc::split(const std::string& data, std::vector<std::string>& result, char seq) {
    std::string s;
    std::stringstream ss(data);
    while (std::getline(ss, s, seq)) result.push_back(s);
}

bool misc::sendIPC(const std::string& sock, const std::string& payload) {
#ifdef _WIN32
    HANDLE hPipe = CreateFile(sock.c_str(), GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (hPipe == INVALID_HANDLE_VALUE) return false;

    if (!WriteFile(hPipe, payload.c_str(), payload.size(), NULL, NULL)) {
        brls::Logger::warning("sendIPC `{}` failed: {}", payload, GetLastError());
    }

    CloseHandle(hPipe);
    return true;
#elif defined(__APPLE__) || defined(__linux__)
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd == -1) return false;

    struct sockaddr_un addr;
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock.c_str(), sizeof(addr.sun_path) - 1);

    if ((connect(fd, (struct sockaddr*)&addr, sizeof(addr))) == -1) {
        close(fd);
        return false;
    }

    if (write(fd, payload.c_str(), payload.size()) == -1) {
        brls::Logger::warning("sendIPC `{}` failed: {}", payload, errno);
    }

    close(fd);
    return true;
#else
    return false;
#endif
}

namespace {

// Scripts the Switch system fonts have no glyphs for. Each is drawn as a box
// with an X, so it is replaced by a short note instead. An empty name gives
// the generic note.
struct UnsupportedScript {
    uint32_t first;
    uint32_t last;
    const char* name;
};

constexpr UnsupportedScript UNSUPPORTED_SCRIPTS[] = {
    {0x0530, 0x058F, ""},        // Armenian
    {0x0590, 0x05FF, "Hebrew"},
    {0x0600, 0x06FF, "Arabic"},
    {0x0700, 0x074F, ""},        // Syriac
    {0x0750, 0x077F, "Arabic"},
    {0x0780, 0x07BF, ""},        // Thaana
    {0x08A0, 0x08FF, "Arabic"},
    {0x0900, 0x097F, "Hindi"},   // Devanagari
    {0x0980, 0x0DFF, ""},        // Bengali to Sinhala
    {0x0E00, 0x0E7F, "Thai"},
    {0x0E80, 0x0EFF, ""},        // Lao
    {0x1000, 0x109F, ""},        // Myanmar
    {0x10A0, 0x10FF, ""},        // Georgian
    {0x1200, 0x139F, ""},        // Ethiopic
    {0x1780, 0x17FF, ""},        // Khmer
    {0xFB1D, 0xFB4F, "Hebrew"},
    {0xFB50, 0xFDFF, "Arabic"},
    {0xFE70, 0xFEFF, "Arabic"},
};

const UnsupportedScript* unsupportedScript(uint32_t codepoint) {
    for (const auto& script : UNSUPPORTED_SCRIPTS) {
        if (codepoint >= script.first && codepoint <= script.last) return &script;
    }
    return nullptr;
}

// Spaces, ASCII punctuation and digits, general punctuation and the
// direction marks: they stay inside a run of another script.
bool isNeutral(uint32_t codepoint) {
    if (codepoint < 0x80) return !std::isalpha(static_cast<int>(codepoint));
    return codepoint == 0x00A0 || (codepoint >= 0x2000 && codepoint <= 0x206F);
}

// Decodes the UTF-8 sequence at `pos`, moving `pos` past it. Malformed bytes
// are returned one at a time as themselves.
uint32_t nextCodepoint(const std::string& text, size_t& pos) {
    const auto byte = [&](size_t i) { return static_cast<unsigned char>(text[i]); };
    const unsigned char lead = byte(pos);
    const size_t length = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3 : (lead >> 3) == 0x1E ? 4 : 1;
    if (length == 1 || pos + length > text.size()) {
        ++pos;
        return lead;
    }
    uint32_t codepoint = lead & (0x7F >> length);
    for (size_t i = 1; i < length; ++i) codepoint = (codepoint << 6) | (byte(pos + i) & 0x3F);
    pos += length;
    return codepoint;
}

}  // namespace

std::string misc::replaceUnsupportedText(const std::string& text) {
    std::string result;
    bool replaced = false;
    bool readable = false;     // anything left that the fonts can draw
    const char* wholeName = nullptr;
    bool oneScript = true;

    const UnsupportedScript* run = nullptr;  // script of the open run
    size_t pending = 0;                      // neutral bytes held after the run

    const auto closeRun = [&]() {
        if (!run) return;
        const std::string name = run->name;
        result += name.empty() ? "(text not shown)" : "(" + name + " text not shown)";
        replaced = true;
        if (!wholeName) wholeName = run->name;
        else if (std::string(wholeName) != run->name) oneScript = false;
        run = nullptr;
    };

    size_t pos = 0;
    while (pos < text.size()) {
        const size_t start = pos;
        const uint32_t codepoint = nextCodepoint(text, pos);
        const auto* script = unsupportedScript(codepoint);

        if (script) {
            // Neutral text between two runs of the same script joins them.
            if (run && std::string(run->name) == script->name) {
                pending = 0;
            } else {
                if (run) {
                    closeRun();
                    result.append(text, start - pending, pending);
                }
                pending = 0;
                run = script;
            }
            continue;
        }
        if (run && isNeutral(codepoint)) {
            pending += pos - start;
            continue;
        }
        if (run) {
            closeRun();
            result.append(text, start - pending, pending);
            pending = 0;
        }
        if (!isNeutral(codepoint)) readable = true;
        result.append(text, start, pos - start);
    }
    if (run) {
        closeRun();
        result.append(text, text.size() - pending, pending);
    }

    if (!replaced) return text;
    if (readable) return result;
    // Nothing readable is left: one line instead of a note per paragraph.
    if (oneScript && wholeName && *wholeName) return std::string(wholeName) + " text not available";
    return "Text not available in this language";
}
