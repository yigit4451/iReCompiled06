import Foundation
import SwiftUI
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
    }

    let installDirectory: String
    let onInstallationCompleted: () -> Void

    @State private var currentStep: Step = .welcome
    @State private var isImporterPresented = false
    @State private var activePickerMode: PickerMode = .file

    @State private var selectedSourceURL: URL?
    @State private var selectedSourceSummary: String = ""
    @State private var skipHashChecks = false

    @State private var isExtracting = false
    @State private var extractionFinished = false
    @State private var progressValue: Double = 0.0
    @State private var bytesProcessed: UInt64 = 0
    @State private var bytesTotal: UInt64 = 0
    @State private var statusMessage: String = "Kurulum başlatılmaya hazır."
    @State private var errorMessage: String?

    private var supportedImporterTypes: [UTType] {
        switch activePickerMode {
        case .folder:
            return [.folder]
        case .file:
            var types: [UTType] = []
            if let isoType = UTType(filenameExtension: "iso") {
                types.append(isoType)
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

            VStack(spacing: 20) {
                wizardHeader

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
                .frame(maxWidth: 760, maxHeight: .infinity)
                .padding(24)
                .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 24, style: .continuous))
                .overlay(
                    RoundedRectangle(cornerRadius: 24, style: .continuous)
                        .strokeBorder(Color.white.opacity(0.14), lineWidth: 1)
                )

                wizardFooter
                    .frame(maxWidth: 760)
            }
            .padding(.horizontal, 24)
            .padding(.vertical, 18)
        }
        .fileImporter(
            isPresented: $isImporterPresented,
            allowedContentTypes: supportedImporterTypes,
            allowsMultipleSelection: false
        ) { result in
            handleFileImporterResult(result)
        }
    }

    private var wizardHeader: some View {
        HStack(spacing: 12) {
            ForEach(Step.allCases) { step in
                let isActive = step == currentStep
                let isCompleted = step.rawValue < currentStep.rawValue || (step == .extractionProgress && extractionFinished)

                HStack(spacing: 8) {
                    ZStack {
                        Circle()
                            .fill(isCompleted ? Color.green : (isActive ? Color.cyan : Color.white.opacity(0.14)))
                            .frame(width: 28, height: 28)

                        if isCompleted {
                            Image(systemName: "checkmark")
                                .font(.caption.weight(.bold))
                                .foregroundStyle(.black)
                        } else {
                            Text("\(step.rawValue + 1)")
                                .font(.caption.weight(.bold))
                                .foregroundStyle(isActive ? .black : .white.opacity(0.8))
                        }
                    }

                    Text(step.title)
                        .font(.subheadline.weight(isActive ? .semibold : .regular))
                        .foregroundStyle(isActive ? .white : .white.opacity(0.65))
                        .lineLimit(1)
                }

                if step != Step.allCases.last {
                    Rectangle()
                        .fill(step.rawValue < currentStep.rawValue ? Color.cyan : Color.white.opacity(0.16))
                        .frame(height: 2)
                        .frame(maxWidth: 48)
                }
            }
        }
        .frame(maxWidth: 760)
    }

    private var welcomeStepContent: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 18) {
                HStack(spacing: 14) {
                    Image(systemName: "gamecontroller.fill")
                        .font(.system(size: 34))
                        .foregroundStyle(.cyan)

                    VStack(alignment: .leading, spacing: 4) {
                        Text("iReCompiled06 Kurulum Sihirbazına Hoş Geldiniz")
                            .font(.title2.weight(.bold))
                            .foregroundStyle(.white)

                        Text("Sonic the Hedgehog (2006) — iOS ARM64 Statik Yeniden Derleme & Metal / MoltenVK")
                            .font(.subheadline)
                            .foregroundStyle(.secondary)
                    }
                }

                Divider().overlay(Color.white.opacity(0.15))

                VStack(alignment: .leading, spacing: 12) {
                    featureRow(
                        icon: "folder.fill.badge.gearshape",
                        title: "iOS Sandbox Kurulum Dizini",
                        detail: "Oyun arşivleri, DLC paketleri ve kayıtlar doğrudan Documents/iReCompiled06/ dizinine kurulur ve Dosyalar uygulamasından erişilebilir."
                    )

                    featureRow(
                        icon: "icloud.and.arrow.down.fill",
                        title: "Kopyalamadan Doğrudan Akış (Zero-Copy)",
                        detail: "NSFileCoordinator aracılığıyla iPhone'umda, iCloud Drive, Google Drive ve OneDrive üzerindeki büyük ISO/XEX dosyaları geçici kopya oluşturulmadan doğrudan çıkartılır."
                    )

                    featureRow(
                        icon: "memorychip.fill",
                        title: "Jetsam Bellek & GameController Uyumu",
                        detail: "4 GB – 6 GB RAM'li iPhone modelleri için optimize edilmiş doku havuzu ve MFi, DualSense, Xbox kablosuz kontrolcü desteği hazırdır."
                    )
                }

                Text("Hedef Dizin: \(installDirectory)")
                    .font(.caption.monospaced())
                    .foregroundStyle(.cyan.opacity(0.9))
                    .padding(10)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .background(Color.black.opacity(0.32), in: RoundedRectangle(cornerRadius: 10))
            }
        }
    }

    private var sourceSelectionStepContent: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 18) {
                Text("ISO veya XEX Kaynağını Seçin")
                    .font(.title3.weight(.bold))
                    .foregroundStyle(.white)

                Text("Dosyalar seçicisinden yerel depolamanızdaki (\"iPhone'umda\") veya bulut sağlayıcılarındaki (iCloud Drive, Google Drive, OneDrive) Xbox 360 Sonic '06 ISO kalıbını, çıkartılmış oyun klasörünü ya da default.xex dosyasını seçin.")
                    .font(.subheadline)
                    .foregroundStyle(.secondary)

                HStack(spacing: 14) {
                    Button {
                        activePickerMode = .file
                        isImporterPresented = true
                    } label: {
                        Label("ISO / XEX Dosyası Seç", systemImage: "doc.zipper")
                            .frame(maxWidth: .infinity)
                            .padding(.vertical, 6)
                    }
                    .buttonStyle(.borderedProminent)
                    .tint(.cyan)

                    Button {
                        activePickerMode = .folder
                        isImporterPresented = true
                    } label: {
                        Label("Çıkartılmış Klasör Seç", systemImage: "folder.fill")
                            .frame(maxWidth: .infinity)
                            .padding(.vertical, 6)
                    }
                    .buttonStyle(.bordered)
                }

                if let url = selectedSourceURL {
                    VStack(alignment: .leading, spacing: 6) {
                        HStack {
                            Image(systemName: "checkmark.seal.fill")
                                .foregroundStyle(.green)
                            Text(url.lastPathComponent)
                                .font(.headline)
                                .foregroundStyle(.white)
                                .lineLimit(1)
                            Spacer()
                            Text(detectProviderBadge(for: url))
                                .font(.caption.weight(.semibold))
                                .padding(.horizontal, 10)
                                .padding(.vertical, 4)
                                .background(Color.cyan.opacity(0.22), in: Capsule())
                                .foregroundStyle(.cyan)
                        }

                        if !selectedSourceSummary.isEmpty {
                            Text(selectedSourceSummary)
                                .font(.caption)
                                .foregroundStyle(.secondary)
                        }
                    }
                    .padding(14)
                    .background(Color.white.opacity(0.07), in: RoundedRectangle(cornerRadius: 14))
                } else {
                    HStack(spacing: 10) {
                        Image(systemName: "info.circle")
                            .foregroundStyle(.cyan)
                        Text("Henüz bir ISO kalıbı, XEX dosyası veya oyun klasörü seçilmedi.")
                            .font(.subheadline)
                            .foregroundStyle(.secondary)
                    }
                    .padding(14)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .background(Color.white.opacity(0.05), in: RoundedRectangle(cornerRadius: 14))
                }

                Toggle(isOn: $skipHashChecks) {
                    VStack(alignment: .leading, spacing: 2) {
                        Text("Modlu ISO/XEX için Hash Doğrulamasını Atla")
                            .font(.subheadline.weight(.medium))
                            .foregroundStyle(.white)
                        Text("Orijinal Xbox 360 perakende kopyaları için kapalı bırakılması önerilir.")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                }
                .tint(.cyan)

                if let errorMessage, currentStep == .sourceSelection {
                    Text(errorMessage)
                        .font(.footnote)
                        .foregroundStyle(.red)
                }
            }
        }
    }

    private var extractionProgressStepContent: some View {
        VStack(alignment: .leading, spacing: 20) {
            HStack(spacing: 12) {
                if extractionFinished {
                    Image(systemName: "checkmark.circle.fill")
                        .font(.system(size: 32))
                        .foregroundStyle(.green)
                } else if errorMessage != nil {
                    Image(systemName: "xmark.octagon.fill")
                        .font(.system(size: 32))
                        .foregroundStyle(.red)
                } else {
                    ProgressView()
                        .controlSize(.regular)
                        .tint(.cyan)
                }

                VStack(alignment: .leading, spacing: 4) {
                    Text(extractionFinished ? "Kurulum Tamamlandı" : (errorMessage != nil ? "Kurulum Başarısız" : "Oyun Dosyaları Çıkartılıyor"))
                        .font(.title3.weight(.bold))
                        .foregroundStyle(.white)

                    Text(statusMessage)
                        .font(.subheadline)
                        .foregroundStyle(.secondary)
                        .lineLimit(2)
                }
            }

            VStack(alignment: .leading, spacing: 8) {
                ProgressView(value: min(max(progressValue, 0.0), 1.0), total: 1.0)
                    .progressViewStyle(.linear)
                    .tint(extractionFinished ? .green : .cyan)
                    .scaleEffect(x: 1.0, y: 1.8, anchor: .center)

                HStack {
                    Text("\(Int((min(max(progressValue, 0.0), 1.0) * 100.0).rounded()))%")
                        .font(.subheadline.monospacedDigit().weight(.semibold))
                        .foregroundStyle(.cyan)

                    Spacer()

                    if bytesTotal > 0 {
                        Text("\(formatByteCount(bytesProcessed)) / \(formatByteCount(bytesTotal))")
                            .font(.caption.monospacedDigit())
                            .foregroundStyle(.secondary)
                    }
                }
            }
            .padding(16)
            .background(Color.black.opacity(0.28), in: RoundedRectangle(cornerRadius: 14))

            if let errorMessage {
                VStack(alignment: .leading, spacing: 8) {
                    Label("Hata Ayrıntısı", systemImage: "exclamationmark.triangle.fill")
                        .font(.subheadline.weight(.semibold))
                        .foregroundStyle(.orange)

                    Text(errorMessage)
                        .font(.footnote)
                        .foregroundStyle(.white.opacity(0.9))
                }
                .padding(14)
                .frame(maxWidth: .infinity, alignment: .leading)
                .background(Color.red.opacity(0.18), in: RoundedRectangle(cornerRadius: 14))
            }

            Spacer(minLength: 0)
        }
    }

    private var wizardFooter: some View {
        HStack {
            switch currentStep {
            case .welcome:
                Spacer()
                Button {
                    withAnimation(.easeInOut(duration: 0.2)) {
                        currentStep = .sourceSelection
                    }
                } label: {
                    Label("İleri", systemImage: "chevron.right")
                        .labelStyle(.titleAndIcon)
                        .padding(.horizontal, 14)
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
                }
                .buttonStyle(.bordered)

                Spacer()

                Button {
                    withAnimation(.easeInOut(duration: 0.2)) {
                        currentStep = .extractionProgress
                    }
                    startCoordinatedExtraction()
                } label: {
                    Label("İleri — Kurulumu Başlat", systemImage: "play.fill")
                        .padding(.horizontal, 12)
                }
                .buttonStyle(.borderedProminent)
                .tint(.cyan)
                .disabled(selectedSourceURL == nil)

            case .extractionProgress:
                if isExtracting {
                    Button(role: .destructive) {
                        statusMessage = "İptal ediliyor ve kısmi dosyalar temizleniyor..."
                        CancelMarathonExtraction()
                    } label: {
                        Label("İptal Et", systemImage: "xmark")
                    }
                    .buttonStyle(.bordered)

                    Spacer()
                } else if extractionFinished {
                    Spacer()
                    Button {
                        onInstallationCompleted()
                    } label: {
                        Label("Oyunu Başlat (SDL + Metal)", systemImage: "play.circle.fill")
                            .padding(.horizontal, 16)
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
                    }
                    .buttonStyle(.bordered)

                    Spacer()

                    Button {
                        startCoordinatedExtraction()
                    } label: {
                        Label("Tekrar Dene", systemImage: "arrow.clockwise")
                    }
                    .buttonStyle(.borderedProminent)
                    .tint(.cyan)
                }
            }
        }
    }

    private func featureRow(icon: String, title: String, detail: String) -> some View {
        HStack(alignment: .top, spacing: 12) {
            Image(systemName: icon)
                .font(.title3)
                .foregroundStyle(.cyan)
                .frame(width: 28)

            VStack(alignment: .leading, spacing: 3) {
                Text(title)
                    .font(.subheadline.weight(.semibold))
                    .foregroundStyle(.white)
                Text(detail)
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
    }

    private func handleFileImporterResult(_ result: Result<[URL], Error>) {
        switch result {
        case .success(let urls):
            guard let url = urls.first else { return }
            errorMessage = nil
            selectedSourceURL = url
            selectedSourceSummary = "Seçilen kaynak kurulum sırasında NSFileCoordinator ile doğrulanıp doğrudan aktarılacak."

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

    private func startCoordinatedExtraction() {
        guard let sourceURL = selectedSourceURL, !isExtracting else { return }

        isExtracting = true
        extractionFinished = false
        errorMessage = nil
        progressValue = 0.01
        bytesProcessed = 0
        bytesTotal = 0
        statusMessage = "Güvenlik kapsamlı kaynak bağlanıyor (NSFileCoordinator)..."

        let shouldSkipHashes = skipHashChecks

        DispatchQueue.global(qos: .userInitiated).async {
            let hasSecurityScope = sourceURL.startAccessingSecurityScopedResource()
            defer {
                if hasSecurityScope {
                    sourceURL.stopAccessingSecurityScopedResource()
                }
            }

            let callbackBox = ExtractionCallbackBox { progress, done, total, message in
                DispatchQueue.main.async {
                    self.progressValue = Double(progress)
                    self.bytesProcessed = done
                    self.bytesTotal = total
                    self.statusMessage = message
                }
                return true
            }

            var coordinatorError: NSError?
            var extractionSucceeded = false
            var nativeErrorText = ""

            withExtendedLifetime(callbackBox) {
                let coordinator = NSFileCoordinator(filePresenter: nil)
                coordinator.coordinate(
                    readingItemAt: sourceURL,
                    options: [.withoutChanges],
                    error: &coordinatorError
                ) { coordinatedURL in
                    var detailBuf = [CChar](repeating: 0, count: 512)
                    let kind = coordinatedURL.path.withCString { cPath in
                        ValidateMarathonSource(cPath, &detailBuf, Int32(detailBuf.count))
                    }
                    let detailText = Self.decodeCStringBuffer(detailBuf)
                    if !detailText.isEmpty {
                        DispatchQueue.main.async {
                            self.selectedSourceSummary = detailText
                            self.statusMessage = detailText
                        }
                    }

                    if kind == MARATHON_SOURCE_INVALID {
                        nativeErrorText = detailText.isEmpty
                            ? "Seçilen dosya geçerli bir Sonic '06 ISO/XEX kaynağı değil."
                            : detailText
                        extractionSucceeded = false
                        return
                    }

                    var cErrorBuf = [CChar](repeating: 0, count: 1024)
                    let opaqueBox = Unmanaged.passUnretained(callbackBox).toOpaque()
                    extractionSucceeded = coordinatedURL.path.withCString { cPath in
                        StartMarathonExtraction(
                            cPath,
                            shouldSkipHashes,
                            marathonProgressTrampoline,
                            opaqueBox,
                            &cErrorBuf,
                            Int32(cErrorBuf.count)
                        )
                    }

                    if !extractionSucceeded {
                        nativeErrorText = Self.decodeCStringBuffer(cErrorBuf)
                    }
                }
            }

            DispatchQueue.main.async {
                self.isExtracting = false

                if let coordinatorError {
                    self.errorMessage = "Bulut/Dosya koordinasyon hatası: \(coordinatorError.localizedDescription)"
                    self.statusMessage = "Dosya sağlayıcısından okuma başarısız oldu."
                    return
                }

                if extractionSucceeded && IsMarathonInstalled() {
                    self.progressValue = 1.0
                    self.extractionFinished = true
                    self.statusMessage = "Documents/iReCompiled06 dizinine kurulum tamamlandı!"
                } else {
                    self.errorMessage = nativeErrorText.isEmpty
                        ? "Kurulum tamamlanamadı veya eksik oyun dosyaları tespit edildi."
                        : nativeErrorText
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
