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

        let code = root.path.withCString { rootPtr in
            "echo tier-c-probe-ok; uname -a; cat /etc/alpine-release".withCString { commandPtr in
                output.path.withCString { outputPtr in
                    pocket_ish_run(rootPtr, commandPtr, outputPtr)
                }
            }
        }

        let text = (try? String(contentsOf: output, encoding: .utf8)) ?? "<no output>"
        return "exit=\(code)\n\n\(text)"
    }
}
