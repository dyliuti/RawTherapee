# 原生 arm64 Session API smoke test

## V5 reference working-noise 工程映射

新增独立 `rte_raw_session_get_reference_noise_model`，Session ABI=5、reference
struct version=1；legacy API=2、V1 linear/V3 noise/V4 geometry 布局均不变。
caller 设置 `struct_size`，较大结构的 tail 不写；非法 handle/短结构返回 -1，
在给定容量内清零已知字段（保留 struct_size）。成功 fallback 仍返回 0。

数值 contract（不是物理标定）：

- 四通道按 R,G1,B,G2；`normalized_a=A/65535`，`normalized_b=B/65535²`。
  对名义归一化参考信号 x 使用 `Var(x)≈normalized_a*x+normalized_b`。
- `gain[c]=preprocessingScale[c]*renderMultiplier[RGB(c)]*
  (decoderWhite[c]-decoderBlack[c])/65535`，绿色 gain 是 G1/G2 算术均值。
- `camera_to_working=M*diag(gainR,mean(gainG1,gainG2),gainB)`，M 取实际执行的
  direct matrix 颜色转换；输入和输出均为名义 unit-range（linear view /65535）。
- `stage3_gain=1`，WB、baseline exposure 等已在 renderMultiplier/map 内，不能再乘 WB。
- `reference_usable` 独立于严格物理 calibrated/working valid；后两者仍为 false。
  这是三通道 reference approximation，**不是精确 CFA/demosaic covariance**，
  不证明局部处理、NR、几何变换后的实际像素噪声分布。
- 仅 direct matrix、renderSkip=1、未缩放 retained canvas 可用。DCP/ICC、
  非有限/退化矩阵、非法黑白电平/gain fail closed，但保留 normalized reference。
  Preview 空间缩放单独报告 `resolution_scale`/`spatially_resampled`；不伪造
  downsample 方差修正，发生 resize 的 preview 返回 reference_usable=0。
- snapshot 来自 open-time render 与 conversion 合并证据，不依赖已 flush 的源数据；
  finish（含编辑）不改变 snapshot。hash 使用显式字段 FNV-1a，包含全部 normalized
  参数、map、gain、原始观测、完整 profile identity、native/retained size、scale、stage，
  不 hash struct padding 或指针。hash 非加密摘要，不表示物理标定质量。

独立测试（人工 fixture，不链接引擎）：

```sh
clang++ -std=c++11 -O1 -g -Wall -Wextra -Wpedantic -Werror \
  -fsanitize=address,undefined -fno-sanitize-recover=all \
  raw/RawTherapee/tests/reference_working_noise_test.cpp \
  -o /tmp/photoeditor-rte-session-build/reference_working_noise_test
/tmp/photoeditor-rte-session-build/reference_working_noise_test
```

真实 FULL/preview 仍使用下文 smoke 命令；测试已升级为检查 V5 独立 reference
ABI、数值 contract 和 edit/restore snapshot/hash 稳定性。FULL 对已匹配且 direct
matrix 的 Canon EOS R 要求 noncalibrated reference_usable=1；缩放 preview 要求拒绝。
不接入 rawNR GPU，不修改相机 profile 匹配，不修改 PhotoEditor/extra/Minimal。

2026-10-27 V5 本机验证：reference synthetic ASAN/UBSAN **107 checks**，
既有 raw_noise_model ASAN/UBSAN **1072 checks**，camera profile regression
与 source geometry **27 checks** 通过。公共头 C11 和 smoke C++17 严格警告编译通过。
构建 `bash /tmp/photoeditor-rte-session-build/build-session.sh` 退出 0，原生 arm64；
仍有既有 mismatched-tags/VLA 和依赖高于 macOS 12 链接目标版本的警告，非跨平台认证。

真实 `IMG_6771.CR3`（Canon EOS R、ISO 100），FULL V1/source smoke 退出 0：
两者 retained=4490×6734、reference_usable=1、working_model_valid=0、
sensor_calibration_valid=0、stage3_gain=1。normalized A（R,G1,B,G2）：
`[0.000013745326925,0.000014363317311,0.000016781872282,0.000014363317311]`；
normalized B：`[0.000000012228639,0.000000025607496,0.000000033195678,0.000000025607496]`。
gain：`[1.943465064132941,1.000062938064285,1.442348391483911,1.000062965512297]`。
合成 reference map（行优先）：

