import Foundation
import XCTest
@testable import PocketDSHCore

final class WorkspaceCoreTests: XCTestCase {
    private var root: URL!
    private var metadata: URL!
    private var workspace: WorkspaceFileSystem!

    override func setUpWithError() throws {
        try super.setUpWithError()
        root = FileManager.default.temporaryDirectory
            .appendingPathComponent("PocketDSHCoreTests-\(UUID().uuidString)", isDirectory: true)
        metadata = FileManager.default.temporaryDirectory
            .appendingPathComponent("PocketDSHMetadata-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        try FileManager.default.createDirectory(at: metadata, withIntermediateDirectories: true)
        workspace = WorkspaceFileSystem(root: root, metadataRoot: metadata)
    }

    override func tearDownWithError() throws {
        if let root { try? FileManager.default.removeItem(at: root) }
        if let metadata { try? FileManager.default.removeItem(at: metadata) }
        try super.tearDownWithError()
    }

    func testPathGuardRejectsTraversalAndAbsolutePaths() throws {
        let guardrail = WorkspacePathGuard(root: root)
        XCTAssertThrowsError(try guardrail.resolve("../escape.txt"))
        XCTAssertThrowsError(try guardrail.resolve("/tmp/escape.txt"))
        XCTAssertThrowsError(try guardrail.resolve("~/escape.txt"))
        XCTAssertEqual(try guardrail.resolve("hello.txt").lastPathComponent, "hello.txt")
    }

    func testWriteReadSearchAndTree() throws {
        let result = try workspace.write(
            path: "Sources/Main.swift",
            content: "let greeting = \"hello\"\nprint(greeting)\n",
            overwrite: false
        )
        XCTAssertTrue(result.created)
        XCTAssertEqual(result.bytesWritten, 39)
        XCTAssertNil(result.backupPath)
        XCTAssertTrue(result.diff.contains("+let greeting"))

        let read = try workspace.read(path: "Sources/Main.swift")
        XCTAssertEqual(read.startLine, 1)
        XCTAssertEqual(read.lines.count, 3)
        XCTAssertTrue(read.lines[0].contains("greeting"))

        let matches = try workspace.search(query: "hello")
        XCTAssertEqual(matches.count, 1)
        XCTAssertEqual(matches[0].path, "Sources/Main.swift")
        XCTAssertEqual(matches[0].line, 1)

        let tree = try workspace.tree()
        XCTAssertEqual(tree.first?.name, "Sources")
        XCTAssertEqual(tree.first?.children.first?.name, "Main.swift")
    }

    func testOverwriteRequiresFlagAndCreatesBackup() throws {
        _ = try workspace.write(path: "notes.txt", content: "one\n", overwrite: false)
        XCTAssertThrowsError(
            try workspace.write(path: "notes.txt", content: "two\n", overwrite: false)
        )

        let updated = try workspace.write(path: "notes.txt", content: "two\n", overwrite: true)
        XCTAssertNotNil(updated.backupPath)
        XCTAssertEqual(try workspace.read(path: "notes.txt").lines.first, "two")
        XCTAssertEqual(workspace.changes().count, 2)

        let backup = try XCTUnwrap(workspace.changes().last)
        XCTAssertEqual(backup.kind, .write)
        XCTAssertNotNil(backup.backupPath)

        try workspace.restore(backup)
        XCTAssertEqual(try workspace.read(path: "notes.txt").lines.first, "one")
    }

    func testPatchAppliesMultipleFiles() throws {
        _ = try workspace.write(path: "a.txt", content: "one\ntwo\nthree", overwrite: false)
        _ = try workspace.write(path: "b.txt", content: "red\nblue", overwrite: false)

        let patch = """
        --- a/a.txt
        +++ b/a.txt
        @@ -1,3 +1,3 @@
         one
        -two
        +TWO
         three
        --- a/b.txt
        +++ b/b.txt
        @@ -1,2 +1,2 @@
         red
        -blue
        +green
        """

        let result = try workspace.applyPatch(patch)
        XCTAssertEqual(Set(result.changedPaths), Set(["a.txt", "b.txt"]))
        XCTAssertEqual(try workspace.read(path: "a.txt").lines[1], "TWO")
        XCTAssertEqual(try workspace.read(path: "b.txt").lines[1], "green")
        XCTAssertEqual(result.backupPaths.count, 2)
    }

    func testPatchMismatchDoesNotWrite() throws {
        _ = try workspace.write(path: "a.txt", content: "one\ntwo\n", overwrite: false)
        let patch = """
        --- a/a.txt
        +++ b/a.txt
        @@ -1,2 +1,2 @@
         one
        -wrong
        +right
        """
        XCTAssertThrowsError(try workspace.applyPatch(patch))
        XCTAssertEqual(try workspace.read(path: "a.txt").lines[1], "two")
    }

    func testLineDiffMarksInsertionsAndRemovals() {
        let diff = LineDiff.compute(old: "a\nb\nc", new: "a\nB\nc\nd")
        XCTAssertTrue(diff.lines.contains { $0.kind == .remove && $0.text == "b" })
        XCTAssertTrue(diff.lines.contains { $0.kind == .insert && $0.text == "B" })
        XCTAssertTrue(diff.lines.contains { $0.kind == .insert && $0.text == "d" })
    }

    func testWhitespaceOnlyQueryReturnsNoMatches() throws {
        _ = try workspace.write(path: "a.txt", content: "alpha\n", overwrite: false)
        XCTAssertTrue(try workspace.search(query: "   ").isEmpty)
    }
}
