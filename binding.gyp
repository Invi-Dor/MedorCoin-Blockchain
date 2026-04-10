{
  "targets": [
    {
      "target_name": "medorcoin_addon",
      "sources": [ "medorcoin_addon.cpp" ],
      "include_dirs": [
        "<!@(node -p \"require('node-addon-api').include\")"
      ],
      "dependencies": [
        "<!(node -p \"require('node-addon-api').gyp\")"
      ],
      "libraries": [
        "-lssl",
        "-lcrypto",
        "-lz"
      ],
      "cflags!": [ "-fno-exceptions" ],
      "cflags_cc!": [ "-fno-exceptions" ],
      "xcode_settings": {
        "GCC_ENABLE_CPP_EXCEPTIONS": "YES",
        "CLANG_CXX_LIBRARY": "libc++",
        "MACOSX_DEPLOYMENT_TARGET": "10.15",
        "CLANG_CXX_LANGUAGE_STANDARD": "c++17"
      },
      "msvs_settings": {
        "VCCLCompilerTool": { 
          "ExceptionHandling": 1,
          "AdditionalOptions": [ "/std:c++17" ] 
        }
      },
      "conditions": [
        ["OS=='linux'", {
          "cflags_cc": [
            "-std=c++17",
            "-Wall",
            "-Wextra",
            "-fPIC",
            "-pthread"
          ],
          "ldflags": [ "-pthread" ]
        }],
        ["OS=='win'", {
          "defines": [ "_HAS_EXCEPTIONS=1" ]
        }]
      ],
      "defines": [ "NAPI_CPP_EXCEPTIONS" ],
      "configurations": {
        "Debug": {
          "defines": [ "DEBUG=1", "_DEBUG" ],
          "cflags_cc": [ "-g", "-O0" ]
        },
        "Release": {
          "defines": [ "NDEBUG", "PRODUCTION_BUILD=1" ],
          "cflags_cc": [ "-O3", "-flto" ]
        }
      }
    }
  ]
}
