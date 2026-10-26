// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "XScreen",
    platforms: [.macOS(.v14)],
    targets: [
        .executableTarget(
            name: "XScreen",
            path: "Sources/XScreen"
        ),
    ]
)
