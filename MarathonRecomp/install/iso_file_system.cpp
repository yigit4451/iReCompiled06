// Referenced from: https://github.com/xenia-canary/xenia-canary/blob/canary_experimental/src/xenia/vfs/devices/disc_image_device.cc

/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2023 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif

#include "iso_file_system.h"

#include <cerrno>
#include <cstring>
#include <stack>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

ISOFileSystem::~ISOFileSystem()
{
    closeHandles();
}

void ISOFileSystem::closeHandles()
{
    if (mappedFile.isOpen())
    {
        mappedFile.close();
    }
#if !defined(_WIN32)
    if (posixFd != -1)
    {
        ::close(posixFd);
        posixFd = -1;
    }
#endif
    if (streamFile != nullptr)
    {
        std::fclose(streamFile);
        streamFile = nullptr;
    }
    totalFileSize = 0;
}

bool ISOFileSystem::readBytesAt(uint64_t offset, void *dst, size_t byteCount) const
{
    if (byteCount == 0)
        return true;

    if (dst == nullptr || offset > totalFileSize || (totalFileSize - offset) < byteCount)
        return false;

    if (mappedFile.isOpen())
    {
        const uint8_t *mappedFileData = mappedFile.data();
        std::memcpy(dst, &mappedFileData[static_cast<size_t>(offset)], byteCount);
        return true;
    }

#if !defined(_WIN32)
    if (posixFd != -1)
    {
        uint8_t *outPtr = static_cast<uint8_t *>(dst);
        size_t remaining = byteCount;
        off_t curOffset = static_cast<off_t>(offset);
        while (remaining > 0)
        {
            const ssize_t n = ::pread(posixFd, outPtr, remaining, curOffset);
            if (n < 0)
            {
                if (errno == EINTR)
                    continue;
                return false;
            }
            if (n == 0)
                return false;

            outPtr += static_cast<size_t>(n);
            curOffset += n;
            remaining -= static_cast<size_t>(n);
        }
        return true;
    }
#endif

    if (streamFile != nullptr)
    {
#if defined(_WIN32)
        if (_fseeki64(streamFile, static_cast<__int64>(offset), SEEK_SET) != 0)
            return false;
#else
        if (fseeko(streamFile, static_cast<off_t>(offset), SEEK_SET) != 0)
            return false;
#endif
        return std::fread(dst, 1, byteCount, streamFile) == byteCount;
    }

    return false;
}