```text
 1.962983069479546 -0.169934853934221  0.230604453653246
 0.014985684822895  1.410361200070863 -0.602877573022745
 0.003557790293192 -0.325275210368583  1.908838740859702
```

V1 snapshot hash=`dbc4d0d2c392ffb6`，source=`72a0897c5858f193`（stage 参与 hash）。
重复/null/original、+0.5EV 编辑后恢复、非法输入后恢复均通过，reference snapshot
逐字节稳定。所有无编辑输出对 legacy `changed_bytes=0 / max_abs=0`；RGBA
hash=`b9569efdec654fcf`，linear hash=`10b7de72397df1b3`。
FULL 日志：`/Users/yanminwei/.comate-engine/store/terminals/xykgqy.output`；
构建日志：`/Users/yanminwei/.comate-engine/store/terminals/vpg6rh.output`。
初次 preview 测试因测试内残留 `api_version()==4` 检查在 init 前退出 1；已改为
头文件版本常量并重新编译运行，不改引擎路径或放宽数值判据。
最终 preview V1/source 三档均退出 0：2K=1334×2000（scale≈0.297）、
3K=2000×3000（≈0.4455）、4K=2667×4000（≈0.594001）；均保留 normalized
reference，但 `reference_usable=0 / spatially_resampled=1`，reason 为
`spatial_resampling_not_modelled`。所有 repeat/edit/restore 的 reference snapshot/hash
稳定，临时输入隐藏后 finish 通过。V1 对 legacy 三档均零字节差异；source 本样本
也零差异，仅作诊断，不推广为所有规则 parity。
最终 preview 日志：`/Users/yanminwei/.comate-engine/store/terminals/yqcbmg.output`。

## V4 独立 preview smoke

`RTE_RAW_SESSION_API_VERSION=4`，legacy API 仍为 2；linear view 和 V3 noise
struct 不扩展。新增 `rte_raw_session_get_dimensions`（caller 设置 struct_size），
报告 early resize 前的 full oriented native canvas、retained canvas 和空间缩放因子。
Native 不是 masked sensor array；缩放因子不表示 sensor 噪声标定或工作 RGB 噪声传播。
最终输出尺寸读取 finish 的 image buffer；V2 保留 full scaled canvas，输出可能再裁剪。

```sh
arch -arm64 clang++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror \
  -I raw/RawTherapee/rtengine \
  raw/RawTherapee/tests/rte_session_preview_smoke.cpp \
  /tmp/photoeditor-rte-session-build/rtengine/libtherapee.dylib \
  -Wl,-rpath,/tmp/photoeditor-rte-session-build/rtengine \
  -o /tmp/photoeditor-rte-session-build/rte_session_preview_smoke
arch -arm64 /tmp/photoeditor-rte-session-build/rte_session_preview_smoke \
  IMG_6771.CR3 raw/RawTherapee/installer/output_arm/resource
```

独立 smoke 对真实 RAW 的 2K/3K/4K 逐个测试：V1 与同 scale legacy decode
逐字节比较；V2 与 legacy 仅报告差异，不要求 parity。两种模式均检查尺寸、重复
null/original finish、+0.5EV、null/original restore、retained hash 和 immutable geometry，
并覆盖 geometry oversized tail / undersized / invalid handle 安全性。
测试复制 RAW 到临时目录，open 后隐藏副本才 finish，原始文件不修改；证明 finish
不依赖再次读取原文件。Session fallback EXIF crop 的证据也在 open 时缓存。

