// swift-tools-version: 5.9

import PackageDescription

let package = Package(
    name: "ChainVisualiser",
    platforms: [.macOS(.v14)],
    targets: [
        .executableTarget(
            name: "ChainVisualiser",
            path: "Sources/ChainVisualiser"
        ),
        .testTarget(name: "ChainVisualiserTests", dependencies: ["ChainVisualiser"]),
    ]
)
