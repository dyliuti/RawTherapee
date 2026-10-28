#pragma once

#include <stdint.h>
#include <stddef.h>

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
#define RTE_API_VERSION 2

// Additive Session ABI revision, independent of the legacy decode API version.
// Query rte_raw_session_api_version() from the same deployed library; source
// headers alone do not imply an older binary implements the Session symbols.
// V2 adds source-session open and per-handle stage query; V1 open is unchanged.
// V4 adds preview sessions and an independent spatial geometry snapshot.
// V3 adds immutable noise evidence snapshots; existing layouts are unchanged.
// V5 adds an independent reference approximation; all previous layouts unchanged.
#define RTE_RAW_SESSION_API_VERSION 5
#define RTE_REFERENCE_NOISE_MODEL_VERSION 1

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
    int             enable_vignette;    // 1=启用光学暗角补偿
    int             distortion_amount;  // 0..200，100=完整 Profile 校正
    int             vignette_amount;    // 0..200，100=完整 Profile 补偿
    const char*     camera_make;        // MANUAL 模式: 如 "Canon"（其余模式传 NULL）
    const char*     camera_model;       // MANUAL 模式: 如 "EOS 5D Mark IV"
    const char*     lens_name;          // MANUAL 模式: 如 "Canon EF 50mm f/1.4 USM"
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

// ===== 去马赛克档位（预览可选快速算法）=====
// 作为解码选项按次传入(rte_decode_options.demosaic)，运行时切换、无需重编引擎库。
// DEFAULT = 沿用规则/默认(通常 AMaZE)；其余为具体 Bayer 去马赛克算法，大致按速度→质量排序，
// 预留足够梯度，后续改选项不必再动枚举/重编。仅作用于 Bayer 传感器；X-Trans 维持默认。
typedef enum rte_demosaic_quality {
    RTE_DEMOSAIC_DEFAULT       = 0,  // 规则/默认（非 Android 平台维持现状）
    RTE_DEMOSAIC_FAST          = 1,  // 最快，质量最低
    RTE_DEMOSAIC_VNG4          = 2,
    RTE_DEMOSAIC_RCD           = 3,  // 速度/质量平衡（预览推荐）
    RTE_DEMOSAIC_DCB           = 4,
    RTE_DEMOSAIC_LMMSE         = 5,
    RTE_DEMOSAIC_IGV           = 6,
    RTE_DEMOSAIC_AHD           = 7,
    RTE_DEMOSAIC_EAHD          = 8,
    RTE_DEMOSAIC_HPHD          = 9,
    RTE_DEMOSAIC_AMAZE         = 10, // 高质量（导出/全质量）
    RTE_DEMOSAIC_AMAZEVNG4     = 11,
    RTE_DEMOSAIC_RCDVNG4       = 12,
    RTE_DEMOSAIC_DCBVNG4       = 13,
    RTE_DEMOSAIC_AMAZEBILINEAR = 14,
    RTE_DEMOSAIC_RCDBILINEAR   = 15,
    RTE_DEMOSAIC_DCBBILINEAR   = 16,
    RTE_DEMOSAIC_MONO          = 17,
    RTE_DEMOSAIC_NONE          = 18,
} rte_demosaic_quality;

// ===== 解码选项与输出 =====
typedef struct rte_decode_options {
    int                     struct_size;    // 必须填 sizeof(rte_decode_options)
    rte_scale_mode          scale;          // 输出尺寸档位
    const rte_lens_options* lens;           // 镜头校正参数，NULL = 不校正
    int                     demosaic;       // rte_demosaic_quality；0=DEFAULT（原 reserved[0]，布局不变）
    int                     force_raw;      // 1=强制按 RAW 解码、忽略扩展名；0=按扩展名判定(默认，原 reserved[0]→现 reserved[1]，布局不变)
                                            // 用于内容是 RAW 但扩展名被误写成 tiff/jpg/png 的情形（如 Android SAF 导入把 ARW 命名为 .tiff）
    int                     reserved[2];    // 预留，必须置 0
} rte_decode_options;

typedef struct rte_image_buffer {
    void* data;    // 8bit RGBA 像素，rte_image_buffer_free 释放
    int   size;    // 字节数 = width * height * 4
    int   width;
    int   height;
} rte_image_buffer;

// Additive ABI: does not reuse any reserved fields in existing options.
typedef struct rte_raw_session rte_raw_session;

// Stage of the borrowed linear view; independent of its unchanged V1 layout.
typedef enum rte_raw_session_stage {
    RTE_RAW_SESSION_STAGE_UNKNOWN = 0,
    RTE_RAW_SESSION_STAGE_POST_TRANSFORM = 1, // V1 open: base/geometry baked in
    RTE_RAW_SESSION_STAGE_PROPHOTO_SOURCE = 2 // V2 source open: before working base
} rte_raw_session_stage;

