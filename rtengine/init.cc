/*
 *  This file is part of RawTherapee.
 *
 *  Copyright (c) 2004-2010 Gabor Horvath <hgabor@rawtherapee.com>
 *
 *  RawTherapee is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  RawTherapee is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with RawTherapee.  If not, see <https://www.gnu.org/licenses/>.
 */
#include <fftw3.h>
#include <glibmm/miscutils.h>
#include <glibmm/ustring.h>
#include "color.h"
#include "rtengine.h"
#include "iccstore.h"
#include "dcp.h"
#include "camconst.h"
#include "curves.h"
#include "rawimagesource.h"
#include "improcfun.h"
#include "improccoordinator.h"
#include "dfmanager.h"
#include "ffmanager.h"
#include "rtthumbnail.h"
#include "profilestore.h"
#include "rtgui/threadutils.h"
#include "rtlensfun.h"
#include "metadata.h"
#include "procparams.h"

#ifdef __ANDROID__
#include <android/log.h>
#define RTE_INIT_SECTION_WARN(sec) \
    __android_log_print(ANDROID_LOG_WARN, "RTE_INIT", "init section '%s' failed (optional resource skipped)", sec)
#else
#define RTE_INIT_SECTION_WARN(sec) ((void)0)
#endif

// 在 OpenMP 并行区（omp sections）内抛出的 C++ 异常不允许跨越并行区边界，
// 否则运行时会直接 std::terminate -> abort。init() 里的各 store 初始化
// （lensfun / icc / dcp / 暗场 / 平场等）在资源目录缺失时可能抛异常，
// 因此每个 section 内部各自捕获：这些资源均为可选，失败只记日志、不影响
// 基础 RAW 解码，也避免并行区逃逸异常导致进程崩溃。
#define RTE_INIT_SECTION(sec, ...)         \
    do {                                   \
        try { __VA_ARGS__ }                \
        catch (...) {                      \
            RTE_INIT_SECTION_WARN(sec);    \
        }                                  \
    } while (0)

namespace rtengine
{

const Settings* settings;

MyMutex* lcmsMutex = nullptr;
MyMutex *fftwMutex = nullptr;
MyMutex *librawMutex = nullptr;

int init (const Settings* s, const Glib::ustring& baseDir, const Glib::ustring& userSettingsDir, bool loadAll)
{
    settings = s;
#ifdef __ANDROID__
    // 诊断：引擎实际使用的资源根（iccprofiles/lensfun/camconst 都在它下面查）。
    // 若这里不是 AppDataLocation/resources/rtresources，说明 RT_RESOURCE_ROOT 没传进来。
    __android_log_print(ANDROID_LOG_WARN, "RTE_INIT", "engine init baseDir='%s'", baseDir.c_str());
#endif
    ProcParams::init();
    PerceptualToneCurve::init();
    RawImageSource::init();

#ifdef _OPENMP
#pragma omp parallel sections if (!settings->verbose)
#endif
{
#ifdef _OPENMP
#pragma omp section
#endif
RTE_INIT_SECTION("lensfun", {
    bool ok;

    if (s->lensfunDbDirectory.empty() || Glib::path_is_absolute(s->lensfunDbDirectory)) {
        ok = LFDatabase::init(s->lensfunDbDirectory);
    } else {
        ok = LFDatabase::init(Glib::build_filename(baseDir, s->lensfunDbDirectory));
    }

    if (!ok && !s->lensfunDbBundleDirectory.empty() && s->lensfunDbBundleDirectory != s->lensfunDbDirectory) {
        if (Glib::path_is_absolute(s->lensfunDbBundleDirectory)) {
            LFDatabase::init(s->lensfunDbBundleDirectory);
        } else {
            LFDatabase::init(Glib::build_filename(baseDir, s->lensfunDbBundleDirectory));
        }
    }
});
#ifdef _OPENMP
#pragma omp section
#endif
RTE_INIT_SECTION("profilestore", {
    ProfileStore::getInstance()->init(loadAll);
});
#ifdef _OPENMP
#pragma omp section
#endif
RTE_INIT_SECTION("iccstore", {
    ICCStore::getInstance()->init(s->iccDirectory, Glib::build_filename (baseDir, "iccprofiles"), loadAll);
});
#ifdef _OPENMP
#pragma omp section
#endif
RTE_INIT_SECTION("dcpstore", {
    DCPStore::getInstance()->init(Glib::build_filename (baseDir, "dcpprofiles"), loadAll);
});
#ifdef _OPENMP
#pragma omp section
#endif
RTE_INIT_SECTION("camconst", {
    CameraConstantsStore::getInstance()->init(baseDir, userSettingsDir);
});
#ifdef _OPENMP
#pragma omp section
#endif
RTE_INIT_SECTION("darkframes", {
    DFManager::getInstance().init(s->darkFramesPath);
});
#ifdef _OPENMP
#pragma omp section
#endif
RTE_INIT_SECTION("flatfields", {
    ffm.init(s->flatFieldsPath);
});
}

    Color::init ();
    Exiv2Metadata::init();

    delete lcmsMutex;
    lcmsMutex = new MyMutex;
    fftwMutex = new MyMutex;
    delete librawMutex;
    librawMutex = new MyMutex;
    return 0;
}

void cleanup ()
{
    Exiv2Metadata::cleanup();
    ProcParams::cleanup ();
    Color::cleanup ();
    RawImageSource::cleanup ();

#ifdef RT_FFTW3F_OMP
    fftwf_cleanup_threads();
#else
    fftwf_cleanup();
#endif

}

StagedImageProcessor* StagedImageProcessor::create (InitialImage* initialImage)
{

    ImProcCoordinator* ipc = new ImProcCoordinator ();
    ipc->assign (initialImage->getImageSource ());
    return ipc;
}

void StagedImageProcessor::destroy (StagedImageProcessor* sip)
{

    delete sip;
}

Settings* Settings::create  ()
{

    return new Settings;
}

void Settings::destroy (Settings* s)
{

    delete s;
}


}

