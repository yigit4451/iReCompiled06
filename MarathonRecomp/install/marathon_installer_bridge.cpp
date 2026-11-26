#include <stdafx.h>
#include <MarathonInstallerBridge.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <exception>
#include <file.h>
#include <image.h>
#include <install/directory_file_system.h>
#include <install/installer.h>
#include <install/iso_file_system.h>
#include <install/xcontent_file_system.h>
#include <user/paths.h>

#ifdef MARATHON_RECOMP_IOS
#include <os/ios/sandbox_ios.h>
#endif

#if defined(MARATHON_HAS_LIBARCHIVE)
#include <archive.h>
#include <archive_entry.h>
#endif

namespace
{
    std::atomic<bool> s_cancelExtraction{ false };

    void WriteStringToBuffer(std::string_view text, char* buffer, int bufferSize)
    {
        if (buffer == nullptr || bufferSize <= 0)
            return;

        const size_t maxChars = static_cast<size_t>(bufferSize - 1);
        const size_t copyLen = std::min(text.size(), maxChars);
        if (copyLen > 0)
            std::memcpy(buffer, text.data(), copyLen);
        buffer[copyLen] = '\0';
    }

    std::string LowerAscii(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return value;
    }

    std::filesystem::path GetInstallRoot()
    {
#ifdef MARATHON_RECOMP_IOS
        return os::ios::GetGameInstallPath();
#else
        return GetGamePath();
#endif
    }

    bool IsXexExtension(const std::filesystem::path& path)
    {
        return LowerAscii(path.extension().string()) == ".xex";
    }

    bool IsIsoExtension(const std::filesystem::path& path)
    {
        return LowerAscii(path.extension().string()) == ".iso";
    }

    bool Is7zExtension(const std::filesystem::path& path)
    {
        return LowerAscii(path.extension().string()) == ".7z";
    }

    struct ScopedTempDirectoryCleanup
    {
        std::filesystem::path path;

        ~ScopedTempDirectoryCleanup()
        {
            if (!path.empty())
            {
                std::error_code ec;
                std::filesystem::remove_all(path, ec);
            }
        }
    };

#if defined(MARATHON_HAS_LIBARCHIVE)
    std::filesystem::path SanitizeArchiveEntryPath(const char* rawEntryPath)
    {
        if (rawEntryPath == nullptr || rawEntryPath[0] == '\0')
            return {};

        const std::filesystem::path raw = std::filesystem::u8path(rawEntryPath);
        std::filesystem::path clean;
        for (const auto& part : raw)
        {
            if (part == "." || part == "/" || part == "\\")
                continue;
            if (part == "..")
                continue;
            clean /= part;
        }
        return clean;
    }

    bool LocateExtractedPayloadInDirectory(
        const std::filesystem::path& rootDir,
        std::filesystem::path& outPayloadPath,
        std::string& outError)
    {
        std::error_code ec;
        std::filesystem::path foundIso;
        std::filesystem::path foundXexDir;
        std::filesystem::path foundStandaloneXex;

        for (std::filesystem::recursive_directory_iterator it(rootDir, ec), end; it != end && !ec; it.increment(ec))
        {
            if (!it->is_regular_file(ec))
                continue;

            const std::filesystem::path filePath = it->path();
            if (IsIsoExtension(filePath))
            {
                foundIso = filePath;
                break;
            }

            if (LowerAscii(filePath.filename().string()) == "default.xex" || IsXexExtension(filePath))
            {
                const std::filesystem::path parentDir = filePath.parent_path();
                if (!parentDir.empty() && std::filesystem::exists(parentDir / "xenon", ec))
                {
                    foundXexDir = parentDir;
                }
                else if (foundStandaloneXex.empty())
                {
                    foundStandaloneXex = filePath;
                }
            }
        }

        if (!foundIso.empty())
        {
            outPayloadPath = foundIso;
            return true;
        }

        if (!foundXexDir.empty())
        {
            outPayloadPath = foundXexDir;
            return true;
        }

        if (!foundStandaloneXex.empty())
        {
            outPayloadPath = foundStandaloneXex;
            return true;
        }

        outError = ".7z arşivi içinde geçerli bir .iso kalıbı veya default.xex oyun dosyası bulunamadı.";
        return false;
    }

