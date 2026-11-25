import Observation
import SwiftUI
import UIKit

final class iReCompiled06AppDelegate: NSObject, UIApplicationDelegate {
    func application(
        _ application: UIApplication,
        supportedInterfaceOrientationsFor window: UIWindow?
    ) -> UIInterfaceOrientationMask {
        return .landscape
    }
}

@Observable
@MainActor
final class MarathonLaunchController {
    enum Phase: Equatable {
        case checkingSandbox
        case setupWizard
        case runningEngine
        case engineExited(Int32)
    }

    var phase: Phase = .checkingSandbox
    private var didStartEngine = false

    var installDirectoryPath: String {
        guard let rawPtr = GetMarathonInstallDirectory() else {
            return "Documents/iReCompiled06"
        }
        return String(cString: rawPtr)
    }

    func bootstrapOnLaunch() {
        guard !didStartEngine else { return }

        if IsMarathonInstalled() {
            launchMarathonOnMainThread()
        } else {
            phase = .setupWizard
        }
    }

    func launchMarathonOnMainThread() {
        guard !didStartEngine else { return }
        didStartEngine = true
        phase = .runningEngine

        UIApplication.shared.isIdleTimerDisabled = true

        // Dispatch onto the next Main RunLoop turn so the active UIWindowScene is ready
        // before SDL3 + Metal/MoltenVK creates and promotes its fullscreen window.
        DispatchQueue.main.async { [weak self] in
            precondition(Thread.isMainThread, "MarathonMain() must be invoked on the Main Thread.")
            let exitCode = Self.invokeMarathonMain(arguments: CommandLine.arguments)
            UIApplication.shared.isIdleTimerDisabled = false
            if exitCode != 0 {
                self?.didStartEngine = false
                self?.phase = .engineExited(exitCode)
            }
        }
    }

    private static func invokeMarathonMain(arguments: [String]) -> Int32 {
        let safeArgs = arguments.isEmpty ? ["iReCompiled06"] : arguments
        var cStrings: [UnsafeMutablePointer<CChar>?] = safeArgs.map { strdup($0) }
        cStrings.append(nil)
        defer {
            for ptr in cStrings where ptr != nil {
                free(ptr)
            }
        }

        return cStrings.withUnsafeMutableBufferPointer { buffer in
            MarathonMain(Int32(safeArgs.count), buffer.baseAddress)
        }
    }
}

@main
struct iReCompiled06App: App {
    @UIApplicationDelegateAdaptor(iReCompiled06AppDelegate.self) private var appDelegate
    @State private var launchController = MarathonLaunchController()

    var body: some Scene {
        WindowGroup {
            ZStack {
                Color.black.ignoresSafeArea()

                switch launchController.phase {
                case .checkingSandbox:
                    EngineTransitionPlaceholderView(
                        title: "iReCompiled06 Hazırlanıyor",
                        subtitle: "Documents/iReCompiled06 dizini denetleniyor..."
                    )

                case .setupWizard:
                    SetupWizardView(
                        installDirectory: launchController.installDirectoryPath,
                        onInstallationCompleted: {
                            launchController.launchMarathonOnMainThread()
                        }
                    )

                case .runningEngine:
                    EngineTransitionPlaceholderView(
                        title: "MarathonCore Başlatılıyor",
                        subtitle: "SDL3 + Metal / MoltenVK tam ekran penceresine geçiliyor..."
                    )

                case .engineExited(let code):
                    EngineErrorRecoveryView(
                        exitCode: code,
                        onReopenWizard: {
                            launchController.phase = .setupWizard
                        },
                        onRetryLaunch: {
                            launchController.launchMarathonOnMainThread()
                        }
                    )
                }
            }
            .preferredColorScheme(.dark)
            .statusBarHidden(true)
            .persistentSystemOverlays(.hidden)
            .task {
                launchController.bootstrapOnLaunch()
            }
        }
    }
}

private struct EngineTransitionPlaceholderView: View {
    let title: String
    let subtitle: String

    var body: some View {
        VStack(spacing: 16) {
            ProgressView()
                .controlSize(.large)
                .tint(.cyan)

            Text(title)
                .font(.title3.weight(.semibold))
                .foregroundStyle(.white)

            Text(subtitle)
                .font(.subheadline)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
        }
        .padding(32)
    }
}

private struct EngineErrorRecoveryView: View {
    let exitCode: Int32
    let onReopenWizard: () -> Void
    let onRetryLaunch: () -> Void

    var body: some View {
        VStack(spacing: 20) {
            Image(systemName: "exclamationmark.triangle.fill")
                .font(.system(size: 44))
                .foregroundStyle(.orange)

            Text("Oyun Motoru Sonlandı (Kod: \(exitCode))")
                .font(.title2.weight(.bold))
                .foregroundStyle(.white)

            Text("Documents/iReCompiled06 içindeki oyun dosyalarını doğrulayabilir veya kurulum sihirbazını yeniden çalıştırabilirsiniz.")
                .font(.subheadline)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
                .frame(maxWidth: 480)

            HStack(spacing: 14) {
                Button("Kurulum Sihirbazını Aç", action: onReopenWizard)
                    .buttonStyle(.bordered)

                Button("Yeniden Başlat", action: onRetryLaunch)
                    .buttonStyle(.borderedProminent)
                    .tint(.cyan)
            }
        }
        .padding(32)
    }
}