V1 preview 精确复用 init → transform → earlyresize → denoise → retain；finish
继承 resize 后参数，绝不重新套用 native crop/resize。FULL 顺序不变。
V2 preview 为 init(no legacyNR) → toWorking → earlyresize → retain；finish 才执行
baseops → finish。earlyresize 保留完整画布，暂不裁剪，恢复 axis-aligned scaled crop；
不恢复 resize.enabled，避免重复 resize。**不承诺复杂几何已正确缩放**：当前首版
对 `needsTransform`（含手动畸变/旋转/透视/非单位缩放、镜头配置及空间效果）、framing、
locallab、upscale、缩放后退化/越界 crop 显式返回 `-2`。EXIF/coarse 方向已在 init
应用，仍支持。未被规则转为显式 crop 的 EXIF fallback 在 source preview 也返回 `-2`，
因为旧 fallback 是 native 像素 margin 算法，不是通用 fractional geometry 映射。
失败 open 清空 handle，不静默改走 V1；V1/FULL 的既有几何及 fallback 不改变。
V2 的重排、Lab resize 和 scaled geometry **不保证 legacy preview byte parity**。
此 smoke 样本不能替代所有镜头、非平凡畸变/旋转、locallab 规则的视觉几何验证。

### Source preview 边界回归

小 crop 的 upscale 怀疑不适用于当前公开 open：规则执行后强制
`allowUpscaling=false`，`ResizeParams` 默认 `dataspec=3`（FitBox）和
`appliesTo="Cropped area"`；真实 `resizeScale` 在 FitBox 分支将比例钳到 1。
例如 6000×4000 画布、1000×1000 crop、2K 请求保留 6000×4000 canvas，输出
1000×1000，而非放大两倍。不能为此改动 V1 或 legacy fast export。
内部其它 dataspec 或允许 upscale 的参数不享受该证明；source preview 显式拒绝比例 >1。

纯几何测试不读 RAW、不链接引擎；包含 portrait/oriented canvas、偏移 crop、非整数
舍入、scale=1、小 crop、退化/越界和 unsupported 组合。以下从工作区根目录运行：

```sh
clang++ -std=c++11 -O1 -g -Wall -Wextra -Wpedantic -Werror \
  -fsanitize=address,undefined -fno-sanitize-recover=all \
  -I raw/RawTherapee/rtengine raw/RawTherapee/tests/source_preview_geometry_test.cpp \
  -o /tmp/source_preview_geometry_test
/tmp/source_preview_geometry_test
```

引擎 fixture 调用真实 `resizeScale`，不复制算法；也检查 rotation/distortion/perspective
被 `needsTransform` 识别。额外 warning 豁免仅针对既有引擎头的 unused parameter / class-struct
前置声明差异，不修改生产头以消除这些告警：

```sh
arch -arm64 clang++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror \
  -Wno-unused-parameter -Wno-mismatched-tags -DRTE_GEOMETRY_ENGINE_FIXTURES \
  -I raw/RawTherapee/rtengine -I raw/RawTherapee -I /tmp/photoeditor-rte-session-build \
  $(/opt/homebrew/bin/pkg-config --cflags glibmm-2.4 giomm-2.4 cairomm-1.0) \
  raw/RawTherapee/tests/source_preview_geometry_test.cpp \
  /tmp/photoeditor-rte-session-build/rtengine/libtherapee.dylib \
  $(/opt/homebrew/bin/pkg-config --libs glibmm-2.4 giomm-2.4 cairomm-1.0) \
  -Wl,-rpath,/tmp/photoeditor-rte-session-build/rtengine \
  -o /tmp/photoeditor-rte-session-build/source_preview_geometry_engine_test
/tmp/photoeditor-rte-session-build/source_preview_geometry_engine_test
```

公共 API fixture 生成临时 64×48 TIFF，写入 EXIF DefaultCrop，检查 source preview
拒绝并清空哨兵 handle、V1 与 legacy 逐字节 parity。V1 open 后删除临时输入，再重复
finish 验证 fallback 已缓存。这是人工 fixture，不是相机 RAW 几何认证，不修改原始样图。