ISOFileSystem::ISOFileSystem(const std::filesystem::path &isoPath)
{
    errno = 0;
    std::error_code fsEc;
    const uint64_t fsSize = std::filesystem::file_size(isoPath, fsEc);

#if !defined(_WIN32)
    // On iOS, total user virtual address space is ~3.5 GB, so mmap() on a 7.8 GB Xbox 360 ISO
    // fails with ENOMEM ("Cannot allocate memory"). Use 64-bit positional file I/O (open + pread)
    // directly instead of mmap on iOS or whenever mmap fails.
#if !defined(MARATHON_RECOMP_IOS)
    mappedFile.open(isoPath);
#endif
    if (mappedFile.isOpen())
    {
        totalFileSize = mappedFile.size();
    }
    else
    {
        errno = 0;
        posixFd = ::open(isoPath.c_str(), O_RDONLY);
        if (posixFd == -1)
        {
            const int openErr = errno;
            openErrorDetail = std::string("open() failed: ") + std::strerror(openErr) +
                " (errno=" + std::to_string(openErr) +
                (fsEc ? std::string(", fs_error=") + fsEc.message() : std::string("")) + ")";
            return;
        }

#ifdef F_NOCACHE
        (void)::fcntl(posixFd, F_NOCACHE, 1);
#endif

        struct stat st{};
        if (::fstat(posixFd, &st) == 0 && st.st_size > 0)
        {
            totalFileSize = static_cast<uint64_t>(st.st_size);
        }
        else if (!fsEc && fsSize > 0)
        {
            totalFileSize = fsSize;
        }
        else
        {
            const off_t endPos = ::lseek(posixFd, 0, SEEK_END);
            if (endPos == static_cast<off_t>(-1))
            {
                const int seekErr = errno;
                openErrorDetail = std::string("lseek(SEEK_END) failed: ") + std::strerror(seekErr) +
                    " (errno=" + std::to_string(seekErr) + ")";
                closeHandles();
                return;
            }
            totalFileSize = static_cast<uint64_t>(endPos);
        }
    }
#else
    mappedFile.open(isoPath);
    if (mappedFile.isOpen())
    {
        totalFileSize = mappedFile.size();
    }
    else
    {
        errno = 0;
        streamFile = _wfopen(isoPath.c_str(), L"rb");
        if (streamFile == nullptr)
        {
            const int openErr = errno;
            openErrorDetail = std::string("_wfopen() failed: ") + std::strerror(openErr) +
                " (errno=" + std::to_string(openErr) + ")";
            return;
        }
        totalFileSize = !fsEc ? fsSize : 0;
    }
#endif

    const auto u8Name = isoPath.filename().u8string();
    name = std::string(u8Name.begin(), u8Name.end());

    // Find root sector.
    uint64_t gameOffset = 0;
    const uint64_t XeSectorSize = 2048;
    static const uint64_t PossibleOffsets[] = { 0x00000000ULL, 0x0000FB20ULL, 0x00020600ULL, 0x02080000ULL, 0x0FD90000ULL };
    bool magicFound = false;
    const char RefMagic[] = "MICROSOFT*XBOX*MEDIA";
    const size_t RefMagicLen = sizeof(RefMagic) - 1;
    char magicBuf[32] = {};

    for (size_t i = 0; i < std::size(PossibleOffsets); i++)
    {
        const uint64_t fileOffset = PossibleOffsets[i] + (32ULL * XeSectorSize);
        if ((fileOffset + RefMagicLen) > totalFileSize)
        {
            continue;
        }

        if (readBytesAt(fileOffset, magicBuf, RefMagicLen) &&
            std::memcmp(magicBuf, RefMagic, RefMagicLen) == 0)
        {
            gameOffset = PossibleOffsets[i];
            magicFound = true;
        }
    }

    const uint64_t rootInfoOffset = gameOffset + (32ULL * XeSectorSize) + 20ULL;
    if (!magicFound || (rootInfoOffset + 8ULL) > totalFileSize)
    {
        openErrorDetail = "Xbox 360 XGD2/XGD3 medya imzası (MICROSOFT*XBOX*MEDIA) bulunamadı (dosya boyutu: " +
            std::to_string(totalFileSize) + " bayt).";
        closeHandles();
        return;
    }

    // Parse root information.
    uint32_t rootInfo[2] = {};
    if (!readBytesAt(rootInfoOffset, rootInfo, sizeof(rootInfo)))
    {
        const int readErr = errno;
        openErrorDetail = std::string("ISO kök dizin bilgisi okunamadı: ") + std::strerror(readErr);
        closeHandles();
        return;
    }

    const uint32_t rootSector = rootInfo[0];
    const uint32_t rootSize = rootInfo[1];
    const uint64_t rootOffset = gameOffset + (static_cast<uint64_t>(rootSector) * XeSectorSize);
    const uint32_t MinRootSize = 13;
    const uint32_t MaxRootSize = 32 * 1024 * 1024;
    if ((rootSize < MinRootSize) || (rootSize > MaxRootSize))
    {
        openErrorDetail = "ISO kök dizin tablosu boyutu geçersiz (" + std::to_string(rootSize) + " bayt).";
        closeHandles();
        return;
    }

    struct IterationStep
    {
        std::string fileNameBase;
        uint64_t nodeOffset = 0;
        uint64_t entryOffset = 0;

        IterationStep() = default;
        IterationStep(std::string fileNameBase, uint64_t nodeOffset, uint64_t entryOffset)
            : fileNameBase(std::move(fileNameBase)), nodeOffset(nodeOffset), entryOffset(entryOffset) { }
    };

    std::stack<IterationStep> iterationStack;
    iterationStack.emplace("", rootOffset, 0);

    IterationStep step;
    uint16_t nodeL, nodeR;
    uint32_t sector, length;
    uint8_t attributes, nameLength;
    char fileName[256];
    uint8_t entryHeader[14];
    const uint8_t FileAttributeDirectory = 0x10;
    while (!iterationStack.empty())
    {
        step = iterationStack.top();
        iterationStack.pop();

        const uint64_t infoOffset = step.nodeOffset + step.entryOffset;
        if ((infoOffset + 14ULL) > totalFileSize || !readBytesAt(infoOffset, entryHeader, sizeof(entryHeader)))
        {
            openErrorDetail = "ISO dizin düğümü okunamadı (offset=" + std::to_string(infoOffset) + ").";
            closeHandles();
            return;
        }

        std::memcpy(&nodeL, &entryHeader[0], sizeof(uint16_t));
        std::memcpy(&nodeR, &entryHeader[2], sizeof(uint16_t));
        std::memcpy(&sector, &entryHeader[4], sizeof(uint32_t));
        std::memcpy(&length, &entryHeader[8], sizeof(uint32_t));
        attributes = entryHeader[12];
        nameLength = entryHeader[13];

        const uint64_t nameOffset = infoOffset + 14ULL;
        if ((nameOffset + nameLength) > totalFileSize || !readBytesAt(nameOffset, fileName, nameLength))
        {
            openErrorDetail = "ISO dosya adı girdisi okunamadı.";
            closeHandles();
            return;
        }

        fileName[nameLength] = '\0';

        if (nodeL)
        {
            iterationStack.emplace(step.fileNameBase, step.nodeOffset, static_cast<uint64_t>(nodeL) * 4ULL);
        }

        if (nodeR)
        {
            iterationStack.emplace(step.fileNameBase, step.nodeOffset, static_cast<uint64_t>(nodeR) * 4ULL);
        }

        std::string fileNameUTF8 = step.fileNameBase + fileName;
        if (attributes & FileAttributeDirectory)
        {
            if (length > 0)
            {
                iterationStack.emplace(fileNameUTF8 + "/", gameOffset + static_cast<uint64_t>(sector) * XeSectorSize, 0);
            }
        }
        else
        {
            fileMap[fileNameUTF8] = { gameOffset + static_cast<uint64_t>(sector) * XeSectorSize, static_cast<size_t>(length) };
        }
    }
}