    bool Extract7zArchiveToTemp(
        const std::filesystem::path& archivePath,
        std::filesystem::path& outTempDir,
        std::filesystem::path& outResolvedPayloadPath,
        MarathonExtractionProgressCallback progressCallback,
        void* userData,
        std::string& outError)
    {
        std::error_code ec;
        std::filesystem::path baseTmpDir = std::filesystem::temp_directory_path(ec);
        if (ec || baseTmpDir.empty())
        {
            baseTmpDir = GetInstallRoot().parent_path() / "tmp";
        }

        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        outTempDir = baseTmpDir / ("iReCompiled06_7z_" + std::to_string(stamp));
        std::filesystem::create_directories(outTempDir, ec);
        if (ec)
        {
            outError = "Geçici .7z çıkartma dizini (tmp/) oluşturulamadı.";
            return false;
        }

        const uint64_t compressedTotalBytes = std::filesystem::file_size(archivePath, ec);

        struct archive* a = archive_read_new();
        if (a == nullptr)
        {
            outError = "libarchive okuyucusu başlatılamadı.";
            return false;
        }

        archive_read_support_format_7zip(a);
        archive_read_support_format_all(a);
        archive_read_support_filter_all(a);

        const std::string archivePathStr = archivePath.string();
        if (archive_read_open_filename(a, archivePathStr.c_str(), 1024 * 256) != ARCHIVE_OK)
        {
            const char* errStr = archive_error_string(a);
            outError = std::string(".7z arşivi açılamadı: ") + (errStr ? errStr : "Bilinmeyen libarchive hatası.");
            archive_read_free(a);
            return false;
        }

        uint64_t totalUnpackedBytes = 0;
        struct archive_entry* entry = nullptr;

        while (true)
        {
            if (s_cancelExtraction.load(std::memory_order_relaxed))
            {
                archive_read_close(a);
                archive_read_free(a);
                outError = "Kurulum kullanıcı tarafından iptal edildi.";
                return false;
            }

            const int r = archive_read_next_header(a, &entry);
            if (r == ARCHIVE_EOF)
                break;

            if (r < ARCHIVE_OK && r != ARCHIVE_WARN)
            {
                const char* errStr = archive_error_string(a);
                outError = std::string(".7z başlığı okunurken hata oluştu: ") + (errStr ? errStr : "Arşiv bozuk.");
                archive_read_close(a);
                archive_read_free(a);
                return false;
            }

            const std::filesystem::path relPath = SanitizeArchiveEntryPath(archive_entry_pathname(entry));
            if (relPath.empty())
            {
                archive_read_data_skip(a);
                continue;
            }

            const std::filesystem::path dstPath = outTempDir / relPath;
            const mode_t fileType = archive_entry_filetype(entry);
            if (fileType == AE_IFDIR)
            {
                std::filesystem::create_directories(dstPath, ec);
                continue;
            }

            if (fileType != AE_IFREG)
            {
                archive_read_data_skip(a);
                continue;
            }

            std::filesystem::create_directories(dstPath.parent_path(), ec);
            std::ofstream outFile(dstPath, std::ios::binary | std::ios::trunc);
            if (!outFile.is_open())
            {
                outError = "Geçici dizine dosya yazılamadı: " + relPath.filename().string();
                archive_read_close(a);
                archive_read_free(a);
                return false;
            }

            const void* chunkBuff = nullptr;
            size_t chunkSize = 0;
            la_int64_t chunkOffset = 0;
            const std::string entryFilename = relPath.filename().string();

            while (true)
            {
                if (s_cancelExtraction.load(std::memory_order_relaxed))
                {
                    outFile.close();
                    archive_read_close(a);
                    archive_read_free(a);
                    outError = "Kurulum kullanıcı tarafından iptal edildi.";
                    return false;
                }

                const int dr = archive_read_data_block(a, &chunkBuff, &chunkSize, &chunkOffset);
                if (dr == ARCHIVE_EOF)
                    break;

                if (dr < ARCHIVE_OK && dr != ARCHIVE_WARN)
                {
                    const char* errStr = archive_error_string(a);
                    outFile.close();
                    archive_read_close(a);
                    archive_read_free(a);
                    outError = std::string(".7z veri akışı çözülürken hata: ") + (errStr ? errStr : "LZMA akış hatası.");
                    return false;
                }

                if (chunkBuff != nullptr && chunkSize > 0)
                {
                    outFile.write(static_cast<const char*>(chunkBuff), static_cast<std::streamsize>(chunkSize));
                    if (!outFile.good())
                    {
                        outFile.close();
                        archive_read_close(a);
                        archive_read_free(a);
                        outError = "Geçici diske yazma hatası (cihazda yeterli boş alan olduğundan emin olun).";
                        return false;
                    }
                    totalUnpackedBytes += chunkSize;
                }

                if (progressCallback)
                {
                    const uint64_t compPos = static_cast<uint64_t>(std::max<la_int64_t>(0, archive_filter_bytes(a, -1)));
                    float archiveRatio = 0.20f;
                    if (compressedTotalBytes > 0 && compPos > 0)
                    {
                        const double frac = std::clamp(static_cast<double>(compPos) / static_cast<double>(compressedTotalBytes), 0.0, 1.0);
                        archiveRatio = static_cast<float>(0.02 + frac * 0.46);
                    }
                    const std::string msg = ".7z arşivi geçici dizine açılıyor: " + entryFilename;
                    if (!progressCallback(archiveRatio, totalUnpackedBytes, compressedTotalBytes, msg.c_str(), userData))
                    {
                        outFile.close();
                        archive_read_close(a);
                        archive_read_free(a);
                        outError = "Kurulum kullanıcı tarafından iptal edildi.";
                        return false;
                    }
                }
            }
        }

        archive_read_close(a);
        archive_read_free(a);

        return LocateExtractedPayloadInDirectory(outTempDir, outResolvedPayloadPath, outError);
    }
#endif

