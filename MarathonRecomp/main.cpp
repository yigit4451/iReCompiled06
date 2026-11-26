#include <cstdio>
#include <stdafx.h>
#ifdef __x86_64__
#include <cpuid.h>
#endif
#include <cpu/guest_thread.h>
#include <gpu/imgui/imgui_snapshot.h>
#include <gpu/video.h>
#include <kernel/function.h>
#include <kernel/memory.h>
#include <kernel/heap.h>
#include <kernel/xam.h>
#include <kernel/io/file_system.h>
#include <file.h>
#include <vector>
#include <image.h>
#include <apu/audio.h>
#include <hid/hid.h>
#include <user/config.h>
#include <user/paths.h>
#include <user/registry.h>
#include <kernel/xdbf.h>
#include <install/installer.h>
#include <install/update_checker.h>
#include <os/logger.h>
#include <os/process.h>
#include <os/registry.h>
#include <ui/game_window.h>
#include <ui/installer_wizard.h>
#include <mod/mod_loader.h>
#include <preload_executable.h>
#include <iostream>
#include <app.h>

#ifdef _WIN32
#include <timeapi.h>
#endif

#if defined(_WIN32) && defined(MARATHON_RECOMP_D3D12)
static std::array<std::string_view, 3> g_D3D12RequiredModules =
{
    "D3D12/D3D12Core.dll",
    "dxcompiler.dll",
    "dxil.dll"
};
#endif

const size_t XMAIOBegin = 0x7FEA0000;
const size_t XMAIOEnd = XMAIOBegin + 0x0000FFFF;

Memory g_memory;
Heap g_userHeap;
XDBFWrapper g_xdbfWrapper;
std::unordered_map<uint16_t, GuestTexture*> g_xdbfTextureCache;

void HostStartup()
{
#ifdef _WIN32
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif

    HID::GetManager()->Init();
}

// Name inspired from nt's entry point
void KiSystemStartup()
{
    if (g_memory.base == nullptr)
    {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, GameWindow::GetTitle(), Localise("System_MemoryAllocationFailed").c_str(), GameWindow::s_pWindow);
        std::_Exit(1);
    }

    g_userHeap.Init();

    const auto gameContent = XamMakeContent(XCONTENTTYPE_RESERVED, "Game");
    const std::string gamePath = (const char*)(GetGamePath() / "game").u8string().c_str();

    BuildPathCache(gamePath);

    XamRegisterContent(gameContent, gamePath);

    const auto saveFilePath = GetSaveFilePath(true);
    bool saveFileExists = std::filesystem::exists(saveFilePath);

    if (!saveFileExists)
    {
        // Copy base save data to modded save as fallback.
        std::error_code ec;
        std::filesystem::create_directories(saveFilePath.parent_path(), ec);

        if (!ec)
        {
            std::filesystem::copy_file(GetSaveFilePath(false), saveFilePath, ec);
            saveFileExists = !ec;
        }
    }

    if (saveFileExists)
    {
        std::u8string savePathU8 = saveFilePath.parent_path().u8string();
        XamRegisterContent(XamMakeContent(XCONTENTTYPE_SAVEDATA, "SonicNextSaveData.bin"), (const char*)(savePathU8.c_str()));
    }

    // Mount game
    XamContentCreateEx(0, "game", &gameContent, OPEN_EXISTING, nullptr, nullptr, 0, 0, nullptr);

    // OS mounts game data to D:
    XamContentCreateEx(0, "D", &gameContent, OPEN_EXISTING, nullptr, nullptr, 0, 0, nullptr);

    std::error_code ec;
    for (auto& file : std::filesystem::directory_iterator(GetGamePath() / "dlc", ec))
    {
        if (file.is_directory())
        {
            // Only include directories with valid DLC content, otherwise the game will try to load it and error.
            const auto arcFile = file.path() / "download.arc";
            if (std::filesystem::exists(arcFile))
            {
                std::u8string fileNameU8 = file.path().filename().u8string();
                std::u8string filePathU8 = file.path().u8string();
                XamRegisterContent(XamMakeContent(XCONTENTTYPE_DLC, (const char*)(fileNameU8.c_str())), (const char*)(filePathU8.c_str()));
            }
        }
    }

    XAudioInitializeSystem();
}

