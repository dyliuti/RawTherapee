// main-cli.cc — RawTherapee rawengine-cli: minimal [Timer] and [Monitor] output

#include "../raw_engine.h"

#include <iostream>
#include <vector>
#include <string>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <cstdio>
#include <cerrno>
#include <jpeglib.h>
#include <glib.h>
#include <glibmm/convert.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

// ===== 参数 =====
#ifndef INPUT_DIR
#define INPUT_DIR  "."
#endif

// Windows UTF-8 helpers for filesystem paths.
#ifdef _WIN32
static std::wstring utf8_to_wide(const std::string& utf8) {
    if (utf8.empty()) return std::wstring();
    int wide_len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (wide_len <= 0) return std::wstring();
    std::wstring wide(static_cast<size_t>(wide_len), L'\0');
    int result = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &wide[0], wide_len);
    if (result <= 0) return std::wstring();
    if (!wide.empty() && wide.back() == L'\0') wide.pop_back();
    return wide;
}

static std::string normalize_path_utf8(const std::string& path) {
    if (path.empty()) return path;
    if (g_utf8_validate(path.c_str(), -1, nullptr)) {
        return path;
    }
    try {
        return Glib::locale_to_utf8(path);
    } catch (...) {
        return path;
    }
}
#endif

#ifndef OUTPUT_DIR
#define OUTPUT_DIR "."
#endif

#ifndef MAX_THREADS
#define MAX_THREADS 1
#endif

// ===== Windows 依赖（仅用于资源监控）=====
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#define PSAPI_VERSION 2           // 映射到 Kernel32 的 K32GetProcessMemoryInfo（无需链接 psapi.lib）
#include <psapi.h>
#elif defined(__APPLE__)
#include <sys/types.h>
#include <sys/sysctl.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <unistd.h>
#include <libproc.h>
#include <sys/resource.h>
#endif

// ===== 全局 =====
static std::vector<std::string> g_file_paths;
static std::mutex g_mtx;
static std::condition_variable g_cv;
static std::atomic<int> g_active_threads(0);
static std::string g_output_path = OUTPUT_DIR;
static int g_max_threads = (MAX_THREADS <= 0 ? 1 : MAX_THREADS);

// ===== 时间戳 =====
static inline std::string now_ts() {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto ms  = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    std::time_t t = system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S")
        << '.' << std::setw(3) << std::setfill('0') << ms.count();
    return oss.str();
}

// ===== 资源监控 =====
struct MonitorSnapshot {
    double cpu_percent;      // 0~100, 当前进程 CPU 占比（归一到单核=100%）
    size_t working_set_mb;   // 工作集（包含共享页），更接近"正在使用的物理内存"
    size_t private_mb;       // 私有提交（Private Bytes），更接近"该进程独占的内存"
};

