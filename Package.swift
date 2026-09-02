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
        "Examples",
        "Server",
        "Testing",
        "Tests",
        "Tools",
        "LICENSE",
        "README.md",
        "run_sanitizers.sh",
        "cross_build_windows.sh",
        "callgraph_audit_report.md",
        "compile_commands.json",
        "dsp_engine_public_api_alignment.md",
        "engine_state_management.md",
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
        .define("ACCELERATE_NEW_LAPACK"),
        .define("ENABLE_LIBDISPATCH"),
        .unsafeFlags([
          "-I/opt/homebrew/include",
          "-I/usr/local/include",
        ]),
      ],
      linkerSettings: [
        .linkedFramework("Accelerate"),
        .linkedFramework("AudioToolbox"),
        .linkedFramework("CoreAudio"),
        .linkedFramework("CoreFoundation"),
        .unsafeFlags([
          "/opt/homebrew/lib/libfftw3.a",
          "/opt/homebrew/lib/libfftw3f.a",
        ]),
      ]
    )
  ]
)
