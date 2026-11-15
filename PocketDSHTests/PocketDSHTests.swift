import XCTest
@testable import PocketDSH
import PocketDSHCore

final class PocketDSHTests: XCTestCase {
    func testToolCatalogContainsRequiredLocalTools() {
        let names = Set(ToolCatalog.specs.map(\.function.name))
        XCTAssertTrue(names.contains(ToolNames.listFiles))
        XCTAssertTrue(names.contains(ToolNames.readFile))
        XCTAssertTrue(names.contains(ToolNames.writeFile))
        XCTAssertTrue(names.contains(ToolNames.searchFiles))
        XCTAssertTrue(names.contains(ToolNames.applyPatch))
        XCTAssertTrue(names.contains(ToolNames.runLua))
    }

    func testToolSchemasAreObjectsWithRequiredFields() {
        for spec in ToolCatalog.specs {
            guard case .object(let schema) = spec.function.parameters else {
                return XCTFail("Tool schema is not an object: \(spec.function.name)")
            }
            XCTAssertEqual(schema["type"]?.stringValue, "object")
            XCTAssertNotNil(schema["properties"]?.objectValue)
            XCTAssertNotNil(schema["required"])
        }
    }

    func testSystemPromptStatesIPhoneWorkspaceAndNoShell() {
        let prompt = ToolCatalog.systemPrompt(projectName: "Demo")
        XCTAssertTrue(prompt.contains("iPhone"))
        XCTAssertTrue(prompt.contains("real files"))
        XCTAssertTrue(prompt.contains("shell commands"))
    }

    func testEmbeddedExamplePluginIsValidAndHasRealTools() throws {
        let data = Data(EmbeddedExamplePlugin.manifestJSON.utf8)
        let manifest = try JSONDecoder().decode(PluginManifest.self, from: data)
        try manifest.validate()
        XCTAssertEqual(manifest.permissions, ["workspace.read", "workspace.write"])
        XCTAssertEqual(Set(manifest.tools.map(\.name)), Set(["workspace_report", "write_pocket_note"]))
        XCTAssertTrue(EmbeddedExamplePlugin.luaSource.contains("plugin.workspace_write"))
    }

    func testCompositePluginToolNameIsModelSafe() {
        let name = PluginRuntimeManager.compositeToolName(
            pluginID: "com.example.workspace-inspector",
            toolName: "workspace_report"
        )
        XCTAssertEqual(name, "plugin__com_example_workspace_inspector__workspace_report")
        XCTAssertTrue(name.range(of: "^[A-Za-z0-9_-]+$", options: .regularExpression) != nil)
    }
}