    bool ExtractStandaloneXex(
        const std::filesystem::path& xexPath,
        const std::filesystem::path& installRoot,
        MarathonExtractionProgressCallback progressCallback,
        void* userData,
        std::string& outError)
    {
        std::error_code ec;
        const uint64_t rawXexSize = std::filesystem::file_size(xexPath, ec);
        if (ec || rawXexSize == 0)
        {
            outError = "Seçilen XEX dosyası okunamadı veya boş.";
            return false;
        }

        const uint64_t estimatedTotal = rawXexSize * 2 + 0xD98000ull;

        if (progressCallback && !progressCallback(0.05f, 0, estimatedTotal, "default.xex okunuyor...", userData))
        {
            outError = "Kurulum kullanıcı tarafından iptal edildi.";
            return false;
        }

        const auto rawBytes = LoadFile(xexPath);
        if (rawBytes.empty())
        {
            outError = "default.xex belleğe aktarılamadı.";
            return false;
        }

        if (s_cancelExtraction.load(std::memory_order_relaxed))
        {
            outError = "Kurulum kullanıcı tarafından iptal edildi.";
            return false;
        }

        const std::filesystem::path gameDir = installRoot / "game";
        const std::filesystem::path decryptedDir = installRoot / "decrypted";
        std::filesystem::create_directories(gameDir, ec);
        std::filesystem::create_directories(decryptedDir, ec);
        if (ec)
        {
            outError = "Documents/iReCompiled06 alt dizinleri oluşturulamadı.";
            return false;
        }

        const std::filesystem::path dstXexPath = gameDir / "default.xex";
        {
            std::ofstream xexOut(dstXexPath, std::ios::binary | std::ios::trunc);
            if (!xexOut.is_open())
            {
                outError = "game/default.xex dosyası oluşturulamadı.";
                return false;
            }
            xexOut.write(reinterpret_cast<const char*>(rawBytes.data()), static_cast<std::streamsize>(rawBytes.size()));
            if (!xexOut.good())
            {
                outError = "game/default.xex yazılırken G/Ç hatası oluştu.";
                return false;
            }
        }

        if (progressCallback && !progressCallback(0.45f, rawXexSize, estimatedTotal, "XEX şifresi çözülüyor ve açılıyor (Image::ParseImage)...", userData))
        {
            std::filesystem::remove(dstXexPath, ec);
            outError = "Kurulum kullanıcı tarafından iptal edildi.";
            return false;
        }

        const auto image = Image::ParseImage(rawBytes.data(), rawBytes.size());
        if (image.data == nullptr || image.size == 0)
        {
            std::filesystem::remove(dstXexPath, ec);
            outError = "Geçersiz veya desteklenmeyen Xbox 360 XEX görüntüsü.";
            return false;
        }

        DecryptedExecutableHeader header{
            .magic = DECRYPTED_EXECUTABLE_MAGIC,
            .base = image.base,
            .entry_point = image.entry_point,
            .resource_offset = image.resource_offset,
            .resource_size = image.resource_size,
        };

        const std::filesystem::path decryptedPath = decryptedDir / "default.bin";
        {
            std::ofstream decOut(decryptedPath, std::ios::binary | std::ios::trunc);
            if (!decOut.is_open())
            {
                std::filesystem::remove(dstXexPath, ec);
                outError = "decrypted/default.bin dosyası oluşturulamadı.";
                return false;
            }

            decOut.write(reinterpret_cast<const char*>(&header), sizeof(header));
            decOut.write(reinterpret_cast<const char*>(image.data.get()), static_cast<std::streamsize>(image.size));
            if (!decOut.good())
            {
                std::filesystem::remove(decryptedPath, ec);
                std::filesystem::remove(dstXexPath, ec);
                outError = "decrypted/default.bin yazılırken G/Ç hatası oluştu.";
                return false;
            }
        }

        const uint64_t finalBytes = rawXexSize + sizeof(header) + image.size;
        if (progressCallback)
            progressCallback(1.0f, finalBytes, finalBytes, "Kurulum tamamlandı!", userData);

        return true;
    }
}