#ifdef _WIN32
static inline MonitorSnapshot sample_monitor() {
    // CPU：当前进程 vs 系统
    static bool inited = false;
    static ULONGLONG last_sys_kernel = 0, last_sys_user = 0;
    static ULONGLONG last_proc_kernel = 0, last_proc_user = 0;
    static int cpu_count = []{ SYSTEM_INFO si{}; GetSystemInfo(&si); return (int)si.dwNumberOfProcessors; }();

    FILETIME ft_sys_idle, ft_sys_kernel, ft_sys_user;
    FILETIME ft_create, ft_exit, ft_kernel, ft_user;
    GetSystemTimes(&ft_sys_idle, &ft_sys_kernel, &ft_sys_user);
    GetProcessTimes(GetCurrentProcess(), &ft_create, &ft_exit, &ft_kernel, &ft_user);

    auto to_ull = [](const FILETIME& ft)->ULONGLONG {
        return ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    };
    ULONGLONG sys_kernel = to_ull(ft_sys_kernel);
    ULONGLONG sys_user   = to_ull(ft_sys_user);
    ULONGLONG proc_kernel= to_ull(ft_kernel);
    ULONGLONG proc_user  = to_ull(ft_user);

    double cpu_percent = 0.0;
    if (inited) {
        ULONGLONG sys_total_delta  = (sys_kernel - last_sys_kernel) + (sys_user - last_sys_user);
        ULONGLONG proc_total_delta = (proc_kernel - last_proc_kernel) + (proc_user - last_proc_user);
        // 归一到"单核=100%"
        if (sys_total_delta > 0) {
            cpu_percent = 100.0 * (double)proc_total_delta / (double)sys_total_delta * (double)cpu_count;
            if (cpu_percent < 0) cpu_percent = 0;
            if (cpu_percent > 100.0) cpu_percent = 100.0;
        }
    }
    last_sys_kernel = sys_kernel; last_sys_user = sys_user;
    last_proc_kernel= proc_kernel; last_proc_user= proc_user; inited = true;

    // 内存：当前进程
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    SIZE_T ws_mb = 0, pv_mb = 0;
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
        ws_mb = pmc.WorkingSetSize / (1024 * 1024);   // 工作集（包含共享）
        pv_mb = pmc.PrivateUsage   / (1024 * 1024);   // 私有提交（独占为主）
    }
    return MonitorSnapshot{ cpu_percent, (size_t)ws_mb, (size_t)pv_mb };
}
#elif defined(__APPLE__)
class ProcessCPUUsage {
private:
    pid_t pid;
    uint64_t lastTime;
    uint64_t lastSystemTime;

public:
    ProcessCPUUsage() : pid(getpid()) {
        reset();
    }

    void reset() {
        lastTime = getCurrentTime();
        lastSystemTime = getSystemTime();
    }

    double getUsage() {
        uint64_t currentTime = getCurrentTime();
        uint64_t currentSystemTime = getSystemTime();

        if (currentTime == lastTime || currentSystemTime == lastSystemTime) {
            return 0.0;
        }

        double usage = (double)(currentSystemTime - lastSystemTime) * 100.0 /
                      (double)(currentTime - lastTime);

        lastTime = currentTime;
        lastSystemTime = currentSystemTime;

        return usage;
    }

private:
    uint64_t getCurrentTime() {
        return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    }

    uint64_t getSystemTime() {
        struct proc_taskinfo pti;
        int size = proc_pidinfo(pid, PROC_PIDTASKINFO, 0, &pti, sizeof(pti));
        if (size <= 0) {
            return 0;
        }
        return pti.pti_total_user + pti.pti_total_system;
    }
};

static inline MonitorSnapshot sample_monitor() {
    // 获取内存使用情况
    task_basic_info_data_t info;
    mach_msg_type_number_t count = TASK_BASIC_INFO_COUNT;
    kern_return_t kr = task_info(mach_task_self(), TASK_BASIC_INFO, (task_info_t)&info, &count);
    MonitorSnapshot snapshot;
    snapshot.cpu_percent = 0.0;
    snapshot.private_mb = 0;
    if (kr != KERN_SUCCESS) {
        std::cerr << "Failed to get task info: " << kr << std::endl;
        return snapshot;
    }
    snapshot.working_set_mb = info.resident_size / 1024 / 1024;

    // 获取CPU使用情况
    ProcessCPUUsage cpuMonitor;
    snapshot.cpu_percent = cpuMonitor.getUsage();
    return snapshot;
}
#else
static inline MonitorSnapshot sample_monitor() {
    // 非 Windows/macOS 简易占位
    return MonitorSnapshot{ -1.0, 0, 0 };
}
#endif

static inline void print_monitor() {
    const auto s = sample_monitor();
    if (s.cpu_percent >= 0.0) {
        std::ostringstream oss; oss.setf(std::ios::fixed); oss << std::setprecision(4) << s.cpu_percent;
        std::cout << now_ts()
                  << " [Monitor] CPU: " << oss.str()
                  << "%, Mem(WS): " << s.working_set_mb << " MB"
                  << ", Mem(Private): " << s.private_mb << " MB\n";
    } else {
        std::cout << now_ts() << " [Monitor] CPU: N/A, Memory: N/A\n";
    }
}

