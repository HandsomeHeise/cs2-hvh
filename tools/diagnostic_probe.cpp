#include "game_data.hpp"
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace {
bool read(void* process, std::uintptr_t address, void* output, std::size_t count) {
    SIZE_T copied = 0;
    return ReadProcessMemory(process, reinterpret_cast<const void*>(address), output, count, &copied) && copied == count;
}
game::Module metadata(const game::Memory& memory, std::uintptr_t base) {
    IMAGE_DOS_HEADER dos{}; IMAGE_NT_HEADERS64 nt{};
    if (!memory.get(base, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0 || dos.e_lfanew > 0x100000 ||
        !memory.get(base + dos.e_lfanew, nt) || nt.Signature != IMAGE_NT_SIGNATURE) return {};
    return {base, nt.OptionalHeader.SizeOfImage, nt.FileHeader.TimeDateStamp};
}
std::string quoted(const std::string& value) {
    std::ostringstream out; out << '"';
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') out << '\\' << c;
        else if (c < 32) out << "\\u" << std::hex << std::setfill('0') << std::setw(4) << static_cast<unsigned>(c);
        else out << c;
    }
    out << '"'; return out.str();
}
}
int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "Usage: diagnostic_probe.exe PID [output.json]\n"); return 2; }
    char* end = nullptr; const DWORD pid = std::strtoul(argv[1], &end, 10);
    if (!pid || !end || *end) return 2;
    HANDLE process = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) { std::fprintf(stderr, "OpenProcess: %lu\n", GetLastError()); return 3; }
    const game::Memory memory{read, process};
    game::Sources sources{memory};
    HANDLE modules = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    MODULEENTRY32W entry{}; entry.dwSize = sizeof(entry);
    if (modules != INVALID_HANDLE_VALUE && Module32FirstW(modules, &entry)) do {
        if (_wcsicmp(entry.szModule, L"client.dll") == 0) sources.client = metadata(memory, reinterpret_cast<std::uintptr_t>(entry.modBaseAddr));
        if (_wcsicmp(entry.szModule, L"engine2.dll") == 0) sources.engine = metadata(memory, reinterpret_cast<std::uintptr_t>(entry.modBaseAddr));
    } while (Module32NextW(modules, &entry));
    if (modules != INVALID_HANDLE_VALUE) CloseHandle(modules);
    const auto sample = game::collect(sources);
    CloseHandle(process);
    std::ostringstream out;
    out << "{\n  \"method\": \"Read-only probe using the DLL's actual game::collect implementation\",\n"
        << "  \"build\": " << sample.build << ",\n  \"state\": " << quoted(game::state_name(sample.state))
        << ",\n  \"profile_valid\": " << (sample.profile_valid ? "true" : "false")
        << ",\n  \"background\": " << (sample.background ? "true" : "false")
        << ",\n  \"local_valid\": " << (sample.local_valid ? "true" : "false")
        << ",\n  \"alive\": " << (sample.alive ? "true" : "false")
        << ",\n  \"map\": " << quoted(sample.map) << ",\n  \"health\": " << sample.health << ",\n  \"team\": " << sample.team
        << ",\n  \"weapon\": " << quoted(sample.weapon) << ",\n  \"ammo\": " << sample.ammo
        << ",\n  \"item_id\": " << sample.item_id << ",\n  \"detail\": " << quoted(sample.detail) << "\n}\n";
    std::printf("%s", out.str().c_str());
    if (argc > 2) std::ofstream(std::filesystem::path(argv[2])) << out.str();
    return sample.profile_valid ? 0 : 1;
}
