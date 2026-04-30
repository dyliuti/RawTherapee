#pragma once

#if defined(_WIN32) || defined(_WIN64)
    #ifdef RAWENGINE_EXPORTS
        #define RAWENGINE_API __declspec(dllexport)
    #else
        #define RAWENGINE_API __declspec(dllimport)
    #endif
#elif defined(__GNUC__) || defined(__clang__)
    #define RAWENGINE_API __attribute__((visibility("default")))
#else
    #define RAWENGINE_API
#endif


typedef struct RawEngineLensParams {
    int lens_mode;              // RAWENGINE_LENS_MODE_*
    int use_distortion;         // 1=启用畸变校正, 0=关闭（默认1）
    const char* camera_make;    // MANUAL 模式: 如 "Canon"（AUTO/NONE 时传 NULL）
    const char* camera_model;   // MANUAL 模式: 如 "EOS 5D Mark IV"
    const char* lens_name;      // MANUAL 模式: 如 "Canon EF 50mm f/1.4 USM"
} RawEngineLensParams;

typedef struct RawEngineCameraInfo {
    const char* make;           // 制造商, 如 "Canon"
    const char* model;          // 型号, 如 "EOS R5"
    const char* display_string; // 显示名, 如 "Canon EOS R5"
} RawEngineCameraInfo;

typedef struct RawEngineLensInfo {
    const char* lens_name;      // 完整镜头名, 如 "Canon EF 50mm f/1.4 USM"
    const char* make;           // 制造商, 如 "Canon"
} RawEngineLensInfo;
