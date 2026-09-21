# LibRaw 版本 pin 记录

## 当前 pin
- 上游：https://github.com/LibRaw/LibRaw
- 分支：master（0.22 系列开发快照）
- **Pinned commit：`116924e5bcb9483ef9c5fbe7f3cfc5a8a4b97064`**
- 落地位置：`raw/RawTherapee/rtengine/libraw/`（vendored 源码，非 submodule）
- 替换日期：2026-09-21，替换前为 LibRaw 0.22.2 Release

## 为什么用 master 快照而不是 0.22 正式版
- 需要解码 **Sony A7 V / ILCE-7M5** 的 RAW。
- LibRaw 0.22 Release / 0.22.1 / 0.22.2 **均不含 A7 V**；A7 V 的完整支持（无损 + Compressed/Compressed HQ 有损）是在 0.22 正式版之后的 master 开发快照才加入。
- 已实测该 commit 能完整解码 ILCE-7M5 的「Compressed HQ 有损」ARW（`raw-identify` 正确识别、`dcraw_emu` 出全尺寸图；RawTherapee `rawengine-cli` 走完整管线出 6996×4664 JPEG）。

## 升级/替换步骤（vendored 源码）
1. `curl -sSL -o lr.tar.gz https://github.com/LibRaw/LibRaw/archive/<commit>.tar.gz && tar xzf lr.tar.gz`
2. 用干净解压树替换 `raw/RawTherapee/rtengine/libraw/` 全部内容。
3. **务必**给脚本加可执行权限，否则 configure 时版本号为空导致 libtool 链接失败：
   `chmod +x raw/RawTherapee/rtengine/libraw/*.sh`（尤其 `version.sh`、`shlib-version.sh`）。
4. **重新套用 RT 私有补丁**（见下）：`cd raw/RawTherapee/rtengine/libraw && patch -p1 < ../../patches/rt_hasselblad_correct.patch`。
5. 删除构建目录里的旧拷贝（如 `build_arm/rtengine/libraw`）后重新配置、构建。

## RT 私有补丁（整体替换 vendored LibRaw 时必须重新套用！）
upstream LibRaw（含 master 与任何正式版）**从不包含**这些，是 RawTherapee 从 dcraw 移植的功能补丁，
整体替换 LibRaw 会把它们冲掉，必须重新套用：
- `patches/rt_hasselblad_correct.patch` —— 哈苏（Hasselblad）3FR 平场/增益校正
  （`hasselblad_correct()` / `parse_hasselblad_gain()` + `levels/unknown1/flatfield` 字段 + makernote 0x0019 钩子 + raw2image 调用）。
  不套用 → 哈苏 RAW 会缺少平场校正（暗角/亮度不均不被修正）。
  当前 pin `116924e5` 上已用 `patch -p1` 干净套用、编译通过、ILCE-7M5 解码验证正常。
- 说明：RT 当年在 0.22.2 上私加的相机（ILCE-1M2/A1 II、X2D II 100C、DMC-TZ82 等）在此 master 快照里**已自带**，无需回搬。
- `rtengine/libraw/configure.ac` 的 autotools 兼容改动（整体替换 LibRaw 时会被冲掉，需重新套用）——
  在 MSYS2 的 `autoconf 2.73` + pkgconf `pkg.m4` 下，上游 configure.ac 会构建失败，已就地改：
  - OpenMP 段：用核心宏 `AC_OPENMP` 取代 autoconf-archive 的 `AX_OPENMP`
    （`AX_OPENMP` 2024.10 用 `m4_default([$1],…)` 包裹 action，把里面的 `AC_SUBST`/`AC_MSG_WARN`
    再引用一层 → "overquoted macro" → autoreconf 失败）。
  - zlib / lcms 段：`PKG_CHECK_MODULES` 的 action 参数只传纯 shell 变量（`have_xxx=yes/no`），
    把 `AC_SUBST`/`AC_MSG_WARN` 移到宏外的 `if` 里（本机 pkg.m4 不 rescan action 参数里的宏，
    会把它们原样漏进 `configure`）。
  - 这些是构建期兼容修，不改 LibRaw 解码行为，跨平台安全（mac 亦可）。