bool ISOFileSystem::load(const std::string &path, uint8_t *fileData, size_t fileDataMaxByteCount) const
{
    auto it = fileMap.find(path);
    if (it != fileMap.end())
    {
        const uint64_t fileOffset = std::get<0>(it->second);
        const size_t fileLength = std::get<1>(it->second);
        if (fileDataMaxByteCount < fileLength)
        {
            return false;
        }

        return readBytesAt(fileOffset, fileData, fileLength);
    }
    else
    {
        return false;
    }
}

size_t ISOFileSystem::getSize(const std::string &path) const
{
    auto it = fileMap.find(path);
    if (it != fileMap.end())
    {
        return std::get<1>(it->second);
    }
    else
    {
        return 0;
    }
}

bool ISOFileSystem::exists(const std::string &path) const
{
    return fileMap.find(path) != fileMap.end();
}

const std::string &ISOFileSystem::getName() const
{
    return name;
}

bool ISOFileSystem::empty() const 
{
    return !mappedFile.isOpen() && posixFd == -1 && streamFile == nullptr;
}

std::unique_ptr<ISOFileSystem> ISOFileSystem::create(const std::filesystem::path &isoPath, std::string *outErrorDetail)
{
    std::unique_ptr<ISOFileSystem> isoFs = std::make_unique<ISOFileSystem>(isoPath);
    if (!isoFs->empty())
    {
        return isoFs;
    }
    else
    {
        if (outErrorDetail != nullptr)
        {
            *outErrorDetail = isoFs->getOpenErrorDetail();
        }
        return nullptr;
    }
}