```sh
arch -arm64 clang++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror \
  -I raw/RawTherapee/rtengine $(/opt/homebrew/bin/pkg-config --cflags exiv2 libtiff-4) \
  raw/RawTherapee/tests/source_preview_fallback_test.cpp \
  /tmp/photoeditor-rte-session-build/rtengine/libtherapee.dylib \
  $(/opt/homebrew/bin/pkg-config --libs exiv2 libtiff-4) \
  -Wl,-rpath,/tmp/photoeditor-rte-session-build/rtengine \
  -o /tmp/photoeditor-rte-session-build/source_preview_fallback_test
/tmp/photoeditor-rte-session-build/source_preview_fallback_test \
  raw/RawTherapee/installer/output_arm/resource
```

同一 fixture 可加 `-DRTE_GEOMETRY_ENGINE_FIXTURES` 编译成 processor 集成测试。
只 load 一次人工 TIFF，以 `flush=false` 复用 InitialImage 创建多份 job，验证小 crop
保留原 canvas、非整数比例的偏移 crop 输出，以及实际 processor 对上述 unsupported
组合的 `-2` 拒绝路径（测试复用不能使用会清空源像素的 `flush=true`）：

```sh
arch -arm64 clang++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror \
  -Wno-unused-parameter -Wno-mismatched-tags -DRTE_GEOMETRY_ENGINE_FIXTURES \
  -I raw/RawTherapee/rtengine -I raw/RawTherapee -I /tmp/photoeditor-rte-session-build \
  $(/opt/homebrew/bin/pkg-config --cflags glibmm-2.4 giomm-2.4 cairomm-1.0 exiv2 libtiff-4) \
  raw/RawTherapee/tests/source_preview_fallback_test.cpp \
  /tmp/photoeditor-rte-session-build/rtengine/libtherapee.dylib \
  $(/opt/homebrew/bin/pkg-config --libs glibmm-2.4 giomm-2.4 cairomm-1.0 exiv2 libtiff-4) \
  -Wl,-rpath,/tmp/photoeditor-rte-session-build/rtengine \
  -o /tmp/photoeditor-rte-session-build/source_preview_pipeline_test
/tmp/photoeditor-rte-session-build/source_preview_pipeline_test \
  raw/RawTherapee/installer/output_arm/resource
```

2026-10-27 本轮实测：纯几何 ASAN/UBSAN **27 checks**、链接真实引擎的几何
fixture **40 checks**、公共 EXIF fallback fixture 及 processor 集成 fixture 均退出 0。
最终增量 rebuild 和真实 `IMG_6771.CR3` preview 退出 0：native 4490×6734，
2K/3K/4K retained/output 分别为 1334×2000、2000×3000、2667×4000。
V1 三档对 legacy `changed_bytes=0 / max_abs=0`；V1/source 均通过重复、
edit/restore、immutable geometry/hash 和隐藏 open 输入后的 finish。
source 此样本也零差异，但不扩展成所有规则 parity 声明。
构建仍有既有 Homebrew dylib 最低系统版本高于 macOS 12 目标的链接警告；
验证限当前原生 arm64 主机。未替换安装库、未部署、未修改 PhotoEditor extra。
最终 rebuild/preview 日志：`/Users/yanminwei/.comate-engine/store/terminals/pwxgaj.output`；
fixture 集成日志：`/Users/yanminwei/.comate-engine/store/terminals/ju7r1a.output`。
最新重新编译的 FULL V1/source smoke 也退出 0，包含 V3 noise snapshot、非法输入和
重复/恢复断言：`/Users/yanminwei/.comate-engine/store/terminals/9aapc9.output`。

2026-10-27 原生 arm64 实测 `IMG_6771.CR3`：独立构建及两个 smoke 的
C++17 `-Wall -Wextra -Wpedantic -Werror` 编译通过。native 为 4490×6734；
2K=1334×2000、3K=2000×3000、4K=2667×4000。三档 V1 对 legacy 均
`changed_bytes=0 / max_abs=0 / mean_abs=0`；V1/V2 三档均通过 repeat、
edit/restore 和隐藏 open 输入副本后的 finish。此样本 V2 诊断差异也为零，
但不能据此推导重排 clean pipeline 对其他规则的 legacy parity。
FULL V1/V2 回归退出 0，仍为 4490×6734，RGBA FNV-1a `b9569efdec654fcf`，
包含 V3 noise snapshot 稳定性与非法输入恢复。
构建日志：`/Users/yanminwei/.comate-engine/store/terminals/s1ywi5.output`；
preview 日志：`/Users/yanminwei/.comate-engine/store/terminals/3ux7bw.output`；
最终含 geometry 结构安全断言的 preview 重跑退出 0：
`/Users/yanminwei/.comate-engine/store/terminals/cyz5fv.output`；
FULL 日志：`/Users/yanminwei/.comate-engine/store/terminals/08krpf.output`。

