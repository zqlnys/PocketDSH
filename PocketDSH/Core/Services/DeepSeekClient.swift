import Foundation
import PocketDSHCore

struct DeepSeekClient {
    private let endpoint = URL(string: "https://api.deepseek.com/chat/completions")!

    private struct ChatRequest: Encodable {
        let model: String
        let messages: [LLMMessage]
        let tools: [LLMToolSpec]?
        let toolChoice: String?
        let stream: Bool
        let thinking: Thinking?
        let reasoningEffort: String?
        let maxTokens: Int

        enum CodingKeys: String, CodingKey {
            case model
            case messages
            case tools
            case toolChoice = "tool_choice"
            case stream
            case thinking
            case reasoningEffort = "reasoning_effort"
            case maxTokens = "max_tokens"
        }
    }

    private struct Thinking: Encodable {
        let type: String
    }

    private struct ChatResponse: Decodable {
        struct Choice: Decodable {
            let message: LLMMessage
            let finishReason: String?

            enum CodingKeys: String, CodingKey {
                case message
                case finishReason = "finish_reason"
            }
        }

        let choices: [Choice]
    }

    private struct ErrorEnvelope: Decodable {
        struct APIError: Decodable {
            let message: String?
            let type: String?
            let code: String?
        }

        let error: APIError?
        let message: String?
    }

    enum DeepSeekError: Error, LocalizedError {
        case missingKey
        case invalidResponse
        case http(Int, String)
        case api(String)
        case emptyChoices

        var errorDescription: String? {
            switch self {
            case .missingKey:
                return "DeepSeek API key is not configured. Add it in Settings."
            case .invalidResponse:
                return "DeepSeek returned an unreadable response."
            case .http(let status, let message):
                return "DeepSeek HTTP \(status): \(message)"
            case .api(let message):
                return "DeepSeek: \(message)"
            case .emptyChoices:
                return "DeepSeek returned no assistant message."
            }
        }
    }

    /// Check that a key works, without doing any real work.
    ///
    /// A one-token request is the cheapest thing that exercises everything the
    /// agent will use: the endpoint, the bearer header, the model name and the
    /// response decoder. Without this the first sign of a wrong key is a task
    /// failing halfway through.
    func verify(apiKey: String, model: String) async throws -> String {
        let reply = try await complete(
            apiKey: apiKey,
            model: model,
            messages: [LLMMessage(role: .user, content: "Reply with the single word OK.")],
            tools: [],
            thinkingEnabled: false,
            reasoningEffort: .low,
            maxTokens: 8
        )
        let text = (reply.content ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
        return text.isEmpty ? "The key works, but the model returned no text." : "Key works. Model replied: \(text.prefix(40))"
    }

    func complete(
        apiKey: String,
        model: String,
        messages: [LLMMessage],
        tools: [LLMToolSpec],
        thinkingEnabled: Bool,
        reasoningEffort: ReasoningEffort,
        maxTokens: Int = 4096
    ) async throws -> LLMMessage {
        guard !apiKey.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else {
            throw DeepSeekError.missingKey
        }

        var request = URLRequest(url: endpoint)
        request.httpMethod = "POST"
        request.timeoutInterval = 180
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        request.setValue("application/json", forHTTPHeaderField: "Accept")
        request.setValue("Bearer \(apiKey)", forHTTPHeaderField: "Authorization")

        let payload = ChatRequest(
            model: model,
            messages: messages,
            tools: tools.isEmpty ? nil : tools,
            toolChoice: tools.isEmpty ? nil : "auto",
            stream: false,
            thinking: Thinking(type: thinkingEnabled ? "enabled" : "disabled"),
            reasoningEffort: thinkingEnabled ? reasoningEffort.rawValue : nil,
            maxTokens: maxTokens
        )
        request.httpBody = try JSONEncoder().encode(payload)

        try Task.checkCancellation()
        let (data, response) = try await URLSession.shared.data(for: request)
        try Task.checkCancellation()

        guard let http = response as? HTTPURLResponse else {
            throw DeepSeekError.invalidResponse
        }

        guard (200..<300).contains(http.statusCode) else {
            let message = decodeError(data)
            throw DeepSeekError.http(http.statusCode, message)
        }

        do {
            let decoded = try JSONDecoder().decode(ChatResponse.self, from: data)
            guard let message = decoded.choices.first?.message else {
                throw DeepSeekError.emptyChoices
            }
            return message
        } catch let error as DeepSeekError {
            throw error
        } catch {
            if let envelope = try? JSONDecoder().decode(ErrorEnvelope.self, from: data),
               let message = envelope.error?.message ?? envelope.message {
                throw DeepSeekError.api(message)
            }
            throw DeepSeekError.invalidResponse
        }
    }

    private func decodeError(_ data: Data) -> String {
        if let envelope = try? JSONDecoder().decode(ErrorEnvelope.self, from: data),
           let message = envelope.error?.message ?? envelope.message {
            return message
        }
        return String(data: data, encoding: .utf8)?.prefix(500).description ?? "Unknown error"
    }
}
