#define UNICODE
#define _UNICODE
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <cstdint>

namespace fs = std::filesystem;
using Bytes = std::vector<unsigned char>;
struct Entry { const wchar_t* path; const wchar_t* hash; unsigned resource; };
#include "manifest.generated.h"

struct Failure { std::wstring message; };
const wchar_t* BackupDirectory = L".shenzhen-io-ko-win32-original";
constexpr wchar_t PriorPatchedExe[] = L"b935ece554bb4acb59fd46f6bb69997992ea63cb8d33289544c650a28e41e09e";
constexpr wchar_t PriorManualExe[] = L"a5c9b6b29da61bd355dbf64d3caf271232b996141093701d83f72dd6218abb4d";
constexpr wchar_t PriorRuntime[] = L"9ffbf78fdbbdccd01fd2a7154b30b5b9479d219001ba669b0aaaf12174e73f98";
constexpr int Pick = 100, Check = 101, Install = 102, Restore = 103;
constexpr int PickCopy = 104, Launch = 105;
HWND folderBox, copyBox, shortcutBox, statusBox;
std::wstring initialFolder;

void Fail(const std::wstring& message, const fs::path& path = {})
{
    throw Failure{path.empty() ? message : message + L"\r\n" + path.wstring()};
}

Bytes Read(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) Fail(L"파일을 열 수 없습니다.", path);
    const auto size = input.tellg();
    if (size < 0 || size > 512 * 1024 * 1024) Fail(L"파일 크기가 지원 범위를 벗어났습니다.", path);
    Bytes data(static_cast<size_t>(size));
    input.seekg(0);
    if (!data.empty() && !input.read(reinterpret_cast<char*>(data.data()), data.size()))
        Fail(L"파일을 읽을 수 없습니다.", path);
    return data;
}

std::wstring Hash(const Bytes& data)
{
    BCRYPT_ALG_HANDLE provider{};
    if (BCryptOpenAlgorithmProvider(&provider, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        Fail(L"SHA-256을 초기화할 수 없습니다.");
    unsigned char digest[32]{};
    unsigned char empty = 0;
    const auto status = BCryptHash(provider, nullptr, 0,
        data.empty() ? &empty : const_cast<unsigned char*>(data.data()),
        static_cast<ULONG>(data.size()), digest, sizeof(digest));
    BCryptCloseAlgorithmProvider(provider, 0);
    if (status < 0) Fail(L"SHA-256 계산에 실패했습니다.");
    std::wstring result;
    for (unsigned char value : digest) {
        result += L"0123456789abcdef"[value >> 4];
        result += L"0123456789abcdef"[value & 15];
    }
    return result;
}

void Verify(const fs::path& path, const wchar_t* expected)
{
    if (Hash(Read(path)) != expected) Fail(L"SHA-256 불일치.", path);
}

Bytes Payload(unsigned id, const wchar_t* expected, const wchar_t* name)
{
    Bytes bytes;
    if (id != 0) {
        const HMODULE module = GetModuleHandleW(nullptr);
        const HRSRC located = FindResourceW(module, MAKEINTRESOURCEW(id), RT_RCDATA);
        const HGLOBAL loaded = located ? LoadResource(module, located) : nullptr;
        const auto* data = loaded ? static_cast<const unsigned char*>(LockResource(loaded)) : nullptr;
        const DWORD size = located ? SizeofResource(module, located) : 0;
        if (!data || !size) Fail(L"내장 패치 리소스를 읽을 수 없습니다.", name);
        bytes.assign(data, data + size);
    }
    if (Hash(bytes) != expected) Fail(L"내장 패치 리소스의 SHA-256이 일치하지 않습니다.", name);
    return bytes;
}

void Publish(const fs::path& path, const Bytes& data, const wchar_t* expected, bool replace)
{
    fs::create_directories(path.parent_path());
    fs::path temporary = path;
    temporary += L".ko-tmp-" + std::to_wstring(GetCurrentProcessId());
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) Fail(L"임시 파일을 만들 수 없습니다.", temporary);
    try {
        DWORD count = 0;
        const BOOL wrote = data.empty() || WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &count, nullptr);
        const BOOL flushed = FlushFileBuffers(file);
        CloseHandle(file);
        if (!wrote || !flushed || count != data.size()) Fail(L"파일을 쓸 수 없습니다.", temporary);
        Verify(temporary, expected);
        DWORD flags = MOVEFILE_WRITE_THROUGH | (replace ? MOVEFILE_REPLACE_EXISTING : 0);
        if (!MoveFileExW(temporary.c_str(), path.c_str(), flags))
            Fail(L"파일을 교체할 수 없습니다. 게임을 닫고 쓰기 권한을 확인하세요.", path);
    } catch (...) {
        DeleteFileW(temporary.c_str());
        throw;
    }
}

