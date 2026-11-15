// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "PocketDSHKit",
    platforms: [
        .iOS(.v17),
        .macOS(.v14)
    ],
    products: [
        .library(name: "PocketDSHCore", targets: ["PocketDSHCore"]),
        .library(name: "LuaRuntime", targets: ["LuaRuntime"])
    ],
    targets: [
        .target(
            name: "PocketDSHCore",
            path: "Sources/PocketDSHCore"
        ),
        .target(
            name: "CLua",
            path: "Sources/CLua",
            publicHeadersPath: "include",
            cSettings: [
                .headerSearchPath("."),
                .headerSearchPath("include")
            ]
        ),
        .target(
            name: "LuaRuntime",
            dependencies: ["CLua", "PocketDSHCore"],
            path: "Sources/LuaRuntime"
        ),
        .testTarget(
            name: "PocketDSHCoreTests",
            dependencies: ["PocketDSHCore"],
            path: "Tests/PocketDSHCoreTests"
        ),
        .testTarget(
            name: "LuaRuntimeTests",
            dependencies: ["LuaRuntime"],
            path: "Tests/LuaRuntimeTests"
        )
    ]
)
