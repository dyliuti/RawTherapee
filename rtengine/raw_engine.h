#ifndef __RAW_ENGINE_H
#define __RAW_ENGINE_H

#include "rawengine_types.h"

#ifdef __cplusplus
extern "C"
{
#endif

// 当前导出 API 版本，与 RTE_API_VERSION 一致。
int RAWENGINE_API rte_api_version(void);

// 引擎初始化。options 为 NULL 或 resource_path 为 NULL/空时，
// 按模块所在目录自动推断资源路径（profiles、ICC/DCP 数据、JSON 数据库）。
int RAWENGINE_API rte_engine_init(const rte_engine_options* options);

// 解码 RAW 文件，输出 8bit RGBA。
// options 为 NULL 时等价于 { scale=RTE_SCALE_FULL, lens=NULL }。
// 成功返回 0（错误码见 raw_engine_error_def.h），out 需用 rte_image_buffer_free 释放。
int RAWENGINE_API rte_decode(const char* filename,
                             const rte_decode_options* options,
                             rte_image_buffer* out);

// 释放 rte_decode 输出的缓冲区并清零结构体。
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