Bytes ExpandDelta(const Bytes& original, const Bytes& delta)
{
    size_t cursor = 4;
    const auto number = [&]() {
        if (delta.size() - cursor < 4) Fail(L"실행 파일 패치 데이터가 잘렸습니다.");
        uint32_t value = 0;
        for (unsigned shift = 0; shift < 32; shift += 8) value |= uint32_t(delta[cursor++]) << shift;
        return value;
    };
    if (delta.size() < 8 || std::string(delta.begin(), delta.begin() + 4) != "SKP1")
        Fail(L"실행 파일 패치 형식을 인식할 수 없습니다.");
    const uint32_t size = number();
    if (size > 16 * 1024 * 1024) Fail(L"실행 파일 패치 크기가 올바르지 않습니다.");
    Bytes output;
    output.reserve(size);
    while (cursor < delta.size()) {
        const auto offset = number();
        const auto length = number();
        if (length == 0 || length > size - output.size()) Fail(L"실행 파일 패치 구간 오류.");
        if (offset == UINT32_MAX) {
            if (length > delta.size() - cursor) Fail(L"실행 파일 패치 구간이 잘렸습니다.");
            output.insert(output.end(), delta.begin() + cursor, delta.begin() + cursor + length);
            cursor += length;
        } else {
            if (offset > original.size() || length > original.size() - offset)
                Fail(L"실행 파일 원본 구간 오류.");
            output.insert(output.end(), original.begin() + offset, original.begin() + offset + length);
        }
    }
    if (output.size() != size || Hash(output) != PatchedExe)
        Fail(L"패치 후 실행 파일 SHA-256이 일치하지 않습니다.");
    return output;
}

bool GameRunning(const fs::path& exe)
{
    const HANDLE list = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (list == INVALID_HANDLE_VALUE) Fail(L"게임 실행 상태를 확인할 수 없습니다.");
    PROCESSENTRY32W item{};
    item.dwSize = sizeof(item);
    bool running = false;
    for (BOOL more = Process32FirstW(list, &item); more; more = Process32NextW(list, &item)) {
        if (_wcsicmp(item.szExeFile, L"Shenzhen.exe") != 0) continue;
        const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, item.th32ProcessID);
        if (!process) { running = true; break; }
        std::wstring path(32768, L'\0');
        DWORD size = static_cast<DWORD>(path.size());
        const BOOL found = QueryFullProcessImageNameW(process, 0, path.data(), &size);
        CloseHandle(process);
        path.resize(size);
        if (!found || _wcsicmp(path.c_str(), exe.c_str()) == 0) { running = true; break; }
    }
    CloseHandle(list);
    return running;
}

bool LegacyResource(const Entry& entry, const std::wstring& hash)
{
    return std::wstring(entry.path) == L"KoreanLocaleRuntime.dll" && hash == PriorRuntime;
}

