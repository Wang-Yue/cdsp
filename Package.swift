// swift-tools-version:6.0
import PackageDescription

let package = Package(
  name: "CDSP",
  platforms: [.macOS(.v15)],
  products: [
    .library(name: "CDSP", targets: ["CDSP"])
  ],
  targets: [
    .target(
      name: "CDSP",
      path: ".",
      exclude: [
        "main.c",
        "Makefile",
        ".build-make",
        "libdsp.a",
        "Tests",
        "LICENSE",
        "README.md",
      ],
      publicHeadersPath: ".",
      cSettings: [
        .headerSearchPath("."),
        .headerSearchPath("Audio"),
        .headerSearchPath("Backend"),
        .headerSearchPath("Config"),
        .headerSearchPath("DoP"),
        .headerSearchPath("Engine"),
        .headerSearchPath("FFT"),
        .headerSearchPath("Filters"),
        .headerSearchPath("Logging"),
        .headerSearchPath("Mixer"),
        .headerSearchPath("Pipeline"),
        .headerSearchPath("Processors"),
        .headerSearchPath("Resampler"),
        .headerSearchPath("Server"),
        .define("ENABLE_COREAUDIO"),
        .define("ENABLE_ACCELERATE"),
      ],
      linkerSettings: [
        .linkedFramework("Accelerate"),
        .linkedFramework("AudioToolbox"),
        .linkedFramework("CoreAudio"),
        .linkedFramework("CoreFoundation"),
      ]
    )
  ]
)
