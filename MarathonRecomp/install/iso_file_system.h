// Referenced from: https://github.com/xenia-canary/xenia-canary/blob/canary_experimental/src/xenia/vfs/devices/disc_image_device.cc

/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2023 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <tuple>

#include "virtual_file_system.h"

#include <memory_mapped_file.h>

struct ISOFileSystem : VirtualFileSystem
{
    MemoryMappedFile mappedFile;
    int posixFd = -1;
    FILE *streamFile = nullptr;
    uint64_t totalFileSize = 0;
    std::map<std::string, std::tuple<uint64_t, size_t>> fileMap;
    std::string name;
    std::string openErrorDetail;

    ISOFileSystem(const std::filesystem::path &isoPath);
    ~ISOFileSystem() override;
    bool load(const std::string &path, uint8_t *fileData, size_t fileDataMaxByteCount) const override;
    size_t getSize(const std::string &path) const override;
    bool exists(const std::string &path) const override;
    const std::string &getName() const override;
    bool empty() const;
    const std::string &getOpenErrorDetail() const { return openErrorDetail; }

    static std::unique_ptr<ISOFileSystem> create(const std::filesystem::path &isoPath, std::string *outErrorDetail = nullptr);

private:
    bool readBytesAt(uint64_t offset, void *dst, size_t byteCount) const;
    void closeHandles();
};
