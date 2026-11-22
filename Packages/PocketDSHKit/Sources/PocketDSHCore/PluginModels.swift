import Foundation

public struct PluginToolManifest: Codable, Hashable, Sendable {
    public var name: String
    public var description: String
    public var parameters: JSONValue

    public init(name: String, description: String, parameters: JSONValue) {
        self.name = name
        self.description = description
        self.parameters = parameters
    }
}

public struct PluginConfigField: Codable, Hashable, Sendable, Identifiable {
    public enum FieldType: String, Codable, Hashable, Sendable {
        case string
        case boolean
        case number
    }

    public var id: String { key }
    public var key: String
    public var title: String
    public var description: String?
    public var type: FieldType
    public var defaultValue: JSONValue?

    public init(
        key: String,
        title: String,
        description: String? = nil,
        type: FieldType,
        defaultValue: JSONValue? = nil
    ) {
        self.key = key
        self.title = title
        self.description = description
        self.type = type
        self.defaultValue = defaultValue
    }
}

public struct PluginManifest: Codable, Hashable, Sendable {
    public var id: String
    public var name: String
    public var version: String
    public var description: String
    public var author: String?
    public var apiVersion: Int
    public var entry: String
    /// Which runtime executes this plugin: "lua" (the default, and what every existing plugin
    /// is) or "python", which runs in the guest CPython that Tier D provides.
    public var runtime: String?
    public var permissions: [String]
    public var tools: [PluginToolManifest]
    public var config: [PluginConfigField]

    public init(
        id: String,
        name: String,
        version: String,
        description: String,
        author: String? = nil,
        apiVersion: Int = 1,
        entry: String = "main.lua",
        runtime: String? = nil,
        permissions: [String] = [],
        tools: [PluginToolManifest] = [],
        config: [PluginConfigField] = []
    ) {
        self.id = id
        self.name = name
        self.version = version
        self.description = description
        self.author = author
        self.apiVersion = apiVersion
        self.entry = entry
        self.runtime = runtime
        self.permissions = permissions
        self.tools = tools
        self.config = config
    }

    public func validate() throws {
        guard apiVersion == 1 else {
            throw WorkspaceError.fileSystem("Unsupported plugin apiVersion: \(apiVersion)")
        }
        let idPattern = try NSRegularExpression(pattern: "^[A-Za-z0-9][A-Za-z0-9._-]{2,80}$")
        let idRange = NSRange(id.startIndex..<id.endIndex, in: id)
        guard idPattern.firstMatch(in: id, range: idRange) != nil else {
            throw WorkspaceError.fileSystem("Invalid plugin id: \(id)")
        }
        guard !entry.contains(".."), !entry.hasPrefix("/") else {
            throw WorkspaceError.fileSystem("Invalid plugin entry path")
        }
        guard !tools.isEmpty else {
            throw WorkspaceError.fileSystem("Plugin must declare at least one tool")
        }
        let toolPattern = try NSRegularExpression(pattern: "^[A-Za-z][A-Za-z0-9_]{1,60}$")
        for tool in tools {
            let range = NSRange(tool.name.startIndex..<tool.name.endIndex, in: tool.name)
            guard toolPattern.firstMatch(in: tool.name, range: range) != nil else {
                throw WorkspaceError.fileSystem("Invalid plugin tool name: \(tool.name)")
            }
        }
        let allowedPermissions = Set(["workspace.read", "workspace.write", "workspace.search"])
        for permission in permissions where !allowedPermissions.contains(permission) {
            throw WorkspaceError.fileSystem("Unknown plugin permission: \(permission)")
        }
    }
}

public struct InstalledPlugin: Identifiable, Codable, Hashable, Sendable {
    public var id: String { manifest.id }
    public var manifest: PluginManifest
    public var enabled: Bool
    public var configValues: [String: JSONValue]
    public var installedAt: Date
    public var lastError: String?

    public init(
        manifest: PluginManifest,
        enabled: Bool = false,
        configValues: [String: JSONValue] = [:],
        installedAt: Date = .now,
        lastError: String? = nil
    ) {
        self.manifest = manifest
        self.enabled = enabled
        self.configValues = configValues
        self.installedAt = installedAt
        self.lastError = lastError
    }

    public var config: [String: JSONValue] {
        var values = configValues
        for field in manifest.config where values[field.key] == nil {
            if let defaultValue = field.defaultValue {
                values[field.key] = defaultValue
            }
        }
        return values
    }
}

public struct PluginLogEntry: Identifiable, Codable, Hashable, Sendable {
    public enum Level: String, Codable, Hashable, Sendable {
        case debug
        case info
        case warning
        case error
    }

    public let id: UUID
    public let pluginID: String
    public let level: Level
    public let message: String
    public let timestamp: Date

    public init(
        id: UUID = UUID(),
        pluginID: String,
        level: Level,
        message: String,
        timestamp: Date = .now
    ) {
        self.id = id
        self.pluginID = pluginID
        self.level = level
        self.message = message
        self.timestamp = timestamp
    }
}
