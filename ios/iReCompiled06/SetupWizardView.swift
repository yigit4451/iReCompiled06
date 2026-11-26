import Foundation
import SwiftUI
import UIKit
import UniformTypeIdentifiers

private final class ExtractionCallbackBox: @unchecked Sendable {
    let onProgress: (Float, UInt64, UInt64, String) -> Bool

    init(onProgress: @escaping (Float, UInt64, UInt64, String) -> Bool) {
        self.onProgress = onProgress
    }
}

private let marathonProgressTrampoline: MarathonExtractionProgressCallback = { progress, bytesDone, bytesTotal, statusPtr, userData in
    guard let userData else { return true }
    let box = Unmanaged<ExtractionCallbackBox>.fromOpaque(userData).takeUnretainedValue()
    let status = statusPtr.map { String(cString: $0) } ?? "Dosyalar çıkartılıyor..."
    return box.onProgress(progress, bytesDone, bytesTotal, status)
}

/// Streams a remote ISO/XEX/DLC file directly to `FileManager.default.temporaryDirectory` via `URLSessionDownloadTask`
/// without buffering the payload in RAM.
private final class RemoteArchiveDownloader: NSObject, URLSessionDownloadDelegate, @unchecked Sendable {
    private let lock = NSLock()
    private var session: URLSession?
    private var downloadTask: URLSessionDownloadTask?
    private var suggestedExtension: String = "iso"

    private var onProgressHandler: ((Double, UInt64, UInt64) -> Void)?
    private var onCompletionHandler: ((Result<URL, Error>) -> Void)?
    private var didFinish = false

    func startDownload(
        from rawURLString: String,
        preferredExtension: String = "iso",
        onProgress: @escaping (Double, UInt64, UInt64) -> Void,
        onCompletion: @escaping (Result<URL, Error>) -> Void
    ) {
        cancel()

        guard let normalizedURL = Self.normalizeDirectDownloadURL(from: rawURLString) else {
            onCompletion(.failure(NSError(
                domain: "iReCompiled06.Download",
                code: -1,
                userInfo: [NSLocalizedDescriptionKey: "Geçersiz HTTP/HTTPS indirme bağlantısı."]
            )))
            return
        }

        lock.lock()
        self.didFinish = false
        self.suggestedExtension = Self.inferExtension(from: normalizedURL, fallback: preferredExtension)
        self.onProgressHandler = onProgress
        self.onCompletionHandler = onCompletion

        let config = URLSessionConfiguration.default
        config.timeoutIntervalForRequest = 60
        config.timeoutIntervalForResource = 60 * 60 * 6
        config.waitsForConnectivity = true

        let newSession = URLSession(configuration: config, delegate: self, delegateQueue: nil)
        var request = URLRequest(url: normalizedURL)
        request.setValue("Mozilla/5.0 (iPhone; CPU iPhone OS 17_0 like Mac OS X) iReCompiled06/1.0", forHTTPHeaderField: "User-Agent")

        let task = newSession.downloadTask(with: request)
        self.session = newSession
        self.downloadTask = task
        lock.unlock()

        task.resume()
    }

    func cancel() {
        lock.lock()
        let task = downloadTask
        let activeSession = session
        downloadTask = nil
        session = nil
        onProgressHandler = nil
        onCompletionHandler = nil
        didFinish = true
        lock.unlock()

        task?.cancel()
        activeSession?.invalidateAndCancel()
    }

    func urlSession(
        _ session: URLSession,
        downloadTask: URLSessionDownloadTask,
        didWriteData bytesWritten: Int64,
        totalBytesWritten: Int64,
        totalBytesExpectedToWrite: Int64
    ) {
        lock.lock()
        let progressCallback = onProgressHandler
        lock.unlock()

        let written = UInt64(max(totalBytesWritten, 0))
        let expected = totalBytesExpectedToWrite > 0 ? UInt64(totalBytesExpectedToWrite) : 0
        let fraction: Double
        if expected > 0 {
            fraction = min(max(Double(written) / Double(expected), 0.0), 1.0)
        } else {
            fraction = 0.0
        }
        progressCallback?(fraction, written, expected)
    }

    func urlSession(
        _ session: URLSession,
        downloadTask: URLSessionDownloadTask,
        didFinishDownloadingTo location: URL
    ) {
        if let httpResponse = downloadTask.response as? HTTPURLResponse,
           !(200...299).contains(httpResponse.statusCode) {
            finishOnce(with: .failure(NSError(
                domain: "iReCompiled06.Download",
                code: httpResponse.statusCode,
                userInfo: [NSLocalizedDescriptionKey: "Sunucu HTTP \(httpResponse.statusCode) yanıtı döndürdü."]
            )))
            return
        }

        lock.lock()
        let ext = Self.inferExtension(from: downloadTask.response?.suggestedFilename, url: downloadTask.currentRequest?.url, fallback: suggestedExtension)
        lock.unlock()

        let tempDir = FileManager.default.temporaryDirectory
        let destURL = tempDir.appendingPathComponent("iReCompiled06-\(UUID().uuidString).\(ext)")

        do {
            try? FileManager.default.removeItem(at: destURL)
            try FileManager.default.moveItem(at: location, to: destURL)
            finishOnce(with: .success(destURL))
        } catch {
            finishOnce(with: .failure(error))
        }
    }

    func urlSession(
        _ session: URLSession,
        task: URLSessionTask,
        didCompleteWithError error: Error?
    ) {
        guard let error else { return }
        finishOnce(with: .failure(error))
    }

    private func finishOnce(with result: Result<URL, Error>) {
        lock.lock()
        guard !didFinish else {
            lock.unlock()
            return
        }
        didFinish = true
        let completion = onCompletionHandler
        let activeSession = session
        onProgressHandler = nil
        onCompletionHandler = nil
        downloadTask = nil
        session = nil
        lock.unlock()

        activeSession?.finishTasksAndInvalidate()
        completion?(result)
    }

    /// Converts share URLs from Google Drive, OneDrive, or Archive.org into direct download URLs when possible.
    static func normalizeDirectDownloadURL(from rawInput: String) -> URL? {
        let trimmed = rawInput.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return nil }

