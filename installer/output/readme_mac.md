# rawengine macOS 集成说明

## 概述

`rawengine` 是基于 [RawTherapee](https://www.rawtherapee.com/) 引擎封装的 C 语言接口库，
提供工业级 RAW 图像解码能力。通过 9 个简洁的 C 函数对外暴露完整的处理流水线（LibRaw 解码、
降噪、色彩管理、镜头校正等）。

**主要特性：**

- 支持 600+ 款相机的 RAW 格式（CR2/CR3/NEF/ARW/RAF/DNG 等）
- 可选输出分辨率：原始尺寸、3K、2K 预览
- 自动 / 手动镜头畸变校正（Lensfun 数据库）
- C 接口，可从 C/C++/Swift/Objective-C 等语言调用
- 返回 RGBA 8-bit 内存缓冲区，可直接送入 Metal / OpenGL 等图形管线

---

## 目录结构

```
output/
├── include/                     # C API 头文件（arm/intel 共享）
│   ├── raw_engine.h             # 主头文件（所有 API 声明）
│   ├── rawengine_types.h        # 数据结构定义
│   └── raw_engine_error_def.h   # 错误码枚举
├── dylib/
│   ├── arm/                     # Apple Silicon (arm64) 产物
│   │   ├── rawengine.dylib      # ★ 主动态库（arm64）
│   │   ├── rawengine-cli        # 命令行工具（arm64）
│   │   └── *.dylib              # 运行时依赖（来自 /opt/homebrew）
│   └── intel/                   # Intel (x86_64) 产物
│       ├── rawengine.dylib      # ★ 主动态库（x86_64）
│       ├── rawengine-cli        # 命令行工具（x86_64）
│       └── *.dylib              # 运行时依赖（来自 /usr/local）
├── dsym/
│   ├── arm/                     # arm64 调试符号包
│   │   ├── rawengine.dylib.dSYM
│   │   └── rawengine-cli.dSYM
│   └── intel/                   # x86_64 调试符号包
│       ├── rawengine.dylib.dSYM
│       └── rawengine-cli.dSYM
├── resource/                    # ★ 运行时色彩/相机资源（arm/intel 共享）
│   ├── dcpprofiles/             # 相机 DCP 色彩校准文件（600+ 款相机）
│   ├── iccprofiles/             # ICC 色彩空间文件（input/output）
│   ├── profiles/                # 处理预设（.pp3）
│   ├── languages/default        # 默认语言包
│   ├── camconst.json            # 相机传感器参数数据库
│   ├── cammatrices.json         # 相机色彩矩阵
│   ├── dcraw.json               # LibRaw/dcraw 相机数据库
│   └── rt.json                  # RawTherapee 内部参数
└── readme_mac.md                # 本文档
```

---

## 构建方式

在 `installer/` 目录下提供以下脚本：

| 脚本 | 架构 | 产物 |
|------|------|------|
| `build_arm.sh` | arm64 (Apple Silicon) | `dylib/arm/` + `dsym/arm/` |
| `build_intel.sh` | x86_64 (Intel) | `dylib/intel/` + `dsym/intel/` |
| `build_arm_gui.sh` | arm64 | `output_gui_arm/`（完整 GUI 应用） |
| `build_intel_gui.sh` | x86_64 | `output_gui_intel/`（完整 GUI 应用） |

**构建 ARM 动态库：**

```bash
cd installer/
bash build_arm.sh [--jobs N] [--skip-build] [--clean]
```

**参数说明：**

- `--jobs N`：并行编译线程数（默认为 CPU 核心数）
- `--skip-build`：跳过编译，仅重新收集已有产物
- `--clean`：构建前清空 build 目录和旧产物

**构建前提（ARM，通过 Homebrew 安装）：**

```bash
brew install cmake ninja pkg-config
brew install libraw lensfun exiv2 lcms2 fftw librsvg
brew install gtk+3 gtkmm3 cairomm libsigc++ glibmm fmt
```

---

## API 参考

所有函数声明在 `include/raw_engine.h`。

### 初始化

```c
int rawengine_init();
```

**必须在调用任何其他函数之前调用一次。**
返回 `0` 表示成功，非零表示失败。

---

### 解码 RAW 文件

```c
int rawengine_decode(
    const char*               filename,     // RAW 文件路径（UTF-8）
    void**                    buffer,       // [OUT] RGBA 像素数据
    int*                      length,       // [OUT] buffer 字节数
    int*                      width,        // [OUT] 图像宽度（像素）
    int*                      height,       // [OUT] 图像高度（像素）
    int                       preview_type, // RAWENGINE_PREVIEW_TYPE_*
    const RawEngineLensParams* lens_params  // 镜头校正参数，传 NULL 使用默认值
);
```

**preview_type 取值：**

- `RAWENGINE_PREVIEW_TYPE_ORIGIN` (0)：原始分辨率
- `RAWENGINE_PREVIEW_TYPE_3K` (1)：长边缩放至 3072px
- `RAWENGINE_PREVIEW_TYPE_2K` (2)：长边缩放至 2048px

输出 `buffer` 为连续 RGBA 字节数组，每像素 4 字节，行优先（row-major）。

---

### 释放缓冲区

```c
int rawengine_free(void* buffer);
```

释放 `rawengine_decode` 返回的 `buffer`，**不可使用 `free()` 直接释放**。

---

### 镜头参数

```c
void rawengine_lens_params_default(RawEngineLensParams* params);

typedef struct RawEngineLensParams {
    int  lens_mode;            // RAWENGINE_LENS_MODE_NONE / AUTO / MANUAL
    int  use_distortion;       // 1=启用畸变校正
    const char* camera_make;   // MANUAL 模式：相机厂商，如 "Canon"
    const char* camera_model;  // MANUAL 模式：相机型号，如 "EOS 5D Mark IV"
    const char* lens_name;     // MANUAL 模式：镜头名，如 "Canon EF 50mm f/1.4 USM"
} RawEngineLensParams;
```

---

### 查询相机/镜头数据库

```c
int rawengine_get_cameras(RawEngineCameraInfo** cameras, int* count);
int rawengine_free_cameras(RawEngineCameraInfo* cameras, int count);

int rawengine_get_lenses(RawEngineLensInfo** lenses, int* count);
int rawengine_free_lenses(RawEngineLensInfo* lenses, int count);
```

---

### 自动识别镜头

```c
int rawengine_detect_lens(const char* filename, int* camera_index, int* lens_index);
```

---

## 集成方式

### Xcode 工程集成（推荐）

1. 将 `dylib/arm/rawengine.dylib` 加入 Xcode Target 的 **Frameworks, Libraries, and Embedded Content**，设为 **Embed & Sign**。
2. 将 `dylib/arm/` 中其余 `*.dylib` 也加入并 Embed（或放到 `@rpath` 可找到的位置）。
3. 在 **Build Settings → Header Search Paths** 中添加 `path/to/output/include`。
4. 将 `resource/` 目录整体复制到 App Bundle 内（Build Phase → Copy Files，设目标为 Resources）。
5. 在 `rawengine_init()` 之前通过环境变量或 `rawengine_set_datadir()` 指定资源路径：

```objc
// Objective-C 示例
NSString *resourcePath = [NSBundle.mainBundle.resourcePath
                          stringByAppendingPathComponent:@"resource"];
setenv("RAWENGINE_DATADIR", resourcePath.UTF8String, 1);
rawengine_init();
```

### CMake 工程集成

```cmake
cmake_minimum_required(VERSION 3.15)
project(MyApp)

# 头文件
include_directories("path/to/output/include")

# 导入 rawengine dylib
add_library(rawengine SHARED IMPORTED)
set_target_properties(rawengine PROPERTIES
    IMPORTED_LOCATION "path/to/output/dylib/arm/rawengine.dylib"
)

add_executable(MyApp main.cpp)
target_link_libraries(MyApp rawengine)

# 确保 dylib 目录在 rpath 中
set_target_properties(MyApp PROPERTIES
    BUILD_RPATH "path/to/output/dylib/arm"
)
```

### C/C++ 命令行示例

```bash
clang main.c \
    -I path/to/output/include \
    -L path/to/output/dylib/arm \
    -lrawengine \
    -Wl,-rpath,@executable_path/../Frameworks \
    -o MyApp
```

---

## 代码示例

```c
#include "raw_engine.h"
#include <stdio.h>

int main() {
    // 1. 初始化（指定资源目录）
    setenv("RAWENGINE_DATADIR", "/path/to/resource", 1);
    if (rawengine_init() != 0) {
        fprintf(stderr, "rawengine_init failed\n");
        return 1;
    }

    // 2. 设置镜头参数（AUTO 模式）
    RawEngineLensParams lens;
    rawengine_lens_params_default(&lens);

    // 3. 解码 RAW 文件
    void* buf  = NULL;
    int   len, w, h;
    int ret = rawengine_decode(
        "photo.CR3",
        &buf, &len, &w, &h,
        RAWENGINE_PREVIEW_TYPE_2K,
        &lens
    );
    if (ret != 0) {
        fprintf(stderr, "rawengine_decode failed: %d\n", ret);
        return 1;
    }

    // 4. 使用 buf（RGBA 内存，w * h * 4 字节）
    printf("Decoded: %dx%d, %d bytes (RGBA)\n", w, h, len);

    // 5. 释放
    rawengine_free(buf);
    return 0;
}
```

---

## 运行时资源部署

`resource/` 目录包含 rawengine 运行时加载的色彩校准和相机参数数据，**集成时必须随程序一起部署**。

| 目录/文件 | 用途 | 缺少时的影响 |
|---|---|---|
| `dcpprofiles/` | 相机 DCP 色彩校准（600+ 款相机） | 色彩还原偏差 |
| `iccprofiles/` | ICC 输入/输出色彩空间 | 色彩管理失效 |
| `profiles/` | 处理预设（.pp3） | 无法加载默认参数 |
| `camconst.json` | 相机传感器参数（白/黑电平等） | 部分相机曝光异常 |
| `cammatrices.json` | 相机色彩矩阵 | 色彩矩阵退化为通用值 |
| `dcraw.json` / `rt.json` | 相机数据库补充 | 相机识别能力降低 |
| `languages/default` | 内部消息语言包 | 错误信息显示异常 |

**推荐目录结构（App Bundle）：**

```
MyApp.app/
└── Contents/
    ├── MacOS/
    │   └── MyApp
    ├── Frameworks/
    │   ├── rawengine.dylib      ← dylib/arm/rawengine.dylib
    │   └── *.dylib              ← dylib/arm/ 中其余依赖
    └── Resources/
        └── resource/            ← output/resource/ 整体复制
            ├── dcpprofiles/
            ├── iccprofiles/
            ├── profiles/
            ├── languages/
            └── *.json
```

---

## 调试

`dsym/arm/` 和 `dsym/intel/` 目录中存放 Apple dSYM 格式调试符号包，供 lldb / Instruments 使用：

```bash
# lldb 加载 dSYM
lldb ./MyApp
(lldb) add-dsym path/to/dsym/arm/rawengine.dylib.dSYM

# 或设置符号路径让 lldb 自动查找
(lldb) settings set target.debug-file-search-paths path/to/dsym/arm
```

使用 `rawengine-cli` 命令行工具快速验证：

```bash
# ARM 版
./dylib/arm/rawengine-cli [input_dir] [output_dir]
```

递归扫描 `input_dir` 中的所有 RAW 文件，解码并输出 JPEG 到 `output_dir`。

---

## 构建环境

| 组件 | ARM (Apple Silicon) | Intel |
|------|--------------------|----|
| 工具链 | Clang (Xcode) | Clang (Xcode) |
| Homebrew | `/opt/homebrew` | `/usr/local` |
| CMake | `/opt/homebrew/bin/cmake` | `/usr/local/bin/cmake` |
| 最低 macOS | 12.0 (Monterey) | 10.15 (Catalina) |
| LibRaw | Homebrew 系统包 | Homebrew 系统包 |

重新构建：

```bash
bash installer/build_arm.sh          # ARM 动态库
bash installer/build_intel.sh        # Intel 动态库
bash installer/build_arm_gui.sh      # ARM 完整 GUI
bash installer/build_intel_gui.sh    # Intel 完整 GUI
```
