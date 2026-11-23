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

    /// A hunk at the very start and one at the very end, which is where an off-by-one in hunk
    /// placement shows: the middle of a file is forgiving because the context line above and below
    /// pin it on both sides.
    func testPatchAtFileEdges() throws {
        _ = try workspace.write(path: "edge.txt", content: "first\nmiddle\nlast", overwrite: false)

        let patch = """
        --- a/edge.txt
        +++ b/edge.txt
        @@ -1 +1 @@
        -first
        +FIRST
        @@ -3 +3 @@
        -last
        +LAST
        """

        let result = try workspace.applyPatch(patch)
        XCTAssertEqual(result.changedPaths, ["edge.txt"])
        let lines = try workspace.read(path: "edge.txt").lines
        XCTAssertEqual(lines[0], "FIRST")
        XCTAssertEqual(lines[1], "middle")
        XCTAssertEqual(lines[2], "LAST")
    }

    /// The same context twice in one file. A patch that picks the first match silently changes the
    /// wrong occurrence, and the result still looks like a successful edit.
    func testPatchWithRepeatedContextIsRejectedOrUnambiguous() throws {
        _ = try workspace.write(path: "dup.txt",
                                content: "block\nalpha\nblock\nbeta", overwrite: false)
        let original = try workspace.read(path: "dup.txt").lines

        // A hunk whose context matches twice at the same offset-from-start. Whether this is
        // applied to the first or second occurrence, the file must either change exactly one
        // place or not change at all - never both.
        let patch = """
        --- a/dup.txt
        +++ b/dup.txt
        @@ -1,2 +1,2 @@
         block
        -alpha
        +ALPHA
        """

        do {
            let result = try workspace.applyPatch(patch)
            XCTAssertEqual(result.changedPaths, ["dup.txt"])
        } catch {
            XCTAssertEqual(try workspace.read(path: "dup.txt").lines, original,
                           "a rejected patch must leave the file alone")
            return
        }
        let lines = try workspace.read(path: "dup.txt").lines
        let changed = zip(lines, original).filter { $0 != $1 }.count
        XCTAssertEqual(changed, 1, "exactly one line should differ: \(lines)")
        XCTAssertEqual(lines[1], "ALPHA", "the first occurrence is the one with alpha after it")
    }

    /// A hunk that does not match must not partially apply its other hunks.
    func testPatchIsAllOrNothing() throws {
        _ = try workspace.write(path: "half.txt", content: "aaa\nbbb\nccc", overwrite: false)
        let original = try workspace.read(path: "half.txt").lines

        let patch = """
        --- a/half.txt
        +++ b/half.txt
        @@ -1 +1 @@
        -aaa
        +AAA
        @@ -3 +3 @@
        -not-present
        +CCC
        """

        XCTAssertThrowsError(try workspace.applyPatch(patch))
        XCTAssertEqual(try workspace.read(path: "half.txt").lines, original,
                       "the first hunk must not have been applied")
    }

    /// A file deep in the tree is a file. `present` used to confirm existence by walking to a
    /// fixed depth, so anything past six levels was reported missing while it sat there.
    /// A search that stopped at a ceiling must say so, because "no matches" is otherwise a claim
    /// about the whole tree.
    func testSearchReportsWhetherItSawEverything() throws {
        _ = try workspace.write(path: "hit.txt", content: "needle", overwrite: false)

        let complete = try workspace.searchOutcome(query: "needle")
        XCTAssertEqual(complete.matches.count, 1)
        XCTAssertFalse(complete.truncated, "a tiny workspace is read completely")
        XCTAssertGreaterThan(complete.scanned, 0)

        // The result ceiling is reached whenever there are more hits than the caller asked for.
        for index in 0..<20 {
            _ = try workspace.write(path: "more\(index).txt", content: "needle", overwrite: false)
        }
        let capped = try workspace.searchOutcome(query: "needle", maxResults: 3)
        XCTAssertEqual(capped.matches.count, 3)
        XCTAssertTrue(capped.truncated, "asking for three of twenty-one hits is a truncated search")
    }

    /// Nothing found in a truncated search is not the same as nothing found.
    func testSearchWithNoMatchesStillReportsCoverage() throws {
        _ = try workspace.write(path: "only.txt", content: "haystack", overwrite: false)
        let outcome = try workspace.searchOutcome(query: "needle")
        XCTAssertTrue(outcome.matches.isEmpty)
        XCTAssertFalse(outcome.truncated)
        XCTAssertGreaterThan(outcome.scanned, 0)
    }

    /// A skill in a subdirectory is a skill. The listing asked for one level and reported that no
    /// skill existed - the third time a display ceiling has been used to answer an existence
    /// question in this codebase.
    func testNestedSkillsAreFound() throws {
        _ = try workspace.createDirectory(path: "skills/testing")
        _ = try workspace.write(path: "skills/top.md", content: "# Top\nDoes top things.",
                                overwrite: false)
        _ = try workspace.write(path: "skills/testing/http.md",
                                content: "# HTTP\nChecks endpoints.", overwrite: false)

        let deep = try workspace.listFiles(path: "skills", maxDepth: 6, includeHidden: false)
            .filter { !$0.isDirectory && $0.path.hasSuffix(".md") }
            .map(\.path)
        XCTAssertEqual(Set(deep), Set(["skills/top.md", "skills/testing/http.md"]))

        let shallow = try workspace.listFiles(path: "skills", maxDepth: 1, includeHidden: false)
            .filter { !$0.isDirectory && $0.path.hasSuffix(".md") }
            .map(\.path)
        XCTAssertEqual(shallow, ["skills/top.md"],
                       "the old depth is what hid the nested skill")
    }

    func testIsRegularFileFindsDeepPaths() throws {
        _ = try workspace.createDirectory(path: "a/b/c/d/e/f/g")
        _ = try workspace.write(path: "a/b/c/d/e/f/g/deep.txt", content: "here", overwrite: false)

        XCTAssertTrue(workspace.isRegularFile(path: "a/b/c/d/e/f/g/deep.txt"))
        XCTAssertFalse(workspace.isRegularFile(path: "a/b/c/d/e/f/g"),
                       "a directory is not a regular file")
        XCTAssertFalse(workspace.isRegularFile(path: "a/b/c/absent.txt"))
        XCTAssertFalse(workspace.isRegularFile(path: "../outside.txt"),
                       "a path outside the workspace is not a file in it")
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