        // Google Drive `/file/d/<ID>/view` -> direct download endpoint
        if trimmed.contains("drive.google.com"),
           let range = trimmed.range(of: #"/file/d/([a-zA-Z0-9_-]+)"#, options: .regularExpression) {
            let match = String(trimmed[range])
            let fileID = match.replacingOccurrences(of: "/file/d/", with: "")
            if !fileID.isEmpty,
               let directURL = URL(string: "https://drive.usercontent.google.com/download?id=\(fileID)&export=download&confirm=t") {
                return directURL
            }
        }

        // OneDrive / Dropbox share links with dl=0 -> dl=1
        if (trimmed.contains("onedrive.live.com") || trimmed.contains("1drv.ms") || trimmed.contains("dropbox.com")),
           trimmed.contains("dl=0") {
            let direct = trimmed.replacingOccurrences(of: "dl=0", with: "dl=1")
            if let url = URL(string: direct) {
                return url
            }
        }

        if let url = URL(string: trimmed),
           let scheme = url.scheme?.lowercased(),
           scheme == "http" || scheme == "https" {
            return url
        }

        if let encoded = trimmed.addingPercentEncoding(withAllowedCharacters: .urlFragmentAllowed),
           let url = URL(string: encoded),
           let scheme = url.scheme?.lowercased(),
           scheme == "http" || scheme == "https" {
            return url
        }

        return nil
    }

    private static func inferExtension(from suggestedFilename: String? = nil, url: URL?, fallback: String) -> String {
        if let suggestedFilename {
            let ext = (suggestedFilename as NSString).pathExtension.lowercased()
            if ext == "iso" || ext == "xex" || ext == "7z" {
                return ext
            }
        }
        if let url {
            return inferExtension(from: url, fallback: fallback)
        }
        return fallback
    }

    private static func inferExtension(from url: URL, fallback: String) -> String {
        let pathExt = url.pathExtension.lowercased()
        if pathExt == "iso" || pathExt == "xex" || pathExt == "7z" {
            return pathExt
        }
        let lowerQuery = (url.query ?? "").lowercased()
        if lowerQuery.contains(".7z") {
            return "7z"
        }
        if lowerQuery.contains(".xex") {
            return "xex"
        }
        if lowerQuery.contains(".iso") {
            return "iso"
        }
        return fallback
    }
}

struct SetupWizardView: View {
    enum Step: Int, CaseIterable, Identifiable {
        case welcome = 0
        case sourceSelection = 1
        case extractionProgress = 2

        var id: Int { rawValue }

        var title: String {
            switch self {
            case .welcome:
                return "Hoş Geldiniz"
            case .sourceSelection:
                return "ISO/XEX Kaynak Seçimi"
            case .extractionProgress:
                return "Kurulum İlerlemesi"
            }
        }

        var iconName: String {
            switch self {
            case .welcome:
                return "sparkles"
            case .sourceSelection:
                return "externaldrive.fill.badge.icloud"
            case .extractionProgress:
                return "arrow.down.doc.fill"
            }
        }
    }

    enum PickerMode {
        case file
        case folder
        case dlc
    }

    enum SourceMode: Equatable {
        case localOrCloudFile
        case remoteURL
    }

    private static let minimumRecommendedFreeBytes: Int64 = 8 * 1024 * 1024 * 1024 // 8 GB

    let installDirectory: String
    let onInstallationCompleted: () -> Void

    @State private var currentStep: Step = .welcome
    @State private var isImporterPresented = false
    @State private var activePickerMode: PickerMode = .file
    @State private var sourceMode: SourceMode = .localOrCloudFile

    @State private var selectedSourceURL: URL?
    @State private var selectedSourceSummary: String = ""
    @State private var remoteDownloadURLText: String = ""
    @State private var isRemoteURLPanelExpanded: Bool = false

    @State private var selectedDLCURLs: [URL] = []
    @State private var skipHashChecks = false

    @State private var availableDiskBytes: Int64 = 0
    @State private var manualVerifyFeedback: String?
    @State private var isManualVerifySuccess: Bool = false

    @State private var isDownloadingRemote = false
    @State private var downloadProgressValue: Double = 0.0
    @State private var downloadBytesReceived: UInt64 = 0
    @State private var downloadBytesExpected: UInt64 = 0
    @State private var activeTemporaryDownloadURL: URL?
    @State private var remoteDownloader = RemoteArchiveDownloader()

    @State private var isExtracting = false
    @State private var extractionFinished = false
    @State private var progressValue: Double = 0.0
    @State private var bytesProcessed: UInt64 = 0
    @State private var bytesTotal: UInt64 = 0
    @State private var statusMessage: String = "Kurulum başlatılmaya hazır."
    @State private var errorMessage: String?

    private var isLowDiskSpace: Bool {
        availableDiskBytes > 0 && availableDiskBytes < Self.minimumRecommendedFreeBytes
    }

    private var canProceedFromSourceSelection: Bool {
        if sourceMode == .remoteURL {
            return RemoteArchiveDownloader.normalizeDirectDownloadURL(from: remoteDownloadURLText) != nil
        }
        return selectedSourceURL != nil || (!selectedDLCURLs.isEmpty && IsMarathonInstalled())
    }

    private var supportedImporterTypes: [UTType] {
        switch activePickerMode {
        case .folder:
            return [.folder]
        case .file, .dlc:
            var types: [UTType] = []
            if let isoType = UTType(filenameExtension: "iso") {
                types.append(isoType)
            }
            if let sevenZipType = UTType(filenameExtension: "7z") {
                types.append(sevenZipType)
            }
            if let xexType = UTType(filenameExtension: "xex") {
                types.append(xexType)
            }
            types.append(contentsOf: [.diskImage, .archive, .data, .item])
            return types
        }
    }

