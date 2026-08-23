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
        "CMakeLists.txt",
        "libdsp.a",
        "Server",
        "Tests",
        "Tools",
        "LICENSE",
        "README.md",
        "run_sanitizers.sh",
        "cross_build.sh",
        "cross_build_windows.sh",
      ],
      sources: [
        "Audio",
        "Backend",
        "Config",
        "DoP",
        "Engine",
        "FFT",
        "Filters",
        "Logging",
        "Mixer",
        "Pipeline",
        "Processors",
        "Public",
        "Resampler",
        "Utils",
      ],
      publicHeadersPath: "Public",
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
        .define("ENABLE_COREAUDIO"),
        .define("ENABLE_ACCELERATE"),
        .define("ENABLE_LIBDISPATCH"),
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