extern "C" MARATHON_CORE_API bool IsMarathonInstalled(void)
{
    try
    {
        const std::filesystem::path installRoot = GetInstallRoot();
        std::filesystem::path modulePath;
        if (Installer::checkGameInstall(installRoot, modulePath))
            return true;

        std::error_code ec;
        if (std::filesystem::exists(installRoot / "default.xex", ec) &&
            std::filesystem::exists(installRoot / "xenon", ec))
        {
            return true;
        }
        return false;
    }
    catch (...)
    {
        return false;
    }
}

extern "C" MARATHON_CORE_API bool VerifyMarathonInstallIntegrity(
    char* detailBuffer,
    int detailBufferSize)
{
    WriteStringToBuffer("", detailBuffer, detailBufferSize);

    try
    {
#ifdef MARATHON_RECOMP_IOS
        os::ios::EnsureSandboxDirectories();
#endif
        const std::filesystem::path installRoot = GetInstallRoot();
        std::error_code ec;

        const std::filesystem::path decryptedBin = installRoot / "decrypted" / "default.bin";
        const std::filesystem::path gameXex = installRoot / "game" / "default.xex";
        const std::filesystem::path rootXex = installRoot / "default.xex";

        if (std::filesystem::exists(decryptedBin, ec) && std::filesystem::file_size(decryptedBin, ec) > sizeof(DecryptedExecutableHeader))
        {
            WriteStringToBuffer("Mevcut kurulum doğrulandı (decrypted/default.bin hazır).", detailBuffer, detailBufferSize);
            return true;
        }

        std::filesystem::path candidateXex;
        if (std::filesystem::exists(gameXex, ec))
        {
            candidateXex = gameXex;
        }
        else if (std::filesystem::exists(rootXex, ec))
        {
            candidateXex = rootXex;
            if (std::filesystem::exists(installRoot / "xenon", ec) && !std::filesystem::exists(installRoot / "game" / "xenon", ec))
            {
                std::filesystem::create_directories(installRoot / "game", ec);
                std::filesystem::rename(installRoot / "xenon", installRoot / "game" / "xenon", ec);
            }
        }

        if (!candidateXex.empty())
        {
            std::string xexError;
            if (ExtractStandaloneXex(candidateXex, installRoot, nullptr, nullptr, xexError))
            {
                WriteStringToBuffer("Manuel kopyalanan default.xex doğrulandı ve decrypted/default.bin hazırlandı.", detailBuffer, detailBufferSize);
                return true;
            }
            WriteStringToBuffer(xexError.empty() ? "default.xex doğrulanamadı." : xexError, detailBuffer, detailBufferSize);
            return false;
        }

        WriteStringToBuffer(
            "Documents/iReCompiled06 dizininde kurulu oyun (decrypted/default.bin veya game/default.xex) bulunamadı. Lütfen dosyaları kopyaladığınızdan emin olun veya kurulum sihirbazını kullanın.",
            detailBuffer,
            detailBufferSize);
        return false;
    }
    catch (const std::exception& e)
    {
        WriteStringToBuffer(e.what(), detailBuffer, detailBufferSize);
        return false;
    }
}

