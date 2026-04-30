#ifndef __RAW_ENGINE_H
#define __RAW_ENGINE_H

#include "rawengine_types.h"

#define RAWENGINE_PREVIEW_TYPE_ORIGIN 0
#define RAWENGINE_PREVIEW_TYPE_3K 1
#define RAWENGINE_PREVIEW_TYPE_2K 2

// 镜头校正模式
#define RAWENGINE_LENS_MODE_NONE    0   // 不做镜头校正
#define RAWENGINE_LENS_MODE_AUTO    1   // 从 EXIF 自动匹配 lensfun 数据库
#define RAWENGINE_LENS_MODE_MANUAL  2   // 用户手动指定相机+镜头

#ifdef __cplusplus
extern "C"
{
#endif

int RAWENGINE_API rawengine_init();
int RAWENGINE_API rawengine_decode(const char* filename, void** buffer, int* length, int* widht, int* height, int preview_type, const RawEngineLensParams* lens_params);
int RAWENGINE_API rawengine_free(void* buffer);

// 初始化默认镜头参数（AUTO 模式 + 畸变校正开启）
void RAWENGINE_API rawengine_lens_params_default(RawEngineLensParams* params);

// 查询 lensfun 数据库中所有相机，结果需用 rawengine_free_cameras 释放
int RAWENGINE_API rawengine_get_cameras(RawEngineCameraInfo** cameras, int* count);
int RAWENGINE_API rawengine_free_cameras(RawEngineCameraInfo* cameras, int count);

// 查询 lensfun 数据库中所有镜头，结果需用 rawengine_free_lenses 释放
int RAWENGINE_API rawengine_get_lenses(RawEngineLensInfo** lenses, int* count);
int RAWENGINE_API rawengine_free_lenses(RawEngineLensInfo* lenses, int count);

// 从文件 EXIF 自动识别相机和镜头，返回在 get_cameras/get_lenses 列表中的索引
// camera_index/lens_index 为 -1 表示未识别到
int RAWENGINE_API rawengine_detect_lens(const char* filename, int* camera_index, int* lens_index);

#ifdef __cplusplus
}

#endif

#endif
