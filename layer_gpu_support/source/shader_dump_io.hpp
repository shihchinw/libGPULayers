/* SPDX-License-Identifier: MIT */
#pragma once

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace ShaderDumpIO
{
inline bool matchesFile(const std::filesystem::path& path, const char* data, size_t size)
{
    std::error_code error;
    if (std::filesystem::file_size(path, error) != size || error) return false;
    std::ifstream stream(path, std::ios::binary);
    std::array<char, 4096> block {};
    for (size_t offset = 0; offset < size;)
    {
        const auto count = std::min(block.size(), size - offset);
        if (!stream.read(block.data(), static_cast<std::streamsize>(count)) ||
            std::memcmp(block.data(), data + offset, count)) return false;
        offset += count;
    }
    return true;
}

// Failed temporary files are intentionally retained. Artifact cleanup belongs
// to host scripts, never to the injected Vulkan layer.
inline bool writeAtomic(const std::filesystem::path& path, const char* data, size_t size)
{
    static std::atomic<uint64_t> sequence {0};
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    auto temporary = path;
    temporary += ".tmp." + std::to_string(stamp) + "." + std::to_string(sequence++);
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream.write(data, static_cast<std::streamsize>(size))) return false;
    stream.flush();
    stream.close();
    if (!stream) return false;
#if defined(_WIN32)
    return MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
#else
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    return !error;
#endif
}

inline bool writeAtomic(const std::filesystem::path& path, const std::string& data)
{
    return writeAtomic(path, data.data(), data.size());
}

inline bool appendTable(const std::filesystem::path& path, const std::string& header, const std::string& rows)
{
    std::error_code error;
    const bool exists = std::filesystem::exists(path, error);
    if (error) return false;
    if (!exists) return writeAtomic(path, header + rows);
    std::ifstream check(path, std::ios::binary);
    std::string first;
    if (!std::getline(check, first) || first + '\n' != header) return false;
    check.seekg(-1, std::ios::end);
    char last = 0;
    if (!check.get(last) || last != '\n') return false;
    check.close();
    std::ofstream stream(path, std::ios::binary | std::ios::app);
    stream.write(rows.data(), static_cast<std::streamsize>(rows.size()));
    stream.flush();
    stream.close();
    return static_cast<bool>(stream);
}
}