uint32_t LdrLoadModule(const std::filesystem::path &path)
{
    GuestModuleDiagnostics diag{};
    diag.modulePath = path.string();

    std::error_code existsEc;
    if (g_memory.base == nullptr)
    {
        diag.loaderStatus = "g_memory.base NULL; 4 GiB guest sanal bellek penceresi ayrilamadi";
        GuestSetModuleDiagnostics(diag);
        return 0;
    }

    const auto loadResult = LoadFile(path);
    if (loadResult.empty())
    {
        diag.loaderStatus = std::filesystem::exists(path, existsEc)
            ? "Dosya mevcut ama okunamadi/bos (LoadFile bos dondu)"
            : "Dosya bulunamadi (default.xex / decrypted/default.bin yok)";
        GuestSetModuleDiagnostics(diag);
        return 0;
    }

    DecryptedExecutableHeader header{};
    memcpy(&header, loadResult.data(), std::min(sizeof(header), loadResult.size()));
    if (header.magic == DECRYPTED_EXECUTABLE_MAGIC) {
        LOG_UTILITY("Loading pre-decrypted executable");

        const size_t payloadSize = loadResult.size() - sizeof(DecryptedExecutableHeader);
        diag.entryPoint = header.entry_point;
        diag.imageBase = header.base;
        diag.imageSize = static_cast<uint32_t>(payloadSize);

        if (header.base < PPC_IMAGE_BASE || payloadSize > PPC_MEMORY_SIZE - header.base)
        {
            diag.loaderStatus = "Decrypted imaj PPC bellek penceresine sigmiyor";
            GuestSetModuleDiagnostics(diag);
            return 0;
        }

        memcpy(g_memory.Translate(header.base), loadResult.data() + sizeof(DecryptedExecutableHeader),
               payloadSize);
        g_xdbfWrapper = XDBFWrapper(static_cast<uint8_t*>(g_memory.Translate(header.resource_offset)), header.resource_size);

        diag.loaderStatus = payloadSize > PPC_IMAGE_SIZE
            ? "Decrypted imaj yuklendi (UYARI: imaj boyutu PPC_IMAGE_SIZE'dan buyuk; fonksiyon tablosu ile cakisabilir)"
            : "Decrypted imaj yuklendi";
        GuestSetModuleDiagnostics(diag);
        return header.entry_point;
    } else {
        LOG_UTILITY("Loading XEX executable");
        const auto image = Image::ParseImage(loadResult.data(), loadResult.size());

        diag.entryPoint = image.entry_point;
        diag.imageBase = image.base;
        diag.imageSize = static_cast<uint32_t>(image.size);

        if (image.data == nullptr || image.size == 0 || image.base < PPC_IMAGE_BASE ||
            image.size > PPC_MEMORY_SIZE - image.base)
        {
            diag.loaderStatus = "XEX ayristirilamadi (sifreli/bozuk default.xex) veya PPC bellek penceresine sigmiyor";
            GuestSetModuleDiagnostics(diag);
            return 0;
        }

        memcpy(g_memory.Translate(image.base), image.data.get(), image.size);
        g_xdbfWrapper = XDBFWrapper(static_cast<uint8_t*>(g_memory.Translate(image.resource_offset)), image.resource_size);

        diag.loaderStatus = image.size > PPC_IMAGE_SIZE
            ? "XEX yuklendi (UYARI: imaj boyutu PPC_IMAGE_SIZE'dan buyuk; fonksiyon tablosu ile cakisabilir)"
            : "XEX yuklendi";
        GuestSetModuleDiagnostics(diag);
        return image.entry_point;
    }
}

#ifdef __x86_64__
#include <cpuid.h>
#endif
#include <cpu/guest_thread.h>
#include <gpu/imgui/imgui_snapshot.h>
#include <gpu/video.h>
#include <kernel/function.h>
#include <kernel/memory.h>
#include <kernel/heap.h>
#include <kernel/xam.h>
#include <kernel/io/file_system.h>
#include <file.h>
#include <vector>
#include <image.h>
#include <apu/audio.h>
#include <hid/hid.h>
#include <user/config.h>
#include <user/paths.h>
#include <user/registry.h>
#include <kernel/xdbf.h>
#include <install/installer.h>
#include <install/update_checker.h>
#include <os/logger.h>
#include <os/process.h>
#include <os/registry.h>
#include <ui/game_window.h>
#include <ui/installer_wizard.h>
#include <mod/mod_loader.h>
#include <preload_executable.h>
#include <iostream>
#include <app.h>

