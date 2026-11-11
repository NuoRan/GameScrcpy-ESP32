#include "gs/support/Files.h"

#include <windows.h>

#include <algorithm>
#include <new>
#include <vector>

namespace gs::platform {

namespace {

std::error_code lastError() noexcept
{
    return {static_cast<int>(::GetLastError()), std::system_category()};
}

class FileHandle
{
public:
    explicit FileHandle(HANDLE handle) noexcept
        : m_handle(handle)
    {
    }

    ~FileHandle()
    {
        if (valid()) {
            ::CloseHandle(m_handle);
        }
    }

    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;

    [[nodiscard]] bool valid() const noexcept { return m_handle != INVALID_HANDLE_VALUE; }
    [[nodiscard]] HANDLE get() const noexcept { return m_handle; }

private:
    HANDLE m_handle;
};

std::error_code readOnce(const std::filesystem::path& file, std::size_t mostBytes, std::string& contents)
{
    contents.clear();
    const FileHandle handle(::CreateFileW(file.c_str(), GENERIC_READ,
                                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!handle.valid()) {
        return lastError();
    }
    LARGE_INTEGER size{};
    if (::GetFileSizeEx(handle.get(), &size) == FALSE) {
        return lastError();
    }
    if (size.QuadPart < 0 || static_cast<unsigned long long>(size.QuadPart) > mostBytes) {
        return std::make_error_code(std::errc::file_too_large);
    }
    contents.resize(static_cast<std::size_t>(size.QuadPart));
    std::size_t done = 0;
    while (done < contents.size()) {
        constexpr std::size_t kChunk = 1U << 20U;
        const auto wanted = static_cast<DWORD>(std::min(contents.size() - done, kChunk));
        DWORD got = 0;
        if (::ReadFile(handle.get(), contents.data() + done, wanted, &got, nullptr) == FALSE) {
            return lastError();
        }
        if (got == 0) {
            break;
        }
        done += got;
    }
    contents.resize(done);
    return {};
}

}

std::error_code readFile(const std::filesystem::path& file, std::size_t mostBytes, std::string& contents) noexcept
{
    try {
        return readOnce(file, mostBytes, contents);
    } catch (const std::bad_alloc&) {
        contents.clear();
        return std::make_error_code(std::errc::not_enough_memory);
    }
}

bool fileMissing(const std::error_code& error) noexcept
{
    if (error.category() != std::system_category()) {
        return false;
    }
    const auto code = static_cast<DWORD>(error.value());
    return code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND;
}

std::filesystem::path moduleFolder() noexcept
{
    try {
        HMODULE module = nullptr;
        if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                 reinterpret_cast<LPCWSTR>(&moduleFolder), &module) == FALSE) {
            return {};
        }
        std::vector<wchar_t> buffer(MAX_PATH);
        for (;;) {
            const DWORD written = ::GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (written == 0) {
                return {};
            }
            if (written < buffer.size()) {
                return std::filesystem::path(std::wstring(buffer.data(), written)).parent_path();
            }
            buffer.resize(buffer.size() * 2);
        }
    } catch (...) {
        return {};
    }
}

}