## 目录惯例

新增前已检查：工程没有 `tests/` 目录或顶层 CMake 测试目标。
已有 `installer/test_rawengine.py` 是独立调用 CLI 的批量解码脚本。
本测试沿用独立运行方式，使用 C++17，不修改生产 CMake、不使用 Minimal、不替换 PhotoEditor extra。

## 构建与运行

从包含 `raw/` 和 `IMG_6771.CR3` 的工作区根目录执行。
依赖已构建的原生 arm64 `libtherapee.dylib` 及其 Homebrew 动态依赖。
输出二进制放在已有的临时构建目录，不替换生产安装目录中的库。
修改引擎后先运行指定的增量构建脚本：

```sh
bash /tmp/photoeditor-rte-session-build/build-session.sh
```

```sh
arch -arm64 clang++ -std=c++17 -O2 -Wall -Wextra -Wpedantic \
  -I raw/RawTherapee/rtengine \
  raw/RawTherapee/tests/rte_session_smoke.cpp \
  /tmp/photoeditor-rte-session-build/rtengine/libtherapee.dylib \
  -Wl,-rpath,/tmp/photoeditor-rte-session-build/rtengine \
  -o /tmp/photoeditor-rte-session-build/rte_session_smoke

arch -arm64 /tmp/photoeditor-rte-session-build/rte_session_smoke \
  IMG_6771.CR3 raw/RawTherapee/installer/output_arm/resource
```

不设置改变规则、histmatching 或去马赛克行为的环境覆盖；若调用环境已有覆盖，请先检查并记录。
测试不自动修改环境、不重试失败。退出码：0 全部通过；1 首个失败；2 参数错误。
每次调用前输出 `BEGIN`，调用后输出返回码与单调时钟耗时，便于定位崩溃或卡住的位置。

## 覆盖与判据

- 使用同一份清零的 FULL/default demosaic/no lens 选项，旧 `rte_decode` **仅一次**；
  V1 `rte_raw_session_open` 与 V2 `rte_raw_source_session_open` **各成功一次**，顺序运行、不并存。
  这三次 RAW 读取只用于验证对比，不是建议产品重复 decode。
- get 检查 RGBRGB float、尺寸、count、紧密行跨度、有限值，报告范围与 FNV-1a 64 位 hash。
  使用 RT 原生线性 ProPhoto 单位（名义 0..65535），不归一化、不截断。
- 旧 FULL 对 `finish(nullptr, 0)`；重复 null finish；原始 interleaved float finish 与重复 finish。
  所有无编辑比较要求 **最大差异=0、平均差异=0、changed_bytes=0**，不提供放宽阈值开关。
- 在唯一的 float 副本上做 **+0.5 EV**（乘 `2^0.5`，不是乘 0.5）；确认输出确实改变。
  再以原始 floats 和 null 路径 finish，要求回到相同 RGBA，借用 linear hash 不变。
- invalid count：null/nonzero、少一个、多一个、nonnull/zero；末尾 NaN；null out。
  非法输入要求 `-1`，有 out 时要求所有字段清零，使用非空哨兵防止“本来就是空”造成假通过。
  非法调用后再做原始 finish，确认 session 仍可用。
- 再次 get 检查借用指针、布局、hash 稳定；close；验证 RGBA 独立于 session 存活；close(nullptr)。
  不访问已关闭 handle，不把 double-close 当作支持的契约。
- 两种模式都验证 stage 查询（V1=POST_TRANSFORM，V2=PROPHOTO_SOURCE），finish 后 stage 不变；
  验证非法 open 的输出清空、非法 scale 拒绝、选项/镜头校验、非法 get/undersized view、Inf 拒绝。
  初始化前 open 必须返回 -2；64-bit view 的 V1 大小/字段偏移由编译期断言保护。