#ifdef _WIN32
#include <timeapi.h>
#endif

#if defined(_WIN32) && defined(MARATHON_RECOMP_D3D12)
static std::array<std::string_view, 3> g_D3D12RequiredModules =
{
    "D3D12/D3D12Core.dll",
    "dxcompiler.dll",
    "dxil.dll"
};
#endif

const size_t XMAIOBegin = 0x7FEA0000;
const size_t XMAIOEnd = XMAIOBegin + 0x0000FFFF;

Memory g_memory;
Heap g_userHeap;
XDBFWrapper g_xdbfWrapper;
std::unordered_map<uint16_t, GuestTexture*> g_xdbfTextureCache;

void HostStartup()
{
#ifdef _WIN32
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif

    HID::GetManager()->Init();
}

// Name inspired from nt's entry point
void KiSystemStartup()
{
    if (g_memory.base == nullptr)
    {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, GameWindow::GetTitle(), Localise("System_MemoryAllocationFailed").c_str(), GameWindow::s_pWindow);
        std::_Exit(1);
    }

    g_userHeap.Init();

    const auto gameContent = XamMakeContent(XCONTENTTYPE_RESERVED, "Game");
    const std::string gamePath = (const char*)(GetGamePath() / "game").u8string().c_str();

    BuildPathCache(gamePath);

    XamRegisterContent(gameContent, gamePath);

    const auto saveFilePath = GetSaveFilePath(true);
    bool saveFileExists = std::filesystem::exists(saveFilePath);

    if (!saveFileExists)
    {
        // Copy base save data to modded save as fallback.
        std::error_code ec;
        std::filesystem::create_directories(saveFilePath.parent_path(), ec);

        if (!ec)
        {
            std::filesystem::copy_file(GetSaveFilePath(false), saveFilePath, ec);
            saveFileExists = !ec;
        }
    }

    if (saveFileExists)
    {
        std::u8string savePathU8 = saveFilePath.parent_path().u8string();
        XamRegisterContent(XamMakeContent(XCONTENTTYPE_SAVEDATA, "SonicNextSaveData.bin"), (const char*)(savePathU8.c_str()));
    }

    // Mount game
    XamContentCreateEx(0, "game", &gameContent, OPEN_EXISTING, nullptr, nullptr, 0, 0, nullptr);

    // OS mounts game data to D:
    XamContentCreateEx(0, "D", &gameContent, OPEN_EXISTING, nullptr, nullptr, 0, 0, nullptr);

    std::error_code ec;
    for (auto& file : std::filesystem::directory_iterator(GetGamePath() / "dlc", ec))
    {
        if (file.is_directory())
        {
            // Only include directories with valid DLC content, otherwise the game will try to load it and error.
            const auto arcFile = file.path() / "download.arc";
            if (std::filesystem::exists(arcFile))
            {
                std::u8string fileNameU8 = file.path().filename().u8string();
                std::u8string filePathU8 = file.path().u8string();
                XamRegisterContent(XamMakeContent(XCONTENTTYPE_DLC, (const char*)(fileNameU8.c_str())), (const char*)(filePathU8.c_str()));
            }
        }
    }

    XAudioInitializeSystem();
}

uint32_t LdrLoadModule(const std::filesystem::path &path)
{
    const auto loadResult = LoadFile(path);
    if (loadResult.empty())
    {
        assert("Failed to load module" && false);
        return 0;
    }

    DecryptedExecutableHeader header{};
    memcpy(&header, loadResult.data(), std::min(sizeof(header), loadResult.size()));
    if (header.magic == DECRYPTED_EXECUTABLE_MAGIC) {
        LOG_UTILITY("Loading pre-decrypted executable");
        memcpy(g_memory.Translate(header.base), loadResult.data() + sizeof(DecryptedExecutableHeader),
               loadResult.size() - sizeof(DecryptedExecutableHeader));
        g_xdbfWrapper = XDBFWrapper(static_cast<uint8_t*>(g_memory.Translate(header.resource_offset)), header.resource_size);

        return header.entry_point;
    } else {
        LOG_UTILITY("Loading XEX executable");
        const auto image = Image::ParseImage(loadResult.data(), loadResult.size());

        memcpy(g_memory.Translate(image.base), image.data.get(), image.size);
        g_xdbfWrapper = XDBFWrapper(static_cast<uint8_t*>(g_memory.Translate(image.resource_offset)), image.resource_size);

        return image.entry_point;
    }
}

