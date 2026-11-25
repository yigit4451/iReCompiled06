# Building `MarathonCore` for iOS (ARM64 + Metal / MoltenVK) & SwiftUI Integration

## 1. Build Native macOS Host Tools First

Because `MarathonRecomp` runs code-generation tools (`file_to_c`, `XenonRecomp`, `u8extract`, `XenosRecomp`) during compilation, build the host tools once on macOS before cross-compiling for iOS:

```bash
cmake . --preset ios-host-tools
cmake --build ./out/build/ios-host-tools
```

## 2. Configure & Build `MarathonCore` for iOS (`arm64`)

### Static Library (`libMarathonCore.a` — Default)

```bash
cmake . --preset ios-release \
  -DMARATHON_IOS_MOLTENVK=/path/to/MoltenVK.xcframework/ios-arm64/libMoltenVK.a \
  -DMARATHON_IOS_FFMPEG_ROOT=/path/to/ffmpeg-ios-arm64
cmake --build ./out/build/ios-release --target MarathonCore
```

### Dynamic Library / `.framework` (`MarathonCore.framework` — Optional)

```bash
cmake . --preset ios-release \
  -DMARATHON_CORE_SHARED=ON \
  -DMARATHON_CORE_FRAMEWORK=ON \
  -DMARATHON_IOS_MOLTENVK=/path/to/MoltenVK.xcframework/ios-arm64/MoltenVK.framework \
  -DMARATHON_IOS_FFMPEG_ROOT=/path/to/ffmpeg-ios-arm64
cmake --build ./out/build/ios-release --target MarathonCore
```

> **Note:** Existing macOS presets (`macos-debug`, `macos-relwithdebinfo`, `macos-release`) remain unchanged and continue building the `Marathon Recompiled.app` bundle executable (`MarathonRecomp`).

## 3. Unified `iReCompiled06` SwiftUI App Target (iOS 17+)

The repository ships a complete iOS 17+ SwiftUI application shell under `ios/iReCompiled06/` (`Bundle ID: com.yigit4451.irecompiled06`):

- `ios/iReCompiled06/iReCompiled06App.swift`: Checks `IsMarathonInstalled()` (`Documents/iReCompiled06/`) at launch; if installed, invokes `MarathonMain()` on the **Main Thread** to enter the fullscreen SDL3 + Metal / MoltenVK window; otherwise presents the 3-step `SetupWizardView`.
- `ios/iReCompiled06/SetupWizardView.swift`: 3-step (*"Hoş Geldiniz"* -> *"ISO/XEX Kaynak Seçimi"* -> *"Kurulum İlerlemesi"*) wizard using `.fileImporter` + `NSFileCoordinator` to stream ISO/XEX archives directly from **iPhone'umda**, **iCloud Drive**, **Google Drive**, or **OneDrive** into `StartMarathonExtraction()` without copying source archives.
- `ios/iReCompiled06/Bridging-Header.h`: Bridges `MarathonCore.h` and `MarathonInstallerBridge.h` to Swift.

To build an unsigned `iReCompiled06.ipa` locally on macOS:

```bash
cmake -S . -B ./out/build/ios-xcode \
  -G Xcode \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_SYSROOT=iphoneos \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 \
  -DMARATHON_BUILD_IOS_APP=ON \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/thirdparty/vcpkg/scripts/buildsystems/vcpkg.cmake" \
  -DVCPKG_TARGET_TRIPLET=arm64-ios \
  -DMARATHON_HOST_TOOLS_DIR="$PWD/out/build/ios-host-tools" \
  -DMARATHON_IOS_FFMPEG_ROOT=/path/to/ffmpeg-ios-arm64 \
  -DMARATHON_IOS_MOLTENVK=/path/to/MoltenVK.framework \
  -DMARATHON_IOS_MOLTENVK_FRAMEWORK=/path/to/MoltenVK.framework

xcodebuild -project ./out/build/ios-xcode/MarathonRecomp-ALL.xcodeproj \
  -target iReCompiled06 -configuration Release -sdk iphoneos \
  ARCHS=arm64 CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO CODE_SIGN_IDENTITY=""
```

## 4. iOS Sandbox Directory Layout

| Sandbox Path | Visibility | Contents |
| :--- | :--- | :--- |
| `<Sandbox>/Documents/iReCompiled06/` | Visible in iOS **Files** app (`UIFileSharingEnabled`) | `game/`, `decrypted/`, `dlc/`, `mods/`, `cpkredir.ini`, `save/`, `mlsave/`, `config.toml` |
| `<Sandbox>/Library/Application Support/iReCompiled06/` | Internal (excluded from iCloud backup) | `pso_caching.txt`, `marathon.log`, `registry.ini`, `cache/` |

> **Optional Entitlements (Paid Apple Developer Accounts):** `MarathonCore` reserves its 4 GiB guest space sparsely via `PROT_NONE` so it runs even on unentitled 4 GB – 6 GB devices, but adding `com.apple.developer.kernel.increased-memory-limit` and `com.apple.developer.kernel.extended-virtual-addressing` in your `.entitlements` raises the iOS Jetsam ceiling further.

## 5. Automatic iOS Jetsam Memory Budgets (`os/ios/memory_ios.mm`)

| Device RAM Tier | Guest User + Physical Heaps | Texture Cache Cap | Upload Ring | Internal Render Cap | PSO Compiler Threads |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **4 GB** (`Low4GB`, ~1.85 GiB Jetsam) | `384 MiB` + `384 MiB` | `384 MiB` (auto mip-clamp) | `4 MiB` × 2 | `720p` (MSAA Off) | `1` |
| **6 GB** (`Mid6GB`, ~2.85 GiB Jetsam) | `512 MiB` + `512 MiB` | `640 MiB` | `8 MiB` × 2 | `900p` (MSAA 2x) | `2` |
| **8 GB+** (`High8GBPlus`) | `768 MiB` + `768 MiB` | `1024 MiB` | `8 MiB` × 3 | `1080p` | `3` |