- NR 不接入、不偷偷开启：检查默认 enabled=0/luminance=0。
  `color_amount=25` 只是未接入 API 的 UI 默认值，不代表启用；当前 `raw_engine.cc`
  的 `set_dirpyr_denoise` 将 `dirpyrDenoise.enabled` 设为 false。
  `simpleprocess.cc` 的 `stage_denoise()` 只有在 `denoiseParams.enabled` 时才执行 RGB 去噪；
  因此日志出现 `stage_denoise` 阶段名本身不代表开启了 NR。
  公共 API 无内部 NR 状态查询，故这里不声称从像素结果独立证明内部参数。

所有输出报告真实 RGBA 尺寸、字节数、hash；比较报告 RGBA/RGB 平均绝对差、最大差异、变化字节数。
不一致时输出首个差异的 x/y/channel 和两侧值并立即退出。
优先核对规则、资源、裁剪/几何、工作色域、stage_transform 分界和后段状态复用；不要加容差掩盖问题。

## 内存

逐份处理 RGBA，只保留一个必要 baseline 和一个当前输出，输出后立即释放。
24MP 时两份 RGBA 共约 192MB；唯一可编辑 RGB float 副本约 288MB。
借用 linear 不另存完整副本，只保存 hash；引擎自身持有的 RAW、工作图及临时图不包含在这些数字里。
hash 用于诊断与借用数据稳定性；RGBA 正确性比较始终逐字节，而非仅比较 hash。

## V2 source 契约

`RTE_API_VERSION` 仍为 **2**；独立 `RTE_RAW_SESSION_API_VERSION` 增为 **2**。
动态加载的可选消费者先解析 `rte_raw_session_api_version` 并确认 >=2，
再解析 `rte_raw_source_session_open` 和 `rte_raw_session_get_stage`。
原 V1 open 仍选择 post-transform 工厂，`rte_linear_rgb_view` 布局不变。

Source 在 `stage_to_working_space` 后、working base operations 前保留数据；每次 finish
重新执行 base/geometry 和后段。Source view 是几何变换前尺寸，可能不同于输出尺寸。
它显式关闭 legacy dirpyr/RGB NR，但不关闭所有噪声相关工具。
WB、配置的 raw preprocess/capture sharpening/Retinex/spot removal、film-negative 和输入
profile/DCP 处理已发生，因此不是 untouched demosaic，也不是 pre-DCP-nonlinear 数据。
这份样图在旧 RT NR 关闭时要求对 decode **逐字节一致**，不声称所有规则等价。
内部 noise-model 没有 validated profiles 时不伪造有效模型，本次不增加复杂 noise ABI。

### 2026-10-26 V1 + V2 扩展实测

使用 `bash /tmp/photoeditor-rte-session-build/build-session.sh` 构建成功（脚本无执行权限，
直接调用曾返回 126，随后通过 bash 执行，未改脚本）。原生 arm64 库和测试二进制均由
`file` 确认；公共头 C11、测试 C++17 `-Wall -Wextra -Wpedantic -Werror` 和
`git diff --check` 均通过。动态加载实测 legacy API=2、Session API=2，两个新增符号可解析。
构建有 Homebrew 依赖最低系统版本高于目标 macOS 12 的既有链接警告；只验证当前主机运行。

同一进程运行真实 `IMG_6771.CR3`，一次 decode、V1/source 各成功 open 一次，**退出 0 / PASS**。
总墙钟 75.115 秒（含编译链接测试和所有比较；不是性能基准）。
未检测到上述 demosaic/histmatching/OMP/DYLD 环境覆盖，未改规则或放宽阈值。

| 项目 | V1 post-transform | V2 source |
|---|---|---|
| stage | 1 | 2 |
| open ms | 5858.823416 | 6528.077709 |
| 首次 null finish ms | 2120.371083 | 1544.991417 |
| 原始 floats 两次 finish | 全部逐字节一致 | 全部逐字节一致 |
| +0.5 EV 后原始/null 恢复 | 全部逐字节一致 | 全部逐字节一致 |
| 非法 count/NaN/Inf 后恢复 | -1、输出清空、恢复一致 | -1、输出清空、恢复一致 |
| 借用 view/hash、stage 稳定 | 通过 | 通过 |