#ifdef __x86_64__
__attribute__((constructor(101), target("no-avx,no-avx2"), noinline))
void init()
{
    uint32_t eax, ebx, ecx, edx;

    // Execute CPUID for processor info and feature bits.
    __get_cpuid(1, &eax, &ebx, &ecx, &edx);

    // Check for AVX support.
    if ((ecx & (1 << 28)) == 0)
    {
        printf("[*] CPU does not support the AVX instruction set.\n");

#ifdef _WIN32
        MessageBoxA(nullptr, "Your CPU does not meet the minimum system requirements.", "Marathon Recompiled", MB_ICONERROR);
#endif

        std::_Exit(1);
    }
}
#endif

#include <MarathonCore.h>
#ifdef MARATHON_RECOMP_IOS
#include <os/ios/sandbox_ios.h>
#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#include <SDL3/SDL_main.h>
#endif

extern "C" MARATHON_CORE_API const char* MarathonGetDocumentsPath(void)
{
#ifdef MARATHON_RECOMP_IOS
    static std::string s_docsPath = os::ios::GetDocumentsPath().string();
#else
    static std::string s_docsPath = GetGamePath().string();
#endif
    return s_docsPath.c_str();
}

static std::string s_marathonLastError;

static void SetMarathonLastError(const std::string& message)
{
    s_marathonLastError = message;
}

extern "C" MARATHON_CORE_API const char* MarathonGetLastErrorMessage(void)
{
    return s_marathonLastError.c_str();
}

extern "C" MARATHON_CORE_API const char* MarathonGetApplicationSupportPath(void)
{
    static std::string s_supportPath = GetAppSupportPath().string();
    return s_supportPath.c_str();
}

extern "C" MARATHON_CORE_API bool MarathonIsGameInstalled(void)
{
    std::filesystem::path modulePath;
    return Installer::checkGameInstall(GetGamePath(), modulePath);
}

extern "C" MARATHON_CORE_API int MarathonMain(int argc, char** argv)
{
#ifdef _WIN32
    timeBeginPeriod(1);
#endif

    App::RefreshMainThreadId();

#ifdef MARATHON_RECOMP_IOS
    SDL_SetMainReady();
    os::ios::EnsureSandboxDirectories();
#endif

    os::process::CheckConsole();

    if (!os::registry::Init())
        LOGN_WARNING("OS does not support registry.");

    os::logger::Init();

    PreloadContext preloadContext;
    preloadContext.PreloadExecutable();

    bool forceInstaller = false;
    bool forceDLCInstaller = false;
    bool useDefaultWorkingDirectory = false;
    bool forceInstallationCheck = false;
    bool graphicsApiRetry = false;
    const char *sdlVideoDriver = nullptr;

    for (int i = 1; i < argc && argv != nullptr; i++)
    {
        if (argv[i] == nullptr)
            continue;

        forceInstaller = forceInstaller || (strcmp(argv[i], "--install") == 0);
        forceDLCInstaller = forceDLCInstaller || (strcmp(argv[i], "--install-dlc") == 0);
        useDefaultWorkingDirectory = useDefaultWorkingDirectory || (strcmp(argv[i], "--use-cwd") == 0);
        forceInstallationCheck = forceInstallationCheck || (strcmp(argv[i], "--install-check") == 0);
        graphicsApiRetry = graphicsApiRetry || (strcmp(argv[i], "--graphics-api-retry") == 0);
        App::s_isSkipLogos = App::s_isSkipLogos || (strcmp(argv[i], "--skip-logos") == 0);

        if (strcmp(argv[i], "--sdl-video-driver") == 0)
        {
            if ((i + 1) < argc && argv[i + 1] != nullptr)
                sdlVideoDriver = argv[++i];
            else
                LOGN_WARNING("No argument was specified for --sdl-video-driver. Option will be ignored.");
        }
    }

    if (!useDefaultWorkingDirectory)
    {
        std::error_code ec;
#ifdef MARATHON_RECOMP_IOS
        // On iOS, the .app bundle is read-only; default CWD to Sandbox Documents/iReCompiled06/.
        std::filesystem::current_path(os::ios::GetGameInstallPath(), ec);
#else
        // Set the current working directory to the executable's path.
        std::filesystem::current_path(os::process::GetExecutablePath().parent_path(), ec);
#endif
    }

    Config::Load();

    // TODO: Remove this when FPS change is supported.
    Config::FPS = 60;

    if (forceInstallationCheck)
    {
        // Create the console to show progress to the user, otherwise it will seem as if the game didn't boot at all.
        os::process::ShowConsole();
        App::CheckIntegrity(IntegrityCheckType::UserRequest);
    }

#if defined(_WIN32) && defined(MARATHON_RECOMP_D3D12)
    for (auto& dll : g_D3D12RequiredModules)
    {
        if (!std::filesystem::exists(g_executableRoot / dll))
        {
            char text[512];
            snprintf(text, sizeof(text), Localise("System_Win32_MissingDLLs").c_str(), dll.data());
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, GameWindow::GetTitle(), text, GameWindow::s_pWindow);
            std::_Exit(1);
        }
    }