std::wstring Run(int action, fs::path game)
{
    if (game.empty()) Fail(L"게임 폴더를 먼저 선택하세요.");
    game = fs::canonical(game);
    const auto exe = game / L"Shenzhen.exe";
    const auto backup = game / BackupDirectory / L"Shenzhen.exe";
    if (action != Check && GameRunning(exe)) Fail(L"선택한 폴더의 게임을 닫은 뒤 다시 실행하세요.");
    const auto exeBytes = Read(exe);
    const auto exeHash = Hash(exeBytes);
    const bool patched = exeHash == PatchedExe;
    const bool prior = !patched && (exeHash == PriorPatchedExe || exeHash == PriorManualExe);
    if (!patched && !prior && exeHash != OriginalExe) Fail(L"지원하는 원본 빌드와 SHA-256이 다릅니다.", exe);

    for (const auto& entry : OriginalFiles) Verify(game / entry.path, entry.hash);
    int installed = 0;
    for (const auto& entry : PatchFiles) {
        if (action != Restore) Payload(entry.resource, entry.hash, entry.path);
        if (fs::exists(game / entry.path)) {
            const auto digest = Hash(Read(game / entry.path));
            if (digest == entry.hash) ++installed;
            else if (!LegacyResource(entry, digest)) Fail(L"SHA-256 불일치.", game / entry.path);
        }
    }
    Bytes delta;
    if (action != Restore) delta = Payload(DeltaResource, DeltaHash, L"Shenzhen.delta");
    if (fs::exists(backup)) Verify(backup, OriginalExe);
    else if (patched || prior) Fail(L"해시가 일치하는 원본 실행 파일 백업이 없습니다.", backup);

    if (action == Install) {
        if (!fs::exists(backup)) Publish(backup, exeBytes, OriginalExe, false);
        const auto payload = ExpandDelta(Read(backup), delta);
        for (const auto& entry : PatchFiles) {
            const auto target = game / entry.path;
            const bool exists = fs::exists(target);
            if (!exists || LegacyResource(entry, Hash(Read(target))))
                Publish(target, Payload(entry.resource, entry.hash, entry.path), entry.hash, exists);
        }
        if (!patched) {
            Verify(exe, exeHash.c_str());
            Publish(exe, payload, PatchedExe, true);
        }
        Verify(exe, PatchedExe);
        for (const auto& entry : PatchFiles) Verify(game / entry.path, entry.hash);
        return L"패치 적용 완료. 실행 파일과 한국어 리소스의 SHA-256이 일치합니다.\r\n"
               L"게임 제어판의 언어에서 한국어를 선택하세요.";
    }
    if (action == Restore) {
        if (patched || prior) {
            Verify(exe, exeHash.c_str());
            Publish(exe, Read(backup), OriginalExe, true);
        }
        for (const auto& entry : PatchFiles)
            if (fs::exists(game / entry.path)) {
                const auto digest = Hash(Read(game / entry.path));
                if (digest != entry.hash && !LegacyResource(entry, digest)) Fail(L"SHA-256 불일치.", game / entry.path);
                if (!DeleteFileW((game / entry.path).c_str())) Fail(L"패치 파일을 지울 수 없습니다.", game / entry.path);
            }
        Verify(exe, OriginalExe);
        for (const auto& entry : PatchFiles)
            if (fs::exists(game / entry.path)) Fail(L"패치 파일이 남아 있습니다.", game / entry.path);
        return L"원본 복원 완료. 원본 SHA-256이 일치합니다.\r\n"
               L"한국어를 선택해 두었다면 복원 후 제어판에서 평소 언어를 선택하세요.";
    }

    return std::wstring(patched ? L"실행 파일: 한글 패치" : prior ? L"실행 파일: 이전 한글 패치" : L"실행 파일: 지원되는 원본")
        + L"\r\nSHA-256: " + exeHash + L"\r\n게임 폴더에 있는 한국어 파일: "
        + std::to_wstring(installed) + L" / " + std::to_wstring(std::size(PatchFiles))
        + L"\r\n원본 리소스와 동봉 패치 데이터의 해시가 일치합니다.";
}

Bytes EncodeUtf8(const std::wstring& text)
{
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) Fail(L"폴더 경로를 인코딩할 수 없습니다.");
    Bytes bytes(size);
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), reinterpret_cast<char*>(bytes.data()), size, nullptr, nullptr))
        Fail(L"폴더 경로를 인코딩할 수 없습니다.");
    return bytes;
}

void WriteNewText(const fs::path& path, const std::wstring& text)
{
    const auto data = EncodeUtf8(text);
    Publish(path, data, Hash(data).c_str(), false);
}

