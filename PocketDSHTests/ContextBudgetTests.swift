import XCTest
import PocketDSHCore
@testable import PocketDSH

/// The transcript must stay inside a budget the model will accept.
///
/// Nothing trimmed it: every tool result was appended and none was ever dropped, so a run with a few
/// dozen calls carried megabytes and the request failed without saying why.
final class ContextBudgetTests: XCTestCase {

    /// A transcript far past the budget keeps the recent results intact and elides older ones.
    func testOldToolResultsAreElided() throws {
        let big = String(repeating: "x", count: 40_000)

        var messages: [LLMMessage] = [LLMMessage(role: .user, content: "start")]
        for index in 0..<12 {
            messages.append(LLMMessage(role: .assistant, content: "call \(index)"))
            messages.append(LLMMessage(role: .tool, content: big,
                                       toolCallID: "id\(index)", name: "read_file"))
        }
        let before = messages.reduce(0) { $0 + $1.content.utf8.count }
        XCTAssertGreaterThan(before, 240_000, "the fixture has to exceed the budget")

        let after = AgentRunner.trimmed(messages)
        let total = after.reduce(0) { $0 + $1.content.utf8.count }

        XCTAssertLessThanOrEqual(total, 240_000 + 60_000,
                                 "still \(total) bytes after trimming")
        XCTAssertEqual(after.count, messages.count,
                       "trimming must not remove messages: a tool call needs its reply")
        XCTAssertEqual(after.last?.content.utf8.count, big.utf8.count,
                       "the newest result must stay whole")
        XCTAssertTrue(after[2].content.contains("[trimmed:"),
                      "an old result should say it was elided")
    }

    /// A conversation already inside the budget is left alone, byte for byte.
    func testSmallTranscriptIsUntouched() throws {
        let messages = [
            LLMMessage(role: .user, content: "短"),
            LLMMessage(role: .tool, content: "small", toolCallID: "a", name: "read_file")
        ]
        let after = AgentRunner.trimmed(messages)
        XCTAssertEqual(after.map(\.content), ["短", "small"])
    }

    /// A reply stays a reply: the tool call id survives elision, or the API rejects the transcript.
    func testToolCallIdentitySurvives() throws {
        let big = String(repeating: "y", count: 50_000)
        var messages: [LLMMessage] = [LLMMessage(role: .user, content: "go")]
        for index in 0..<8 {
            messages.append(LLMMessage(role: .tool, content: big,
                                       toolCallID: "call-\(index)", name: "run_linux"))
        }
        let after = AgentRunner.trimmed(messages)
        XCTAssertEqual(after.compactMap(\.toolCallID),
                       (0..<8).map { "call-\($0)" })
    }
}
