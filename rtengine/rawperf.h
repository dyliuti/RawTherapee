/*
 *  rawperf.h — 解码链路阶段耗时打点（性能分析用）
 *
 *  设计目标：
 *    - 默认关闭，零开销；通过环境变量按需开启，不影响正常解码路径。
 *    - 日志行统一以 "rawperf" 开头，便于 grep 过滤与脚本聚合。
 *    - 落盘到文件而非 stdout/stderr，避免与 CLI 输出解析互相干扰。
 *
 *  环境变量：
 *    RAWENGINE_PERF=1              开启打点
 *    RAWENGINE_PERF_LOG=<path>     指定日志路径（默认 rawengine_perf.log）
 *
 *  日志格式（单行，key=value）：
 *    rawperf file=<basename> stage=<stage> cost_ms=<double> [k=v ...]
 */
#pragma once

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <utility>


namespace rawperf
{

using Clock = std::chrono::steady_clock;

inline bool enabled()
{
    static const bool on = []() -> bool {
        const char* e = std::getenv("RAWENGINE_PERF");
        return e && e[0] != '\0' && e[0] != '0';
    }();
    return on;
}

inline const std::string& log_path()
{
    static const std::string p = []() -> std::string {
        const char* e = std::getenv("RAWENGINE_PERF_LOG");
        return (e && e[0] != '\0') ? std::string(e) : std::string("rawengine_perf.log");
    }();
    return p;
}

inline std::mutex& mutex_ref()
{
    static std::mutex m;
    return m;
}

inline double ms_since(const Clock::time_point& from)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - from).count();
}

// 只取文件名，避免日志里塞满长路径
inline std::string basename_of(const std::string& path)
{
    const size_t pos = path.find_last_of("/\\");
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

// 日志是 "key=value 空格分隔" 的单行格式，值里若含空白会破坏解析。
// RT 的算法名形如 "amaze+bilinear" / "3-pass (best)"，因此统一把空白替换成下划线。
inline std::string sanitize(std::string v)
{
    for (char& c : v) {
        if (c == ' ' || c == '\t') {
            c = '_';
        }
    }
    return v;
}


inline void log_stage(const std::string& file, const char* stage, double cost_ms,
                      const std::string& extra = std::string())
{
    if (!enabled()) {
        return;
    }
    std::lock_guard<std::mutex> lk(mutex_ref());
    if (FILE* f = std::fopen(log_path().c_str(), "a")) {
        std::fprintf(f, "rawperf file=%s stage=%s cost_ms=%.3f%s%s\n",
                     basename_of(file).c_str(), stage, cost_ms,
                     extra.empty() ? "" : " ", extra.c_str());
        std::fclose(f);
    }
}

// 无耗时的附加信息行（如记录本次使用的 demosaic 算法）
inline void log_info(const std::string& file, const char* stage, const std::string& extra)
{
    log_stage(file, stage, 0.0, extra);
}

// RAII 计时器：作用域结束自动打点；也可提前 stop()
class Timer
{
public:
    Timer(std::string file, const char* stage)
        : _file(std::move(file)), _stage(stage), _start(Clock::now()), _stopped(false)
    {
    }

    ~Timer()
    {
        stop();
    }

    void stop(const std::string& extra = std::string())
    {
        if (_stopped) {
            return;
        }
        _stopped = true;
        log_stage(_file, _stage, ms_since(_start), extra);
    }

    double elapsed_ms() const
    {
        return ms_since(_start);
    }

    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;

private:
    std::string _file;
    const char* _stage;
    Clock::time_point _start;
    bool _stopped;
};

} // namespace rawperf