// Quality of SENSOR evidence only, independent of working_model_valid.
typedef enum rte_noise_profile_quality {
    RTE_NOISE_PROFILE_ESTIMATED = 0,
    RTE_NOISE_PROFILE_REFERENCE = 1,
    RTE_NOISE_PROFILE_CALIBRATED = 2
} rte_noise_profile_quality;

// V3 owned snapshot: no borrowed strings/pointers. All flags are 0/1.
// Four-channel arrays are R,G1,B,G2. Reference A/B retain table units;
// they are NOT normalized/calibrated working-RGB shot/read coefficients.
// No ISO interpolation/extrapolation flags exist in the current provider.
typedef struct rte_raw_noise_info {
    uint32_t struct_size; // caller sets sizeof; >= current size accepted
    uint32_t profile_quality; // rte_noise_profile_quality
    uint32_t profile_matched;
    uint32_t working_model_valid; // currently always false, independently
    uint32_t sensor_calibration_valid;
    uint32_t render_metadata_valid;
    uint32_t color_matrix_valid;
    uint32_t profile_generic;
    double iso;
    double profile_iso;
    double raw_code_scale; // nominal RT scale, NOT ADC white DN
    double reference_a[4];
    double reference_b[4];
    double decoder_black_dn[4];
    double decoder_white_dn[4];
    double preprocessing_black_dn[4];
    double preprocessing_white_dn[4];
    double preprocessing_scale[4];
    double render_multiplier[3]; // actual WB/gain/baseline/skip^2 factors
    double baseline_exposure_ev;
    int32_t render_skip;
    uint32_t reserved; // zero
    double camera_to_working[9]; // row-major, valid only if color_matrix_valid
    // UTF-8, NUL terminated, truncated to capacity-1 if necessary.
    char make[128];
    char model[128];
    char profile_make[128];
    char profile_model[128];
    char profile_mode[128];
    char working_profile[64];
    char reason[256];
    char profile_reason[256];
    char reference_reason[256];
    char sensor_reason[256];
    char color_reason[256];
    char fallback[256];
} rte_raw_noise_info;

// V5: engineering reference ONLY, never a calibrated working noise certificate.
// Four channels R,G1,B,G2. A=table A/65535; B=table B/65535^2.
// Input/output of map use nominal unit range (linear view /65535).
// gain[c]=preprocessingScale[c]*renderMultiplier[RGB(c)]*(decoderWhite-Black)/65535.
// camera_to_working = observed direct M * diag(gainR,mean(gainG1,gainG2),gainB).
// This 3x3 approximation is NOT exact CFA/demosaic covariance. Do NOT apply WB
// or exposure again: stage3_gain=1. No downsampling noise correction is claimed.
// Only direct matrix, render_skip=1, non-resampled size is reference_usable.
// DCP/ICC/invalid levels/nonfinite/singular fail closed, retaining A/B evidence.
typedef struct rte_reference_noise_model {
    uint32_t struct_size; // caller sets sizeof; larger tail untouched
    uint32_t version;     // output RTE_REFERENCE_NOISE_MODEL_VERSION
    uint32_t reference_usable; // independent of old working_model_valid=false
    uint32_t spatially_resampled;
    uint64_t snapshot_hash; // FNV-1a of params/full identities/size/scale/stage
    double normalized_a[4];
    double normalized_b[4];
    double channel_gain[4];
    double camera_to_working[9]; // row-major, zero when unusable
    double stage3_gain; // always 1 for a successful query, including fallback
    double resolution_scale; // spatial only; NOT a variance/gain correction
    uint32_t native_width, native_height, retained_width, retained_height;
    uint32_t session_stage;
    int32_t render_skip;
    char reason[256];
    char profile_make[128], profile_model[128], profile_mode[128];
    char working_profile[64];
} rte_reference_noise_model;

typedef struct rte_linear_rgb_view {
    uint32_t     struct_size;       // caller sets sizeof(rte_linear_rgb_view)
    uint32_t     width;
    uint32_t     height;
    uint32_t     channels;          // always 3, interleaved RGBRGB, row-major
    const float* data;              // borrowed, immutable; never free this pointer
    uint64_t     count;             // number of float elements, NOT bytes
    uint64_t     row_stride_bytes;  // width * 3 * sizeof(float), tightly packed
} rte_linear_rgb_view;

// V4 additive geometry snapshot; existing linear/noise view ABIs unchanged.
// Native = full oriented engine canvas before early resize (not masked sensor
// dimensions). Retained = get_linear dimensions; source finish may crop it.
// resolution_scale is spatial only, NOT calibrated sensor-noise propagation.
typedef struct rte_raw_session_dimensions {
    uint32_t struct_size;
    uint32_t native_width;
    uint32_t native_height;
    uint32_t retained_width;
    uint32_t retained_height;
    uint32_t reserved;
    double resolution_scale;
} rte_raw_session_dimensions;

// V1/V2 share this exact view layout. Channel encoding is linear ProPhoto in
// native RT units (nominal white 65535); values are not normalized or clipped.
// Processing boundary is queried separately with rte_raw_session_get_stage.