bool Inside(const fs::path& path, const fs::path& root)
{
    auto item = path.begin();
    for (const auto& part : root) {
        if (item == path.end() || _wcsicmp(item->c_str(), part.c_str()) != 0) return false;
        ++item;
    }
    return true;
}

const wchar_t* CopyMarker = L".shenzhen-io-ko-copy";

fs::path RequireCopy(fs::path path)
{
    if (path.empty()) Fail(L"한글판 복사본 폴더를 지정하세요.");
    path = fs::canonical(path);
    if (!fs::is_regular_file(path / CopyMarker) || Read(path / CopyMarker) != EncodeUtf8(L"SKP-COPY1"))
        Fail(L"이 프로그램에서 만든 한글판 복사본만 사용할 수 있습니다.", path);
    return path;
}

void CopyGameTree(const fs::path& source, const fs::path& staging)
{
    for (const auto* file : {L"Shenzhen.exe", L"LICENSE.txt", L"Ionic.Zip.Reduced.dll", L"libogg-0.dll",
         L"libpng16-16.dll", L"libsteam_api.so", L"libvorbis-0.dll", L"libvorbisfile-3.dll",
         L"MoonSharp.Interpreter.dll", L"Renderer_D3D11.dll", L"SDL2.dll", L"SDL2_image.dll",
         L"SDL2_mixer.dll", L"Steamworks.NET.dll", L"steam_api.dll", L"zlib1.dll"})
        fs::copy_file(source / file, staging / file);

    for (const auto* dir : {L"Content", L"PackedContent"}) {
        const auto root = source / dir;
        fs::create_directory(staging / dir);
        if (GetFileAttributesW(root.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT)
            Fail(L"리소스 폴더의 연결을 복사할 수 없습니다.", root);
        for (const auto& item : fs::recursive_directory_iterator(root)) {
            const auto path = item.path();
            const DWORD flags = GetFileAttributesW(path.c_str());
            if (flags == INVALID_FILE_ATTRIBUTES || (flags & FILE_ATTRIBUTE_REPARSE_POINT))
                Fail(L"리소스의 연결 또는 파일 특성을 확인할 수 없습니다.", path);
            const auto target = staging / path.lexically_relative(source);
            if (item.is_directory()) fs::create_directory(target);
            else if (item.is_regular_file()) fs::copy_file(path, target);
        }
    }
}

std::wstring PrepareCopy(const fs::path& sourceInput, const fs::path& destinationInput)
{
    if (sourceInput.empty() || destinationInput.empty()) Fail(L"Steam 설치 폴더와 복사본 폴더를 지정하세요.");
    const auto source = fs::canonical(sourceInput);
    const auto destination = fs::weakly_canonical(fs::absolute(destinationInput));
    if (Inside(destination, source) || Inside(source, destination))
        Fail(L"복사본 폴더는 Steam 원본 폴더와 분리해야 합니다.", destination);
    if (fs::exists(destination)) return Run(Install, RequireCopy(destination));
    Verify(source / L"Shenzhen.exe", OriginalExe);
    Run(Check, source);

    const auto parent = destination.parent_path();
    if (!fs::is_directory(parent)) Fail(L"복사본 상위 폴더를 먼저 만드세요.", parent);
    wchar_t unique[MAX_PATH];
    if (!GetTempFileNameW(parent.c_str(), L"SKP", 0, unique))
        Fail(L"복사 위치에 임시 폴더를 준비할 수 없습니다.", parent);
    if (!DeleteFileW(unique) || !CreateDirectoryW(unique, nullptr))
        Fail(L"임시 폴더를 만들 수 없습니다.", unique);
    const fs::path staging(unique);
    try {
        CopyGameTree(source, staging);
        Run(Install, staging);
        WriteNewText(staging / L"steam_appid.txt", L"504210");
        const auto saves = destination / L"KoreanSaves";
        fs::create_directory(staging / L"KoreanSaves");
        WriteNewText(staging / L"AlternateSavePath", saves.wstring());
        WriteNewText(staging / CopyMarker, L"SKP-COPY1");
        if (!MoveFileExW(staging.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH))
            Fail(L"완료한 복사본을 지정한 경로로 옮길 수 없습니다.", destination);
    } catch (...) {
        std::error_code ignored;
        if (staging.parent_path() == parent && Inside(staging, parent) &&
            GetFileAttributesW(staging.c_str()) == FILE_ATTRIBUTE_DIRECTORY)
            fs::remove_all(staging, ignored);
        throw;
    }
    return L"한글판 복사본 생성 완료. 패치 후 SHA-256이 일치합니다.\r\n"
           L"게임 실행을 누른 뒤 게임 제어판에서 한국어를 선택하세요.\r\n"
           L"복사본 저장 데이터: " + (destination / L"KoreanSaves").wstring();
}

std::wstring LaunchCopy(const fs::path& path)
{
    const auto game = RequireCopy(path);
    const auto exe = game / L"Shenzhen.exe";
    Verify(exe, PatchedExe);
    for (const auto& entry : PatchFiles) Verify(game / entry.path, entry.hash);
    if (GameRunning(exe)) Fail(L"한글판 복사본이 이미 실행 중입니다.", exe);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(exe.c_str(), nullptr, nullptr, nullptr, FALSE, 0, nullptr, game.c_str(), &startup, &process))
        Fail(L"한글판을 실행할 수 없습니다. Steam 로그인과 게임 실행 상태를 확인하세요.", exe);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return L"한글판 복사본을 실행했습니다.\r\n처음 실행한다면 제어판에서 한국어를 선택하세요.";
}

