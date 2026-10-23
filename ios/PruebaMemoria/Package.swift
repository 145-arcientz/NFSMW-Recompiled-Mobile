// swift-tools-version: 6.0
import PackageDescription

// Fase 0 del port a iOS: no lleva nada del juego. Comprueba en el iPhone lo que
// el SDK necesita de la memoria (ver ../README.md) y que SwiftPM compila C para iOS.
let package = Package(
    name: "PruebaMemoria",
    platforms: [
        .iOS(.v17),
        .macOS(.v14),
    ],
    products: [
        // Un proyecto de xtool tiene un solo producto de biblioteca: la app.
        .library(
            name: "PruebaMemoria",
            targets: ["PruebaMemoria"]
        ),
    ],
    targets: [
        .target(
            name: "PruebaMemoria",
            dependencies: ["MemoriaNativa"]
        ),
        .target(
            name: "MemoriaNativa"
        ),
    ]
)
