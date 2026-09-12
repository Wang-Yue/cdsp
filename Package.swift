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
      sources: [
        "src",
      ],
      publicHeadersPath: "include",
      cSettings: [
        .headerSearchPath("include"),
        .headerSearchPath("include/cdsp"),
        .headerSearchPath("src"),
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
