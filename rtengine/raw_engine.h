#ifndef __RAW_ENGINE_H
#define __RAW_ENGINE_H

#include "rawengine_types.h"

#ifdef __cplusplus
extern "C"
{
#endif

// 当前导出 API 版本，与 RTE_API_VERSION 一致。
int RAWENGINE_API rte_api_version(void);

// Session ABI capability. Existing libraries may not export this symbol;
// optional consumers must resolve it before enabling the Session path.
int RAWENGINE_API rte_raw_session_api_version(void);

// 引擎初始化。options 为 NULL 或 resource_path 为 NULL/空时，
// 按模块所在目录自动推断资源路径（profiles、ICC/DCP 数据、JSON 数据库）。
int RAWENGINE_API rte_engine_init(const rte_engine_options* options);

// 解码 RAW 文件，输出 8bit RGBA。
// options 为 NULL 时等价于 { scale=RTE_SCALE_FULL, lens=NULL }。
// 成功返回 0（错误码见 raw_engine_error_def.h），out 需用 rte_image_buffer_free 释放。
int RAWENGINE_API rte_decode(const char* filename,
                             const rte_decode_options* options,
                             rte_image_buffer* out);

// Session V1: same rules/lens/demosaic/force_raw as decode, FULL/4K/3K/2K.
// Invalid scales and non-ProPhoto working profiles are rejected
// (no silent fallback). Retains the complete post-stage_transform image, including
// geometry/base operations. Linear ProPhoto is native RT float encoding, nominal
// 0..65535 (not 0..1); negative/over-white values are not clipped.
// NR options are NOT connected; the default color_amount=25 does not enable NR.
// Returns 0 on success, -1 invalid arguments, -2 failure/unsupported pipeline.
int RAWENGINE_API rte_raw_session_open(const char* filename,
                                      const rte_decode_options* options,
                                      rte_raw_session** out);

// Session ABI V2 additive clean/source mode; same arguments, validation and
// ownership as V1 open. Uses the SAME get_linear/finish/close functions below.
// Retains after stage_to_working_space, before stage_working_base_operations;
// each finish runs base operations (including geometry) on a fresh source copy.
// Source view dimensions are pre-geometry and may differ from finish output.
// Explicitly disables legacy dirpyrDenoise/RGB NR, NOT every noise-related tool.
// WB, configured raw preprocessing/capture sharpening/Retinex/spot removal,
// film-negative and input profile conversion (including enabled DCP processing)
// have already run. This is NOT untouched demosaic or pre-DCP-nonlinear data.
// FULL no-edit decode parity is only expected with legacy NR off, not for all rules.
// Noise evidence is queried separately in ABI V3, never implied by this view.
// V4 previews reorder toWorking -> early resize -> retain -> baseops -> finish.
// Preview retains the full scaled canvas; axis-aligned crop runs at finish.
// Preview returns -2 (out cleared) for upscaling, unresolved EXIF fallback crop,
// or needsTransform geometry/spatial effects (including lens correction), framing,
// locallab, or a crop that collapses/exceeds the scaled canvas. No fallback to V1.
// EXIF/coarse orientation is applied before retention and remains supported.
// FULL and V1 retain their existing geometry/fallback behavior.
// This reordered clean pipeline does NOT guarantee legacy scaled byte parity.
int RAWENGINE_API rte_raw_source_session_open(const char* filename,
                                             const rte_decode_options* options,
                                             rte_raw_session** out);

// V2 per-handle stage query, without extending rte_linear_rgb_view.
// Returns 0 on success, -1 for invalid arguments/handle, -2 on internal failure.
// When out is non-NULL it is set to UNKNOWN on failure. Optional consumers must
// resolve the version symbol and require >=2 before resolving these V2 symbols.
int RAWENGINE_API rte_raw_session_get_stage(const rte_raw_session* session,
                                           rte_raw_session_stage* out);

// V3: copies immutable open-time noise evidence for either session stage.
// Returns 0 even when unprofiled/working-invalid: inspect independent flags.
// Reference A/B MUST NOT be used as calibrated working-RGB coefficients.
// Snapshot is unaffected by finish (even edited input) and survives close in
// caller storage. All calls use the engine-global lock. Require ABI >=3.
// -1 invalid handle/output/size, -2 internal failure. Failure clears known
// fields within supplied size, preserving struct_size; larger tails untouched.
int RAWENGINE_API rte_raw_session_get_noise_info(const rte_raw_session* session,
                                                rte_raw_noise_info* out);

// V5 independent, immutable reference approximation. No physical calibration
// prerequisite; inspect reference_usable, not V3 working_model_valid. See the
// numeric contract in rte_reference_noise_model. Same size/error/lock semantics
// as get_noise_info. A successful fallback query still exports reference A/B.
int RAWENGINE_API rte_raw_session_get_reference_noise_model(const rte_raw_session* session,
                                                           rte_reference_noise_model* out);

// V4. Set struct_size; requires the complete known struct. Failure clears only
// known fields within caller size, preserves struct_size and oversized tails.
// Snapshot is immutable across finishes; output dimensions can differ (crop).
int RAWENGINE_API rte_raw_session_get_dimensions(const rte_raw_session* session,
                                                rte_raw_session_dimensions* out);

// Set view->struct_size before calling. Other fields are cleared on failure.
// data is borrowed until close, including across finish; copy it before editing.
// Do not race close with use of the borrowed pointer. Library calls are serialized
// by a single internal lock; callers must still coordinate borrowed memory use.
int RAWENGINE_API rte_raw_session_get_linear(const rte_raw_session* session,
                                            rte_linear_rgb_view* view);

// NULL + count=0 is the lossless no-edit path (no interleaved roundtrip).
// Otherwise edited must contain exactly view.count finite floats in the same
// layout/units. Input is borrowed only for this call. Session remains reusable.
// RGBA ownership is independent of the session; release with rte_image_buffer_free.
int RAWENGINE_API rte_raw_session_finish(rte_raw_session* session,
                                        const float* edited, uint64_t count,
                                        rte_image_buffer* out);
// NULL is allowed. A closed handle must not be used again.
int RAWENGINE_API rte_raw_session_close(rte_raw_session* session);

// 释放 rte_decode / rte_raw_session_finish 输出的缓冲区并清零结构体。
int RAWENGINE_API rte_image_buffer_free(rte_image_buffer* buffer);

// ===== 参数默认值 =====

// 镜头参数默认值：AUTO 模式 + 畸变校正开启
void RAWENGINE_API rte_lens_options_default(rte_lens_options* options);

// 去噪参数默认值（字段默认值见 rte_denoise_options 注释）
void RAWENGINE_API rte_denoise_options_default(rte_denoise_options* options);

// ===== lensfun 相机/镜头数据库查询 =====

// 查询所有相机，结果需用 rte_camera_list_free 释放
int RAWENGINE_API rte_camera_list(rte_camera_entry** out, int* count);
int RAWENGINE_API rte_camera_list_free(rte_camera_entry* list, int count);

// 查询所有镜头，结果需用 rte_lens_list_free 释放
int RAWENGINE_API rte_lens_list(rte_lens_entry** out, int* count);
int RAWENGINE_API rte_lens_list_free(rte_lens_entry* list, int count);

// 从文件 EXIF 自动识别相机和镜头，索引对应 list 接口返回的下标，-1 = 未识别
int RAWENGINE_API rte_detect_lens(const char* filename, rte_lens_detection* out);

#ifdef __cplusplus
}
#endif

#endif