extern "C" MARATHON_CORE_API const char* GetMarathonInstallDirectory(void)
{
    static std::string s_installRoot;
    s_installRoot = GetInstallRoot().string();
    return s_installRoot.c_str();
}

extern "C" MARATHON_CORE_API MarathonSourceKind ValidateMarathonSource(
    const char* sourcePath,
    char* detailBuffer,
    int detailBufferSize)
{
    if (sourcePath == nullptr || sourcePath[0] == '\0')
    {
        WriteStringToBuffer("Dosya yolu boş.", detailBuffer, detailBufferSize);
        return MARATHON_SOURCE_INVALID;
    }

    try
    {
        std::error_code ec;
        const std::filesystem::path path = std::filesystem::u8path(sourcePath);
        if (!std::filesystem::exists(path, ec))
        {
            WriteStringToBuffer("Seçilen kaynak dosyaya erişilemiyor.", detailBuffer, detailBufferSize);
            return MARATHON_SOURCE_INVALID;
        }

        if (std::filesystem::is_directory(path, ec))
        {
            if (Installer::parseGame(path))
            {
                WriteStringToBuffer("Çıkartılmış Xbox 360 oyun klasörü doğrulandı.", detailBuffer, detailBufferSize);
                return MARATHON_SOURCE_XEX_DIRECTORY;
            }
            WriteStringToBuffer("Seçilen klasörde default.xex bulunamadı.", detailBuffer, detailBufferSize);
            return MARATHON_SOURCE_INVALID;
        }

        if (Is7zExtension(path))
        {
            const uint64_t archiveSize = std::filesystem::file_size(path, ec);
            if (ec || archiveSize < 32)
            {
                WriteStringToBuffer("Seçilen .7z arşivi boş veya okunamıyor.", detailBuffer, detailBufferSize);
                return MARATHON_SOURCE_INVALID;
            }
            WriteStringToBuffer("7-Zip (.7z) arşivi doğrulandı (geçici dizine akış halinde açılıp kurulacak).", detailBuffer, detailBufferSize);
            return MARATHON_SOURCE_7Z_ARCHIVE;
        }

        if (IsIsoExtension(path))
        {
            if (Installer::parseGame(path))
            {
                WriteStringToBuffer("Xbox 360 ISO kalıbı doğrulandı (doğrudan akışa hazır).", detailBuffer, detailBufferSize);
                return MARATHON_SOURCE_ISO;
            }
            WriteStringToBuffer("ISO kalıbı içerisinde geçerli Sonic '06 (default.xex) bulunamadı.", detailBuffer, detailBufferSize);
            return MARATHON_SOURCE_INVALID;
        }

        if (IsXexExtension(path))
        {
            const std::filesystem::path parentDir = path.parent_path();
            if (!parentDir.empty() && std::filesystem::exists(parentDir / "xenon", ec) && Installer::parseGame(parentDir))
            {
                WriteStringToBuffer("XEX ve oyun arşiv dizini (xenon/) doğrulandı.", detailBuffer, detailBufferSize);
                return MARATHON_SOURCE_XEX_DIRECTORY;
            }

            WriteStringToBuffer("Xbox 360 çalıştırılabilir XEX dosyası doğrulandı.", detailBuffer, detailBufferSize);
            return MARATHON_SOURCE_STANDALONE_XEX;
        }

        if (Installer::parseDLC(path) != DLC::Unknown)
        {
            WriteStringToBuffer("Xbox 360 DLC paketi doğrulandı.", detailBuffer, detailBufferSize);
            return MARATHON_SOURCE_DLC;
        }

        WriteStringToBuffer("Desteklenmeyen dosya biçimi. Lütfen geçerli bir .iso, .7z, default.xex veya oyun klasörü seçin.", detailBuffer, detailBufferSize);
        return MARATHON_SOURCE_INVALID;
    }
    catch (const std::exception& e)
    {
        WriteStringToBuffer(e.what(), detailBuffer, detailBufferSize);
        return MARATHON_SOURCE_INVALID;
    }
}

