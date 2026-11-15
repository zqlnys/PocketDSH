import XCTest
@testable import LuaRuntime

final class LuaRuntimeTests: XCTestCase {
    func testArithmeticAndTables() async {
        let result = await LuaEngine().run(
            """
            local total = 0
            for i = 1, 5 do total = total + i end
            local t = { name = "pocket", total = total }
            print(t.name, t.total)
            """
        )

        XCTAssertEqual(result.exitCode, 0)
        XCTAssertEqual(result.stdout, "pocket\t15\n")
        XCTAssertTrue(result.stderr.isEmpty)
        XCTAssertTrue(result.succeeded)
    }

    func testRuntimeErrorHasNonZeroExitAndStderr() async {
        let result = await LuaEngine().run("error('intentional failure')")

        XCTAssertEqual(result.exitCode, 1)
        XCTAssertTrue(result.stderr.contains("intentional failure"))
        XCTAssertFalse(result.succeeded)
    }

    func testShellAndFileLibrariesAreNotExposed() async {
        let result = await LuaEngine().run(
            """
            print(type(os), type(io), type(package), type(debug), type(dofile), type(loadfile))
            """
        )

        XCTAssertEqual(result.exitCode, 0)
        XCTAssertEqual(result.stdout, "nil\tnil\tnil\tnil\tnil\tnil\n")
    }

    func testTimeoutIsEnforced() async {
        let startedAt = Date()
        let result = await LuaEngine().run("while true do end", timeout: 0.15)

        XCTAssertEqual(result.exitCode, 1)
        XCTAssertTrue(result.stderr.contains("timeout") || result.stderr.contains("interrupted"))
        XCTAssertLessThan(Date().timeIntervalSince(startedAt), 3)
    }

    func testPrintOutputIsCaptured() async {
        let result = await LuaEngine().run("print('hello'); print(1 + 2)")

        XCTAssertEqual(result.stdout, "hello\n3\n")
    }
}