## 各平台重建备注（踩过的坑）
- **mac arm64**：`build_arm`（`/opt/homebrew`，Ninja）→ `therapee.dylib`；拷进 `mac/arm64/`，把 `/opt/homebrew/*` 依赖 `install_name_tool -change` 回 `@rpath/<name>` 再 adhoc 重签。
- **mac intel (x86_64)**：`build_intel`（`/usr/local` Intel Homebrew，Ninja）。**注意**：LibRaw autotools 子构建靠 pkg-config 找 lcms2，默认会命中 **arm64 的 `/opt/homebrew` lcms2** 导致 `ld: ignoring ... arm64, required x86_64` 链接失败。重建时必须显式指向 Intel 的 lcms2：
  `PKG_CONFIG_PATH=/usr/local/opt/little-cms2/lib/pkgconfig:/usr/local/lib/pkgconfig`（configure 与 build 都带上）。
  产物拷进 `mac/intel/`，同样把 `/usr/local/*`+`/opt/homebrew/*` 依赖改 `@rpath` 再重签。
- **iOS (ios-arm64 device)**：`installer/build_ios.sh --device-only`（依赖 `installer/output_ios_deps/`，由 `build_ios_deps.sh` 预先交叉编译）。产物 `libtherapee.a` 直接覆盖
  `mac/ios/RawEngine.xcframework/ios-arm64/libtherapee.a`（静态库，无需 @rpath/签名）。
- **Windows**：需在 Windows 工具链下重建 `therapee.dll` / `therapee.lib`（本机无法交叉构建）。
  - 用 MSYS2 **ucrt64**（本机在 `C:\pack\app\code\msys2`）。入口 `installer/build.bat` → `installer/build.sh`。
  - **必须让 build 走 vendored（pinned+patch）LibRaw，而不是系统 libraw**：`build.sh` 会在
    `pkg-config --exists 'libraw_r>=0.21'` 命中系统包时自动 `WITH_SYSTEM_LIBRAW=ON`；MSYS2 装了
    `mingw-w64-ucrt-x86_64-libraw`（0.22.1，`libraw_r-25.dll`，不含 A7 V / 哈苏补丁）就会误用。
    已给 `build.sh` 加环境变量开关，构建时显式：`WITH_SYSTEM_LIBRAW=OFF`。
  - `WITH_SYSTEM_LIBRAW=OFF` 时 `cmake/Dependencies.cmake` 把 vendored `libraw_r.a` **静态**链进
    `therapee.dll`（约 12.1MB），**不再依赖也不再随附 `libraw_r-*.dll`**。
  - 还需：`export MSYSTEM=UCRT64`（`LibRaw.cmake` 用 `sh -l -c "./configure"`，登录 shell 会按
    MSYSTEM 重置 PATH，不设则丢掉 `/ucrt64/bin`，gcc 找不到 as/ld → "C compiler cannot create executables"）。
  - 依赖：`pacman -S autoconf-archive`（LibRaw `configure.ac` 的 `AX_OPENMP` 需要；否则 autoreconf 失败）。
  - 产物在 `installer/output/{bin,lib}`；替换到 `PhotoEditor/src/3rdparty/extra/rawtherapee/windows/{bin,lib}`
    时只覆盖 `*.dll` + `therapee.lib`，并删掉旧的 `libraw_r-25.dll`（静态化后不再需要）。

## 下游同步（重要）
- PhotoEditor 通过 `src/3rdparty/extra/rawtherapee/{mac/arm64,mac/intel,mac/ios,windows}` 里的
  预编译 RawEngine（`therapee.dylib` / `libtherapee.a` / `therapee.lib`）消费本引擎。
  升级 LibRaw 后，**这些各平台产物都要用同一 pin 重新构建并替换**，应用侧才会生效。
- `image_coder_sdk.cpp` 的机型白名单 `is_vehicle_match` 需同步加入对应机型（如 `ILCE-7M5`），
  否则即使 LibRaw 能解，仍会走内嵌预览而不进 RAW 解码。