#endif

    // Check the time since the last time an update was checked. Store the new time if the difference is more than six hours.
    constexpr double TimeBetweenUpdateChecksInSeconds = 6 * 60 * 60;
    time_t timeNow = std::time(nullptr);
    double timeDifferenceSeconds = difftime(timeNow, Config::LastChecked);
    if (timeDifferenceSeconds > TimeBetweenUpdateChecksInSeconds)
    {
        UpdateChecker::initialize();
        UpdateChecker::start();
        Config::LastChecked = timeNow;
        Config::Save();
    }

    if (Config::ShowConsole)
        os::process::ShowConsole();
    LOGN_WARNING("Host Startup");
    HostStartup();

    std::filesystem::path modulePath;
    bool isGameInstalled = Installer::checkGameInstall(GetGamePath(), modulePath);
    bool runInstallerWizard = forceInstaller || forceDLCInstaller || !isGameInstalled;
    if (runInstallerWizard)
    {
        if (!Video::CreateHostDevice(sdlVideoDriver, graphicsApiRetry))
        {
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, GameWindow::GetTitle(), Localise("Video_BackendError").c_str(), GameWindow::s_pWindow);
#if defined(MARATHON_RECOMP_IOS) || defined(MARATHON_CORE_LIBRARY)
            return 1;
#else
            std::_Exit(1);
#endif
        }

        ImFontAtlasSnapshot::Init(true);

        if (!InstallerWizard::Run(GetGamePath(), isGameInstalled && forceDLCInstaller))
        {
#if defined(MARATHON_RECOMP_IOS) || defined(MARATHON_CORE_LIBRARY)
            return 0;
#else
            std::_Exit(0);
#endif
        }
    }

    // ModLoader::Init();

    KiSystemStartup();

    uint32_t entry = LdrLoadModule(modulePath);

    if (!runInstallerWizard)
    {
        if (!Video::CreateHostDevice(sdlVideoDriver, graphicsApiRetry))
        {
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, GameWindow::GetTitle(), Localise("Video_BackendError").c_str(), GameWindow::s_pWindow);
#if defined(MARATHON_RECOMP_IOS) || defined(MARATHON_CORE_LIBRARY)
            return 1;
#else
            std::_Exit(1);
#endif
        }
    }

    LOGN_WARNING("Start Guest Thread");
    LOGN_WARNING(modulePath.string());

    GuestThread::Start({ entry, 0, 0 });

    if (GuestHasFatalError())
    {
        const std::string report = GuestGetFatalError();
        SetMarathonLastError(report);
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "iReCompiled06 - Guest Hatasi", report.c_str(), GameWindow::s_pWindow);
        return 3;
    }

    return 0;
}

#if !defined(MARATHON_RECOMP_IOS) && !defined(MARATHON_CORE_LIBRARY)
int main(int argc, char *argv[])
{
    return MarathonMain(argc, argv);
}
#endif

GUEST_FUNCTION_STUB(__imp__vsprintf);
GUEST_FUNCTION_STUB(__imp___vsnprintf);
GUEST_FUNCTION_STUB(__imp__sprintf);
GUEST_FUNCTION_STUB(__imp___snprintf);
GUEST_FUNCTION_STUB(__imp___snwprintf);
GUEST_FUNCTION_STUB(__imp__vswprintf);
GUEST_FUNCTION_STUB(__imp___vscwprintf);
GUEST_FUNCTION_STUB(__imp__swprintf);