bool ShortcutMatches(const fs::path& file, const fs::path& target)
{
    Microsoft::WRL::ComPtr<IShellLinkW> link;
    Microsoft::WRL::ComPtr<IPersistFile> persist;
    wchar_t path[32768]{};
    return SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(link.GetAddressOf())))
        && SUCCEEDED(link.As(&persist))
        && SUCCEEDED(persist->Load(file.c_str(), STGM_READ))
        && SUCCEEDED(link->GetPath(path, static_cast<int>(std::size(path)), nullptr, SLGP_RAWPATH))
        && _wcsicmp(path, target.c_str()) == 0;
}

fs::path CreateDesktopShortcut(const fs::path& path)
{
    const auto game = RequireCopy(path);
    const auto target = game / L"Shenzhen.exe";
    Verify(target, PatchedExe);
    PWSTR desktopPath = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Desktop, KF_FLAG_DEFAULT, nullptr, &desktopPath)))
        Fail(L"바탕 화면 폴더를 찾을 수 없습니다.");
    fs::path desktop(desktopPath);
    CoTaskMemFree(desktopPath);

    fs::path shortcut;
    for (int number = 1; number <= 100; ++number) {
        std::wstring name = L"SHENZHEN I-O 한국어";
        if (number > 1) name += L" (" + std::to_wstring(number) + L")";
        const auto candidate = desktop / (name + L".lnk");
        if (!fs::exists(candidate)) { shortcut = candidate; break; }
        if (ShortcutMatches(candidate, target)) return candidate;
    }
    if (shortcut.empty()) Fail(L"바로가기 파일 이름을 정할 수 없습니다.", desktop);

    Microsoft::WRL::ComPtr<IShellLinkW> link;
    Microsoft::WRL::ComPtr<IPersistFile> persist;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(link.GetAddressOf())))
        || FAILED(link->SetPath(target.c_str()))
        || FAILED(link->SetWorkingDirectory(game.c_str()))
        || FAILED(link->SetDescription(L"SHENZHEN I/O 독립 한국어판"))
        || FAILED(link.As(&persist)))
        Fail(L"한글판 바로가기를 준비할 수 없습니다.", shortcut);

    GUID id{};
    wchar_t unique[40]{};
    if (FAILED(CoCreateGuid(&id)) || !StringFromGUID2(id, unique, static_cast<int>(std::size(unique))))
        Fail(L"바로가기 임시 파일 이름을 생성할 수 없습니다.", desktop);
    const auto temporary = desktop / (std::wstring(L".SHENZHEN-KO-") + unique + L".lnk");
    try {
        if (FAILED(persist->Save(temporary.c_str(), FALSE)) || !ShortcutMatches(temporary, target))
            Fail(L"바로가기 파일을 저장하거나 검증할 수 없습니다.", shortcut);
        if (!MoveFileExW(temporary.c_str(), shortcut.c_str(), MOVEFILE_WRITE_THROUGH))
            Fail(L"바로가기 이름이 이미 사용 중이거나 바탕 화면에 쓸 수 없습니다.", shortcut);
    } catch (...) {
        DeleteFileW(temporary.c_str());
        throw;
    }
    return shortcut;
}

