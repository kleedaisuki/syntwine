#include "syntwine/detail/platform/writer.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace syntwine {
namespace {

#ifdef _WIN32

using Handle = HANDLE;

struct OutputFile {
    Handle handle = INVALID_HANDLE_VALUE;

    void open(const std::filesystem::path& path) {
        handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            throw std::system_error(GetLastError(), std::system_category(), "CreateFile");
    }

    void write(std::string_view bytes) const {
        while (!bytes.empty()) {
            const auto count = static_cast<DWORD>(std::min<std::size_t>(bytes.size(), 1 << 30));
            DWORD written = 0;
            if (!WriteFile(handle, bytes.data(), count, &written, nullptr))
                throw std::system_error(GetLastError(), std::system_category(), "WriteFile");
            if (written == 0)
                throw std::system_error(std::make_error_code(std::errc::io_error), "WriteFile");
            bytes.remove_prefix(written);
        }
    }

    void close() {
        const auto current = handle;
        handle = INVALID_HANDLE_VALUE;
        if (!CloseHandle(current))
            throw std::system_error(GetLastError(), std::system_category(), "CloseHandle");
    }

    void discard() noexcept {
        if (handle != INVALID_HANDLE_VALUE)
            CloseHandle(handle);
        handle = INVALID_HANDLE_VALUE;
    }

    ~OutputFile() { discard(); }
};

bool already_exists(const std::error_code& error) {
    return error.value() == ERROR_FILE_EXISTS || error.value() == ERROR_ALREADY_EXISTS;
}

void replace(const std::filesystem::path& from, const std::filesystem::path& to) {
    if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING))
        throw std::system_error(GetLastError(), std::system_category(), "MoveFileEx");
}

unsigned long process_id() { return GetCurrentProcessId(); }

#else

struct OutputFile {
    int handle = -1;

    void open(const std::filesystem::path& path) {
        handle = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (handle == -1)
            throw std::system_error(errno, std::generic_category(), "open");
    }

    void write(std::string_view bytes) const {
        while (!bytes.empty()) {
            const auto count = std::min<std::size_t>(bytes.size(), 1 << 30);
            const auto written = ::write(handle, bytes.data(), count);
            if (written < 0 && errno == EINTR)
                continue;
            if (written < 0)
                throw std::system_error(errno, std::generic_category(), "write");
            if (written == 0)
                throw std::system_error(std::make_error_code(std::errc::io_error), "write");
            bytes.remove_prefix(static_cast<std::size_t>(written));
        }
    }

    void close() {
        const auto current = handle;
        handle = -1;
        if (::close(current) == -1)
            throw std::system_error(errno, std::generic_category(), "close");
    }

    void discard() noexcept {
        if (handle != -1)
            ::close(handle);
        handle = -1;
    }

    ~OutputFile() { discard(); }
};

bool already_exists(const std::error_code& error) {
    return error == std::errc::file_exists;
}

void replace(const std::filesystem::path& from, const std::filesystem::path& to) {
    std::filesystem::rename(from, to);
}

unsigned long process_id() { return static_cast<unsigned long>(getpid()); }

#endif

std::filesystem::path open_temporary(OutputFile& file, const std::filesystem::path& parent) {
    static std::atomic<unsigned long long> sequence{};
    const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto prefix = ".syntwine-" + std::to_string(process_id()) + "-" +
                        std::to_string(timestamp) + "-";
    for (int attempt = 0; ; ++attempt) {
        auto path = parent / (prefix + std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
        try {
            file.open(path);
            return path;
        } catch (const std::system_error& error) {
            if (attempt == 15 || !already_exists(error.code()))
                throw;
        }
    }
}

} // namespace

void atomic_write(const std::filesystem::path& path, std::string_view payload) {
    OutputFile file;
    const auto temporary = open_temporary(file, path.parent_path());
    try {
        file.write(payload);
#ifndef _WIN32
        struct stat previous{};
        if (lstat(path.c_str(), &previous) == 0 && S_ISREG(previous.st_mode) &&
            fchmod(file.handle, previous.st_mode & 0777) == -1)
            throw std::system_error(errno, std::generic_category(), "fchmod");
#endif
        file.close();
        replace(temporary, path);
    } catch (...) {
        file.discard();
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}

} // namespace syntwine
