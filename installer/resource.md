# resource 目录资源说明

本文档说明 `installer/output/resource` 目录下各资源文件（目录）的作用、当前结构与缺失影响，方便打包与排障。

## 当前资源结构（已更新）

当前 `resource/` 目录包含以下顶层内容：

- `dcpprofiles/`
  - 大量相机 DCP（如 Canon/Nikon/Sony/Fujifilm 等）
  - `camera_model_aliases.json`
- `external/`
  - 额外 DCP（通常是标准风格或补充版本）
- `iccprofiles/`
  - `input/`
  - `output/`
- `profiles/`
  - 顶层 `.pp3`（如 `Auto-Matched Curve - ISO Low.pp3`）
  - `Non-raw/`
  - `Pixel Shift/`
  - `Pop/`
- `languages/`
  - `default`
- `camconst.json`
- `cammatrices.json`
- `dcraw.json`
- `rt.json`

## 各资源作用

### `dcpprofiles/`

- **作用**：相机 DCP 色彩配置（双光源标定、色调曲线、色彩映射等）。
- **用途**：提高不同机型 RAW 的色彩还原准确性。
- **缺失影响**：颜色可能偏差，机型间一致性下降。

### `external/`

- **作用**：补充 DCP 目录，通常放置标准风格或第三方/扩展版本 DCP。
- **用途**：在默认 DCP 之外提供可选或更细分的色彩映射。
- **缺失影响**：不会导致引擎无法运行，但可能失去部分机型的替代色彩配置。

### `iccprofiles/`

- **作用**：ICC 色彩配置资源。
- **结构**：
  - `iccprofiles/input`：输入侧（相机/素材侧）ICC。
  - `iccprofiles/output`：输出色彩空间 ICC（如 RTv4_sRGB、Rec2020 等）。
- **用途**：保证完整色彩管理链路（输入到输出）。
- **缺失影响**：色彩管理异常，导出结果可能出现明显色偏。

### `profiles/`

- **作用**：处理预设（`.pp3`），决定解码后的默认处理行为。
- **当前包含**：
  - 顶层基础预设（Auto-Matched / Standard Film Curve / Unclipped 等）
  - 分类子目录：`Non-raw/`、`Pixel Shift/`、`Pop/`
- **缺失影响**：默认处理流程退化，风格/参数可能回落到兜底值。

### `languages/default`

- **作用**：程序内部文案与提示信息默认语言资源。
- **用途**：本地化展示与文案回退。
- **缺失影响**：文本可能回退、缺失或显示异常。

### `camconst.json`

- **作用**：相机传感器常量数据库（黑电平、白电平、裁剪等）。
- **用途**：RAW 线性化、曝光基线与传感器级校正。
- **缺失影响**：部分机型出现曝光、高光或底噪处理异常。

### `cammatrices.json`

- **作用**：相机色彩矩阵数据库（相机空间到标准色彩空间变换）。
- **用途**：提供基础颜色变换能力。
- **缺失影响**：颜色映射精度下降，可能退化到通用矩阵。

### `dcraw.json`

- **作用**：dcraw/LibRaw 侧相机支持补充数据。
- **用途**：增强相机识别与机型参数覆盖。
- **缺失影响**：边缘机型或新机型兼容性下降。

### `rt.json`

- **作用**：RawTherapee 内部补充参数数据库。
- **用途**：补足处理策略与机型相关参数。
- **缺失影响**：部分处理路径行为不完整或效果退化。

## 打包建议（按当前结构）

- 若追求稳定与一致性，建议完整携带 `resource/` 全量内容。
- 最低建议保留：
  - `dcpprofiles/`
  - `external/`
  - `iccprofiles/`
  - `profiles/`
  - `languages/default`
  - `camconst.json`
  - `cammatrices.json`
  - `dcraw.json`
  - `rt.json`
- 对色彩精度要求较高时，不建议移除 `dcpprofiles/`、`external/`、`iccprofiles/`。