extern "C" MARATHON_CORE_API bool StartMarathonExtraction(
    const char* sourcePath,
    bool skipHashChecks,
    MarathonExtractionProgressCallback progressCallback,
    void* userData,
    char* errorBuffer,
    int errorBufferSize)
{
    s_cancelExtraction.store(false, std::memory_order_relaxed);
    WriteStringToBuffer("", errorBuffer, errorBufferSize);

    if (sourcePath == nullptr || sourcePath[0] == '\0')
    {
        WriteStringToBuffer("Geçersiz kaynak dosya yolu.", errorBuffer, errorBufferSize);
        return false;
    }

    try
    {
#ifdef MARATHON_RECOMP_IOS
        os::ios::EnsureSandboxDirectories();
#endif
        const std::filesystem::path installRoot = GetInstallRoot();
        std::error_code ec;
        std::filesystem::create_directories(installRoot, ec);

        std::filesystem::path inputPath = std::filesystem::u8path(sourcePath);
        if (!std::filesystem::exists(inputPath, ec))
        {
            WriteStringToBuffer("Kaynak dosyaya erişilemedi. Bulut dosyasının çevrimiçi olduğundan emin olun.", errorBuffer, errorBufferSize);
            return false;
        }

        if (progressCallback && !progressCallback(0.01f, 0, 0, "Kaynak doğrulanıyor ve sanal dosya sistemi bağlanıyor...", userData))
        {
            WriteStringToBuffer("Kurulum kullanıcı tarafından iptal edildi.", errorBuffer, errorBufferSize);
            return false;
        }

        ScopedTempDirectoryCleanup temp7zCleanup{};
        float baseProgressOffset = 0.0f;
        float progressScale = 1.0f;

        if (Is7zExtension(inputPath))
        {
#if defined(MARATHON_HAS_LIBARCHIVE)
            std::filesystem::path resolvedPayload;
            std::string archiveErr;
            if (!Extract7zArchiveToTemp(inputPath, temp7zCleanup.path, resolvedPayload, progressCallback, userData, archiveErr))
            {
                WriteStringToBuffer(archiveErr.empty() ? ".7z arşivi geçici dizine çıkartılamadı." : archiveErr, errorBuffer, errorBufferSize);
                return false;
            }
            inputPath = resolvedPayload;
            baseProgressOffset = 0.48f;
            progressScale = 0.52f;
#else
            WriteStringToBuffer("Bu derlemede libarchive (.7z) desteği etkin değil.", errorBuffer, errorBufferSize);
            return false;
#endif
        }

        Installer::Input installerInput{};
        if (std::filesystem::is_directory(inputPath, ec) || IsIsoExtension(inputPath))
        {
            installerInput.gameSource = inputPath;
        }
        else if (IsXexExtension(inputPath))
        {
            const std::filesystem::path parentDir = inputPath.parent_path();
            if (!parentDir.empty() && std::filesystem::exists(parentDir / "xenon", ec) && Installer::parseGame(parentDir))
            {
                installerInput.gameSource = parentDir;
            }
            else
            {
                std::string xexError;
                const bool ok = ExtractStandaloneXex(inputPath, installRoot, progressCallback, userData, xexError);
                if (!ok)
                    WriteStringToBuffer(xexError, errorBuffer, errorBufferSize);
                return ok;
            }
        }
        else if (XContentFileSystem::check(inputPath))
        {
            installerInput.dlcSources.push_back(inputPath);
        }
        else
        {
            WriteStringToBuffer("Seçilen dosya geçerli bir Sonic '06 ISO, .7z, XEX veya DLC kaynağı değil.", errorBuffer, errorBufferSize);
            return false;
        }

        Journal journal{};
        Installer::Sources sources{};
        if (!Installer::parseSources(installerInput, journal, sources))
        {
            const std::string msg = journal.lastErrorMessage.empty()
                ? "Kaynak arşiv ayrıştırılamadı veya eksik oyun dosyaları var."
                : journal.lastErrorMessage;
            WriteStringToBuffer(msg, errorBuffer, errorBufferSize);
            return false;
        }

        if (progressCallback && !progressCallback(baseProgressOffset + 0.02f * progressScale, 0, journal.progressTotal, "Dosyalar Documents/iReCompiled06 dizinine çıkartılıyor...", userData))
        {
            WriteStringToBuffer("Kurulum kullanıcı tarafından iptal edildi.", errorBuffer, errorBufferSize);
            return false;
        }

        auto bridgeProgress = [&]() -> bool
        {
            if (s_cancelExtraction.load(std::memory_order_relaxed))
                return false;

            if (!progressCallback)
                return true;

            const uint64_t done = journal.progressCounter;
            const uint64_t total = journal.progressTotal;
            float ratio = 0.02f;
            if (total > 0)
            {
                const double raw = static_cast<double>(done) / static_cast<double>(total);
                ratio = static_cast<float>(std::clamp(raw, 0.02, 0.99));
            }
            const float scaledRatio = std::clamp(baseProgressOffset + ratio * progressScale, 0.02f, 0.99f);

            std::string status = "Oyun arşivleri çıkartılıyor...";
            if (!journal.createdFiles.empty())
            {
                const std::string lastFile = journal.createdFiles.back().filename().string();
                if (!lastFile.empty())
                    status = "Çıkartılıyor: " + lastFile;
            }

            return progressCallback(scaledRatio, done, total, status.c_str(), userData);
        };

        if (!Installer::install(sources, installRoot, skipHashChecks, journal, std::chrono::seconds(0), bridgeProgress))
        {
            Installer::rollback(journal);
            const std::string msg = journal.lastErrorMessage.empty()
                ? "Kurulum sırasında hata oluştu ve kısmi dosyalar geri alındı."
                : journal.lastErrorMessage;
            WriteStringToBuffer(msg, errorBuffer, errorBufferSize);
            return false;
        }

        if (progressCallback)
            progressCallback(1.0f, journal.progressTotal, journal.progressTotal, "Kurulum başarıyla tamamlandı!", userData);

        return true;
    }
    catch (const std::exception& e)
    {
        WriteStringToBuffer(std::string("Beklenmeyen kurulum hatası: ") + e.what(), errorBuffer, errorBufferSize);
        return false;
    }
    catch (...)
    {
        WriteStringToBuffer("Bilinmeyen bir C++ kurulum hatası oluştu.", errorBuffer, errorBufferSize);
        return false;
    }
}

extern "C" MARATHON_CORE_API void CancelMarathonExtraction(void)
{
    s_cancelExtraction.store(true, std::memory_order_relaxed);
}
