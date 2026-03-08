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
      "cflags_cc": [ "-O2" ],
      "xcode_settings": {
        "OTHER_CPLUSPLUSFLAGS": [ "-std=c++17" ]
      },
      "defines": [ "NAPI_DISABLE_CPP_EXCEPTIONS=1" ]
    }
  ]
}