两种模式所有无编辑比较均 `max_abs=0 / mean_abs=0 / changed_bytes=0`；
RGBA 为 4490×6734、120942640 字节、hash `b9569efdec654fcf`。
两种模式在此样图上的 linear 都是 90706980 floats、hash `10b7de72397df1b3`、
范围 `[-2165.055664062,71396.070312500]`；此样图不证明启用非平凡 base/geometry
规则时两种截点的像素差异，也不证明所有规则等价。
+0.5 EV 两种模式均确实改变输出：hash `8b5db7cb6448752e`，max_abs=59，
mean_abs_rgb=19.057772963，changed_bytes=90335159。

原始日志：`/Users/yanminwei/.comate-engine/store/terminals/dzn1oy.output`；
构建日志：`/Users/yanminwei/.comate-engine/store/terminals/rx2t6n.output`。

下面的执行环境和结果为先前 **V1-only** 记录，不是上述扩展测试的结果。

## 先前 V1 执行环境

2026-10-26：Apple clang 21.0.0（clang-2100.1.1.101），arm64-apple-darwin25.6.0，
macOS 26.6.2（25G83），16GiB 内存，8 个逻辑 CPU。
`file` 确认动态库和测试可执行文件均为 Mach-O arm64。
未检测到 `RAWENGINE_DEMOSAIC`、`RAWENGINE_NO_HISTMATCH`、`OMP_NUM_THREADS`、
`DYLD_LIBRARY_PATH`、`DYLD_INSERT_LIBRARIES` 环境覆盖。

### IMG_6771.CR3 实测结果

单次进程运行完成，退出码 **0 / PASS**；总墙钟时间 32.259 秒（含 hash、逐字节比较）。
旧 decode 一次，session open 一次，无重试。

- RGBA 与 linear 尺寸均为 **4490×6734**，实际约 **30.24MP**，不是 24MP。
- 每份 RGBA：120,942,640 字节；baseline FNV-1a：`b9569efdec654fcf`。
- Linear：90,706,980 个 float，行跨度 53,880 字节，总计 362,827,920 字节。
  数值范围 `[-2165.055664062, 71396.070312500]`，未截断负值/超白值。
  借用数据 hash：`10b7de72397df1b3`，全程稳定，再次 get 的指针与布局稳定。
- 此样图测试侧最高同时持有两份 RGBA 约 241.89MB，加一份可编辑 float 约 362.83MB；
  不含引擎内存，未测量进程峰值 RSS。

| 调用 | 耗时 ms | 结果 |
|---|---:|---|
| engine_init | 249.766833 | 0 |
| legacy_decode_FULL | 6312.382042 | 0 |
| session_open | 5426.403125 | 0 |
| get_linear | 0.003291 | 0 |
| finish_null_0 | 1547.560042 | 0 |
| finish_null_repeat | 1182.211709 | 0 |
| finish_original_interleaved | 1278.491583 | 0 |
| finish_original_repeat | 1224.558708 | 0 |
| finish_exposure_plus_0_5 | 1244.793250 | 0 |
| finish_original_after_edit | 1258.928375 | 0 |
| finish_null_after_edit | 1314.259542 | 0 |
| finish_original_after_invalid | 1239.872417 | 0 |
| session_close | 0.207334 | 0 |

旧 FULL 对 null、null 重复、原始 interleaved、原始重复、编辑后原始/空输入、
非法调用后原始输入，**全部 max_abs=0、mean_abs_rgba=0、mean_abs_rgb=0、changed_bytes=0**。
所有无编辑 RGBA hash 均为 `b9569efdec654fcf`，close 后 baseline 仍有效且 hash 相同。

