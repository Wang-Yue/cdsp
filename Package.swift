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
        "app",
        "CMakeLists.txt",
        "Examples",
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
        "docs",
      ],
      sources: [
        "src",
      ],
      publicHeadersPath: "include",
      cSettings: [
        .headerSearchPath("include"),
        .headerSearchPath("include/cdsp"),
        .headerSearchPath("src"),
        .headerSearchPath("src/Audio"),
        .headerSearchPath("src/Backend"),
        .headerSearchPath("src/Config"),
        .headerSearchPath("src/DoP"),
        .headerSearchPath("src/Engine"),
        .headerSearchPath("src/FFT"),
        .headerSearchPath("src/Filters"),
        .headerSearchPath("src/Logging"),
        .headerSearchPath("src/Mixer"),
        .headerSearchPath("src/Pipeline"),
        .headerSearchPath("src/Processors"),
        .headerSearchPath("src/Public"),
        .headerSearchPath("src/Resampler"),
        .headerSearchPath("src/Utils"),
        .headerSearchPath("app"),
        .headerSearchPath("app/Server"),
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