// ===== 工具 =====
static inline bool has_suffix(const std::string& path, const std::string& suffix) {
    if (path.length() < suffix.length()) return false;
    return g_ascii_strcasecmp(path.c_str() + path.length() - suffix.length(), suffix.c_str()) == 0;
}

static void traverse(const std::string& path) {
    GDir* dir = g_dir_open(path.c_str(), 0, nullptr);
    if (!dir) return;
    const gchar* name;
    while ((name = g_dir_read_name(dir)) != nullptr) {
        if (g_strcmp0(name, ".") == 0 || g_strcmp0(name, "..") == 0) continue;
        std::string full = path + "/" + name;
        if (g_file_test(full.c_str(), G_FILE_TEST_IS_DIR)) traverse(full);
        else if (g_file_test(full.c_str(), G_FILE_TEST_IS_REGULAR)) {
            if (!has_suffix(full, ".xmp") && !has_suffix(full, ".pp3")) g_file_paths.push_back(full);
        }
    }
    g_dir_close(dir);
}

inline bool write_jpeg_from_buffer(
    const unsigned char* data, int width, int height, int channels,
    const char* out_path,
    int quality = 92,
    int dpi = 300,
    bool force_444 = false  // 需要更锐利色彩时可设 true（禁用 4:2:0）
) {
    if (!data || !out_path || width <= 0 || height <= 0) return false;
    if (channels != 3 && channels != 4) return false;

    FILE* fp = nullptr;
#ifdef _WIN32
    std::wstring wide_path = utf8_to_wide(out_path ? out_path : "");
    if (!wide_path.empty()) {
        fp = _wfopen(wide_path.c_str(), L"wb");
    } else {
        fp = std::fopen(out_path, "wb");
    }
#else
    fp = std::fopen(out_path, "wb");
#endif
    if (!fp) return false;

    jpeg_error_mgr jerr;
    jpeg_compress_struct cinfo;
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);
    jpeg_stdio_dest(&cinfo, fp);

    cinfo.image_width      = width;
    cinfo.image_height     = height;
    cinfo.input_components = 3;        // 目标写入为 3 通道
    cinfo.in_color_space   = JCS_RGB;

    jpeg_set_defaults(&cinfo);

    // 写入 JFIF 像素密度（DPI）
    cinfo.density_unit = 1; // 1=每英寸
    cinfo.X_density    = dpi;
    cinfo.Y_density    = dpi;

    jpeg_set_quality(&cinfo, quality, TRUE);

    // 可选：强制 4:4:4（默认大多是 4:2:0）
    if (force_444) {
        jpeg_set_colorspace(&cinfo, JCS_YCbCr);
        for (int i = 0; i < cinfo.num_components; ++i) {
            cinfo.comp_info[i].h_samp_factor = 1;
            cinfo.comp_info[i].v_samp_factor = 1;
        }
    }

    jpeg_start_compress(&cinfo, TRUE);

    JSAMPROW row_pointer[1];
    const int src_stride = width * channels;
    std::vector<unsigned char> row_rgb; // 仅在 RGBA 时使用
    if (channels == 4) row_rgb.resize(width * 3);

    while (cinfo.next_scanline < cinfo.image_height) {
        const unsigned char* src = data + static_cast<size_t>(cinfo.next_scanline) * src_stride;
        if (channels == 3) {
            row_pointer[0] = const_cast<JSAMPROW>(src);
        } else { // RGBA -> RGB（丢 Alpha）
            unsigned char* dst = row_rgb.data();
            for (int x = 0; x < width; ++x) {
                *dst++ = src[0]; *dst++ = src[1]; *dst++ = src[2];
                src += 4;
            }
            row_pointer[0] = row_rgb.data();
        }
        jpeg_write_scanlines(&cinfo, row_pointer, 1);
    }

    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    std::fclose(fp);
    return true;
}