    var body: some View {
        ZStack {
            LinearGradient(
                colors: [
                    Color(red: 0.04, green: 0.07, blue: 0.14),
                    Color(red: 0.07, green: 0.13, blue: 0.25),
                    Color(red: 0.03, green: 0.05, blue: 0.10)
                ],
                startPoint: .topLeading,
                endPoint: .bottomTrailing
            )
            .ignoresSafeArea()

            VStack(spacing: 10) {
                wizardHeader

                ScrollView(.vertical, showsIndicators: false) {
                    Group {
                        switch currentStep {
                        case .welcome:
                            welcomeStepContent
                        case .sourceSelection:
                            sourceSelectionStepContent
                        case .extractionProgress:
                            extractionProgressStepContent
                        }
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
                }
                .frame(maxWidth: 780, maxHeight: .infinity)
                .padding(.horizontal, 18)
                .padding(.vertical, 14)
                .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 20, style: .continuous))
                .overlay(
                    RoundedRectangle(cornerRadius: 20, style: .continuous)
                        .strokeBorder(Color.white.opacity(0.14), lineWidth: 1)
                )

                wizardFooter
                    .frame(maxWidth: 780)
            }
            .padding(.horizontal, 20)
            .padding(.vertical, 10)
        }
        .fileImporter(
            isPresented: $isImporterPresented,
            allowedContentTypes: supportedImporterTypes,
            allowsMultipleSelection: activePickerMode == .dlc
        ) { result in
            handleFileImporterResult(result)
        }
        .onAppear {
            refreshAvailableStorage()
        }
    }

    private var wizardHeader: some View {
        HStack(spacing: 10) {
            ForEach(Step.allCases) { step in
                let isActive = step == currentStep
                let isCompleted = step.rawValue < currentStep.rawValue || (step == .extractionProgress && extractionFinished)

                HStack(spacing: 6) {
                    ZStack {
                        Circle()
                            .fill(isCompleted ? Color.green : (isActive ? Color.cyan : Color.white.opacity(0.14)))
                            .frame(width: 24, height: 24)

                        if isCompleted {
                            Image(systemName: "checkmark")
                                .font(.caption2.weight(.bold))
                                .foregroundStyle(.black)
                        } else {
                            Text("\(step.rawValue + 1)")
                                .font(.caption2.weight(.bold))
                                .foregroundStyle(isActive ? .black : .white.opacity(0.8))
                        }
                    }

                    Text(step.title)
                        .font(.footnote.weight(isActive ? .semibold : .regular))
                        .foregroundStyle(isActive ? .white : .white.opacity(0.65))
                        .lineLimit(1)
                }

                if step != Step.allCases.last {
                    Rectangle()
                        .fill(step.rawValue < currentStep.rawValue ? Color.cyan : Color.white.opacity(0.16))
                        .frame(height: 2)
                        .frame(maxWidth: 36)
                }
            }

            Spacer(minLength: 8)

            storageBadgeView
        }
        .frame(maxWidth: 780)
    }

    private var storageBadgeView: some View {
        Button {
            refreshAvailableStorage()
        } label: {
            HStack(spacing: 5) {
                Image(systemName: isLowDiskSpace ? "exclamationmark.triangle.fill" : "internaldrive.fill")
                    .font(.caption2)
                    .foregroundStyle(isLowDiskSpace ? .orange : .cyan)

                Text("Boş: \(formatSignedByteCount(availableDiskBytes))")
                    .font(.caption2.monospacedDigit().weight(.semibold))
                    .foregroundStyle(isLowDiskSpace ? .orange : .white.opacity(0.9))
            }
            .padding(.horizontal, 9)
            .padding(.vertical, 5)
            .background(
                (isLowDiskSpace ? Color.orange.opacity(0.20) : Color.white.opacity(0.08)),
                in: Capsule()
            )
        }
        .buttonStyle(.plain)
    }

    private var welcomeStepContent: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack(spacing: 12) {
                Image(systemName: "gamecontroller.fill")
                    .font(.system(size: 30))
                    .foregroundStyle(.cyan)

                VStack(alignment: .leading, spacing: 2) {
                    Text("iReCompiled06 Kurulum Sihirbazına Hoş Geldiniz")
                        .font(.title3.weight(.bold))
                        .foregroundStyle(.white)

                    Text("Sonic the Hedgehog (2006) — iOS ARM64 Statik Yeniden Derleme & Metal / MoltenVK")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }

            if isLowDiskSpace {
                lowStorageWarningBanner
            }

            Divider().overlay(Color.white.opacity(0.15))

            VStack(alignment: .leading, spacing: 9) {
                featureRow(
                    icon: "folder.fill.badge.gearshape",
                    title: "iOS Sandbox Kurulum Dizini",
                    detail: "Oyun arşivleri, DLC paketleri ve kayıtlar doğrudan Documents/iReCompiled06/ dizinine kurulur ve Dosyalar uygulamasından erişilebilir."
                )

                featureRow(
                    icon: "icloud.and.arrow.down.fill",
                    title: "Zero-Copy Dosya & Doğrudan URL Akışı",
                    detail: "iPhone'umda, iCloud Drive, Google Drive, OneDrive veya doğrudan HTTP/HTTPS ISO bağlantılarından RAM şişirmeden akışlı kurulum yapar."
                )

                featureRow(
                    icon: "memorychip.fill",
                    title: "Jetsam Bellek & GameController Uyumu",
                    detail: "4 GB – 6 GB RAM'li iPhone modelleri için optimize edilmiş doku havuzu ve MFi, DualSense, Xbox kablosuz kontrolcü desteği hazırdır."
                )
            }

            manualVerificationSection

            Text("Hedef Dizin: \(installDirectory)")
                .font(.caption2.monospaced())
                .foregroundStyle(.cyan.opacity(0.9))
                .padding(8)
                .frame(maxWidth: .infinity, alignment: .leading)
                .background(Color.black.opacity(0.32), in: RoundedRectangle(cornerRadius: 8))
        }
    }

    private var sourceSelectionStepContent: some View {
        VStack(alignment: .leading, spacing: 11) {
            HStack(alignment: .firstTextBaseline) {
                Text("ISO, XEX veya Ağ Kaynağını Seçin")
                    .font(.headline.weight(.bold))
                    .foregroundStyle(.white)

                Spacer()

                Text("Adım 2 / 3")
                    .font(.caption2.weight(.semibold))
                    .foregroundStyle(.cyan)
            }

            Text("Yerel/Bulut Dosyalar seçicisinden veya doğrudan HTTP/HTTPS bağlantısından Sonic '06 ISO/XEX kaynağını belirleyin. İsteğe bağlı olarak DLC / Title Update paketlerini de ekleyebilirsiniz.")
                .font(.caption)
                .foregroundStyle(.secondary)

            if isLowDiskSpace {
                lowStorageWarningBanner
            }

            // 3 Primary Source Options: (1) ISO/.7z/XEX File, (2) Extracted Folder, (3) URL / Network Download
            HStack(spacing: 10) {
                Button {
                    sourceMode = .localOrCloudFile
                    isRemoteURLPanelExpanded = false
                    activePickerMode = .file
                    isImporterPresented = true
                } label: {
                    Label("ISO / .7z / XEX Seç", systemImage: "doc.zipper")
                        .font(.footnote.weight(.semibold))
                        .frame(maxWidth: .infinity)
                        .padding(.vertical, 5)
                }
                .buttonStyle(.borderedProminent)
                .tint(sourceMode == .localOrCloudFile ? .cyan : .gray.opacity(0.5))

                Button {
                    sourceMode = .localOrCloudFile
                    isRemoteURLPanelExpanded = false
                    activePickerMode = .folder
                    isImporterPresented = true
                } label: {
                    Label("Çıkartılmış Klasör", systemImage: "folder.fill")
                        .font(.footnote.weight(.semibold))
                        .frame(maxWidth: .infinity)
                        .padding(.vertical, 5)
                }
                .buttonStyle(.bordered)

                Button {
                    withAnimation(.easeInOut(duration: 0.18)) {
                        sourceMode = .remoteURL
                        isRemoteURLPanelExpanded = true
                    }
                } label: {
                    Label("URL / Ağdan İndir", systemImage: "link.icloud.fill")
                        .font(.footnote.weight(.semibold))
                        .frame(maxWidth: .infinity)
                        .padding(.vertical, 5)
                }
                .buttonStyle(.borderedProminent)
                .tint(sourceMode == .remoteURL ? .indigo : .blue.opacity(0.45))
            }

            if sourceMode == .remoteURL || isRemoteURLPanelExpanded {
                remoteURLInputCard
            } else if let url = selectedSourceURL {
                selectedLocalSourceCard(for: url)
            } else {
                HStack(spacing: 8) {
                    Image(systemName: "info.circle")
                        .foregroundStyle(.cyan)
                    Text("Henüz bir ISO kalıbı, XEX dosyası, klasör veya indirme URL'si seçilmedi.")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                .padding(10)
                .frame(maxWidth: .infinity, alignment: .leading)
                .background(Color.white.opacity(0.05), in: RoundedRectangle(cornerRadius: 10))
            }

            dlcSelectionCard

            Toggle(isOn: $skipHashChecks) {
                VStack(alignment: .leading, spacing: 1) {
                    Text("Modlu ISO/XEX ve DLC için Hash Doğrulamasını Atla")
                        .font(.caption.weight(.medium))
                        .foregroundStyle(.white)
                    Text("Orijinal Xbox 360 perakende kopyaları için kapalı bırakılması önerilir.")
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                }
            }
            .tint(.cyan)

            manualVerificationSection

            if let errorMessage, currentStep == .sourceSelection {
                Text(errorMessage)
                    .font(.caption)
                    .foregroundStyle(.red)
            }
        }
    }

    private var remoteURLInputCard: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Label("Doğrudan ISO / XEX İndirme Bağlantısı (HTTP / HTTPS)", systemImage: "network")
                    .font(.caption.weight(.semibold))
                    .foregroundStyle(.cyan)

                Spacer()

                Button {
                    if let clip = UIPasteboard.general.string?.trimmingCharacters(in: .whitespacesAndNewlines),
                       !clip.isEmpty {
                        remoteDownloadURLText = clip
                        sourceMode = .remoteURL
                        errorMessage = nil
                    }
                } label: {
                    Label("Panodan Yapıştır", systemImage: "doc.on.clipboard")
                        .font(.caption2.weight(.semibold))
                }
                .buttonStyle(.bordered)
                .controlSize(.mini)
            }

            HStack(spacing: 8) {
                TextField("https://.../Sonic2006.iso (Google Drive, OneDrive, Archive.org)", text: $remoteDownloadURLText)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled(true)
                    .keyboardType(.URL)
                    .font(.caption.monospaced())
                    .padding(8)
                    .background(Color.black.opacity(0.35), in: RoundedRectangle(cornerRadius: 8))
                    .foregroundStyle(.white)

                if !remoteDownloadURLText.isEmpty {
                    Button {
                        remoteDownloadURLText = ""
                    } label: {
                        Image(systemName: "xmark.circle.fill")
                            .foregroundStyle(.secondary)
                    }
                    .buttonStyle(.plain)
                }
            }

            Text("URLSessionDownloadTask ile dosya RAM'e yüklenmeden doğrudan geçici diske (temporaryDirectory) indirilir, StartMarathonExtraction() ile ayıklanır ve işlem bitince geçici ISO otomatik silinir.")
                .font(.caption2)
                .foregroundStyle(.secondary)
        }
        .padding(11)
        .background(Color.indigo.opacity(0.16), in: RoundedRectangle(cornerRadius: 12))
        .overlay(
            RoundedRectangle(cornerRadius: 12)
                .strokeBorder(Color.cyan.opacity(0.35), lineWidth: 1)
        )
    }

    private func selectedLocalSourceCard(for url: URL) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                Image(systemName: "checkmark.seal.fill")
                    .foregroundStyle(.green)
                Text(url.lastPathComponent)
                    .font(.subheadline.weight(.semibold))
                    .foregroundStyle(.white)
                    .lineLimit(1)
                Spacer()
                Text(detectProviderBadge(for: url))
                    .font(.caption2.weight(.semibold))
                    .padding(.horizontal, 8)
                    .padding(.vertical, 3)
                    .background(Color.cyan.opacity(0.22), in: Capsule())
                    .foregroundStyle(.cyan)
            }

            if !selectedSourceSummary.isEmpty {
                Text(selectedSourceSummary)
                    .font(.caption2)
                    .foregroundStyle(.secondary)
            }
        }
        .padding(10)
        .background(Color.white.opacity(0.07), in: RoundedRectangle(cornerRadius: 10))
    }

    private var dlcSelectionCard: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack {
                Label("İsteğe Bağlı: DLC / Title Update Paketleri (\(selectedDLCURLs.count) seçili)", systemImage: "shippingbox.and.arrow.backward.fill")
                    .font(.caption.weight(.semibold))
                    .foregroundStyle(.white)

                Spacer()

                if !selectedDLCURLs.isEmpty {
                    Button("Temizle") {
                        selectedDLCURLs.removeAll()
                    }
                    .font(.caption2)
                    .buttonStyle(.plain)
                    .foregroundStyle(.orange)
                }

                Button {
                    activePickerMode = .dlc
                    isImporterPresented = true
                } label: {
                    Label("Çoklu DLC Seç", systemImage: "plus.circle.fill")
                        .font(.caption2.weight(.semibold))
                }
                .buttonStyle(.bordered)
                .controlSize(.small)
            }

            if !selectedDLCURLs.isEmpty {
                ScrollView(.horizontal, showsIndicators: false) {
                    HStack(spacing: 6) {
                        ForEach(selectedDLCURLs, id: \.path) { dlcURL in
                            HStack(spacing: 4) {
                                Image(systemName: "puzzlepiece.extension.fill")
                                    .font(.caption2)
                                    .foregroundStyle(.cyan)
                                Text(dlcURL.lastPathComponent)
                                    .font(.caption2)
                                    .foregroundStyle(.white)
                                    .lineLimit(1)
                            }
                            .padding(.horizontal, 8)
                            .padding(.vertical, 4)
                            .background(Color.cyan.opacity(0.16), in: Capsule())
                        }
                    }
                }
            } else {
                Text("Very Hard görev paketleri, Boss Attack bölümleri veya Team Attack Amigo XContent paketlerini tek seferde çoklu seçebilirsiniz.")
                    .font(.caption2)
                    .foregroundStyle(.secondary)
            }
        }
        .padding(10)
        .background(Color.white.opacity(0.05), in: RoundedRectangle(cornerRadius: 10))
    }

    private var manualVerificationSection: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack(spacing: 10) {
                VStack(alignment: .leading, spacing: 2) {
                    Text("Dosyaları Manuel mi Kopyaladınız?")
                        .font(.caption.weight(.semibold))
                        .foregroundStyle(.white)
                    Text("Oyun dosyalarını Dosyalar / iTunes ile Documents/iReCompiled06/ içine attıysanız kurulumu beklemeden doğrulayıp başlatın.")
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                }

                Spacer()

                Button {
                    verifyAndLaunchExistingInstall()
                } label: {
                    Label("Mevcut Kurulumu Doğrula ve Başlat", systemImage: "checkmark.shield.fill")
                        .font(.caption.weight(.semibold))
                }
                .buttonStyle(.borderedProminent)
                .tint(.green)
                .controlSize(.small)
            }

            if let manualVerifyFeedback {
                Text(manualVerifyFeedback)
                    .font(.caption2)
                    .foregroundStyle(isManualVerifySuccess ? .green : .orange)
            }
        }
        .padding(10)
        .background(Color.green.opacity(0.10), in: RoundedRectangle(cornerRadius: 10))
        .overlay(
            RoundedRectangle(cornerRadius: 10)
                .strokeBorder(Color.green.opacity(0.28), lineWidth: 1)
        )
    }

    private var lowStorageWarningBanner: some View {
        HStack(spacing: 10) {
            Image(systemName: "exclamationmark.triangle.fill")
                .font(.title3)
                .foregroundStyle(.orange)

            VStack(alignment: .leading, spacing: 2) {
                Text("Düşük Depolama Alanı Uyarısı (\(formatSignedByteCount(availableDiskBytes)) boş)")
                    .font(.caption.weight(.bold))
                    .foregroundStyle(.orange)
                Text("Sonic '06 ISO ayıklaması ve şifresi çözülmüş yürütülebilir dosyalar için en az 8 GB boş alan önerilir. İşlem sırasında disk dolarsa kurulum otomatik geri alınır.")
                    .font(.caption2)
                    .foregroundStyle(.white.opacity(0.9))
            }
        }
        .padding(10)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(Color.orange.opacity(0.18), in: RoundedRectangle(cornerRadius: 10))
        .overlay(
            RoundedRectangle(cornerRadius: 10)
                .strokeBorder(Color.orange.opacity(0.45), lineWidth: 1)
        )
    }

    private var extractionProgressStepContent: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack(spacing: 12) {
                if extractionFinished {
                    Image(systemName: "checkmark.circle.fill")
                        .font(.system(size: 30))
                        .foregroundStyle(.green)
                } else if errorMessage != nil {
                    Image(systemName: "xmark.octagon.fill")
                        .font(.system(size: 30))
                        .foregroundStyle(.red)
                } else {
                    ProgressView()
                        .controlSize(.regular)
                        .tint(.cyan)
                }

                VStack(alignment: .leading, spacing: 3) {
                    Text(progressHeaderTitle)
                        .font(.headline.weight(.bold))
                        .foregroundStyle(.white)

                    Text(statusMessage)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .lineLimit(2)
                }
            }

            if isDownloadingRemote {
                VStack(alignment: .leading, spacing: 6) {
                    HStack {
                        Label("Ağdan Geçici Diske İndiriliyor (URLSessionDownloadTask)", systemImage: "arrow.down.circle.fill")
                            .font(.caption.weight(.semibold))
                            .foregroundStyle(.indigo)
                        Spacer()
                        Text("\(Int((min(max(downloadProgressValue, 0.0), 1.0) * 100.0).rounded()))%")
                            .font(.caption.monospacedDigit().weight(.bold))
                            .foregroundStyle(.cyan)
                    }

                    ProgressView(value: min(max(downloadProgressValue, 0.0), 1.0), total: 1.0)
                        .progressViewStyle(.linear)
                        .tint(.indigo)
                        .scaleEffect(x: 1.0, y: 1.6, anchor: .center)

                    HStack {
                        Text(downloadBytesExpected > 0
                             ? "\(formatByteCount(downloadBytesReceived)) / \(formatByteCount(downloadBytesExpected))"
                             : "\(formatByteCount(downloadBytesReceived)) indirildi")
                            .font(.caption2.monospacedDigit())
                            .foregroundStyle(.secondary)
                        Spacer()
                        Text("RAM yerine temporaryDirectory'ye yazılıyor")
                            .font(.caption2)
                            .foregroundStyle(.secondary)
                    }
                }
                .padding(12)
                .background(Color.indigo.opacity(0.18), in: RoundedRectangle(cornerRadius: 12))
            }

            VStack(alignment: .leading, spacing: 6) {
                HStack {
                    Text("Kurulum & Ayıklama İlerlemesi")
                        .font(.caption.weight(.semibold))
                        .foregroundStyle(.white.opacity(0.85))
                    Spacer()
                    Text("\(Int((min(max(progressValue, 0.0), 1.0) * 100.0).rounded()))%")
                        .font(.caption.monospacedDigit().weight(.semibold))
                        .foregroundStyle(.cyan)
                }

                ProgressView(value: min(max(progressValue, 0.0), 1.0), total: 1.0)
                    .progressViewStyle(.linear)
                    .tint(extractionFinished ? .green : .cyan)
                    .scaleEffect(x: 1.0, y: 1.7, anchor: .center)

                if bytesTotal > 0 {
                    HStack {
                        Spacer()
                        Text("\(formatByteCount(bytesProcessed)) / \(formatByteCount(bytesTotal))")
                            .font(.caption2.monospacedDigit())
                            .foregroundStyle(.secondary)
                    }
                }
            }
            .padding(12)
            .background(Color.black.opacity(0.28), in: RoundedRectangle(cornerRadius: 12))

            if let errorMessage {
                VStack(alignment: .leading, spacing: 6) {
                    Label("Hata Ayrıntısı", systemImage: "exclamationmark.triangle.fill")
                        .font(.caption.weight(.semibold))
                        .foregroundStyle(.orange)

                    Text(errorMessage)
                        .font(.caption2)
                        .foregroundStyle(.white.opacity(0.9))
                }
                .padding(12)
                .frame(maxWidth: .infinity, alignment: .leading)
                .background(Color.red.opacity(0.18), in: RoundedRectangle(cornerRadius: 12))
            }
        }
    }

    private var progressHeaderTitle: String {
        if extractionFinished {
            return "Kurulum Tamamlandı"
        }
        if errorMessage != nil {
            return "Kurulum Başarısız"
        }
        if isDownloadingRemote {
            return "Uzaktaki ISO / XEX Kaynağı İndiriliyor"
        }
        return "Oyun Dosyaları Çıkartılıyor"
    }

    private var wizardFooter: some View {
        HStack {
            switch currentStep {
            case .welcome:
                Button {
                    verifyAndLaunchExistingInstall()
                } label: {
                    Label("Mevcut Kurulumu Doğrula ve Başlat", systemImage: "play.fill")
                        .font(.footnote.weight(.semibold))
                }
                .buttonStyle(.bordered)
                .tint(.green)

                Spacer()

                Button {
                    withAnimation(.easeInOut(duration: 0.2)) {
                        currentStep = .sourceSelection
                    }
                } label: {
                    Label("İleri", systemImage: "chevron.right")
                        .labelStyle(.titleAndIcon)
                        .padding(.horizontal, 12)
                }
                .buttonStyle(.borderedProminent)
                .tint(.cyan)

            case .sourceSelection:
                Button {
                    withAnimation(.easeInOut(duration: 0.2)) {
                        currentStep = .welcome
                    }
                } label: {
                    Label("Geri", systemImage: "chevron.left")
                        .font(.footnote)
                }
                .buttonStyle(.bordered)

                Spacer()

                Button {
                    withAnimation(.easeInOut(duration: 0.2)) {
                        currentStep = .extractionProgress
                    }
                    startInstallationFlow()
                } label: {
                    Label(sourceMode == .remoteURL ? "İndir ve Kurulumu Başlat" : "İleri — Kurulumu Başlat", systemImage: "play.fill")
                        .font(.footnote.weight(.semibold))
                        .padding(.horizontal, 10)
                }
                .buttonStyle(.borderedProminent)
                .tint(.cyan)
                .disabled(!canProceedFromSourceSelection)

            case .extractionProgress:
                if isExtracting || isDownloadingRemote {
                    Button(role: .destructive) {
                        statusMessage = "İptal ediliyor ve geçici/kısmi dosyalar temizleniyor..."
                        remoteDownloader.cancel()
                        CancelMarathonExtraction()
                        cleanupTemporaryDownloadIfNeeded()
                    } label: {
                        Label("İptal Et", systemImage: "xmark")
                            .font(.footnote)
                    }
                    .buttonStyle(.bordered)

                    Spacer()
                } else if extractionFinished {
                    Spacer()
                    Button {
                        onInstallationCompleted()
                    } label: {
                        Label("Oyunu Başlat (SDL + Metal)", systemImage: "play.circle.fill")
                            .font(.footnote.weight(.semibold))
                            .padding(.horizontal, 14)
                    }
                    .buttonStyle(.borderedProminent)
                    .tint(.green)
                } else {
                    Button {
                        errorMessage = nil
                        withAnimation(.easeInOut(duration: 0.2)) {
                            currentStep = .sourceSelection
                        }
                    } label: {
                        Label("Kaynak Seçimine Dön", systemImage: "chevron.left")
                            .font(.footnote)
                    }
                    .buttonStyle(.bordered)

                    Spacer()

                    Button {
                        startInstallationFlow()
                    } label: {
                        Label("Tekrar Dene", systemImage: "arrow.clockwise")
                            .font(.footnote.weight(.semibold))
                    }
                    .buttonStyle(.borderedProminent)
                    .tint(.cyan)
                }
            }
        }
    }

    private func featureRow(icon: String, title: String, detail: String) -> some View {
        HStack(alignment: .top, spacing: 10) {
            Image(systemName: icon)
                .font(.subheadline)
                .foregroundStyle(.cyan)
                .frame(width: 22)

            VStack(alignment: .leading, spacing: 2) {
                Text(title)
                    .font(.caption.weight(.semibold))
                    .foregroundStyle(.white)
                Text(detail)
                    .font(.caption2)
                    .foregroundStyle(.secondary)
            }
        }
    }

    private func refreshAvailableStorage() {
        let probeURL = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first
            ?? URL(fileURLWithPath: NSHomeDirectory())
        if let values = try? probeURL.resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey]),
           let available = values.volumeAvailableCapacityForImportantUsage {
            availableDiskBytes = available
        } else if let attrs = try? FileManager.default.attributesOfFileSystem(forPath: probeURL.path),
                  let freeSize = attrs[.systemFreeSize] as? NSNumber {
            availableDiskBytes = freeSize.int64Value
        }
    }

    private func verifyAndLaunchExistingInstall() {
        refreshAvailableStorage()
        var detailBuf = [CChar](repeating: 0, count: 512)
        let ok = VerifyMarathonInstallIntegrity(&detailBuf, Int32(detailBuf.count))
        let msg = Self.decodeCStringBuffer(detailBuf)

        if ok && IsMarathonInstalled() {
            isManualVerifySuccess = true
            manualVerifyFeedback = msg.isEmpty ? "Kurulum doğrulandı, motor başlatılıyor..." : msg
            onInstallationCompleted()
        } else {
            isManualVerifySuccess = false
            manualVerifyFeedback = msg.isEmpty
                ? "Documents/iReCompiled06 içinde geçerli bir oyun kurulumu bulunamadı."
                : msg
        }
    }

    private func handleFileImporterResult(_ result: Result<[URL], Error>) {
        switch result {
        case .success(let urls):
            guard !urls.isEmpty else { return }
            errorMessage = nil

            if activePickerMode == .dlc {
                for url in urls where !selectedDLCURLs.contains(where: { $0.path == url.path }) {
                    selectedDLCURLs.append(url)
                }
            } else if let firstURL = urls.first {
                sourceMode = .localOrCloudFile
                selectedSourceURL = firstURL
                selectedSourceSummary = "Seçilen kaynak kurulum sırasında NSFileCoordinator ile doğrulanıp doğrudan aktarılacak."
            }

        case .failure(let error):
            errorMessage = "Dosya seçici hatası: \(error.localizedDescription)"
        }
    }

    private func detectProviderBadge(for url: URL) -> String {
        let path = url.path.lowercased()
        if path.contains("mobile documents") || path.contains("icloud") {
            return "iCloud Drive"
        } else if path.contains("google") {
            return "Google Drive"
        } else if path.contains("onedrive") {
            return "OneDrive"
        } else if path.contains("file provider storage") {
            return "Bulut Sağlayıcısı"
        }
        return "iPhone'umda"
    }

    private func formatByteCount(_ bytes: UInt64) -> String {
        let formatter = ByteCountFormatter()
        formatter.allowedUnits = [.useMB, .useGB]
        formatter.countStyle = .file
        return formatter.string(fromByteCount: Int64(clamping: bytes))
    }

    private func formatSignedByteCount(_ bytes: Int64) -> String {
        let formatter = ByteCountFormatter()
        formatter.allowedUnits = [.useMB, .useGB]
        formatter.countStyle = .file
        return formatter.string(fromByteCount: max(bytes, 0))
    }

    private func cleanupTemporaryDownloadIfNeeded() {
        if let tempURL = activeTemporaryDownloadURL {
            try? FileManager.default.removeItem(at: tempURL)
            activeTemporaryDownloadURL = nil
        }
    }

    private func startInstallationFlow() {
        guard !isExtracting && !isDownloadingRemote else { return }
        refreshAvailableStorage()

        if sourceMode == .remoteURL {
            startRemoteDownloadThenExtract()
        } else {
            startCoordinatedExtraction(primaryURL: selectedSourceURL, isTemporaryFile: false)
        }
    }

    private func startRemoteDownloadThenExtract() {
        let urlString = remoteDownloadURLText.trimmingCharacters(in: .whitespacesAndNewlines)
        guard RemoteArchiveDownloader.normalizeDirectDownloadURL(from: urlString) != nil else {
            errorMessage = "Lütfen geçerli bir HTTP/HTTPS ISO veya XEX bağlantısı girin."
            return
        }

        isDownloadingRemote = true
        isExtracting = false
        extractionFinished = false
        errorMessage = nil
        downloadProgressValue = 0.0
        downloadBytesReceived = 0
        downloadBytesExpected = 0
        progressValue = 0.01
        bytesProcessed = 0
        bytesTotal = 0
        statusMessage = "Uzaktaki ISO/XEX kaynağı geçici diske indiriliyor..."

        remoteDownloader.startDownload(
            from: urlString,
            preferredExtension: "iso",
            onProgress: { fraction, written, expected in
                DispatchQueue.main.async {
                    self.downloadProgressValue = fraction
                    self.downloadBytesReceived = written
                    self.downloadBytesExpected = expected
                    self.progressValue = max(0.01, fraction * 0.45)
                    if expected > 0 {
                        self.statusMessage = "İndiriliyor: \(self.formatByteCount(written)) / \(self.formatByteCount(expected))"
                    } else {
                        self.statusMessage = "İndiriliyor: \(self.formatByteCount(written))"
                    }
                }
            },
            onCompletion: { result in
                DispatchQueue.main.async {
                    self.isDownloadingRemote = false
                    switch result {
                    case .success(let tempFileURL):
                        self.activeTemporaryDownloadURL = tempFileURL
                        self.downloadProgressValue = 1.0
                        self.statusMessage = "İndirme tamamlandı. StartMarathonExtraction() başlatılıyor..."
                        self.startCoordinatedExtraction(primaryURL: tempFileURL, isTemporaryFile: true)

                    case .failure(let error):
                        let nsErr = error as NSError
                        if nsErr.domain == NSURLErrorDomain && nsErr.code == NSURLErrorCancelled {
                            self.errorMessage = "İndirme kullanıcı tarafından iptal edildi."
                        } else {
                            self.errorMessage = "Ağdan indirme hatası: \(error.localizedDescription)"
                        }
                        self.statusMessage = "İndirme durduruldu."
                    }
                }
            }
        )
    }

    private func startCoordinatedExtraction(primaryURL: URL?, isTemporaryFile: Bool) {
        let dlcQueue = selectedDLCURLs
        guard primaryURL != nil || !dlcQueue.isEmpty else { return }

        isExtracting = true
        extractionFinished = false
        errorMessage = nil
        progressValue = isTemporaryFile ? 0.45 : 0.01
        bytesProcessed = 0
        bytesTotal = 0
        statusMessage = isTemporaryFile
            ? "İndirilen geçici arşiv Documents/iReCompiled06 dizinine çıkartılıyor..."
            : "Güvenlik kapsamlı kaynak bağlanıyor (NSFileCoordinator)..."

        let shouldSkipHashes = skipHashChecks

        DispatchQueue.global(qos: .userInitiated).async {
            defer {
                if isTemporaryFile, let primaryURL {
                    try? FileManager.default.removeItem(at: primaryURL)
                }
                DispatchQueue.main.async {
                    if isTemporaryFile {
                        self.activeTemporaryDownloadURL = nil
                    }
                    self.refreshAvailableStorage()
                }
            }

            var allItems: [(url: URL, needsCoordination: Bool)] = []
            if let primaryURL {
                allItems.append((url: primaryURL, needsCoordination: !isTemporaryFile))
            }
            for dlcURL in dlcQueue {
                allItems.append((url: dlcURL, needsCoordination: true))
            }

            let totalItems = max(allItems.count, 1)
            var overallSuccess = true
            var failureReason = ""

            for (index, item) in allItems.enumerated() {
                let itemBaseProgress = Double(index) / Double(totalItems)
                let itemSpan = 1.0 / Double(totalItems)

                let hasSecurityScope = item.needsCoordination ? item.url.startAccessingSecurityScopedResource() : false
                defer {
                    if hasSecurityScope {
                        item.url.stopAccessingSecurityScopedResource()
                    }
                }

                let callbackBox = ExtractionCallbackBox { progress, done, total, message in
                    DispatchQueue.main.async {
                        let scaled = itemBaseProgress + Double(progress) * itemSpan
                        self.progressValue = min(max(scaled, 0.01), 0.99)
                        self.bytesProcessed = done
                        self.bytesTotal = total
                        if totalItems > 1 {
                            self.statusMessage = "[\(index + 1)/\(totalItems)] \(message)"
                        } else {
                            self.statusMessage = message
                        }
                    }
                    return true
                }

                var itemSucceeded = false
                var itemErrorText = ""

                let extractBlock: (URL) -> Void = { accessibleURL in
                    var detailBuf = [CChar](repeating: 0, count: 512)
                    let kind = accessibleURL.path.withCString { cPath in
                        ValidateMarathonSource(cPath, &detailBuf, Int32(detailBuf.count))
                    }
                    let detailText = Self.decodeCStringBuffer(detailBuf)
                    if !detailText.isEmpty {
                        DispatchQueue.main.async {
                            if index == 0 {
                                self.selectedSourceSummary = detailText
                            }
                            self.statusMessage = detailText
                        }
                    }

                    if kind == MARATHON_SOURCE_INVALID {
                        itemErrorText = detailText.isEmpty
                            ? "Seçilen dosya (\(accessibleURL.lastPathComponent)) geçerli bir Sonic '06 ISO/XEX veya DLC kaynağı değil."
                            : detailText
                        itemSucceeded = false
                        return
                    }

                    var cErrorBuf = [CChar](repeating: 0, count: 1024)
                    let opaqueBox = Unmanaged.passUnretained(callbackBox).toOpaque()
                    itemSucceeded = accessibleURL.path.withCString { cPath in
                        StartMarathonExtraction(
                            cPath,
                            shouldSkipHashes,
                            marathonProgressTrampoline,
                            opaqueBox,
                            &cErrorBuf,
                            Int32(cErrorBuf.count)
                        )
                    }

                    if !itemSucceeded {
                        itemErrorText = Self.decodeCStringBuffer(cErrorBuf)
                    }
                }

                withExtendedLifetime(callbackBox) {
                    if item.needsCoordination {
                        var coordinatorError: NSError?
                        let coordinator = NSFileCoordinator(filePresenter: nil)
                        coordinator.coordinate(
                            readingItemAt: item.url,
                            options: [.withoutChanges],
                            error: &coordinatorError
                        ) { coordinatedURL in
                            extractBlock(coordinatedURL)
                        }
                        if let coordinatorError {
                            itemSucceeded = false
                            itemErrorText = "Bulut/Dosya koordinasyon hatası: \(coordinatorError.localizedDescription)"
                        }
                    } else {
                        extractBlock(item.url)
                    }
                }

                if !itemSucceeded {
                    overallSuccess = false
                    failureReason = itemErrorText
                    break
                }
            }

            DispatchQueue.main.async {
                self.isExtracting = false

                if overallSuccess && IsMarathonInstalled() {
                    self.progressValue = 1.0
                    self.extractionFinished = true
                    self.statusMessage = isTemporaryFile
                        ? "Documents/iReCompiled06 kurulumu tamamlandı ve geçici ISO dosyası silindi!"
                        : "Documents/iReCompiled06 dizinine kurulum tamamlandı!"
                } else {
                    self.errorMessage = failureReason.isEmpty
                        ? "Kurulum tamamlanamadı veya eksik oyun dosyaları tespit edildi."
                        : failureReason
                    self.statusMessage = "Kurulum durduruldu."
                }
            }
        }
    }

    private static func decodeCStringBuffer(_ buffer: [CChar]) -> String {
        let prefixBytes = buffer.prefix { $0 != 0 }.map { UInt8(bitPattern: $0) }
        return String(decoding: prefixBytes, as: UTF8.self)
    }
}