+0.5 EV 的线性乘数为 `1.414213538`，RGBA hash 为 `8b5db7cb6448752e`。
相对基线最大绝对差 **59**，RGBA 平均差 **14.293329722**，RGB 平均差 **19.057772963**，
变化字节数 **90,335,159**。首个变化位于 `(x=0,y=0,R)`，170→195；这是预期编辑效果，不是失败。

四种 invalid count 均返回 `-1` 且 `out_empty=1`（耗时 0.001458–0.005791ms）。
末元素 NaN 返回 `-1`、`out_empty=1`，耗时 123.467208ms。
null out 返回 `-1`，耗时 0.001583ms。之后原始 finish 仍完全一致。
默认 NR `enabled=0 / luma=0 / color=25`；未启用 NR。`close(nullptr)` 返回 0。

原始运行日志（本机）：`/Users/yanminwei/.comate-engine/store/terminals/xw7xa8.output`。
以上为一次正确性 smoke test，不是冷/热缓存公平性能基准。

## raw_noise_model.h 独立 C++11 单元测试

`raw_noise_model_test.cpp` 只依赖标准库和该头文件，不链接引擎、不读取 RAW、
不修改生产 CMake。所有系数和相机标识都是**人工算术 fixture**，不是实测相机数据，
不证明任何真实相机已获授权或完成校准，也不证明 working RGB 噪声模型有效。
fixture 的审核标记仅描述测试内人为定义的来源及单位。

覆盖：

- `Model`、`SensorLookup` 缺省 invalid；有效 sensor lookup 不自动令 `Model` valid。
- 空记录集、授权审核缺失、空 license/provenance、未验证 decoded DN 单位均拒绝。
- 重复精确匹配（包括有效/无效记录混合）拒绝；make/model/mode/ISO 严格匹配，
  未知 mode/ISO 不回退、不插值、不外推，相邻可表示 ISO 也不视作相等。
- 四通道逐项检查 NaN/Inf、负系数、双零系数、非法黑白电平和有限电平相减溢出；
  合法纯 shot/纯 read 及有限正 ISO 边界不被误拒。
- 按 **R、G1、B、G2** 检查 `Var(x)=shot*x+read` 与 decoded **DN²** 方差，
  read 是方差截距而非标准差；DN 缩放两倍时方差缩放四倍。
  人工数值可用二进制精确表示，数值判据不使用容差。
- sample 的 NaN/Inf、有限极值、黑白裁剪边界及外侧拒绝，紧邻内侧接受；
  非法 channel、invalid lookup 和有限输入运算溢出拒绝，失败输出必须清零。

从工作区根目录执行以下命令。使用当前主机原生 `clang++`，不指定交叉编译目标；
二进制写入已有 `/tmp`，无需先构建 RawTherapee。检查不依赖 `assert`，定义 `NDEBUG`
仍会执行；首个失败输出行号和表达式并退出 1，全部通过退出 0。

```sh
clang++ -std=c++11 -O2 -Wall -Wextra -Wpedantic -Werror -pedantic-errors \
  raw/RawTherapee/tests/raw_noise_model_test.cpp \
  -o /tmp/raw_noise_model_test
/tmp/raw_noise_model_test

clang++ -std=c++11 -O1 -g -Wall -Wextra -Wpedantic -Werror -pedantic-errors \
  -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
  raw/RawTherapee/tests/raw_noise_model_test.cpp \
  -o /tmp/raw_noise_model_test_sanitized
ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  /tmp/raw_noise_model_test_sanitized
```

成功输出格式：`PASS raw_noise_model_test (... checks; synthetic fixtures only)`。
这是头文件契约测试，不覆盖引擎集成或真实相机校准质量。

### 2026-10-26 独立测试实测

Apple clang 21.0.0（clang-2100.1.1.101），目标 `arm64-apple-darwin25.6.0`。
以上严格 C++11 编译与 ASAN/UBSAN 联合编译、运行均退出 0，均输出：

```text
PASS raw_noise_model_test (663 checks; synthetic fixtures only)
```

无编译警告或 sanitizer 报告；`file` 确认两个可执行文件均为 Mach-O arm64。
本次仅通过 `apply_patch` 新增测试、追加本章节，未执行 git，未修改生产文件。