// ===== 核心处理 =====
static void process_file(const std::string& file_path, int index) {
    void* buffer = nullptr;
    int length = 0, width = 0, height = 0;

    std::string local_path;
#ifdef _WIN32
    local_path = file_path;
#else
    try {
        local_path = Glib::locale_from_utf8(file_path);
    } catch (...) {
        return; // 转换失败直接跳过
    }
#endif

    auto t0 = std::chrono::high_resolution_clock::now();

    // 原图
    int ret = rawengine_decode(local_path.c_str(), &buffer, &length, &width, &height, 0, nullptr);

    // 3K 图（可选）
    // int ret = rawengine_decode(local_path.c_str(), &buffer, &length, &width, &height, RAWENGINE_PREVIEW_TYPE_3K, nullptr);

    // 2K 图（可选）
    // int ret = rawengine_decode(local_path.c_str(), &buffer, &length, &width, &height, RAWENGINE_PREVIEW_TYPE_2K, nullptr);

    auto t1 = std::chrono::high_resolution_clock::now();
    auto ms_decode = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    std::cout << now_ts() << " [Timer] rawengine_decode preprocess cost " << 0 << " ms\n";
    std::cout << now_ts() << " [Timer] rawengine_decode infer cost " << ms_decode << " ms\n";
    std::cout << now_ts() << " [Timer] rawengine_decode postprocess cost " << 0 << " ms\n";

    print_monitor();

    if (ret != 0 || buffer == nullptr) {
        return; // 解码失败，直接返回
    }

    // === 写出 JPEG ===
    {
        char* base = g_path_get_basename(file_path.c_str());
        std::string base_str(base ? base : ""); g_free(base);
        size_t pos = base_str.find_last_of('.');
        std::string stem = (pos == std::string::npos) ? base_str : base_str.substr(0, pos);
        std::string out_path = g_output_path + "/" + stem + ".jpg";

        std::cout << now_ts() << " [Image] write -> " << out_path
                  << " | size = " << width << "x" << height << "\n";

        auto t2 = std::chrono::high_resolution_clock::now();
        bool ok = write_jpeg_from_buffer(
            reinterpret_cast<const unsigned char*>(buffer),
            width, height,
            /*channels*/4,
            out_path.c_str(),
            /*quality*/90,
            /*dpi*/300,
            /*force_444*/false
        );
        auto t3 = std::chrono::high_resolution_clock::now();
        auto ms_encode = std::chrono::duration_cast<std::chrono::milliseconds>(t3 - t2).count();
        std::cout << now_ts() << " [Timer] jpeg encode cost " << ms_encode << " ms\n";
        (void)ok;
        print_monitor();
    }

    rawengine_free(buffer);
}

// ===== 入口 =====
int main(int argc, char** argv) {
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);

    // 让 cout 每次 << 都自动刷新
    std::cout.setf(std::ios::unitbuf);

#ifdef _WIN32
    if (!GetConsoleWindow()) {
        if (!AttachConsole(ATTACH_PARENT_PROCESS)) AllocConsole();
        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
    }

    std::string input_dir = normalize_path_utf8(INPUT_DIR);
    g_output_path = normalize_path_utf8(g_output_path);
#else
    std::string input_dir = INPUT_DIR;
#endif

    // 支持命令行覆盖 input/output 目录
    if (argc >= 2) {
        input_dir = argv[1];
    }
    if (argc >= 3) {
        g_output_path = argv[2];
    }

    // 初始化 & 遍历
    rawengine_init();
    traverse(input_dir);

    int idx = 0;
    for (const auto& file_path : g_file_paths) {
        std::unique_lock<std::mutex> lock(g_mtx);
        g_cv.wait(lock, [](){ return g_active_threads.load() < g_max_threads; });

        g_active_threads++;
        std::thread([file_path, &idx]() {
            process_file(file_path, ++idx);
            g_active_threads--;
            g_cv.notify_one();
        }).detach();
    }

    // 等待全部完成
    {
        std::unique_lock<std::mutex> lock(g_mtx);
        g_cv.wait(lock, [](){ return g_active_threads.load() == 0; });
    }

    return 0;
}
