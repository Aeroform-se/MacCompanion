// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "MacDrop",
    platforms: [
        .macOS(.v13)
    ],
    products: [
        .executable(name: "MacDrop", targets: ["MacDrop"])
    ],
    targets: [
        .executableTarget(
            name: "MacDrop",
            path: "Sources/MacDrop",
            resources: [
                .process("../../Resources")
            ]
        )
    ]
)
