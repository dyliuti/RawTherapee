# rawengine — RawTherapee C API 集成说明

## 概述

`rawengine` 是基于 [RawTherapee](https://www.rawtherapee.com/) 引擎封装的 C 语言接口库，
提供工业级 RAW 图像解码能力。底层使用 RawTherapee 的完整处理流水线（LibRaw 解码、降噪、色彩管理、
镜头校正等），通过 9 个简洁的 C 函数对外暴露。

**主要特性：**

- 支持 600+ 款相机的 RAW 格式（CR2/CR3/NEF/ARW/RAF/DNG 等）
- 可选输出分辨率：原始尺寸、3K、2K 预览
- 自动 / 手动镜头畸变校正（Lensfun 数据库）
- C 接口，可从 C/C++/Python/Go 等语言调用
- 返回 RGBA 8-bit 内存缓冲区，可直接送入图形管线

---

## 目录结构

```
output/
├── include/                    # C API 头文件
│   ├── raw_engine.h            # 主头文件（包含所有 API 声明）
│   ├── rawengine_types.h       # 数据结构定义
│   └── raw_engine_error_def.h  # 错误码枚举
├── lib/
│   ├── librtengine.a           # RawTherapee 引擎静态库（MinGW/GCC 工程用）
│   ├── rawengine.dll.a         # DLL 导入库（MinGW/GCC 动态链接用）
│   ├── rawengine.lib           # DLL 导入库（MSVC 工程用）
│   └── rawengine.def           # DLL 导出符号定义文件
├── bin/
│   ├── rawengine.dll           # ★ 动态库（MSVC 上层工程集成推荐）
│   ├── rawengine-cli.exe       # 命令行批处理工具
│   └── *.dll                   # 运行时依赖 DLL（需与 exe 同目录或在 PATH 中）
├── pdb/
│   ├── rawengine-cli.exe.debug # DWARF 调试符号（GDB 可加载）
│   ├── rawengine.dll.debug     # DLL 调试符号（GDB 可加载）
│   └── librtengine_symbols.txt # 静态库导出符号列表
├── resource/                   # ★ 运行时色彩/相机资源（集成时需部署）
│   ├── dcpprofiles/            # 相机 DCP 色彩校准文件（600+ 款相机）
│   ├── iccprofiles/            # ICC 色彩空间文件（input/output）
│   ├── profiles/               # 处理预设 .pp3（Auto-Matched Curve 等）
│   ├── languages/default       # 默认语言包
│   ├── camconst.json           # 相机传感器参数数据库
│   ├── cammatrices.json        # 相机色彩矩阵
│   ├── dcraw.json              # LibRaw/dcraw 相机数据库
│   └── rt.json                 # RawTherapee 内部参数
└── readme.md                   # 本文档
```

---

## API 参考

所有函数声明在 `include/raw_engine.h`。

### 初始化

```c
int rawengine_init();
```

**必须在调用任何其他函数之前调用一次。**  
初始化 RawTherapee 引擎、lensfun 数据库、ICC 色彩配置等。  
返回 `0` 表示成功，非零表示失败。

---

### 解码 RAW 文件

```c
int rawengine_decode(
    const char*               filename,     // RAW 文件路径（UTF-8）
    void**                    buffer,       // [OUT] RGBA 像素数据（需调用 rawengine_free 释放）
    int*                      length,       // [OUT] buffer 字节数
    int*                      width,        // [OUT] 图像宽度（像素）
    int*                      height,       // [OUT] 图像高度（像素）
    int                       preview_type, // RAWENGINE_PREVIEW_TYPE_*
    const RawEngineLensParams* lens_params  // 镜头校正参数，传 NULL 使用默认值
);
```

**preview_type 取值：**

| 常量 | 值 | 说明 |
|---|---|---|
| `RAWENGINE_PREVIEW_TYPE_ORIGIN` | 0 | 原始分辨率 |
| `RAWENGINE_PREVIEW_TYPE_3K` | 1 | 长边缩放至 3072px |
| `RAWENGINE_PREVIEW_TYPE_2K` | 2 | 长边缩放至 2048px |

输出 `buffer` 为连续 RGBA 字节数组，每像素 4 字节，行优先（row-major）。

---

### 释放缓冲区

```c
int rawengine_free(void* buffer);
```

释放 `rawengine_decode` 返回的 `buffer`。**不可使用 `free()` 直接释放。**

---

### 镜头参数

```c
// 以默认值填充 params（AUTO 模式，开启畸变校正）
void rawengine_lens_params_default(RawEngineLensParams* params);
```

```c
typedef struct RawEngineLensParams {
    int  lens_mode;        // RAWENGINE_LENS_MODE_NONE / AUTO / MANUAL
    int  use_distortion;   // 1=启用畸变校正
    const char* camera_make;   // MANUAL 模式下：相机厂商，如 "Canon"
    const char* camera_model;  // MANUAL 模式下：相机型号，如 "EOS 5D Mark IV"
    const char* lens_name;     // MANUAL 模式下：镜头名，如 "Canon EF 50mm f/1.4 USM"
} RawEngineLensParams;
```

**镜头模式：**

| 常量 | 说明 |
|---|---|
| `RAWENGINE_LENS_MODE_NONE` | 不做镜头校正 |
| `RAWENGINE_LENS_MODE_AUTO` | 从文件 EXIF 自动匹配 lensfun 数据库 |
| `RAWENGINE_LENS_MODE_MANUAL` | 手动指定相机/镜头字符串 |

---

### 查询相机/镜头数据库

```c
int rawengine_get_cameras(RawEngineCameraInfo** cameras, int* count);
int rawengine_free_cameras(RawEngineCameraInfo* cameras, int count);

int rawengine_get_lenses(RawEngineLensInfo** lenses, int* count);
int rawengine_free_lenses(RawEngineLensInfo* lenses, int count);
```

枚举 lensfun 数据库中的全部相机和镜头，结果通过对应的 `_free_*` 函数释放。

---

### 自动识别镜头

```c
int rawengine_detect_lens(const char* filename, int* camera_index, int* lens_index);
```

从 RAW 文件 EXIF 中识别相机和镜头，返回在 `get_cameras` / `get_lenses` 列表中的索引。  
未识别时对应索引返回 `-1`。

---

## 集成方式

### 前提

- Windows 10/11 x64
- MSYS2 ucrt64 运行时 DLL（已包含在 `bin/` 目录中）

### 方式一：MSVC 工程动态链接（推荐）

MSVC 工程**无法**静态链接 `librtengine.a`（GCC 编译，ABI 不兼容）。  
应使用 `rawengine.dll` + 导入库 `rawengine.lib` 进行动态链接。

**CMake（MSVC）示例：**

```cmake
cmake_minimum_required(VERSION 3.15)
project(MyApp)

# 头文件
include_directories("path/to/output/include")

# DLL 导入库
add_library(rawengine SHARED IMPORTED)
set_target_properties(rawengine PROPERTIES
    IMPORTED_LOCATION     "path/to/output/bin/rawengine.dll"
    IMPORTED_IMPLIB       "path/to/output/lib/rawengine.lib"
)

add_executable(MyApp main.cpp)
target_link_libraries(MyApp rawengine)
```

**MSVC 命令行示例：**

```bat
cl main.c ^
    /I "path\to\output\include" ^
    "path\to\output\lib\rawengine.lib" ^
    /Fe:MyApp.exe
```

**运行时部署：**  
将 `bin/rawengine.dll` 及 `bin/` 中所有运行时 DLL 与你的程序放到同一目录。

---

### 方式二：MinGW/GCC 工程静态链接

将 `lib/librtengine.a` 静态链接到 MinGW 工程中。

**CMake 示例：**

```cmake
cmake_minimum_required(VERSION 3.15)
project(MyApp)

include_directories("path/to/output/include")

add_library(rtengine STATIC IMPORTED)
set_target_properties(rtengine PROPERTIES
    IMPORTED_LOCATION "path/to/output/lib/librtengine.a"
)

add_executable(MyApp main.cpp)
target_link_libraries(MyApp rtengine)
```

**GCC/MinGW 命令行示例：**

```bash
g++ main.cpp \
    -I path/to/output/include \
    -L path/to/output/lib \
    -lrtengine \
    -o MyApp.exe
```

### 方式三：MinGW/GCC 工程动态链接

```bash
g++ main.cpp \
    -I path/to/output/include \
    -L path/to/output/lib \
    -lrawengine \
    -o MyApp.exe
```

### 方式四：使用 rawengine-cli 批处理工具

`bin/rawengine-cli.exe` 可直接从命令行调用：

```bash
rawengine-cli.exe [input_dir] [output_dir]
```

- 递归扫描 `input_dir` 中的所有 RAW 文件
- 每个文件解码为 RGBA 后写出 JPEG 至 `output_dir`
- 输出 `[Timer]` 耗时和 `[Monitor]` CPU/内存监控信息

---

## C 代码示例

```c
#include "raw_engine.h"
#include <stdio.h>
#include <stdlib.h>

int main() {
    // 1. 初始化
    if (rawengine_init() != 0) {
        fprintf(stderr, "rawengine_init failed\n");
        return 1;
    }

    // 2. 设置镜头参数（AUTO 模式）
    RawEngineLensParams lens;
    rawengine_lens_params_default(&lens);

    // 3. 解码 RAW 文件
    void* buf = NULL;
    int   len, w, h;
    int ret = rawengine_decode(
        "photo.CR2",
        &buf, &len, &w, &h,
        RAWENGINE_PREVIEW_TYPE_2K,
        &lens
    );

    if (ret != 0) {
        fprintf(stderr, "rawengine_decode failed: %d\n", ret);
        return 1;
    }

    printf("Decoded: %dx%d, %d bytes (RGBA)\n", w, h, len);

    // 4. 使用 buf（RGBA 内存，w*h*4 字节）
    // ...

    // 5. 释放
    rawengine_free(buf);
    return 0;
}
```

---

## 运行时资源部署

`resource/` 目录包含 rawengine 在运行时加载的色彩校准和相机参数数据，**集成时必须随程序一起部署**。

### 资源说明

| 目录/文件 | 用途 | 缺少时的影响 |
|---|---|---|
| `dcpprofiles/` | 相机 DCP 色彩校准（600+ 款相机） | 色彩还原偏差 |
| `iccprofiles/` | ICC 输入/输出色彩空间 | 色彩管理失效 |
| `profiles/` | 处理预设 (.pp3) | 无法加载默认处理参数 |
| `camconst.json` | 相机传感器参数（白电平/黑电平等） | 部分相机曝光异常 |
| `cammatrices.json` | 相机色彩矩阵 | 色彩矩阵退化为通用值 |
| `dcraw.json` / `rt.json` | 相机数据库补充 | 相机识别能力降低 |
| `languages/default` | 内部消息语言包 | 错误信息显示乱码 |

### 部署方式

将 `resource/` 目录放到与 `rawengine.dll` 同级目录：

```
MyApp/
├── MyApp.exe
├── rawengine.dll
├── *.dll              ← 其他运行时 DLL（来自 bin/）
└── resource/          ← 资源目录（来自 output/resource/）
    ├── dcpprofiles/
    ├── iccprofiles/
    ├── profiles/
    ├── languages/
    └── *.json
```

rawengine 初始化时会自动从 dll 同级 `resource/` 目录加载资源。

---

## 运行时 DLL 部署

`bin/` 目录中包含运行所需的全部 DLL（来自 MSYS2 ucrt64 工具链）。
部署时将 `bin/` 中的所有文件放到可执行文件同目录，或将该目录加入系统 `PATH`。

主要依赖包括：
- `libraw_r.dll` — LibRaw RAW 解码
- `libexiv2*.dll` — EXIF 元数据读写
- `liblensfun.dll` — 镜头畸变数据库
- `liblcms2*.dll` — ICC 色彩管理
- `libgtkmm-3.0*.dll` 及 GTK+ 链 — GUI 工具支持
- `libfftw3f*.dll` — 频域降噪
- GCC 运行时（`libgcc_s_seh-1.dll`、`libstdc++-6.dll`、`libwinpthread-1.dll`）

---

## 调试

`pdb/` 目录存放 DWARF 格式的调试符号文件（MinGW 不生成 MSVC .pdb）：

```bash
# GDB 加载调试符号
gdb rawengine-cli.exe
(gdb) symbol-file pdb/rawengine-cli.exe.debug
```

`pdb/librtengine_symbols.txt` 包含静态库的全部导出符号列表，方便查阅可用接口。

---

## 构建环境

| 组件 | 版本 |
|---|---|
| 工具链 | MSYS2 ucrt64 / GCC 15.x |
| CMake | 3.15+ |
| LibRaw | 系统 ucrt64 包（0.22.x） |
| RawTherapee | dev branch |

重新构建请运行 `installer/build.bat`（Windows）或 `installer/build.sh`（MSYS2 shell）。
