      "configurations": {
        "Debug": { ... },
        "Release": { ... },
        "CI_ASAN": {
          "defines": [ "DEBUG=1", "CI_TESTING=1" ],
          "cflags_cc": [ "-g", "-O1", "-fsanitize=address", "-fno-omit-frame-pointer" ],
          "ldflags": [ "-fsanitize=address" ]
        },
        "CI_TSAN": {
          "defines": [ "DEBUG=1", "CI_TESTING=1" ],
          "cflags_cc": [ "-g", "-O1", "-fsanitize=thread", "-fno-omit-frame-pointer", "-fPIE" ],
          "ldflags": [ "-fsanitize=thread", "-pie" ]
        }
      }