void Browse(HWND owner, HWND box, bool destination)
{
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
        Fail(L"폴더 선택기를 열 수 없습니다.");
    dialog->SetOptions(FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dialog->SetTitle(destination ? L"한글판 복사본을 만들 상위 폴더 선택" : L"Steam 설치 폴더 선택");
    if (SUCCEEDED(dialog->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                const auto selected = destination ? fs::path(path) / L"SHENZHEN IO Korean" : fs::path(path);
                SetWindowTextW(box, selected.c_str());
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dialog->Release();
}

std::wstring Text(HWND box)
{
    const int length = GetWindowTextLengthW(box);
    std::wstring value(length + 1, L'\0');
    GetWindowTextW(box, value.data(), length + 1);
    value.resize(length);
    return value;
}

HWND Control(HWND parent, const wchar_t* type, const wchar_t* label, DWORD style,
             int x, int y, int w, int h, int id = 0)
{
    const DWORD extended = _wcsicmp(type, L"EDIT") == 0 ? WS_EX_CLIENTEDGE : 0;
    HWND child = CreateWindowExW(extended, type, label, WS_CHILD | WS_VISIBLE | style, x, y, w, h,
                                  parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
    SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    return child;
}

LRESULT CALLBACK Window(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_CREATE) {
        Control(window, L"STATIC", L"Steam 원본 설치 폴더", 0, 20, 20, 540, 22);
        folderBox = Control(window, L"EDIT", initialFolder.c_str(), WS_TABSTOP | ES_AUTOHSCROLL, 20, 45, 490, 30, 110);
        Control(window, L"BUTTON", L"찾아보기", WS_TABSTOP, 522, 45, 100, 30, Pick);
        Control(window, L"STATIC", L"한글판 복사본 폴더", 0, 20, 88, 540, 22);
        copyBox = Control(window, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, 20, 113, 490, 30, 111);
        Control(window, L"BUTTON", L"위치 선택", WS_TABSTOP, 522, 113, 100, 30, PickCopy);
        Control(window, L"BUTTON", L"원본 SHA-256 검사", WS_TABSTOP, 20, 161, 142, 34, Check);
        Control(window, L"BUTTON", L"복사본 패치", WS_TABSTOP, 173, 161, 142, 34, Install);
        Control(window, L"BUTTON", L"게임 실행", WS_TABSTOP, 326, 161, 142, 34, Launch);
        Control(window, L"BUTTON", L"복사본 복원", WS_TABSTOP, 479, 161, 143, 34, Restore);
        shortcutBox = Control(window, L"BUTTON", L"복사본 패치 후 바탕 화면에 바로가기 만들기",
                             WS_TABSTOP | BS_AUTOCHECKBOX, 20, 206, 602, 26, 106);
        statusBox = Control(window, L"EDIT", L"Steam 설치 폴더와 한글판을 만들 위치를 선택하세요.",
                            ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, 20, 248, 602, 144, 112);
        return 0;
    }
    if (message == WM_COMMAND && HIWORD(wParam) == BN_CLICKED) {
        try {
            const int action = LOWORD(wParam);
            if (action == Pick) { Browse(window, folderBox, false); return 0; }
            if (action == PickCopy) { Browse(window, copyBox, true); return 0; }
            std::wstring result;
            if (action == Check) result = Run(Check, Text(folderBox));
            else if (action == Install) {
                const auto destination = Text(copyBox);
                result = PrepareCopy(Text(folderBox), destination);
                if (SendMessageW(shortcutBox, BM_GETCHECK, 0, 0) == BST_CHECKED) {
                    try {
                        const auto shortcut = CreateDesktopShortcut(destination);
                        result += L"\r\n바탕 화면 바로가기: " + shortcut.filename().wstring();
                    } catch (const Failure& error) {
                        result += L"\r\n바로가기 생성 실패: " + error.message;
                    } catch (const fs::filesystem_error& error) {
                        result += L"\r\n바로가기 생성 실패: " + error.path1().wstring();
                    }
                }
            }
            else if (action == Launch) result = LaunchCopy(Text(copyBox));
            else if (action == Restore) {
                const auto copy = RequireCopy(Text(copyBox));
                if (MessageBoxW(window, L"한글판 복사본의 실행 파일과 리소스를 복원할까요?", L"복사본 복원",
                    MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return 0;
                result = Run(Restore, copy);
            } else return 0;
            SetWindowTextW(statusBox, result.c_str());
        } catch (const Failure& error) {
            SetWindowTextW(statusBox, error.message.c_str());
        } catch (const fs::filesystem_error& error) {
            SetWindowTextW(statusBox, (L"파일 작업 실패. 폴더와 쓰기 권한을 확인하세요.\r\n" +
                         error.path1().wstring()).c_str());
        }
        return 0;
    }
    if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(window, message, wParam, lParam);
}

void CommandOutput(DWORD stream, const std::wstring& value)
{
    HANDLE handle = GetStdHandle(stream);
    if (!handle || handle == INVALID_HANDLE_VALUE) return;
    const std::wstring line = value + L"\r\n";
    DWORD count = 0;
    if (GetConsoleMode(handle, &count)) {
        WriteConsoleW(handle, line.data(), static_cast<DWORD>(line.size()), &count, nullptr);
        return;
    }
    const int length = WideCharToMultiByte(CP_UTF8, 0, line.data(), static_cast<int>(line.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(length, '\0');
    WideCharToMultiByte(CP_UTF8, 0, line.data(), static_cast<int>(line.size()), utf8.data(), length, nullptr, nullptr);
    WriteFile(handle, utf8.data(), static_cast<DWORD>(utf8.size()), &count, nullptr);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show)
{
    int count = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (arguments && count == 4 && std::wstring(arguments[1]) == L"/copy") {
        const fs::path source = arguments[2], destination = arguments[3];
        LocalFree(arguments);
        try { CommandOutput(STD_OUTPUT_HANDLE, PrepareCopy(source, destination)); return 0; }
        catch (const Failure& error) { CommandOutput(STD_ERROR_HANDLE, error.message); return 1; }
        catch (const fs::filesystem_error& error) { CommandOutput(STD_ERROR_HANDLE, error.path1().wstring()); return 1; }
    }
    if (arguments && count == 3) {
        const std::wstring operation = arguments[1];
        const std::wstring game = arguments[2];
        LocalFree(arguments);
        const int action = operation == L"/check" ? Check : operation == L"/apply" ? Install :
            operation == L"/restore" ? Restore : operation == L"/launch" ? Launch : 0;
        if (!action) return 2;
        try {
            CommandOutput(STD_OUTPUT_HANDLE, action == Launch ? LaunchCopy(game) : Run(action, game));
            return 0;
        }
        catch (const Failure& error) { CommandOutput(STD_ERROR_HANDLE, error.message); return 1; }
        catch (const fs::filesystem_error& error) { CommandOutput(STD_ERROR_HANDLE, error.path1().wstring()); return 1; }
    }
    if (arguments && count == 2) initialFolder = arguments[1];
    if (arguments) LocalFree(arguments);
    SetProcessDPIAware();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    WNDCLASSW window{};
    window.hInstance = instance;
    window.lpszClassName = L"ShenzhenKoreanPatcher";
    window.lpfnWndProc = Window;
    window.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassW(&window);
    const std::wstring title = std::wstring(L"SHENZHEN I/O 한글 패치 ") + Version;
    HWND handle = CreateWindowExW(0, window.lpszClassName, title.c_str(),
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT,
        659, 457, nullptr, nullptr, instance, nullptr);
    if (!handle) { CoUninitialize(); return 1; }
    ShowWindow(handle, show);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
        if (!IsDialogMessageW(handle, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
    CoUninitialize();
    return static_cast<int>(message.wParam);
}
