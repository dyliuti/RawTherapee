#pragma once

#include <stdint.h>

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

// ===== API 版本 =====
// 结构体带 struct_size 字段做前向兼容；不兼容的行为变更递增此版本。
#define RTE_API_VERSION 1

// ===== 解码输出尺寸档位 =====
// FULL: 原始全尺寸直出；其余档位等比缩放到长边不超过对应像素，只缩不放，
// 且 fast 路径下会触发提前缩放（先缩再处理），比全尺寸解码后再缩明显更快。
typedef enum rte_scale_mode {
    RTE_SCALE_FULL = 0,   // 原始全尺寸
    RTE_SCALE_4K   = 1,   // 长边 <= 4000
    RTE_SCALE_3K   = 2,   // 长边 <= 3000
    RTE_SCALE_2K   = 3,   // 长边 <= 2000
} rte_scale_mode;

// ===== 镜头校正 =====
typedef enum rte_lens_mode {
    RTE_LENS_OFF    = 0,  // 不做镜头校正
    RTE_LENS_AUTO   = 1,  // 从 EXIF 自动匹配 lensfun 数据库
    RTE_LENS_MANUAL = 2,  // 手动指定相机 + 镜头
} rte_lens_mode;

// 镜头校正参数（decode 时通过 rte_decode_options.lens 传入，NULL = 不校正）
typedef struct rte_lens_options {
    int             struct_size;        // 必须填 sizeof(rte_lens_options)
    rte_lens_mode   mode;               // RTE_LENS_*
    int             enable_distortion;  // 1=启用畸变校正
    const char*     camera_make;        // MANUAL 模式: 如 "Canon"（其余模式传 NULL）
    const char*     camera_model;       // MANUAL 模式: 如 "EOS 5D Mark IV"
    const char*     lens_name;          // MANUAL 模式: 如 "Canon EF 50mm f/1.4 USM"
    int             reserved[4];        // 预留，必须置 0
} rte_lens_options;

// 去噪参数结构（当前引擎尚未接入去噪流程，仅提供参数结构与默认值，
// 供调用方构建 UI / 保存配置使用；字段语义对齐线上 LibRAWEngine）
typedef struct rte_denoise_options {
    int struct_size;            // 必须填 sizeof(rte_denoise_options)
    int enabled;                // 1=在 RAW 高精度流程中去噪, 0=关闭（默认 0）
    int luminance_amount;       // 0..100（默认 0）
    int luminance_detail;       // 0..100（默认 50）
    int luminance_contrast;     // 0..100（默认 0）
    int color_amount;           // 0..100（默认 25）
    int color_detail;           // 0..100（默认 50）
    int color_smoothness;       // 0..100（默认 50）
    int reserved[4];            // 预留，必须置 0
} rte_denoise_options;

// ===== lensfun 数据库查询结果 =====
typedef struct rte_camera_entry {
    const char* make;           // 制造商, 如 "Canon"
    const char* model;          // 型号, 如 "EOS R5"
    const char* display_name;   // 显示名, 如 "Canon EOS R5"
} rte_camera_entry;

typedef struct rte_lens_entry {
    const char* name;           // 完整镜头名, 如 "Canon EF 50mm f/1.4 USM"
    const char* make;           // 制造商, 如 "Canon"
} rte_lens_entry;

// EXIF 识别结果：索引对应 rte_camera_list / rte_lens_list 返回列表的下标，-1 = 未识别
typedef struct rte_lens_detection {
    int camera_index;
    int lens_index;
} rte_lens_detection;

// ===== 引擎初始化选项 =====
typedef struct rte_engine_options {
    const char* resource_path;  // 资源根目录（profiles/ICC/DCP/JSON 数据库）
                                // NULL 或空字符串 = 按模块所在目录自动推断
    int         reserved[4];    // 预留，必须置 0
} rte_engine_options;

// ===== 解码选项与输出 =====
typedef struct rte_decode_options {
    int                     struct_size;    // 必须填 sizeof(rte_decode_options)
    rte_scale_mode          scale;          // 输出尺寸档位
    const rte_lens_options* lens;           // 镜头校正参数，NULL = 不校正
    int                     reserved[4];    // 预留，必须置 0
} rte_decode_options;

typedef struct rte_image_buffer {
    void* data;    // 8bit RGBA 像素，rte_image_buffer_free 释放
    int   size;    // 字节数 = width * height * 4
    int   width;
    int   height;
} rte_image_buffer;
