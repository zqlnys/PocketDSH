import SwiftUI

@main
struct TierCProbeApp: App {
    var body: some Scene {
        WindowGroup {
            ContentView()
        }
    }
}

struct ContentView: View {
    @State private var output = "Running /bin/sh inside iSH..."
    @State private var running = true

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Text("Tier C probe")
                .font(.largeTitle.weight(.bold))
            Text("iSH · Alpine Linux · no JIT")
                .font(.caption.monospaced())
                .foregroundStyle(.secondary)
            ScrollView {
                Text(output)
                    .font(.system(.caption, design: .monospaced))
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .textSelection(.enabled)
            }
            .padding(12)
            .background(Color.secondary.opacity(0.08))
            .clipShape(RoundedRectangle(cornerRadius: 12))
            if !running {
                Text("Exit code captured above.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
        .padding(24)
        .task {
            await runProbe()
        }
    }

    private func runProbe() async {
        let result = await Task.detached(priority: .userInitiated) {
            TierCProbeRunner.run()
        }.value
        output = result
        running = false
    }
}

enum TierCProbeRunner {
    static func run() -> String {
        guard let bundledRoot = Bundle.main.url(forResource: "rootfs", withExtension: nil) else {
            return "rootfs not found in bundle"
        }

        let fm = FileManager.default
        let support = fm.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("TierCProbe", isDirectory: true)
        let root = support.appendingPathComponent("rootfs", isDirectory: true)
        let output = support.appendingPathComponent("output.txt")
        do {
            try fm.createDirectory(at: support, withIntermediateDirectories: true)
            if !fm.fileExists(atPath: root.path) {
                try fm.copyItem(at: bundledRoot, to: root)
            }
        } catch {
            return "rootfs preparation failed: \(error.localizedDescription)"
        }

        let stage = support.appendingPathComponent("stage.txt")
        func mark(_ text: String) {
            try? text.write(to: stage, atomically: true, encoding: .utf8)
        }
        if pocket_ish_start(root.path) != 0 {
            return "iSH failed to start"
        }

        let marker = "__POCKET_DSH_DONE__"
        let command = "echo tier-c-probe-ok; uname -a; cat /etc/alpine-release; echo \(marker)\n"
        _ = pocket_ish_send(command)

        var outputText = ""
        var buffer = [CChar](repeating: 0, count: 4096)
        let deadline = Date().addingTimeInterval(8)
        while Date() < deadline && !outputText.contains(marker) {
            let count = pocket_ish_read(&buffer, buffer.count, 200)
            if count > 0 {
                outputText += String(cString: buffer)
            }
        }
        pocket_ish_close()
        let clean = outputText.replacingOccurrences(of: marker, with: "")
        return clean.trimmingCharacters(in: .whitespacesAndNewlines)
    }
}
