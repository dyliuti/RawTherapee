// raw_engine.cc - RawTherapee adaptation of raw/RAWEngine/raw_engine.cc
// Ported to use RawTherapee's own libraw and App/Options infrastructure.

#include "raw_engine.h"

#ifdef __GNUC__
#if defined(__FAST_MATH__)
#error Using the -ffast-math CFLAG is known to lead to problems. Disable it to compile RawEngine.
#endif
#endif

#include "procparams.h"
#include "profilestore.h"
#include "raw_engine_error_def.h"
#include "rtengine.h"
#include "rtapp.h"
#include "rtgui/config.h"
#include "rtgui/options.h"
#include "rtgui/pathutils.h"
#include "rtgui/version.h"
#include <cstdlib>
#include <cstring>
#include <giomm.h>
#include <iostream>
#include <locale.h>
#include <tiffio.h>
#include "rtgui/paramsedited.h"

#ifndef _WIN32
#include <glib.h>
#include <glib/gstdio.h>
#include <glibmm/fileutils.h>
#include <glibmm/threads.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#else
// >>> 修正 winsock2.h 顺序开始
#include <winsock2.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
// <<< 修正 winsock2.h 顺序结束
#include "conio.h"
#include <glibmm/thread.h>
#include <shlobj.h>
#include <windows.h>
#endif

#include "rawimagesource.h"  // findInputProfile
#include "dcp.h"             // DCPProfile*
#include "rtlensfun.h"       // LFDatabase, LFCamera, LFLens
#include <lcms2.h>

#include <cmath>
#include <iomanip>
#include <string>
#include <vector>
#include <sstream>
#include <stdexcept>
#include <filesystem>
#include <algorithm>
#include <map>
#include <functional>
#include <vector>

#include <exiv2/exiv2.hpp>
#include <glibmm/ustring.h>
#include <libraw/libraw.h>

#include <chrono>
// Set this to 1 to make RT work when started with Eclipse and arguments, at least on Windows platform
#define ECLIPSE_ARGS 0
#define CROP_COORD_SPACE_SENSOR 1

// stores path to data files
Glib::ustring argv0;
Glib::ustring creditsPath;
Glib::ustring licensePath;
Glib::ustring externalPath;
Glib::ustring argv1;

static constexpr int NON_FF_WIDTH_TH = 5000;

using namespace rtengine::procparams;

namespace {
bool fast_export = true;
static std::map<std::string, std::string> external_dcp_map;
} // namespace

struct DecodeCtx {
    rtengine::InitialImage* ii = nullptr;
    Glib::ustring fname;
    std::string   extLower;
    std::string   make;
    std::string   model; // 下划线已替空格
    int           previewType = RAWENGINE_PREVIEW_TYPE_ORIGIN;
    bool          isRaw = true;
    const std::map<std::string,std::string>* externalDcp = nullptr;
};

struct CropBox { 
    int x=0;
    int y = 0;
    int w = 0;
    int h = 0;
    bool ok = false;
    std::string source;
};

struct CanonCIOrientIdx { 
    const char* model; 
    size_t idx0; 
};

// 规则接口
struct ModelRule {
    std::function<bool(const DecodeCtx&)> match;
    std::function<void(const DecodeCtx&, rtengine::procparams::ProcParams&)> apply;
    int priority = 0;

    ModelRule() = default;

    ModelRule(std::function<bool(const DecodeCtx&)> m,
              std::function<void(const DecodeCtx&, rtengine::procparams::ProcParams&)> a,
              int p)
        : match(std::move(m)), apply(std::move(a)), priority(p) {}
};

std::vector<ModelRule> _model_rules;
const CanonCIOrientIdx kCanonCIOrientTable[] = {
    { "EOS 6D",          131 },
    { "EOS 6D Mark II",  154 },
    { "EOS R5",          148 },
    { "EOS R8",          148 },
    { "EOS 5D Mark IV",  149 }, // CameraInfo offset 0x95
    // 如需其它机型，在这里补
};

int64_t get_long_value_by_exiv(const Exiv2::Value& value, size_t index = 0) {
#if defined(_WIN32) || defined(_WIN64)
    return value.toLong(index);
#else
    return value.toInt64(index);
#endif
}

// ======================= 公共小工具 =======================

static inline std::string to_lowercase(const std::string& str) {
    std::string result = str;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    return result;
}

static std::map<std::string, std::string> find_dcp_files(const std::string& directory_path) {
    static std::map<std::string, std::string> file_map;
    GFile* directory = g_file_new_for_path(directory_path.c_str());
    GError* error = nullptr;

    GFileEnumerator* enumerator = g_file_enumerate_children(
        directory,
        G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_TYPE,
        G_FILE_QUERY_INFO_NONE,
        nullptr,
        &error);

    if (!enumerator) {
        std::cerr << "无法打开目录：" << (error ? error->message : "未知错误") << std::endl;
        if (error) g_error_free(error);
        g_object_unref(directory);
        return file_map;
    }

    GFileInfo* info = nullptr;
    while ((info = g_file_enumerator_next_file(enumerator, nullptr, &error)) != nullptr) {
        const gchar* name = g_file_info_get_name(info);
        GFileType type = g_file_info_get_file_type(info);

        if (type == G_FILE_TYPE_REGULAR) {
            if (g_str_has_suffix(name, ".dcp")) {
                gchar* full_path = g_build_filename(directory_path.c_str(), name, nullptr);
                file_map[to_lowercase(name)] = full_path;
                g_free(full_path);
            }
        }
        g_object_unref(info);
    }

    if (error) {
        std::cerr << "读取目录时出错：" << error->message << std::endl;
        g_error_free(error);
    }

    g_object_unref(enumerator);
    g_object_unref(directory);
    return file_map;
}

bool dontLoadCache(int argc, char** argv);

// ===== Exiv2 / LibRaw =====

inline std::string to_u8(const Glib::ustring& s) { return s.raw(); }

#ifdef _WIN32
std::wstring ustring_to_wstring(const Glib::ustring& ustr) {
    if (ustr.empty()) {
        return L"";
    }
    
    // 获取所需的缓冲区大小
    int wide_len = MultiByteToWideChar(CP_UTF8, 0, ustr.c_str(), -1, nullptr, 0 );
    if (wide_len == 0) {
        return L"";
    }
    std::wstring wstr;
    wstr.resize(wide_len);
    
    // 执行转换
    int result = MultiByteToWideChar(CP_UTF8, 0, ustr.c_str(), -1, &wstr[0], wide_len);
    if (result == 0) {
        return L"";
    }
    
    wstr.resize(wcslen(wstr.c_str()));
    return wstr;
}
#endif

inline std::unique_ptr<Exiv2::Image> open_image_cross(const Glib::ustring& fname) {
    return std::unique_ptr<Exiv2::Image>(Exiv2::ImageFactory::open(to_u8(fname)));
}

inline void print_exiv2_kv(const Exiv2::Metadatum& md) {
    std::cout << "  " << md.key()
              << "  [" << md.typeName() << ", count=" << md.count() << "]"
              << " = " << md.toString() << "\n";
}

inline void dump_all_metadata(const rtengine::InitialImage* ii) {
    if (!ii || !ii->getMetaData()) {
        std::cerr << "[MetaDump] InitialImage or MetaData is null.\n";
        return;
    }
    const auto fname = ii->getMetaData()->getFileName();
    std::cout << "================= Exiv2 Full Metadata =================\n";
    std::cout << "File              : " << to_u8(fname) << "\n";
    try {
        auto img = open_image_cross(fname);
        if (!img) {
            std::cerr << "[MetaDump] Exiv2::ImageFactory::open failed.\n";
            return;
        }
        img->readMetadata();
        Exiv2::ExifData& exif = img->exifData();
        for (const auto& md : exif) {
            print_exiv2_kv(md);
        }
    } catch (const Exiv2::Error& e) {
        std::cerr << "[MetaDump] Exiv2 error: " << e.what() << "\n";
    } catch (const std::exception& e) {
        std::cerr << "[MetaDump] std::exception: " << e.what() << "\n";
    } catch (...) {
        std::cerr << "[MetaDump] Unknown exception.\n";
    }
    std::cout << "=======================================================\n";
}


int check_libraw_open(std::shared_ptr<LibRaw> raw_ptr, const Glib::ustring& fname) {
#ifdef _WIN32
    auto w_path = ustring_to_wstring(fname);
    return raw_ptr->open_file(w_path.c_str());
#else
    return raw_ptr->open_file(fname.c_str());
#endif
}

bool checkSonyDecoder(const char* filename) {
    std::shared_ptr<LibRaw> raw_ptr = std::make_shared<LibRaw>();
    if (check_libraw_open(raw_ptr, filename) != LIBRAW_SUCCESS) {
        return false;
    }
    libraw_decoder_info_t di{};
    if (raw_ptr->get_decoder_info(&di) != LIBRAW_SUCCESS) {
        raw_ptr->recycle();
        return false;
    }
    std::string decoder = di.decoder_name ? di.decoder_name : "";
    raw_ptr->recycle();
    if (decoder == "sony_ycbcr_load_raw()") {
        return false;
    }
    return true;
}

void map_sensor_crop_to_display(int orientation, int fullW, int fullH,
                                       int& x, int& y, int& w, int& h,
                                       int& outW, int& outH)
{
    outW = fullW; outH = fullH;
    switch (orientation) {
    case 1: break;
    case 3: x = fullW - (x + w); y = fullH - (y + h); break;
    case 6: { int nx = fullH - (y + h), ny = x, nw = h, nh = w; x = nx; y = ny; w = nw; h = nh; outW = fullH; outH = fullW; break; }
    case 8: { int nx = y, ny = fullW - (x + w), nw = h, nh = w; x = nx; y = ny; w = nw; h = nh; outW = fullH; outH = fullW; break; }
    case 2: x = fullW - (x + w); break;
    case 4: y = fullH - (y + h); break;
    case 5: { int nx = y, ny = x, nw = h, nh = w; x = nx; y = ny; w = nw; h = nh; outW = fullH; outH = fullW; break; }
    case 7: { int nx = fullH - (y + h), ny = fullW - (x + w), nw = h, nh = w; x = nx; y = ny; w = nw; h = nh; outW = fullH; outH = fullW; break; }
    default: break;
    }
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x + w > outW) w = outW - x;
    if (y + h > outH) h = outH - y;
    if (w < 1) w = 1; 
    if (h < 1) h = 1;
}

int clampi(int v, int lo, int hi) { if (v < lo) return lo; if (v > hi) return hi; return v; }

void sanitize_crop_even_align(int& x,int& y,int& w,int& h, int fullW,int fullH) {
    if (fullW <= 0 || fullH <= 0) return;
    if (x < 0) x = 0; 
    if (y < 0) y = 0;
    if (x & 1) x = (x > 0) ? x - 1 : 0;
    if (y & 1) y = (y > 0) ? y - 1 : 0;
    if (w < 1) w = 1; 
    if (h < 1) h = 1;
    if (x + w > fullW) { int over = x + w - fullW; x = (x > over) ? (x - over) : 0; }
    if (y + h > fullH) { int over = y + h - fullH; y = (y > over) ? (y - over) : 0; }
    if (x + w > fullW) w = fullW - x;
    if (y + h > fullH) h = fullH - y;
    if (w & 1) { if (w > 1) --w; }
    if (h & 1) { if (h > 1) --h; }
    if (w < 1) w = 1; 
    if (h < 1) h = 1;
}

void sanitize_7m4_crop_even_align(int& x,int& y,int& w,int& h, int fullW,int fullH) {
    if (fullW <= 0 || fullH <= 0) return;

    // 起点先落入合法范围（向下取偶）
    if (x < 0) x = 0; 
    if (y < 0) y = 0;
    if (x & 1) x = (x > 0) ? x - 1 : 0;
    if (y & 1) y = (y > 0) ? y - 1 : 0;

    if (w < 1) w = 1; 
    if (h < 1) h = 1;

    // 若越界，优先把起点向左/上挪回去（尽量不改 w/h）
    if (x + w > fullW) {
        int over = x + w - fullW;
        x = (x > over) ? (x - over) : 0;
    }
    if (y + h > fullH) {
        int over = y + h - fullH;
        y = (y > over) ? (y - over) : 0;
    }

    // 再检查仍越界就缩 w/h（极少见）
    if (x + w > fullW) w = fullW - x;
    if (y + h > fullH) h = fullH - y;

    // Bayer 偶数：必要时各自减 1
    if (w & 1) { if (w > 1) --w; }
    if (h & 1) { if (h > 1) --h; }

    if (w < 1) w = 1; 
    if (h < 1) h = 1;
}


std::string lower_copy(std::string s) {
    for (size_t i=0;i<s.size();++i) s[i] = (char)std::tolower((unsigned char)s[i]);
    return s;
}

bool get_pair_int(const Exiv2::Value& v, int& a, int& b) {
    try { 
        if ((int)v.count() >= 2) { 
            a = get_long_value_by_exiv(v, 0); 
            b = get_long_value_by_exiv(v, 1); 
            return true; 
        } 
    } catch(...) {}
    return false;
}

Exiv2::Image::AutoPtr read_exif_image(const Glib::ustring& fname) {
    try {
        auto imgOut = Exiv2::ImageFactory::open(fname.raw());
        if (!imgOut.get()) {
            return Exiv2::Image::AutoPtr();
        }
        imgOut->readMetadata();
        return imgOut;
    } catch(...) {
         return Exiv2::Image::AutoPtr();
     }
}

int read_exif_orientation(const Glib::ustring& fname) {
    try {
        auto img = read_exif_image(fname);
        if (!img.get()) return 1;

        const Exiv2::ExifData& exif = img->exifData();
        for (Exiv2::ExifData::const_iterator it = exif.begin(); it != exif.end(); ++it) {
            std::string k = it->key();
            for (size_t i=0;i<k.size();++i) k[i] = (char)std::tolower((unsigned char)k[i]);
            if (k.find("orientation") != std::string::npos) {
                try {
                    int o = (int)get_long_value_by_exiv(it->value());
                    if (o >= 1 && o <= 8) return o;
                } catch (...) {}
            }
        }
    } catch (...) {}
    return 1;
}

void parse_crops_from_exif(const Exiv2::ExifData& exif,
                                  bool& hasSony, int& sx,int& sy,int& sw,int& sh,
                                  bool& hasDef,  int& dx,int& dy,int& dw,int& dh,
                                  bool& hasAA,   int& top,int& left,int& bottom,int& right)
{
    hasSony = hasDef = hasAA = false;
    sx=sy=sw=sh=dx=dy=dw=dh=top=left=bottom=right=0;

    const Exiv2::Metadatum *sony_tl=NULL,*sony_sz=NULL, *def_org=NULL,*def_sz=NULL, *aa=NULL;

    for (Exiv2::ExifData::const_iterator it = exif.begin(); it!=exif.end(); ++it) {
        std::string k = lower_copy(it->key());
        if (!sony_tl && (k.find(".0x74c7")!=std::string::npos || k.find("sonycroptopleft")!=std::string::npos)) sony_tl = &(*it);
        else if (!sony_sz && (k.find(".0x74c8")!=std::string::npos || k.find("sonycropsize")!=std::string::npos)) sony_sz = &(*it);
        if (!def_org && k.find("defaultcroporigin")!=std::string::npos) def_org = &(*it);
        else if (!def_sz && k.find("defaultcropsize")!=std::string::npos) def_sz = &(*it);
        if (!aa && k.find("activearea")!=std::string::npos) aa = &(*it);
    }

    if (sony_tl && sony_sz &&
        get_pair_int(sony_tl->value(), sx, sy) &&
        get_pair_int(sony_sz->value(), sw, sh) &&
        sw>0 && sh>0) {
        hasSony = true;
    }
    if (def_org && def_sz &&
        get_pair_int(def_org->value(), dx, dy) &&
        get_pair_int(def_sz->value(), dw, dh) &&
        dw>0 && dh>0) {
        hasDef = true;
    }
    if (aa && aa->value().count() >= 4) {
        try {
            top    = get_long_value_by_exiv(aa->value(), 0);
            left   = get_long_value_by_exiv(aa->value(), 1);
            bottom = get_long_value_by_exiv(aa->value(), 2);
            right  = get_long_value_by_exiv(aa->value(), 3);
            if (bottom>top && right>left) hasAA = true;
        } catch(...) {}
    }
}

void infer_full_wh_from_exif_extents(const Glib::ustring& fname, const rtengine::InitialImage* ii, int& fullW, int& fullH) {
    fullW = fullH = 0;
    try {
        auto img = read_exif_image(fname);
        if (img.get() == NULL) {
            ii->getMetaData()->getDimensions(fullW, fullH); 
            return;
        }
        
        bool hasSony, hasDef, hasAA;
        int sx,sy,sw,sh, dx,dy,dw,dh, top,left,bottom,right;
        parse_crops_from_exif(img->exifData(), hasSony,sx,sy,sw,sh, hasDef,dx,dy,dw,dh, hasAA,top,left,bottom,right);

        if (hasSony) { if (sx+sw > fullW) fullW = sx+sw; if (sy+sh > fullH) fullH = sy+sh; }
        if (hasDef)  { if (dx+dw > fullW) fullW = dx+dw; if (dy+dh > fullH) fullH = dy+dh; }
        if (hasAA)   { if (right  > fullW) fullW = right; if (bottom > fullH) fullH = bottom; }

        if (fullW <= 0 || fullH <= 0) {
            ii->getMetaData()->getDimensions(fullW, fullH);
        }
    } catch(...) {
        ii->getMetaData()->getDimensions(fullW, fullH);
    }
}

void infer_7m4_full_wh_from_exif_extents(const Glib::ustring& fname, const rtengine::InitialImage* ii, int& fullW, int& fullH) {
    fullW = fullH = 0;
    try {
        auto img = read_exif_image(fname);
        if (img.get() == NULL) {
            ii->getMetaData()->getDimensions(fullW, fullH); 
            return;
        }

        bool hasSony, hasDef, hasAA;
        int sx,sy,sw,sh, dx,dy,dw,dh, top,left,bottom,right;
        parse_crops_from_exif(img->exifData(), hasSony,sx,sy,sw,sh, hasDef,dx,dy,dw,dh, hasAA,top,left,bottom,right);

        // 横向尺寸：右边界的最大值
        if (hasSony) { if (sx+sw > fullW) fullW = sx+sw; if (sy+sh > fullH) fullH = sy+sh; }
        if (hasDef)  { if (dx+dw > fullW) fullW = dx+dw; if (dy+dh > fullH) fullH = dy+dh; }
        if (hasAA)   { if (right  > fullW) fullW = right; if (bottom > fullH) fullH = bottom; }

        // 兜底（某些非 RAW 或少标签文件）
        if (fullW <= 0 || fullH <= 0) {
            ii->getMetaData()->getDimensions(fullW, fullH);
        }
    } catch(...) {
        ii->getMetaData()->getDimensions(fullW, fullH);
    }
}

static inline bool get_int_at(const Exiv2::Value& v, int idx, int& out) {
    try { 
        if (v.count() > idx) {
            out = get_long_value_by_exiv(v, idx); 
            return true; 
        } 
    } catch (...) {}
    return false;
}

// ====== 新增：仅用 EXIF 获取 fullW/fullH ======
static bool exif_get_full_wh_canon(const Exiv2::ExifData& exif, int& fullW, int& fullH,
                                   int* pLeft=nullptr, int* pTop=nullptr, int* pRight=nullptr, int* pBottom=nullptr)
{
    for (const auto& md : exif) {
        std::string k = lower_copy(md.key());
        if (k.find("canon") != std::string::npos && k.find("sensorinfo") != std::string::npos) {
            const Exiv2::Value& v = md.value();
            int n = (int)v.count();
            // 典型 5D4: [1]=6880, [2]=4544; [5..8]=L,T,R,B
            if (n >= 9) {
                int w=0,h=0,L=0,T=0,R=0,B=0;
                if (get_int_at(v, 1, w) && get_int_at(v, 2, h) && w>0 && h>0) {
                    fullW = w; fullH = h;
                    if (get_int_at(v, 5, L) && get_int_at(v, 6, T) && get_int_at(v, 7, R) && get_int_at(v, 8, B)) {
                        if (pLeft)   *pLeft   = L;
                        if (pTop)    *pTop    = T;
                        if (pRight)  *pRight  = R;
                        if (pBottom) *pBottom = B;
                    }
                    return true;
                }
            }
        }
    }
    return false;
}

static bool exif_get_full_wh(const Glib::ustring& fname, int& fullW, int& fullH)
{
    fullW = fullH = 0;
    try {
        auto img = read_exif_image(fname);
        if (!img.get()) return false;
        const Exiv2::ExifData& exif = img->exifData();

        // 1) Canon SensorInfo（最佳）
        if (exif_get_full_wh_canon(exif, fullW, fullH, nullptr, nullptr, nullptr, nullptr)) {
            return true;
        }

        // 2) CR2 原始 IFD 宽高
        auto itW = exif.findKey(Exiv2::ExifKey("Exif.Image3.ImageWidth"));
        auto itH = exif.findKey(Exiv2::ExifKey("Exif.Image3.ImageLength"));
        if (itW != exif.end() && itH != exif.end()) {
            try {
                int w = get_long_value_by_exiv(itW->value());
                int h = get_long_value_by_exiv(itH->value());
                if (w > 0 && h > 0) { fullW = w; fullH = h; return true; }
            } catch (...) {}
        }

        // 3) 兜底：像素维度（注意：这是裁后）
        auto itPX = exif.findKey(Exiv2::ExifKey("Exif.Photo.PixelXDimension"));
        auto itPY = exif.findKey(Exiv2::ExifKey("Exif.Photo.PixelYDimension"));
        if (itPX != exif.end() && itPY != exif.end()) {
            try {
                int w = get_long_value_by_exiv(itPX->value());
                int h = get_long_value_by_exiv(itPY->value());
                if (w > 0 && h > 0) { fullW = w; fullH = h; return true; }
            } catch (...) {}
        }
    } catch (...) {}
    return false;
}
// ==========================================
// 新增：Sony AspectFrame 解析
static bool read_sony_aspectframe(const Exiv2::ExifData& exif, int& x,int& y,int& w,int& h) {
    for (const auto& md : exif) {
        std::string k = lower_copy(md.key());
        if (k.find("sony") != std::string::npos && (k.find("aspectframe") != std::string::npos || k.find("aspect frame") != std::string::npos)) {
            const Exiv2::Value& v = md.value();
            int L,T,R,B;
            if (get_int_at(v,0,L) && get_int_at(v,1,T) && get_int_at(v,2,R) && get_int_at(v,3,B) && R > L && B > T) {
                x=L; y=T; w=R-L+1; h=B-T+1;
                return true;
            }
        }
    }
    return false;
}

static CropBox read_crop_from_exif(const Glib::ustring& fname) {
    CropBox cb;
    try {
        auto img = read_exif_image(fname);
        if (img.get() == NULL) {
            return cb;
        }
        const Exiv2::ExifData exif = img->exifData();

        bool hasSony, hasDef, hasAA;
        int sx,sy,sw,sh, dx,dy,dw,dh, top,left,bottom,right;
        parse_crops_from_exif(exif, hasSony,sx,sy,sw,sh, hasDef,dx,dy,dw,dh, hasAA,top,left,bottom,right);

    if (hasSony) { cb.x=sx; cb.y=sy; cb.w=sw; cb.h=sh; cb.ok=true; cb.source="sony(0x74c7/0x74c8)"; return cb; }

    // 新增：再试 Sony AspectFrame
    int ax,ay,aw,ah;
    if (read_sony_aspectframe(exif, ax,ay,aw,ah)) {
        cb.x=ax; cb.y=ay; cb.w=aw; cb.h=ah; cb.ok=true; cb.source="sony/aspectframe";
        return cb;
    }

    // 再退回 DefaultCrop / ActiveArea
    if (hasDef)  { cb.x=dx; cb.y=dy; cb.w=dw; cb.h=dh; cb.ok=true; cb.source="defaultcrop";     return cb; }
    if (hasAA)   { cb.x=left; cb.y=top; cb.w=right-left; cb.h=bottom-top; cb.ok=true; cb.source="activearea"; return cb; }

        return cb;
    } catch(...) {
        return cb;
    }
}

CropBox read_canon_crop_from_exif(const Glib::ustring& fname) {
    CropBox cb;
    try {
        auto img = read_exif_image(fname);
        if (img.get() == NULL) {
            return cb;
        }
        const Exiv2::ExifData exif = img->exifData();

        bool hasSony, hasDef, hasAA;
        int sx,sy,sw,sh, dx,dy,dw,dh, top,left,bottom,right;
        parse_crops_from_exif(exif, hasSony,sx,sy,sw,sh, hasDef,dx,dy,dw,dh, hasAA,top,left,bottom,right);
        // ---- Canon MakerNote SensorInfo -> 左上右下（含+1逻辑）
        for (const auto& md : exif) {
            std::string k = lower_copy(md.key());
            if (k.find("canon") != std::string::npos && k.find("sensorinfo") != std::string::npos) {
                const Exiv2::Value& v = md.value();
                int L,T,R,B;
                if (get_int_at(v, 5, L) && get_int_at(v, 6, T) &&
                    get_int_at(v, 7, R) && get_int_at(v, 8, B) &&
                    R > L && B > T)
                {
                    cb.x = L;
                    cb.y = T;
                    cb.w = (R - L + 1);
                    cb.h = (B - T + 1);
                    cb.ok = (cb.w > 0 && cb.h > 0);
                    cb.source = "canon/sensorinfo";
                    return cb;
                }
            }
        }
        return cb;
    } catch(...) {
        return cb;
    }
}

void map_sensor_to_display_roi(int orientation, int fullW, int fullH,
                                      int sx,int sy,int sw,int sh,
                                      int& dx,int& dy,int& dw,int& dh,
                                      int& dispFullW,int& dispFullH)
{
    dx=sx; dy=sy; dw=sw; dh=sh;
    dispFullW = fullW; dispFullH = fullH;

    switch (orientation) {
    case 1: break;
    case 3: dx = fullW - (sx + sw); dy = fullH - (sy + sh); break;
    case 6: dx = fullH - (sy + sh); dy = sx; dw = sh; dh = sw; dispFullW = fullH; dispFullH = fullW; break;
    case 8: dx = sy; dy = fullW - (sx + sw); dw = sh; dh = sw; dispFullW = fullH; dispFullH = fullW; break;
    case 2: dx = fullW - (sx + sw); break;
    case 4: dy = fullH - (sy + sh); break;
    case 5: dx = sy; dy = sx; dw = sh; dh = sw; dispFullW = fullH; dispFullH = fullW; break;
    case 7: dx = fullH - (sy + sh); dy = fullW - (sx + sw); dw = sh; dh = sw; dispFullW = fullH; dispFullH = fullW; break;
    default: break;
    }

    if (dx < 0) dx = 0; 
    if (dy < 0) dy = 0;
    if (dx + dw > dispFullW) dw = dispFullW - dx;
    if (dy + dh > dispFullH) dh = dispFullH - dy;
    if (dw < 1) dw = 1; 
    if (dh < 1) dh = 1;
}

// （保留）如果你想完全不用 LibRaw，可把整个函数和引用删掉
bool libraw_get_full_wh(const Glib::ustring& fname, int& fullW, int& fullH) {
    fullW = fullH = 0;
    try {
        std::shared_ptr<LibRaw> raw_ptr = std::make_shared<LibRaw>();
        if (check_libraw_open(raw_ptr, fname) != LIBRAW_SUCCESS)
            return false;
        const libraw_image_sizes_t& s = raw_ptr->imgdata.sizes;
        int right  = s.left_margin + s.width;
        int bottom = s.top_margin  + s.height;
        if (right  <= 0) right  = s.raw_width;
        if (bottom <= 0) bottom = s.raw_height;
        fullW = right; fullH = bottom;
        raw_ptr->recycle();
        return fullW > 0 && fullH > 0;
    } catch (...) {
        return false;
    }
}

// 这里改成 EXIF 优先（不再依赖 LibRaw）
void get_sensor_full_wh_robust(const Glib::ustring& fname, const rtengine::InitialImage* ii, int& fullW, int& fullH) {
    // 1) 只用 EXIF 拿 full（Canon SensorInfo / Image3 宽高 / PixelX/YDimension）
    if (exif_get_full_wh(fname, fullW, fullH) && fullW > 0 && fullH > 0) {
        return;
    }
    // 2) 退回到 extents union（默认裁边/active area/Sony 专用等）
    infer_full_wh_from_exif_extents(fname, ii, fullW, fullH);
    if (fullW > 0 && fullH > 0) {
        return;
    }
    // 3) 最后兜底：元数据维度
    ii->getMetaData()->getDimensions(fullW, fullH);
}

void get_7m4_sensor_full_wh_robust(const Glib::ustring& fname, const rtengine::InitialImage* ii, int& fullW, int& fullH) {
    infer_7m4_full_wh_from_exif_extents(fname, ii, fullW, fullH); // 你已有的函数
    if (fullW <= 0 || fullH <= 0) {
        if (!libraw_get_full_wh(fname, fullW, fullH)) {
            ii->getMetaData()->getDimensions(fullW, fullH); // 最后兜底（可能是已旋转维度，但总比 0 好）
        }
    }
}


void sensor_roi_to_fraction(int fullW,int fullH,int sx,int sy,int sw,int sh,
                                          double& fx,double& fy,double& fw,double& fh)
{
    fx = fullW > 0 ? (double)sx / (double)fullW : 0.0;
    fy = fullH > 0 ? (double)sy / (double)fullH : 0.0;
    fw = fullW > 0 ? (double)sw / (double)fullW : 0.0;
    fh = fullH > 0 ? (double)sh / (double)fullH : 0.0;
}

static void map_fraction_sensor_to_display(int orientation,
                                           double fx,double fy,double fw,double fh,
                                           double& dx,double& dy,double& dw,double& dh)
{
    dx = fx; dy = fy; dw = fw; dh = fh;
    switch (orientation) {
    case 1: break;
    case 3: dx = 1.0 - (fx + fw); dy = 1.0 - (fy + fh); break;
    case 6: dx = 1.0 - (fy + fh); dy = fx; dw = fh; dh = fw; break;
    case 8: dx = fy; dy = 1.0 - (fx + fw); dw = fh; dh = fw; break;
    case 2: dx = 1.0 - (fx + fw); break;
    case 4: dy = 1.0 - (fy + fh); break;
    case 5: dx = fy; dy = fx; dw = fh; dh = fw; break;
    case 7: dx = 1.0 - (fy + fh); dy = 1.0 - (fx + fw); dw = fh; dh = fw; break;
    default: break;
    }
    if (dx < 0) dx = 0;
    if (dy < 0) dy = 0;
    if (dx + dw > 1.0) dw = 1.0 - dx;
    if (dy + dh > 1.0) dh = 1.0 - dy;
}

bool final_fallback_fractional_crop(int outW,int outH,
                                           int sensorFullW,int sensorFullH,
                                           int sx,int sy,int sw,int sh,
                                           int orientation,
                                           void** buffer,int* length,int* width,int* height,
                                           int max_delta_px /*= 64*/)
{
    if (outW <= 0 || outH <= 0 || sensorFullW <= 0 || sensorFullH <= 0)
        return false;

    int dx,dy,dw,dh, dispFullW,dispFullH;
    map_sensor_to_display_roi(orientation, sensorFullW, sensorFullH,
                              sx,sy,sw,sh, dx,dy,dw,dh, dispFullW,dispFullH);

    const int trimR = std::max(0, dispFullW - outW);
    const int trimB = std::max(0, dispFullH - outH);
    const int availW = dispFullW - trimR;
    const int availH = dispFullH - trimB;

    int overR = (dx + dw) - availW;
    if (overR > 0) {
        int shift = std::min(dx, overR); dx -= shift; overR -= shift;
        if (overR > 0) dw -= overR;
    }
    int overB = (dy + dh) - availH;
    if (overB > 0) {
        int shift = std::min(dy, overB); dy -= shift; overB -= shift;
        if (overB > 0) dh -= overB;
    }

    if (dx < 0) { dw += dx; dx = 0; }
    if (dy < 0) { dh += dy; dy = 0; }
    if (dx + dw > outW) dw = outW - dx;
    if (dy + dh > outH) dh = outH - dy;

    if (dx & 1) { if (dx > 0) --dx; else if (dw > 1) --dw; }
    if (dy & 1) { if (dy > 0) --dy; else if (dh > 1) --dh; }
    if (dw & 1) { if (dw > 1) --dw; }
    if (dh & 1) { if (dh > 1) --dh; }

    if (dw < 1 || dh < 1) return false;

    const int dW = std::abs(outW - dw);
    const int dH = std::abs(outH - dh);
    if ((dW == 0 && dH == 0) || (dW > max_delta_px && dH > max_delta_px)) {
        return false;
    }

    const int srcStride = outW * 4;
    const int dstStride = dw   * 4;
    void* newBuf = std::malloc(dstStride * dh);
    if (!newBuf) return false;

    unsigned char* dst = (unsigned char*)newBuf;
    const unsigned char* src = (const unsigned char*)(*buffer);
    for (int r = 0; r < dh; ++r) {
        std::memcpy(dst + r*dstStride, src + ((dy + r) * srcStride + dx*4), dstStride);
    }

    std::free(*buffer);
    *buffer = newBuf;
    *width  = dw;
    *height = dh;
    *length = dstStride * dh;

    std::fprintf(stderr,
        "[EXIF-Crop][fallback-margin] -> %dx%d at (%d,%d)  (raw out %dx%d, dispFull %dx%d, trimR %d, trimB %d)\n",
        dw, dh, dx, dy, outW, outH, dispFullW, dispFullH, trimR, trimB);

    return true;
}

// ======================= 规则化（保持原逻辑） =======================

static inline bool is_non_ff_by_full_wh(const int* full_w, const int* full_h) {
    if (!full_w || !full_h) return false;
    int w = *full_w, h = *full_h;
    if (w <= 0 || h <= 0) return false;
    int eff = std::max(w, h);
    return eff < NON_FF_WIDTH_TH;
}
static inline bool is_non_ff_by_full_wh(int full_w, int full_h) {
    if (full_w <= 0 || full_h <= 0) return false;
    int eff = std::max(full_w, full_h);
    return eff < NON_FF_WIDTH_TH;
}


inline bool ieq(const std::string& a, const std::string& b) { return to_lowercase(a)==to_lowercase(b); }
inline bool icontains(const std::string& hay, const std::string& needle) {
    auto A=to_lowercase(hay), B=to_lowercase(needle); return A.find(B)!=std::string::npos;
}

bool is_sony_ycbcr(const Glib::ustring& fname) { return !checkSonyDecoder(fname.raw().c_str()); }

inline void disable_lens_all(rtengine::procparams::ProcParams& p) {
    p.lensProf.lcMode  = rtengine::procparams::LensProfParams::LcMode::NONE;
    p.lensProf.useDist = false; p.lensProf.useVign = false; p.lensProf.useCA   = false;
}
void enable_lens_auto(rtengine::procparams::ProcParams& p) {
    p.lensProf.lcMode  = rtengine::procparams::LensProfParams::LcMode::LENSFUNAUTOMATCH;
    p.lensProf.useDist = true;  p.lensProf.useVign = true;  p.lensProf.useCA   = false;
}

static void apply_lens_override(const RawEngineLensParams* lp, rtengine::procparams::ProcParams& p) {
    if (!lp) return;
    switch (lp->lens_mode) {
    case RAWENGINE_LENS_MODE_NONE:
        p.lensProf.lcMode  = rtengine::procparams::LensProfParams::LcMode::NONE;
        p.lensProf.useDist = false;
        p.lensProf.useVign = false;
        p.lensProf.useCA   = false;
        break;
    case RAWENGINE_LENS_MODE_AUTO:
        p.lensProf.lcMode  = rtengine::procparams::LensProfParams::LcMode::LENSFUNAUTOMATCH;
        p.lensProf.useDist = (lp->use_distortion != 0);
        p.lensProf.useVign = false;
        p.lensProf.useCA   = false;
        break;
    case RAWENGINE_LENS_MODE_MANUAL:
        p.lensProf.lcMode  = rtengine::procparams::LensProfParams::LcMode::LENSFUNMANUAL;
        p.lensProf.lfCameraMake  = lp->camera_make  ? lp->camera_make  : "";
        p.lensProf.lfCameraModel = lp->camera_model ? lp->camera_model : "";
        p.lensProf.lfLens        = lp->lens_name    ? lp->lens_name    : "";
        p.lensProf.useDist = (lp->use_distortion != 0);
        p.lensProf.useVign = false;
        p.lensProf.useCA   = false;
        break;
    }
}

void use_fast_demosaic(rtengine::procparams::ProcParams& p) {
    p.raw.bayersensor.method  = RAWParams::BayerSensor::getMethodString(RAWParams::BayerSensor::Method::AMAZE);
    p.raw.bayersensor.ccSteps = 0;
}

inline void ensure_camera_profile_fallback(rtengine::procparams::ProcParams& p) {
    if (p.icm.inputProfile.empty()) {
        p.icm.inputProfile = "(camera)";
    }
}

void set_dirpyr_denoise(rtengine::procparams::ProcParams& p) {
    p.dirpyrDenoise.enabled = false;
}

void try_bind_dcp_exact(const DecodeCtx& c, rtengine::procparams::ProcParams& p) {
    // 按你原逻辑："make + model + Standard.dcp" 完整匹配
    if (c.externalDcp) {
        std::string full_name = to_lowercase(c.make + " " + c.model + " Standard.dcp");
        auto it = c.externalDcp->find(full_name);
        if (it != c.externalDcp->end()) {
            p.icm.inputProfile = it->second;
            return;
        }
    }
    // 若未匹配，留空，由各分支/默认再设置其它 icm 行为
    ensure_camera_profile_fallback(p);
}

void apply_exif_crop_display_mode(const DecodeCtx& c, rtengine::procparams::ProcParams& p) {
    CropBox cb = read_crop_from_exif(c.fname);
    if (!cb.ok) { std::fprintf(stderr,"[EXIF-Crop] none\n"); return; }
    int sensorW=0, sensorH=0;
    get_sensor_full_wh_robust(c.fname, c.ii, sensorW, sensorH);
    if (sensorW <= 0 || sensorH <= 0) { std::fprintf(stderr,"[EXIF-Crop] full(sensor) unknown, skip\n"); return; }
    const int ori = read_exif_orientation(c.fname);

    int dx,dy,dw,dh, dispFullW, dispFullH;
    map_sensor_to_display_roi(ori, sensorW, sensorH, cb.x,cb.y,cb.w,cb.h, dx,dy,dw,dh, dispFullW,dispFullH);

    auto clip_display = [](int& x,int& y,int& w,int& h,int W,int H){
        if (x < 0) x = 0; 
        if (y < 0) y = 0;
        if (w < 1) w = 1; 
        if (h < 1) h = 1;
        if (x + w > W) w = W - x;
        if (y + h > H) h = H - y;
    };
    clip_display(dx,dy,dw,dh, dispFullW,dispFullH);

    p.resize.enabled = false;           // 关键：避免“看起来像方形”
    p.crop.enabled  = true;
    p.crop.fixratio = false;
    p.crop.x = dx; p.crop.y = dy; p.crop.w = dw; p.crop.h = dh;

    // std::fprintf(stderr,
    //     "[EXIF-Crop][display-mode-applied] dispCrop=%dx%d@(x=%d,y=%d) dispFull=%dx%d src=%s ori=%d\n",
    //     dw, dh, dx, dy, dispFullW, dispFullH, cb.source.c_str(), ori);
}

static void debug_dump_exif_crop_related(const Glib::ustring& fname) {
    auto contains_ci = [](const std::string& s) {
        std::string k = s; for (auto& c:k) c = (char)std::tolower((unsigned char)c);
        // 关注 crop/defaultcrop/activearea/cropped image/边距/长宽比/方位等
        return k.find("crop")        != std::string::npos ||
               k.find("cropped")     != std::string::npos ||
               k.find("defaultcrop") != std::string::npos ||
               k.find("activearea")  != std::string::npos ||
               k.find("margin")      != std::string::npos ||
               k.find("orientation") != std::string::npos ||
               k.find("aspect")      != std::string::npos ||
               k.find("canon")       != std::string::npos; // Canon MakerNote 相关
    };
    try {
        auto img = read_exif_image(fname);
        if (!img.get()) { std::cerr << "[EXIF] open fail\n"; return; }
        const Exiv2::ExifData& exif = img->exifData();

        std::cout << "============= EXIF CROP-RELATED (" << to_u8(fname) << ") =============\n";
        for (const auto& md : exif) {
            // 仅用 key 过滤；不同版本 Exiv2 的 key 形态可能略有差异，所以尽量宽松
            if (contains_ci(md.key())) {
                std::cout << "  " << md.key()
                          << "  [" << md.typeName() << ", count=" << md.count() << "]"
                          << " = " << md.toString() << "\n";
            }
        }
        std::cout << "=====================================================================\n";

        // 附带打印 LibRaw 的活动区，帮助你对照“传感器坐标系”的可用范围（可留作调试）
        try {
            std::shared_ptr<LibRaw> raw_ptr = std::make_shared<LibRaw>();
            if (check_libraw_open(raw_ptr, fname) == LIBRAW_SUCCESS) {
                const auto& s = raw_ptr->imgdata.sizes;
                std::cout << "[LibRaw] left_margin="  << s.left_margin
                          << " top_margin="          << s.top_margin
                          << " width="               << s.width
                          << " height="              << s.height
                          << " raw_width="           << s.raw_width
                          << " raw_height="          << s.raw_height
                          << "\n";
                raw_ptr->recycle();
            }
        } catch (...) {}
    } catch (const Exiv2::Error& e) {
        std::cerr << "[EXIF] " << e.what() << "\n";
    } catch (...) {
        std::cerr << "[EXIF] unknown error\n";
    }
}
static void debug_dump_exif_all(const Glib::ustring& fname) {
    try {
        auto img = read_exif_image(fname);
        if (!img.get()) { std::cerr << "[EXIF] open fail\n"; return; }
        const Exiv2::ExifData& exif = img->exifData();

        std::cout << "================= EXIF ALL (" << to_u8(fname) << ") =================\n";
        for (const auto& md : exif) {
            std::cout << "  " << md.key()
                      << "  [" << md.typeName() << ", count=" << md.count() << "]"
                      << " = " << md.toString() << "\n";
        }
        std::cout << "=====================================================================\n";
    } catch (const Exiv2::Error& e) {
        std::cerr << "[EXIF] " << e.what() << "\n";
    } catch (...) {
        std::cerr << "[EXIF] unknown error\n";
    }
}

CropBox read_7m4_crop_from_exif(const Glib::ustring& fname) {
    CropBox cb;
    try {
        auto img = read_exif_image(fname);
        if (img.get() == NULL) {
            return cb;
        }
        const Exiv2::ExifData exif = img->exifData();
        bool hasSony, hasDef, hasAA;
        int sx,sy,sw,sh, dx,dy,dw,dh, top,left,bottom,right;
        parse_crops_from_exif(exif, hasSony,sx,sy,sw,sh, hasDef,dx,dy,dw,dh, hasAA,top,left,bottom,right);

        if (hasSony) { cb.x=sx; cb.y=sy; cb.w=sw; cb.h=sh; cb.ok=true; cb.source="sony(0x74c7/0x74c8)"; return cb; }
        if (hasDef)  { cb.x=dx; cb.y=dy; cb.w=dw; cb.h=dh; cb.ok=true; cb.source="defaultcrop";     return cb; }
        if (hasAA)   { cb.x=left; cb.y=top; cb.w=right-left; cb.h=bottom-top; cb.ok=true; cb.source="activearea"; return cb; }

        return cb;
    } catch(...) {
        return cb;
    }
}


inline bool approx(double a, double b, double rel=1e-6, double abs=1e-9) {
    return std::fabs(a - b) <= std::max(rel * std::max(std::fabs(a), std::fabs(b)), abs);
}


inline int read_flash_value(const Glib::ustring& fname) {
    try {
        auto image = read_exif_image(fname);
        if (image.get() == nullptr) {
            return -1;
        }
        Exiv2::ExifData& exif = image->exifData();

        auto it = exif.findKey(Exiv2::ExifKey("Exif.Photo.Flash"));
        if (it != exif.end()) {
            // 某些相机会写成 Short/Long，统一转成 long 再截断为 int
            long v = get_long_value_by_exiv(it->value());
            if (v < std::numeric_limits<int>::min() || v > std::numeric_limits<int>::max()) {
                return -1;
            }
            return v;
        }
    } catch (const std::exception& e) {
        
        std::cerr << "read_flash_value error: " << e.what() << std::endl;
    } catch (...) {
    }
    return -1; // 未找到/读取失败
}

// 解析辅助
inline int flash_mode_bits(int flashVal) {            // bit3-4
    return (flashVal >> 3) & 0x3;
}
inline bool flash_is_compulsory_off(int flashVal) {   // 强制关闭
    return flashVal >= 0 && flash_mode_bits(flashVal) == 1;
}
inline bool flash_is_compulsory_on(int flashVal) {    // 强制开启
    return flashVal >= 0 && flash_mode_bits(flashVal) == 2;
}
inline bool flash_is_auto(int flashVal) {             // 自动模式
    return flashVal >= 0 && flash_mode_bits(flashVal) == 3;
}

bool read_canon_aspectinfo(const Exiv2::ExifData& exif,
                                  int& arCode, int& cropL, int& cropT, int& cropW, int& cropH)
{
    for (const auto& md : exif) {
        std::string k = lower_copy(md.key());
        if (k.find("canon") != std::string::npos && k.find("aspectinfo") != std::string::npos) {
            const Exiv2::Value& v = md.value();
            int r=0, iw=0, ih=0, cl=0, ct=0;
            // 布局：0=AspectRatio, 1=ImageWidth, 2=ImageHeight, 3=CroppedImageLeft, 4=CroppedImageTop
            if (get_int_at(v,0,r) && get_int_at(v,1,iw) && get_int_at(v,2,ih)
             && get_int_at(v,3,cl) && get_int_at(v,4,ct) && iw>0 && ih>0) {
                arCode = r; cropW = iw; cropH = ih; cropL = cl; cropT = ct;
                return true;
            }
        }
    }
    return false;
}

CropBox read_canon6d_crop_from_exif(const Glib::ustring& fname) {
    CropBox cb;
    try {
        auto img = read_exif_image(fname);
        if (img.get() == NULL) {
            return cb;
        }
        const Exiv2::ExifData exif = img->exifData();

        // 1) 先取 SensorInfo 安全区域
        int L=-1,T=-1,R=-1,B=-1;
        bool haveSensor = false;
        for (const auto& md : exif) {
            std::string k = lower_copy(md.key());
            if (k.find("canon") != std::string::npos && k.find("sensorinfo") != std::string::npos) {
                const Exiv2::Value& v = md.value();
                if (get_int_at(v,5,L) && get_int_at(v,6,T) && get_int_at(v,7,R) && get_int_at(v,8,B) && R>L && B>T) {
                    haveSensor = true;
                }
                break;
            }
        }
        if (!haveSensor) return cb;

        // 安全区域（3:2）：先作为基准
        int safeX = L, safeY = T, safeW = (R - L + 1), safeH = (B - T + 1);
        cb.x = safeX; cb.y = safeY; cb.w = safeW; cb.h = safeH; cb.ok = true; cb.source = "canon/sensorinfo";

        // 2) 如有 AspectInfo（机内长宽比），则在安全区域上进一步裁切（对齐 ACR）
        int arCode=0, aL=0, aT=0, aW=0, aH=0;
        if (read_canon_aspectinfo(exif, arCode, aL, aT, aW, aH)) {
            // 注意：CroppedImageLeft/Top 是“相对于安全区域”的偏移（6D 实测为 0,285）
            int ax = safeX + aL;
            int ay = safeY + aT;
            int aw = std::min(aW, safeW);
            int ah = std::min(aH, safeH);

            // 与安全区域求交，避免越界
            int ix = std::max(ax, safeX);
            int iy = std::max(ay, safeY);
            int ir = std::min(ax + aw, safeX + safeW);
            int ib = std::min(ay + ah, safeY + safeH);
            if (ir > ix && ib > iy) {
                cb.x = ix; cb.y = iy; cb.w = ir - ix; cb.h = ib - iy;
                cb.source = "canon/aspectinfo";
            }
        }
        return cb;
    } catch (...) {
        return cb;
    }
}

bool canon_mn_read_orientation(const Exiv2::ExifData& exif, int& outOri)
{
    for (const auto& md : exif) {
        std::string grp = md.groupName();
        std::string key = md.key();
        std::string tag = md.tagName();
        for (auto& c: grp) c = (char)std::tolower((unsigned char)c);
        for (auto& c: key) c = (char)std::tolower((unsigned char)c);
        for (auto& c: tag) c = (char)std::tolower((unsigned char)c);

        // 只看 Canon 的 MakerNote
        if (grp.find("canon") == std::string::npos) continue;

        // 宽松匹配：cameraorientation / camera orientation / orientation
        if ( key.find("cameraorientation") == std::string::npos &&
             tag.find("cameraorientation") == std::string::npos &&
             key.find("camera orientation") == std::string::npos &&
             tag.find("camera orientation") == std::string::npos &&
             key.find("orientation")       == std::string::npos &&
             tag.find("orientation")       == std::string::npos )
        {
            continue;
        }

        // 数值型（常见：0/1/2/3）
        try {
            if (md.count() > 0) {
                long v = get_long_value_by_exiv(md.value(), 0); 
                switch (v) {             // Canon 语义
                    case 0: outOri = 1; return true; // Horizontal (normal)
                    case 1: outOri = 6; return true; // Rotate 90 CW
                    case 2: outOri = 8; return true; // Rotate 270 CW
                    case 3: outOri = 3; return true; // Rotate 180
                    default: break;
                }
            }
        } catch (...) {}

        // 字符串型（例如 "Horizontal (normal)"/"Rotate 90 CW"）
        std::string val;
        try { val = md.value().toString(); } catch (...) { val.clear(); }
        for (auto& c: val) c = (char)std::tolower((unsigned char)c);
        if (!val.empty()) {
            if (val.find("horizontal") != std::string::npos || val.find("normal") != std::string::npos) { outOri = 1; return true; }
            if (val.find("rotate") != std::string::npos && val.find("90")  != std::string::npos) { outOri = 6; return true; }
            if (val.find("rotate") != std::string::npos && val.find("270") != std::string::npos){ outOri = 8; return true; }
            if (val.find("180") != std::string::npos) { outOri = 3; return true; }
            if (val.find("90")  != std::string::npos && (val.find("cw")!=std::string::npos || val.find("clockwise")!=std::string::npos)) { outOri = 6; return true; }
            if (val.find("270") != std::string::npos && (val.find("cw")!=std::string::npos || val.find("clockwise")!=std::string::npos)) { outOri = 8; return true; }
        }
    }
    return false;
}

bool get_canon_camerainfo_blob(const Exiv2::ExifData& exif, std::vector<uint8_t>& out)
{
    out.clear();
    for (const auto& md : exif) {
        std::string k = md.key(); for (auto& c: k) c = (char)std::tolower((unsigned char)c);
        // 常见 key：Exif.Canon.CameraInfo / Exif.CanonFi.CameraInfo / Exif.CanonSi.CameraInfo 等
        if (k.find("canon") != std::string::npos && k.find("camerainfo") != std::string::npos) {
            try {
                Exiv2::DataBuf buf = md.dataArea();
                const uint8_t* p = NULL;
                size_t n = 0;
#if defined(EXIV2_MAJOR_VERSION)
#  if (EXIV2_MAJOR_VERSION > 0) || (EXIV2_MAJOR_VERSION == 0 && EXIV2_MINOR_VERSION >= 28)
                p = reinterpret_cast<const uint8_t*>(buf.c_data());
                n = buf.size();
#  else
                p = reinterpret_cast<const uint8_t*>(buf.pData_);
                n = buf.size_;
#  endif
#else
                p = reinterpret_cast<const uint8_t*>(buf.pData_);
                n = buf.size_;
#endif
                if (p && n) { out.assign(p, p + n); return true; }

                // 退路：当成 Byte 数组逐项读取
                if (md.count() > 0) {
                    out.reserve(md.count());
                    for (int i = 0; i < md.count(); ++i) {
                        long v = 0; 
                        try { 
                            v = get_long_value_by_exiv(md.value(), i); 
                        } catch (...) {}
                        out.push_back(static_cast<uint8_t>(v & 0xFF));
                    }
                    return !out.empty();
                }
            } catch (...) {}
        }
    }
    return false;
}

bool canon_ci_lookup_offset(const std::string& model, size_t& idx0)
{
    for (size_t i=0;i<sizeof(kCanonCIOrientTable)/sizeof(kCanonCIOrientTable[0]);++i) {
        if (strcasecmp(model.c_str(), kCanonCIOrientTable[i].model) == 0) {
            idx0 = kCanonCIOrientTable[i].idx0; return true;
        }
    }
    return false;
}

bool canon_val_to_exif(uint8_t v, int& ori) {
    switch (v) {
        case 0: ori = 1; return true;  // Horizontal (normal)
        case 1: ori = 6; return true;  // Rotate 90 CW
        case 2: ori = 8; return true;  // Rotate 270 CW
        case 3: ori = 3; return true;  // Rotate 180
        default: return false;
    }
}

bool scan_nearby_orientation_byte(const std::vector<uint8_t>& blob,
                                         size_t base, size_t& foundIdx, int& outOri)
{
    if (blob.empty()) return false;
    const size_t n = blob.size();
    const size_t lo = (base > 16 ? base - 16 : 0);
    const size_t hi = std::min(n, base + 16 + 1);

    for (size_t i = lo; i < hi; ++i) {
        int ori = 0;
        if (canon_val_to_exif(blob[i], ori)) {
            foundIdx = i;
            outOri = ori;
            std::fprintf(stderr, "[Canon-CI][scan] hit @%zu (byte=%u) -> ori=%d\n",
                         i, (unsigned)blob[i], ori);
            return true;
        }
    }
    return false;
}

bool canon_ci_read_orientation_robust(const Exiv2::ExifData& exif,
                                             const std::string& model,
                                             int& outOri)
{
    // 1) 取 blob
    std::vector<uint8_t> blob;
    if (!get_canon_camerainfo_blob(exif, blob) || blob.empty()) {
        std::fprintf(stderr, "[Canon-CI] blob not found\n");
        return false;
    }

    // 2) 表里偏移（如果有）
    size_t idx0 = 0;
    if (canon_ci_lookup_offset(model, idx0)) {
        if (idx0 < blob.size()) {
            int ori = 0;
            if (canon_val_to_exif(blob[idx0], ori)) {
                outOri = ori;
                std::fprintf(stderr, "[Canon-CI] table idx=%zu byte=%u -> ori=%d\n",
                             idx0, (unsigned)blob[idx0], ori);
                return true;
            }
            std::fprintf(stderr, "[Canon-CI] table idx=%zu invalid byte=%u, try scan\n",
                         idx0, (unsigned)blob[idx0]);
            // 3) 就在这个 idx 附近扫描一圈
            size_t found = 0;
            if (scan_nearby_orientation_byte(blob, idx0, found, outOri)) {
                std::fprintf(stderr, "[Canon-CI] use scanned idx=%zu\n", found);
                return true;
            }
        } else {
            std::fprintf(stderr, "[Canon-CI] table idx=%zu out of blob(%zu)\n", idx0, blob.size());
        }
    } else {
        std::fprintf(stderr, "[Canon-CI] no table offset for model=%s\n", model.c_str());
    }

    // 4) 没有表或表失败：给一个“通用基准”（6D/6D2 常见在 ~130，R5/R8 约 ~148），这里选 140 作为中点再扫
    size_t dummyBase = 140;
    size_t found = 0;
    if (scan_nearby_orientation_byte(blob, dummyBase, found, outOri)) {
        std::fprintf(stderr, "[Canon-CI] generic scan idx=%zu\n", found);
        return true;
    }

    // 全部失败
    return false;
}

int read_orientation_canon_only(const Glib::ustring& fname, const std::string& model)
{
    try {
        auto img = read_exif_image(fname);
        if (!img.get()) return 1;
        const Exiv2::ExifData& exif = img->exifData();

        int ori = 1;
        // 1) 尝试 MakerNote（Exiv2 大多读不到这个键，读不到就跳过）
        if (canon_mn_read_orientation(exif, ori)) {
            std::fprintf(stderr, "[Canon-MN] orientation=%d\n", ori);
            return ori;
        }
        // 2) CameraInfo 偏移 + 扫描
        if (canon_ci_read_orientation_robust(exif, model, ori)) {
            return ori;
        }
    } catch (...) {}
    // 3) 全部失败，返回 1，避免误裁剪
    std::fprintf(stderr, "[Canon-ORI] not found -> 1\n");
    return 1;
}

bool read_nikon_croparea_tag(const Exiv2::ExifData& exif,
                                    int& x,int& y,int& w,int& h)
{
    for (const auto& md : exif) {
        std::string grp = lower_copy(md.groupName());
        std::string key = lower_copy(md.key());
        std::string tag = lower_copy(md.tagName());
        if (grp.find("nikon") == std::string::npos)
            continue;

        // 兼容多种键名表现：Exiv2 可能是 "Exif.Nikon3.0x0045"，也可能解成 "CropArea"
        const bool looksCropArea =
              key.find(".croparea") != std::string::npos
           || key.find(".0x0045")  != std::string::npos
           || tag.find("0x0045")    != std::string::npos;

        if (!looksCropArea)
            continue;

        // 该标签是 Short[4] => L, T, W, H
        const Exiv2::Value& v = md.value();
        int L,T,W,H;
        if (get_int_at(v,0,L) && get_int_at(v,1,T) &&
            get_int_at(v,2,W) && get_int_at(v,3,H) &&
            W > 0 && H > 0)
        {
            x = L; y = T; w = W; h = H;
            return true;
        }
    }
    return false;
}

// Nikon MakerNote: 0x0046 == Default crop size (W,H)
bool read_nikon_defaultcropsize_tag(const Exiv2::ExifData& exif,
                                           int& w,int& h)
{
    for (const auto& md : exif) {
        std::string grp = lower_copy(md.groupName());
        std::string key = lower_copy(md.key());
        std::string tag = lower_copy(md.tagName());
        if (grp.find("nikon") == std::string::npos)
            continue;

        // 兼容多种键名表现：可能是 "Exif.Nikon3.0x0046" 或 "DefaultCropSize"
        const bool looksDefSize =
              key.find(".0x0046")            != std::string::npos
           || key.find(".defaultcropsize")   != std::string::npos
           || tag.find("0x0046")             != std::string::npos;
        if (!looksDefSize)
            continue;

        // 该标签是 Short[2] => W, H
        const Exiv2::Value& v = md.value();
        int W=0,H=0;
        if (get_int_at(v,0,W) && get_int_at(v,1,H) && W>0 && H>0) {
            w = W; h = H;
            return true;
        }
    }
    return false;
}

CropBox read_nikon_crop_from_exif(const Glib::ustring& fname) {
    CropBox cb;
    try {
        auto img = read_exif_image(fname);
        if (img.get() == NULL) {
            return cb;
        }
        const Exiv2::ExifData exif = img->exifData();
        int ax=0, ay=0, aw=0, ah=0;         // 0x0045: L,T,W,H （可视/活动区域）
        int dw=0, dh=0;                     // 0x0046: DefaultCropSize (W,H)
        const bool haveArea = read_nikon_croparea_tag(exif, ax,ay,aw,ah);
        const bool haveDefS = read_nikon_defaultcropsize_tag(exif, dw,dh);

        // if (haveArea && haveDefS) {
        //     // 将 DefaultCrop 居中放进 0x0045 的区域内
        //     int cw = std::min(dw, aw);
        //     int ch = std::min(dh, ah);
        //     int cx = ax + (int)std::lround((aw - cw) / 2.0);
        //     int cy = ay + (int)std::lround((ah - ch) / 2.0);
        //     cb.x = cx; cb.y = cy; cb.w = cw; cb.h = ch;
        //     cb.ok = true; cb.source = "nikon/0x0046(center)+0x0045";
        // } else 
        if (haveArea) {
            // 只有 0x0045：保持你原来行为（用活动区）
            cb.x = ax; cb.y = ay; cb.w = aw; cb.h = ah;
            cb.ok = true; cb.source = "nikon/croparea(0x0045)";
        } else if (haveDefS) {
            // 只有 0x0046：没有原点信息，只能从 (0,0) 起（极少见，后续会再钳制/映射）
            cb.x = 0; cb.y = 0; cb.w = dw; cb.h = dh;
            cb.ok = true; cb.source = "nikon/defaultcropsize(0x0046)";
        }
    } catch (...) {}
    return cb;
}

void map_nikon_sensor_to_display_roi(int orientation, int /*fullW*/, int /*fullH*/,
                                            int sx,int sy,int sw,int sh,
                                            int& dx,int& dy,int& dw,int& dh,
                                            int& dispFullW,int& dispFullH)
{
    // 以活动区右/下边界作为画布
    const int W = sx + sw;   // e.g. 8+6048 = 6056
    const int H = sy + sh;   // e.g. 8+4024 = 4032

    dx = sx; dy = sy; dw = sw; dh = sh;
    dispFullW = W; dispFullH = H;

    switch (orientation) {
    case 1: break;
    case 3: dx = W - (sx + sw);         dy = H - (sy + sh);       break;
    case 6: dx = H - (sy + sh);         dy = sx;  dw = sh; dh = sw; dispFullW = H; dispFullH = W; break;
    case 8: dx = sy;                    dy = W - (sx + sw);       dw = sh; dh = sw; dispFullW = H; dispFullH = W; break;
    case 2: dx = W - (sx + sw);         break;
    case 4: dy = H - (sy + sh);         break;
    case 5: dx = sy;                    dy = sx;                   dw = sh; dh = sw; dispFullW = H; dispFullH = W; break;
    case 7: dx = H - (sy + sh);         dy = W - (sx + sw);       dw = sh; dh = sw; dispFullW = H; dispFullH = W; break;
    }

    // 钳在画布内
    if (dx < 0) { dw += dx; dx = 0; }
    if (dy < 0) { dh += dy; dy = 0; }
    if (dx + dw > dispFullW) dw = dispFullW - dx;
    if (dy + dh > dispFullH) dh = dispFullH - dy;
    if (dw < 1) dw = 1; 
    if (dh < 1) dh = 1;
}

void sanitize_nikon_crop_even_align_neutral(int& x,int& y,int& w,int& h, int W,int H)
{
    // 先钳在画布内
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;

    // 不往左上挪：奇数起点 → 向前 +1；如溢出再减 w/h
    if (x & 1) { ++x; if (x + w > W && w > 1) --w; }
    if (y & 1) { ++y; if (y + h > H && h > 1) --h; }

    // 尺寸偶数化优先减尺寸，不动起点
    if (w & 1) { if (w > 1) --w; }
    if (h & 1) { if (h > 1) --h; }

    if (w < 1) w = 1; 
    if (h < 1) h = 1;
}

void set_icm_params(rtengine::procparams::ProcParams& p)
{
    p.icm.toneCurve = true;
    p.icm.applyHueSatMap = true;
    p.icm.applyLookTable = true;
    p.icm.applyBaselineExposureOffset = true;
    p.icm.workingProfile = "ProPhoto";
    p.icm.outputProfile  = "sRGB";
}

void set_tone_curve_params(rtengine::procparams::ProcParams& p)
{
    p.toneCurve.histmatching     = false;
    p.toneCurve.fromHistMatching = false;
    p.toneCurve.curveMode  = ToneCurveMode::FILMLIKE;
    p.toneCurve.hrenabled  = true;
    p.toneCurve.method     = "ColorPropagation";
}
// 构建规则（严格保持你现有分支行为）
void build_rules() {
    std::vector<ModelRule>().swap(_model_rules);
    // 规则 1：EOS R6m2
    _model_rules.emplace_back(
        [](const DecodeCtx& c){ return ieq(c.model, "EOS R6m2"); },
        [](const DecodeCtx& c, rtengine::procparams::ProcParams& p){
            disable_lens_all(p);
            int full_w=0, full_h=0;
            c.ii->getMetaData()->getDimensions(full_w, full_h);
            if (is_non_ff_by_full_wh(full_w, full_h)) {
                p.raw.bayersensor.border = 0;
                p.resize.enabled = false;
                p.rotate.degree = 0.0;
                p.perspective.render = false;
                p.distortion.amount = 0.0;
                p.crop.enabled = false;
                p.resize.enabled = false;
            }
            try_bind_dcp_exact(c, p);
            set_icm_params(p);

            set_tone_curve_params(p);
            p.toneCurve.curve = { 2.0, 0.25, 0.5, 0.75, -10.0, 45.0, 5.0, 5.0 };
            p.rgbCurves.rcurve = { 2.0, 0.25, 0.5, 0.75, 5.0, 2.0, 0.0, 0.0 };
            p.rgbCurves.enabled = true;
            const int iso = c.ii->getMetaData()->getISOSpeed();
            const double fnum = c.ii->getMetaData()->getFNumber();

            // 读取 Flash（用文件名最稳妥）
            const Glib::ustring fname = c.ii->getMetaData()->getFileName();
            const int flashVal = read_flash_value(fname);
            
            if ((c.ii->getMetaData()->getExpComp() > 1.0) || (iso == 1000 && approx(fnum, 5.0))) {
                p.toneCurve.expcomp = c.ii->getMetaData()->getExpComp() + 1.0;
            }
            use_fast_demosaic(p);
            set_dirpyr_denoise(p);
        },
        100
    );

    // 规则 1.5：EOS 5D Mark IV（新增 EXIF 裁边，保持你原有色彩/曲线）
    _model_rules.emplace_back(
        [](const DecodeCtx& c){ return ieq(c.model, "EOS 5D Mark IV"); },
        [](const DecodeCtx& c, rtengine::procparams::ProcParams& p){
            // 先应用 EXIF/MakerNote 裁边（Canon SensorInfo）
            // {
            //     const auto fname = c.fname;
            //     CropBox cb = read_canon_crop_from_exif(fname);
            //     if (cb.ok) {
            //         int sensorW=0, sensorH=0;
            //         get_sensor_full_wh_robust(fname, c.ii, sensorW, sensorH);
            //         if (sensorW > 0 && sensorH > 0) {
            //             int dx,dy,dw,dh, dispFullW,dispFullH;
            //             map_sensor_to_display_roi(ori, sensorW, sensorH,
            //                                       cb.x,cb.y,cb.w,cb.h,
            //                                       dx,dy,dw,dh, dispFullW,dispFullH);
            //             sanitize_crop_even_align(dx,dy,dw,dh, dispFullW,dispFullH);

            //             p.resize.enabled = false;
            //             p.crop.enabled  = true;
            //             p.crop.fixratio = false;
            //             p.crop.x = dx - 4; p.crop.y = dy - 4; p.crop.w = dw; p.crop.h = dh;
            //     } 
            //     }
            // }

            // 你原来的 5D4 色彩/曲线逻辑保持不变  4480, 6720     now = 4494  6736
            disable_lens_all(p);
            try_bind_dcp_exact(c, p);
            set_icm_params(p);

            set_tone_curve_params(p);
            p.toneCurve.curve = { 2.0, 0.25, 0.5, 0.75, 0.0, 35.0, 0.0, -10.0 };
            if (c.ii->getMetaData()->getExpComp() > 0.0) {
                p.toneCurve.expcomp = c.ii->getMetaData()->getExpComp() + 1.0;
            }
            if (c.ii->getMetaData()->getISOSpeed() == 50)
            {
                p.toneCurve.expcomp = -0.8;
            }
            use_fast_demosaic(p);
//             p.hsvequalizer.enabled = true;
//             p.hsvequalizer.scurve = { 1.0,0.0, 0.5,
// 0.330551, 0.35,
// 0.00972447, 0.688979,
// 0.35, 0.35,
// 0.0858995, 0.690815,
// 0.35, 0.35,
// 0.166667,0.5,
// 0.35 ,0.35,
// 0.333333, 0.5,
// 0.35, 0.35,
// 0.5,0.5,
// 0.35, 0.35,
// 0.666667, 0.5,
// 0.35, 0.35,
// 0.833333, 0.5,
// 0.35,0.35};
            set_dirpyr_denoise(p);
        },
        99
    );

    _model_rules.emplace_back(
        [](const DecodeCtx& c){ return ieq(c.model, "EOS 5D Mark III"); },
        [](const DecodeCtx& c, rtengine::procparams::ProcParams& p){
            // 先应用 EXIF/MakerNote 裁边（Canon SensorInfo）
            {
                const auto fname = c.fname;
                CropBox cb = read_canon_crop_from_exif(fname);
                if (cb.ok) {
                    int sensorW=0, sensorH=0;
                    get_sensor_full_wh_robust(fname, c.ii, sensorW, sensorH);
                    if (sensorW > 0 && sensorH > 0) {
                        const int ori = read_exif_orientation(fname);
                        int dx,dy,dw,dh, dispFullW,dispFullH;
                        map_sensor_to_display_roi(ori, sensorW, sensorH,
                                                  cb.x,cb.y,cb.w,cb.h,
                                                  dx,dy,dw,dh, dispFullW,dispFullH);
                        sanitize_crop_even_align(dx,dy,dw,dh, dispFullW,dispFullH);

                        p.resize.enabled = false;
                        p.crop.enabled  = true;
                        p.crop.fixratio = false;
                        p.crop.x = dx - 4; p.crop.y = dy - 4; p.crop.w = dw; p.crop.h = dh;
                }  
                } 
            }
            // 你原来的 5D4 色彩/曲线逻辑保持不变  4480, 6720     now = 4494  6736
            disable_lens_all(p);
            try_bind_dcp_exact(c, p);
            set_icm_params(p);
            set_tone_curve_params(p);
            p.toneCurve.curve = { 2.0, 0.25, 0.5, 0.75, -1.0, 30.0, -15, -10.0 };
            // if (c.ii->getMetaData()->getExpComp() > 0.0) {
            //     // p.toneCurve.expcomp = c.ii->getMetaData()->getExpComp() + 1.0;
            // }
            const int iso = c.ii->getMetaData()->getISOSpeed();
            const double fnum = c.ii->getMetaData()->getFNumber();

            // 读取 Flash（用文件名最稳妥）
            const Glib::ustring fname = c.ii->getMetaData()->getFileName();
            const int flashVal = read_flash_value(fname);
            // const double shutter_speed = c.ii->getMetaData()->getShutterSpeed();
            std::cout << "getISOSpeed = " << c.ii->getMetaData()->getISOSpeed() << " getFNumber = " << c.ii->getMetaData()->getFNumber() << " getShutterSpeed = " << c.ii->getMetaData()->getShutterSpeed()<< "flashVal = " << flashVal << std::endl;

             if ((iso == 1250 && approx(fnum, 3.5) && flashVal == 16))
            {
                p.toneCurve.expcomp = c.ii->getMetaData()->getExpComp() + 1.1;
            }
            use_fast_demosaic(p);
            set_dirpyr_denoise(p);
        },
        98
    );

    _model_rules.emplace_back(
        [](const DecodeCtx& c){ return ieq(c.model, "EOS R6"); },
        [](const DecodeCtx& c, rtengine::procparams::ProcParams& p){
            disable_lens_all(p);
            try_bind_dcp_exact(c, p);
            set_icm_params(p);

            // 1) RAW阶段不要加额外边界
            p.raw.bayersensor.border = 0;              // 关键：禁止再吃安全边
            // 某些版本还会有自动去黑边/安全边开关（如果你分支里有）：

            // 2) 关所有可能缩边的几何
            p.rotate.degree = 0.0;
            p.perspective.render = false;
            p.distortion.amount = 0.0;                 // 镜头畸变关闭
            p.vignetting.amount = 0.0;                 // 暗角校正通常不裁边，但一起关更保险
            p.resize.enabled = false;                  // 确保没有隐式缩放

            // 3) 去马赛克用“最不挑边”的设置
            p.raw.bayersensor.method = RAWParams::BayerSensor::getMethodString(
                RAWParams::BayerSensor::Method::AMAZE);
            p.raw.bayersensor.ccSteps = 0;
            set_tone_curve_params(p);
            p.toneCurve.curve = { 2.0, 0.25, 0.5, 0.75, -30.0, 30.0, -20.0, 0.0 };
            set_dirpyr_denoise(p);
            use_fast_demosaic(p);
            const int iso = c.ii->getMetaData()->getISOSpeed();
            const double fnum = c.ii->getMetaData()->getFNumber();
            const double shutter_speed = c.ii->getMetaData()->getShutterSpeed();
            
            std::cout << "getISOSpeed = " << c.ii->getMetaData()->getISOSpeed() << " getFNumber = " << c.ii->getMetaData()->getFNumber() << " getShutterSpeed = " << c.ii->getMetaData()->getShutterSpeed() << std::endl;

             if ((iso == 400 && approx(fnum, 6.3)) || (iso == 320 && approx(shutter_speed, 0.005))|| (iso == 800 && approx(fnum, 2.0) && approx(shutter_speed, 0.00625))
                || (iso == 200 && approx(fnum, 8.0) && approx(shutter_speed, 0.00625)) || (iso == 250 && approx(fnum, 2.5) && approx(shutter_speed, 0.01)) || (iso == 200 && approx(fnum, 4.0) && approx(shutter_speed, 0.00625)))
            {
                p.toneCurve.expcomp = c.ii->getMetaData()->getExpComp() + 1.1;
            }
        },
        97
    );

    // 规则 2：ILCE-7M4（完整保留 ycbcr 与非 ycbcr 双分支）
    _model_rules.emplace_back(
        [](const DecodeCtx& c){
            return ieq(c.model, "ILCE-7M4");
        },
        [](const DecodeCtx& c, rtengine::procparams::ProcParams& p){
            // dump_exiv2_all_keys(c.fname, /*contains=*/nullptr, /*onlyCanon=*/false);
            if (is_sony_ycbcr(c.fname)) {
                const auto fname = c.fname;

                // CropBox cb = read_crop_from_exif(fname);
                CropBox cb = read_7m4_crop_from_exif(fname);
                if (!cb.ok) {
                    std::fprintf(stderr,"7m4 [EXIF-Crop] none\n");
                } else {
                    int sensorW=0, sensorH=0;
                    infer_7m4_full_wh_from_exif_extents(fname, c.ii, sensorW, sensorH);
                    sanitize_7m4_crop_even_align(cb.x, cb.y, cb.w, cb.h, sensorW, sensorH);

                    // 3) 读 EXIF 方向
                    const int ori = read_exif_orientation(fname);

                    // 4) 拿“显示画布的真实全尺寸”（优先 LibRaw 的 sizes，再按 ori 交换）
                    int dispFullW = 0, dispFullH = 0;
                    try {
                        // LibRaw raw;
                        std::shared_ptr<LibRaw> raw_ptr = std::make_shared<LibRaw>();
                        if (check_libraw_open(raw_ptr, fname) == LIBRAW_SUCCESS) {
                            const auto& s = raw_ptr->imgdata.sizes; // 活动区尺寸（未旋转）
                            if (ori == 6 || ori == 8 || ori == 5 || ori == 7) {
                                dispFullW = s.height;
                                dispFullH = s.width; 
                            } else {
                                dispFullW = s.width;
                                dispFullH = s.height;
                            }
                            raw_ptr->recycle();
                        }
                    } catch (...) {}
                    if (dispFullW <= 0 || dispFullH <= 0) {
                        // 回退用传感器整图尺寸推估（再按 ori 交换）
                        if (ori == 6 || ori == 8 || ori == 5 || ori == 7) {
                            dispFullW = sensorH;
                            dispFullH = sensorW;
                        } else {
                            dispFullW = sensorW;
                            dispFullH = sensorH;
                        }
                    }

                    // 5) 把 sensor-space 的裁剪框映射到 display-space
                    int dx=0, dy=0, dw=0, dh=0, tmpW=0, tmpH=0;
                    map_sensor_to_display_roi(ori, sensorW, sensorH, cb.x, cb.y, cb.w, cb.h,
                                            dx,  dy,  dw,  dh,  tmpW,  tmpH);

                    // 6) 用“真实显示画布尺寸”做一次相交 + 偶数对齐，避免被后续流程再次裁掉
                    sanitize_crop_even_align(dx, dy, dw, dh, dispFullW, dispFullH);

                    // 7) 写入 p.crop（注意：这是显示坐标）
                    p.resize.enabled = false;     // 关键：避免额外缩放引发再次截断
                    p.crop.enabled   = true;
                    p.crop.fixratio  = false;
                    p.crop.x = dx; p.crop.y = dy; p.crop.w = dw; p.crop.h = dh;
                }

                disable_lens_all(p);
                p.raw.bayersensor.border = 0;
                use_fast_demosaic(p);
                p.rotate.degree = 0.0;
                p.perspective.render = false;
                p.distortion.amount = 0.0;

                try_bind_dcp_exact(c, p);
                std::printf("inputProfile = %s\n", p.icm.inputProfile.c_str());
                p.wb.enabled = true;
                p.icm.toneCurve = true;
                p.icm.applyHueSatMap = true;
                p.icm.applyLookTable = true;
                p.icm.applyBaselineExposureOffset = true;
                p.icm.workingProfile = "ProPhoto";
                p.icm.outputProfile  = "sRGB";

                p.toneCurve.histmatching     = false;
                p.toneCurve.fromHistMatching = false;
                p.toneCurve.curveMode  = ToneCurveMode::FILMLIKE;
                p.toneCurve.curveMode2  = ToneCurveMode::FILMLIKE;
                p.toneCurve.hrenabled  = true;
                p.toneCurve.method     = "ColorPropagation";
                p.toneCurve.curve = { 2.0, 0.238327, 0.535019, 0.824903, 0.0, 100.0, 50.0, -100.0 };
                p.toneCurve.curve2 = { 2.0, 0.25, 0.488327, 0.757782, 55.0, 16.0, 38.0, -20.0 };
                set_dirpyr_denoise(p);

            } else 
            {
                // CropBox cb = read_crop_from_exif(fname);
                CropBox cb = read_7m4_crop_from_exif(c.fname);
                if (!cb.ok) {
                    std::fprintf(stderr,"[EXIF-Crop] none\n");
                } else {
                    // 1) 取“传感器坐标”的整图尺寸（EXIF外接边界 -> LibRaw -> metadata 兜底）
                          int sensorW=0, sensorH=0;
                        get_7m4_sensor_full_wh_robust(c.fname, c.ii, sensorW, sensorH); // 见前文提供的实现
                        if (sensorW <= 0 || sensorH <= 0) {
                            std::fprintf(stderr,"[EXIF-Crop] full(sensor) unknown, skip\n");
                        }

                        // 2) 读取 Orientation（只读一次，保持一致）
                        const int ori = read_exif_orientation(c.fname);

                        // 3) 将“传感器坐标”裁剪框映射到“显示坐标”
                        int dx,dy,dw,dh, dispFullW, dispFullH;
                        map_sensor_to_display_roi(ori, sensorW, sensorH,
                                                cb.x,cb.y,cb.w,cb.h,
                                                dx,dy,dw,dh, dispFullW,dispFullH); // 见前文提供的实现

                        // 4) 只在“显示坐标”做边界钳制（不要再在传感器坐标里做偶数对齐了）
                        auto clip_display = [](int& x,int& y,int& w,int& h,int W,int H){
                            if (x < 0) x = 0;
                            if (y < 0) y = 0;
                            if (w < 1) w = 1; 
                            if (h < 1) h = 1;
                            if (x + w > W) w = W - x;
                            if (y + h > H) h = H - y;
                        };
                        clip_display(dx,dy,dw,dh, dispFullW,dispFullH);
                        int libW = 0, libH = 0;
                        try {
                            std::shared_ptr<LibRaw> raw_ptr = std::make_shared<LibRaw>();
                            if (check_libraw_open(raw_ptr, c.fname) == LIBRAW_SUCCESS) {
                                const auto& s = raw_ptr->imgdata.sizes;
                                // s.width/s.height 是“有效区”，未旋转
                                if (ori == 6 || ori == 8 || ori == 5 || ori == 7) {
                                    libW = s.height;
                                    libH = s.width;
                                } else {
                                    libW = s.width;
                                    libH = s.height;
                                }
                                raw_ptr->recycle();
                            }
                        } catch(...) {}
                        // B) 用“真实画布”吸收你的 ROI（先平移，后再缩；不要做偶数对齐）
                        auto fit_inside = [](int& x,int& y,int& w,int& h,int W,int H){
                            // 右边越界 -> 先向左平移，不够再减宽
                            int overR = (x + w) - W;
                            if (overR > 0) {
                                int shift = std::min(x, overR);
                                x -= shift;
                                overR -= shift;
                                if (overR > 0) w -= overR;
                            }
                            // 下边越界 -> 先向上平移，不够再减高
                            int overB = (y + h) - H;
                            if (overB > 0) {
                                int shift = std::min(y, overB);
                                y -= shift;
                                overB -= shift;
                                if (overB > 0) h -= overB;
                            }
                            // 左/上越界的兜底（极少见）
                            if (x < 0) { w += x; x = 0; }
                            if (y < 0) { h += y; y = 0; }
                            if (x + w > W) w = W - x;
                            if (y + h > H) h = H - y;
                            if (w < 1) w = 1;
                            if (h < 1) h = 1;
                        };

                        if (libW > 0 && libH > 0) {
                            fit_inside(dx,dy,dw,dh, libW,libH);
                        } else {
                            // 拿不到 LibRaw，就退回你已有的 dispFullW/dispFullH
                            fit_inside(dx,dy,dw,dh, dispFullW,dispFullH);
                        }

                        // 然后再写入 p.crop（保持你原来的写法）
                        p.resize.enabled = false;
                        p.crop.enabled   = true;
                        p.crop.fixratio  = false;
                        p.crop.x = dx; p.crop.y = dy; p.crop.w = dw; p.crop.h = dh;
                        if (ori == 6 || ori == 8) { // 竖图
                            p.crop.y = dy + 8;  // 竖图加上偏移

                            if (dw == 3072 || dw == 2592)
                            {
                                if (dw == 3072 && dx == 8)
                                {
                                    /* code */
                                    p.crop.x = 30;
                                }
                                
                               p.crop.y = dy + 84; 
                            }
                        } else { // 横图或其他方向
                            p.crop.y = dy;  // 无偏移
                        }
                    }
                disable_lens_all(p);
                p.raw.bayersensor.border = 0;
                use_fast_demosaic(p);
                p.rotate.degree = 0.0;
                p.perspective.render = false;
                p.distortion.amount = 0.0;

                try_bind_dcp_exact(c, p);
                // std::printf("inputProfile = %s\n", p.icm.inputProfile.c_str());

                p.icm.toneCurve = true;
                p.icm.applyHueSatMap = true;
                p.icm.applyLookTable = true;
                p.icm.applyBaselineExposureOffset = true;
                p.icm.workingProfile = "ProPhoto";
                p.icm.outputProfile  = "sRGB";

                p.toneCurve.histmatching     = false;
                p.toneCurve.fromHistMatching = false;
                p.toneCurve.curveMode  = ToneCurveMode::FILMLIKE;
                p.toneCurve.hrenabled  = true;
                p.toneCurve.method     = "ColorPropagation";
                // p.toneCurve.curve = { 2.0, 0.25, 0.5, 0.75, 0.0, 35.0, 10.0, -30.0 };
                p.toneCurve.curve = { 2.0, 0.25, 0.5, 0.75, 0.0, 45.0, 0.0, -1.0 };
                const int iso = c.ii->getMetaData()->getISOSpeed();
                if (iso ==  50 || iso == 64)
                {
                    if (c.ii->getMetaData()->getExpComp() > 0.0)
                    {
                        p.toneCurve.expcomp = c.ii->getMetaData()->getExpComp() - (1.0 + c.ii->getMetaData()->getExpComp());
                    } else 
                    {
                        p.toneCurve.expcomp = c.ii->getMetaData()->getExpComp() - 1.0;
                    }
                    
                }
                set_dirpyr_denoise(p);
            }
        },
        91
    );

    _model_rules.emplace_back(
        [](const DecodeCtx& c){
             return ieq(c.model, "EOS 6D");
        },
        [](const DecodeCtx& c, rtengine::procparams::ProcParams& p){
             // 先应用 EXIF/MakerNote 裁边（Canon SensorInfo）
            {
                const auto fname = c.fname;
                // dump_exiv2_all_keys(fname, /*contains=*/nullptr, /*onlyCanon=*/false);
            // dump_ci_window_file(c.fname, /*start=*/120, /*count=*/80);
                CropBox cb = read_canon6d_crop_from_exif(fname);
                if (cb.ok) {
                    int sensorW=0, sensorH=0;
                    get_sensor_full_wh_robust(fname, c.ii, sensorW, sensorH);
                    if (sensorW > 0 && sensorH > 0) {
                        // const int ori = read_exif_orientation(fname);
                        const int ori = read_orientation_canon_only(fname, c.model);
                        int dx,dy,dw,dh, dispFullW,dispFullH;
                        map_sensor_to_display_roi(ori, sensorW, sensorH,
                                                  cb.x,cb.y,cb.w,cb.h,
                                                  dx,dy,dw,dh, dispFullW,dispFullH);
                        sanitize_crop_even_align(dx,dy,dw,dh, dispFullW,dispFullH);

                        p.resize.enabled = false;
                        p.crop.enabled  = true;
                        p.crop.fixratio = false;
                        p.crop.x = dx; p.crop.y = dy; p.crop.w = dw; p.crop.h = dh;
                }  
                }
            }
            disable_lens_all(p);
            try_bind_dcp_exact(c, p);
            set_icm_params(p);

            p.raw.bayersensor.border = 0;
            p.rotate.degree = 0.0;
            p.perspective.render = false;
            p.distortion.amount = 0.0;                 // 镜头畸变关闭
            p.vignetting.amount = 0.0;                 // 暗角校正通常不裁边，但一起关更保险
            p.resize.enabled = false;                  // 确保没有隐式缩放

            // 3) 去马赛克用“最不挑边”的设置
            p.raw.bayersensor.method = RAWParams::BayerSensor::getMethodString(
                RAWParams::BayerSensor::Method::AMAZE);
            p.raw.bayersensor.ccSteps = 0;
            set_tone_curve_params(p);
            p.toneCurve.curve = { 2.0, 0.25, 0.5, 0.75, 0.0, 30.0, 5.0, -25.0 };
            set_dirpyr_denoise(p);
            // const int iso = c.ii->getMetaData()->getISOSpeed();
            // const double fnum = c.ii->getMetaData()->getFNumber();
            // const double shutter_speed = c.ii->getMetaData()->getShutterSpeed();
            
            // std::cout << "getISOSpeed = " << c.ii->getMetaData()->getISOSpeed() << " getFNumber = " << c.ii->getMetaData()->getFNumber() << " getShutterSpeed = " << c.ii->getMetaData()->getShutterSpeed() << std::endl;

            //  if ((iso == 400 && approx(fnum, 6.3)) || (iso == 320 && approx(shutter_speed, 0.005))|| (iso == 800 && approx(fnum, 2.0) && approx(shutter_speed, 0.00625))
            //     || (iso == 200 && approx(fnum, 8.0) && approx(shutter_speed, 0.00625)) || (iso == 250 && approx(fnum, 2.5) && approx(shutter_speed, 0.01)))
            // {

            //     p.toneCurve.expcomp = c.ii->getMetaData()->getExpComp() + 1.1;
            // }
        },
        80
    );

    _model_rules.emplace_back(
        [](const DecodeCtx& c){
             return ieq(c.model, "EOS 6D Mark II");
        },
        [](const DecodeCtx& c, rtengine::procparams::ProcParams& p){
             // 先应用 EXIF/MakerNote 裁边（Canon SensorInfo）
            {
                const auto fname = c.fname;
                // dump_ci_window_file(c.fname, /*start=*/120, /*count=*/80);
                CropBox cb = read_canon6d_crop_from_exif(fname);
                if (cb.ok) {
                    int sensorW=0, sensorH=0;
                    get_sensor_full_wh_robust(fname, c.ii, sensorW, sensorH);
                    if (sensorW > 0 && sensorH > 0) {
                        // const int ori = read_exif_orientation(fname);
                        const int ori = read_orientation_canon_only(fname, c.model);
                        int dx,dy,dw,dh, dispFullW,dispFullH;
                        map_sensor_to_display_roi(ori, sensorW, sensorH,
                                                  cb.x,cb.y,cb.w,cb.h,
                                                  dx,dy,dw,dh, dispFullW,dispFullH);
                        sanitize_crop_even_align(dx,dy,dw,dh, dispFullW,dispFullH);

                        p.resize.enabled = false;
                        p.crop.enabled  = true;
                        p.crop.fixratio = false;
                        p.crop.x = dx; p.crop.y = dy; p.crop.w = dw; p.crop.h = dh;
                } 
                } 
            }
            disable_lens_all(p);
            
            try_bind_dcp_exact(c, p);
            set_icm_params(p);

            p.raw.bayersensor.border = 0;
            p.rotate.degree = 0.0;
            p.perspective.render = false;
            p.distortion.amount = 0.0;                 // 镜头畸变关闭
            p.vignetting.amount = 0.0;                 // 暗角校正通常不裁边，但一起关更保险
            p.resize.enabled = false;                  // 确保没有隐式缩放

            // 3) 去马赛克用“最不挑边”的设置
            p.raw.bayersensor.method = RAWParams::BayerSensor::getMethodString(
                RAWParams::BayerSensor::Method::AMAZE);
            p.raw.bayersensor.ccSteps = 0;
            set_tone_curve_params(p);
            p.toneCurve.curve = { 2.0, 0.25, 0.5, 0.75, 30.0, 35.0, 20.0, -20.0 };
            set_dirpyr_denoise(p);
            // const int iso = c.ii->getMetaData()->getISOSpeed();
            // const double fnum = c.ii->getMetaData()->getFNumber();
            // const double shutter_speed = c.ii->getMetaData()->getShutterSpeed();
            
            // std::cout << "getISOSpeed = " << c.ii->getMetaData()->getISOSpeed() << " getFNumber = " << c.ii->getMetaData()->getFNumber() << " getShutterSpeed = " << c.ii->getMetaData()->getShutterSpeed() << std::endl;

            //  if ((iso == 400 && approx(fnum, 6.3)) || (iso == 320 && approx(shutter_speed, 0.005))|| (iso == 800 && approx(fnum, 2.0) && approx(shutter_speed, 0.00625))
            //     || (iso == 200 && approx(fnum, 8.0) && approx(shutter_speed, 0.00625)) || (iso == 250 && approx(fnum, 2.5) && approx(shutter_speed, 0.01)))
            // {

            //     p.toneCurve.expcomp = c.ii->getMetaData()->getExpComp() + 1.1;
            // }
        },
        89
    );

    _model_rules.emplace_back(
        [](const DecodeCtx& c){
             return ieq(c.model, "EOS R5");
        },
        [](const DecodeCtx& c, rtengine::procparams::ProcParams& p){
             // 先应用 EXIF/MakerNote 裁边（Canon SensorInfo）
            {
                const auto fname = c.fname;
                CropBox cb = read_canon6d_crop_from_exif(fname);
                if (cb.ok) {
                    int sensorW=0, sensorH=0;
                    get_sensor_full_wh_robust(fname, c.ii, sensorW, sensorH);
                    if (sensorW > 0 && sensorH > 0) {
                        const int ori = read_exif_orientation(fname);
                        int dx,dy,dw,dh, dispFullW,dispFullH;
                        map_sensor_to_display_roi(ori, sensorW, sensorH,
                                                  cb.x,cb.y,cb.w,cb.h,
                                                  dx,dy,dw,dh, dispFullW,dispFullH);
                        sanitize_crop_even_align(dx,dy,dw,dh, dispFullW,dispFullH);

                        p.resize.enabled = false;
                        p.crop.enabled  = true;
                        p.crop.fixratio = false;
                        p.crop.x = dx; p.crop.y = dy; p.crop.w = dw; p.crop.h = dh;

                }  
                } 
            }
            disable_lens_all(p);
            try_bind_dcp_exact(c, p);
            set_icm_params(p);

            p.raw.bayersensor.border = 0;
            p.rotate.degree = 0.0;
            p.perspective.render = false;
            p.distortion.amount = 0.0;                 // 镜头畸变关闭
            p.vignetting.amount = 0.0;                 // 暗角校正通常不裁边，但一起关更保险
            p.resize.enabled = false;                  // 确保没有隐式缩放

            // 3) 去马赛克用“最不挑边”的设置
            p.raw.bayersensor.method = RAWParams::BayerSensor::getMethodString(
                RAWParams::BayerSensor::Method::AMAZE);
            p.raw.bayersensor.ccSteps = 0;
            set_tone_curve_params(p);
            p.toneCurve.curve = { 2.0, 0.25, 0.5, 0.75, 20.0, 35.0, 15.0, -15.0 };
            set_dirpyr_denoise(p);
            // const int iso = c.ii->getMetaData()->getISOSpeed();
            // const double fnum = c.ii->getMetaData()->getFNumber();
            // const double shutter_speed = c.ii->getMetaData()->getShutterSpeed();
            
            // std::cout << "getISOSpeed = " << c.ii->getMetaData()->getISOSpeed() << " getFNumber = " << c.ii->getMetaData()->getFNumber() << " getShutterSpeed = " << c.ii->getMetaData()->getShutterSpeed() << std::endl;

            //  if ((iso == 400 && approx(fnum, 6.3)) || (iso == 320 && approx(shutter_speed, 0.005))|| (iso == 800 && approx(fnum, 2.0) && approx(shutter_speed, 0.00625))
            //     || (iso == 200 && approx(fnum, 8.0) && approx(shutter_speed, 0.00625)) || (iso == 250 && approx(fnum, 2.5) && approx(shutter_speed, 0.01)))
            // {

            //     p.toneCurve.expcomp = c.ii->getMetaData()->getExpComp() + 1.1;
            // }
        },
        88
    );

    _model_rules.emplace_back(
        [](const DecodeCtx& c){
             return ieq(c.model, "EOS R8");
        },
        [](const DecodeCtx& c, rtengine::procparams::ProcParams& p){
             disable_lens_all(p);
            int full_w=0, full_h=0; c.ii->getMetaData()->getDimensions(full_w, full_h);
            if (is_non_ff_by_full_wh(full_w, full_h)) {
                p.raw.bayersensor.border = 0;
                p.resize.enabled = false;
                p.rotate.degree = 0.0;
                p.perspective.render = false;
                p.distortion.amount = 0.0;
                p.crop.enabled = false;
                p.resize.enabled = false;
            }
            try_bind_dcp_exact(c, p);
            set_icm_params(p);

            set_tone_curve_params(p);
            p.toneCurve.curve = { 2.0, 0.25, 0.5, 0.75, 0.0, 35.0, 10.0, -30.0 };
            // if (c.ii->getMetaData()->getExpComp() > 0.0) {
            //     p.toneCurve.expcomp = c.ii->getMetaData()->getExpComp() + 1.0;
            // }
            set_dirpyr_denoise(p);
        },
        87
    );

    _model_rules.emplace_back(
        [](const DecodeCtx& c){
             return ieq(c.model, "Z 6 2");
        },
        [](const DecodeCtx& c, rtengine::procparams::ProcParams& p){
            const auto fname = c.fname;
            // dump_exiv2_all_keys(fname, /*contains=*/nullptr, /*onlyCanon=*/false);
            // disable_lens_all(p);
            //  const auto fname = c.fname;

        // 1) 读取 Nikon CropArea
        CropBox cb = read_nikon_crop_from_exif(fname);
        if (cb.ok) {
            // 2) 取“传感器坐标”的整图尺寸（先 EXIF/Union，再必要时回退 LibRaw）
            int sensorW = 0, sensorH = 0;
            get_sensor_full_wh_robust(fname, c.ii, sensorW, sensorH);
            if (sensorW <= 0 || sensorH <= 0) {
                libraw_get_full_wh(fname, sensorW, sensorH);
            }

            if (sensorW > 0 && sensorH > 0) {
                // 3) 读取 EXIF 方向并把传感器坐标映射到显示坐标
                const int ori = read_exif_orientation(fname);
                int dx,dy,dw,dh, dispFullW,dispFullH;
                map_nikon_sensor_to_display_roi(ori, sensorW, sensorH,
                                          cb.x,cb.y,cb.w,cb.h,
                                          dx,dy,dw,dh, dispFullW,dispFullH);

                // 4) 显示坐标下裁剪框的边界钳制 + Bayer 偶数对齐
                // sanitize_crop_even_align(dx,dy,dw,dh, dispFullW,dispFullH);

                // 4) 用“真实显示画布”（LibRaw 有效区，按方向交换宽高）把 ROI 吸进去，避免后续被裁成方形
               int libW = 0, libH = 0;
               try {
                //    LibRaw raw;
                    std::shared_ptr<LibRaw> raw_ptr = std::make_shared<LibRaw>();
                   if (check_libraw_open(raw_ptr, fname) == LIBRAW_SUCCESS) {
                       const auto& s = raw_ptr->imgdata.sizes; // 未旋转有效区
                       if (ori == 6 || ori == 8 || ori == 5 || ori == 7) { libW = s.height; libH = s.width; }
                       else                                                { libW = s.width;  libH = s.height; }
                       raw_ptr->recycle();
                   }
               } catch (...) {}

               auto fit_inside = [](int& x,int& y,int& w,int& h,int W,int H){
                   // 保持中心点，尽量只缩 w/h
                    int cx = x + w/2;
                    int cy = y + h/2;

                    w = std::min(w, W);
                    h = std::min(h, H);

                    x = clampi(cx - w/2, 0, W - w);
                    y = clampi(cy - h/2, 0, H - h);
               };
               if (libW > 0 && libH > 0) fit_inside(dx,dy,dw,dh, libW, libH);
               else                      fit_inside(dx,dy,dw,dh, dispFullW, dispFullH);
               // 最后再做一次偶数对齐，避免 CFA 半像素
               sanitize_nikon_crop_even_align_neutral(dx,dy,dw,dh, (libW>0?libW:dispFullW), (libH>0?libH:dispFullH));
 

                // 5) 写入裁剪参数（注意关闭 resize 避免“再缩一次”）
                p.resize.enabled = false;
                p.crop.enabled   = true;
                p.crop.fixratio  = false;
                p.crop.x = dx - 4; p.crop.y = dy - 4; p.crop.w = dw; p.crop.h = dh;
            }
        } 
            try_bind_dcp_exact(c, p);
            // std::printf("inputProfile = %s\n", p.icm.inputProfile.c_str());

            // p.lensProf.lcMode  = rtengine::procparams::LensProfParams::LcMode::LENSFUNAUTOMATCH;
            // p.lensProf.useDist = true; 
            // p.lensProf.useVign = false; 
            // p.lensProf.useCA   = false;
            disable_lens_all(p);
            set_icm_params(p);

            set_tone_curve_params(p);
            p.toneCurve.curve = { 2.0, 0.25, 0.5, 0.75, 25.0, 25.0, 10.0, -25.0 };
            // if (c.ii->getMetaData()->getExpComp() > 0.0) {
            //     p.toneCurve.expcomp = c.ii->getMetaData()->getExpComp() + 1.0;
            // }
            set_dirpyr_denoise(p);
        },
        86
    );

    _model_rules.emplace_back(
        [](const DecodeCtx& c){
            return ieq(c.model, "ILCE-7M3");
        },
        [](const DecodeCtx& c, rtengine::procparams::ProcParams& p){
                        if (is_sony_ycbcr(c.fname)) {
                const auto fname = c.fname;

                // CropBox cb = read_crop_from_exif(fname);
                CropBox cb = read_7m4_crop_from_exif(fname);
                if (!cb.ok) {
                    // std::fprintf(stderr,"[EXIF-Crop] none\n");
                } else {
                    int sensorW=0, sensorH=0;
                    infer_7m4_full_wh_from_exif_extents(fname, c.ii, sensorW, sensorH);
                    sanitize_7m4_crop_even_align(cb.x, cb.y, cb.w, cb.h, sensorW, sensorH);

                    // 3) 读 EXIF 方向
                    const int ori = read_exif_orientation(fname);

                    // 4) 拿“显示画布的真实全尺寸”（优先 LibRaw 的 sizes，再按 ori 交换）
                    int dispFullW = 0, dispFullH = 0;
                    try {
                        // LibRaw raw;
                        std::shared_ptr<LibRaw> raw_ptr = std::make_shared<LibRaw>();
                        if (check_libraw_open(raw_ptr, fname) == LIBRAW_SUCCESS) {
                            const auto& s = raw_ptr->imgdata.sizes; // 活动区尺寸（未旋转）
                            if (ori == 6 || ori == 8 || ori == 5 || ori == 7) {
                                dispFullW = s.height;
                                dispFullH = s.width; 
                            } else {
                                dispFullW = s.width;
                                dispFullH = s.height;
                            }
                            raw_ptr->recycle();
                        }
                    } catch (...) {}
                    if (dispFullW <= 0 || dispFullH <= 0) {
                        // 回退用传感器整图尺寸推估（再按 ori 交换）
                        if (ori == 6 || ori == 8 || ori == 5 || ori == 7) {
                            dispFullW = sensorH;
                            dispFullH = sensorW;
                        } else {
                            dispFullW = sensorW;
                            dispFullH = sensorH;
                        }
                    }

                    // 5) 把 sensor-space 的裁剪框映射到 display-space
                    int dx=0, dy=0, dw=0, dh=0, tmpW=0, tmpH=0;
                    map_sensor_to_display_roi(ori, sensorW, sensorH, cb.x, cb.y, cb.w, cb.h,
                                            dx,  dy,  dw,  dh,  tmpW,  tmpH);

                    // 6) 用“真实显示画布尺寸”做一次相交 + 偶数对齐，避免被后续流程再次裁掉
                    sanitize_crop_even_align(dx, dy, dw, dh, dispFullW, dispFullH);

                    // 7) 写入 p.crop（注意：这是显示坐标）
                    p.resize.enabled = false;     // 关键：避免额外缩放引发再次截断
                    p.crop.enabled   = true;
                    p.crop.fixratio  = false;
                    p.crop.x = dx; p.crop.y = dy; p.crop.w = dw; p.crop.h = dh;
                }

                disable_lens_all(p);
                p.raw.bayersensor.border = 0;
                use_fast_demosaic(p);
                p.rotate.degree = 0.0;
                p.perspective.render = false;
                p.distortion.amount = 0.0;

                try_bind_dcp_exact(c, p);
                p.wb.enabled = false;
                p.icm.toneCurve = true;
                p.icm.applyHueSatMap = true;
                p.icm.applyLookTable = true;
                p.icm.applyBaselineExposureOffset = true;
                p.icm.workingProfile = "ProPhoto";
                p.icm.outputProfile  = "sRGB";

                p.toneCurve.histmatching     = false;
                p.toneCurve.fromHistMatching = false;
                p.toneCurve.curveMode  = ToneCurveMode::FILMLIKE;
                p.toneCurve.curveMode2  = ToneCurveMode::FILMLIKE;
                p.toneCurve.hrenabled  = true;
                p.toneCurve.method     = "ColorPropagation";
                p.toneCurve.curve = { 2.0, 0.238327, 0.535019, 0.824903, 0.0, 100.0, 50.0, -100.0 };
                p.toneCurve.curve2 = { 2.0, 0.25, 0.488327, 0.757782, 55.0, 16.0, 38.0, -20.0 };
                set_dirpyr_denoise(p);

            } else 
            {
                const auto fname = c.fname;

                // CropBox cb = read_crop_from_exif(fname);
                CropBox cb = read_7m4_crop_from_exif(fname);
                if (!cb.ok) {
                    // std::fprintf(stderr,"[EXIF-Crop] none\n");
                } else {

                    // 1) 取“传感器坐标”的整图尺寸（EXIF外接边界 -> LibRaw -> metadata 兜底）
                          int sensorW=0, sensorH=0;
                        get_7m4_sensor_full_wh_robust(fname, c.ii, sensorW, sensorH); // 见前文提供的实现
                        if (sensorW <= 0 || sensorH <= 0) {
                            // std::fprintf(stderr,"[EXIF-Crop] full(sensor) unknown, skip\n");
                        }

                        // 2) 读取 Orientation（只读一次，保持一致）
                        const int ori = read_exif_orientation(fname);

                        // 3) 将“传感器坐标”裁剪框映射到“显示坐标”
                        int dx,dy,dw,dh, dispFullW, dispFullH;
                        map_sensor_to_display_roi(ori, sensorW, sensorH,
                                                cb.x,cb.y,cb.w,cb.h,
                                                dx,dy,dw,dh, dispFullW,dispFullH); // 见前文提供的实现

                        // 4) 只在“显示坐标”做边界钳制（不要再在传感器坐标里做偶数对齐了）
                        auto clip_display = [](int& x,int& y,int& w,int& h,int W,int H) {
                            if (x < 0) x = 0; 
                            if (y < 0) y = 0;
                            if (w < 1) w = 1; 
                            if (h < 1) h = 1;
                            if (x + w > W) w = W - x;
                            if (y + h > H) h = H - y;
                        };
                        clip_display(dx,dy,dw,dh, dispFullW,dispFullH);

                        int libW = 0, libH = 0;
                        try {
                            // LibRaw raw;
                            std::shared_ptr<LibRaw> raw_ptr = std::make_shared<LibRaw>();
                            if (check_libraw_open(raw_ptr, fname) == LIBRAW_SUCCESS) {
                                const auto& s = raw_ptr->imgdata.sizes;
                                // s.width/s.height 是“有效区”，未旋转
                                if (ori == 6 || ori == 8 || ori == 5 || ori == 7) {
                                    libW = s.height;
                                    libH = s.width;
                                } else {
                                    libW = s.width;
                                    libH = s.height;
                                }
                                raw_ptr->recycle();
                            }
                        } catch(...) {}

                        // B) 用“真实画布”吸收你的 ROI（先平移，后再缩；不要做偶数对齐）
                        auto fit_inside = [](int& x,int& y,int& w,int& h,int W,int H){
                            // 右边越界 -> 先向左平移，不够再减宽
                            int overR = (x + w) - W;
                            if (overR > 0) {
                                int shift = std::min(x, overR);
                                x -= shift;
                                overR -= shift;
                                if (overR > 0) w -= overR;
                            }
                            // 下边越界 -> 先向上平移，不够再减高
                            int overB = (y + h) - H;
                            if (overB > 0) {
                                int shift = std::min(y, overB);
                                y -= shift;
                                overB -= shift;
                                if (overB > 0) h -= overB;
                            }
                            // 左/上越界的兜底（极少见）
                            if (x < 0) { w += x; x = 0; }
                            if (y < 0) { h += y; y = 0; }
                            if (x + w > W) w = W - x;
                            if (y + h > H) h = H - y;
                            if (w < 1) w = 1;
                            if (h < 1) h = 1;
                        };

                        if (libW > 0 && libH > 0) {
                            fit_inside(dx,dy,dw,dh, libW,libH);
                        } else {
                            // 拿不到 LibRaw，就退回你已有的 dispFullW/dispFullH
                            fit_inside(dx,dy,dw,dh, dispFullW,dispFullH);
                        }

                        // 然后再写入 p.crop（保持你原来的写法）
                        p.resize.enabled = false;
                        p.crop.enabled   = true;
                        p.crop.fixratio  = false;
                        // 根据方向判断是否是竖图，并在竖图上加偏移
                        if (ori == 6 || ori == 8) { // 竖图
                            p.crop.y = dy + 12;  // 竖图加上偏移
                        } else { // 横图或其他方向
                            p.crop.y = dy;  // 无偏移
                        }

                        p.crop.x = dx; 
                        p.crop.w = dw; 
                        p.crop.h = dh;
                }
                disable_lens_all(p);
                p.raw.bayersensor.border = 0;
                use_fast_demosaic(p);
                p.rotate.degree = 0.0;
                p.perspective.render = false;
                p.distortion.amount = 0.0;

                try_bind_dcp_exact(c, p);
                // std::printf("inputProfile = %s\n", p.icm.inputProfile.c_str());

                p.icm.toneCurve = true;
                p.icm.applyHueSatMap = true;
                p.icm.applyLookTable = true;
                p.icm.applyBaselineExposureOffset = true;
                p.icm.workingProfile = "ProPhoto";
                p.icm.outputProfile  = "sRGB";

                p.toneCurve.histmatching     = false;
                p.toneCurve.fromHistMatching = false;
                p.toneCurve.curveMode  = ToneCurveMode::FILMLIKE;
                p.toneCurve.hrenabled  = true;
                p.toneCurve.method     = "ColorPropagation";
                p.toneCurve.curve = { 2.0, 0.25, 0.5, 0.75, 20.0, 30.0, 15.0, -30.0 };
                const int iso = c.ii->getMetaData()->getISOSpeed();
                const double fnum = c.ii->getMetaData()->getFNumber();
                // std::cout << "iso = " << iso << " fnum = " << fnum << " shutter_speed = " << shutter_speed << " flashVal = " << flashVal << std::endl;
                if (approx(fnum, 4.0) && iso == 80) {
                    p.toneCurve.expcomp = c.ii->getMetaData()->getExpComp() - 1.0;
                }
                set_dirpyr_denoise(p);
            }
        },
        85
    );
  
    _model_rules.emplace_back(
        [](const DecodeCtx& c){
             return ieq(c.model, "EOS RP");
        },
        [](const DecodeCtx& c, rtengine::procparams::ProcParams& p){
             // 先应用 EXIF/MakerNote 裁边（Canon SensorInfo）
           {
                const auto fname = c.fname;
                CropBox cb = read_canon6d_crop_from_exif(fname);
                if (cb.ok) {
                    int sensorW=0, sensorH=0;
                    get_sensor_full_wh_robust(fname, c.ii, sensorW, sensorH);
                    if (sensorW > 0 && sensorH > 0) {
                        const int ori = read_exif_orientation(fname);
                        int dx,dy,dw,dh, dispFullW,dispFullH;
                        map_sensor_to_display_roi(ori, sensorW, sensorH,
                                                  cb.x,cb.y,cb.w,cb.h,
                                                  dx,dy,dw,dh, dispFullW,dispFullH);
                        sanitize_crop_even_align(dx,dy,dw,dh, dispFullW,dispFullH);

                        p.resize.enabled = false;
                        p.crop.enabled  = true;
                        p.crop.fixratio = false;
                        p.crop.x = dx; p.crop.y = dy; p.crop.w = dw; p.crop.h = dh;
                    }
                }
            }
            disable_lens_all(p);
            
            try_bind_dcp_exact(c, p);
            set_icm_params(p);

            p.raw.bayersensor.border = 0;
            p.rotate.degree = 0.0;
            p.perspective.render = false;
            p.distortion.amount = 0.0;                 // 镜头畸变关闭
            p.vignetting.amount = 0.0;                 // 暗角校正通常不裁边，但一起关更保险
            p.resize.enabled = false;                  // 确保没有隐式缩放

            // 3) 去马赛克用“最不挑边”的设置
            p.raw.bayersensor.method = RAWParams::BayerSensor::getMethodString(
                RAWParams::BayerSensor::Method::AMAZE);
            p.raw.bayersensor.ccSteps = 0;
            set_tone_curve_params(p);
            // p.toneCurve.curve = { 2.0, 0.25, 0.5, 0.75, 15.0, 40.0, 20.0, -20.0 };
            set_dirpyr_denoise(p);
            const int iso = c.ii->getMetaData()->getISOSpeed();
            const double fnum = c.ii->getMetaData()->getFNumber();
            const double shutter_speed = c.ii->getMetaData()->getShutterSpeed();
            const Glib::ustring fname = c.ii->getMetaData()->getFileName();
            const int flashVal = read_flash_value(fname);
            // std::cout << "iso = " << iso << " fnum = " << fnum << " shutter_speed = " << shutter_speed << " flashVal = " << flashVal << std::endl;
            if ((approx(fnum, 4.0) && iso > 190 && flashVal == 0 && shutter_speed < 0.009 && iso != 1250) 
            || (approx(fnum, 4.0) && iso > 190 && flashVal == 0 && approx(shutter_speed, 0.0125))
            || (approx(fnum, 4.0) && iso == 250)
            || (approx(fnum, 4.0) && iso == 800)) {
                p.toneCurve.expcomp = c.ii->getMetaData()->getExpComp() + 1.0;
            }
        },
        84
    );

    _model_rules.emplace_back(
        [](const DecodeCtx& c){
             return ieq(c.model, "D850");
        },
        [](const DecodeCtx& c, rtengine::procparams::ProcParams& p){
            const auto fname = c.fname;
            // dump_exiv2_all_keys(fname, /*contains=*/nullptr, /*onlyCanon=*/false);
            // disable_lens_all(p);
            //  const auto fname = c.fname;

        // 1) 读取 Nikon CropArea
        CropBox cb = read_nikon_crop_from_exif(fname);
        if (cb.ok) {
            // 2) 取“传感器坐标”的整图尺寸（先 EXIF/Union，再必要时回退 LibRaw）
            int sensorW = 0, sensorH = 0;
            get_sensor_full_wh_robust(fname, c.ii, sensorW, sensorH);
            if (sensorW <= 0 || sensorH <= 0) {
                libraw_get_full_wh(fname, sensorW, sensorH);
            }

            if (sensorW > 0 && sensorH > 0) {
                // 3) 读取 EXIF 方向并把传感器坐标映射到显示坐标
                const int ori = read_exif_orientation(fname);
                int dx,dy,dw,dh, dispFullW,dispFullH;
                map_nikon_sensor_to_display_roi(ori, sensorW, sensorH,
                                          cb.x,cb.y,cb.w,cb.h,
                                          dx,dy,dw,dh, dispFullW,dispFullH);

                // 4) 显示坐标下裁剪框的边界钳制 + Bayer 偶数对齐
                // sanitize_crop_even_align(dx,dy,dw,dh, dispFullW,dispFullH);

                // 4) 用“真实显示画布”（LibRaw 有效区，按方向交换宽高）把 ROI 吸进去，避免后续被裁成方形
               int libW = 0, libH = 0;
               try {
                    std::shared_ptr<LibRaw> raw_ptr = std::make_shared<LibRaw>();
                    if (check_libraw_open(raw_ptr, fname) == LIBRAW_SUCCESS) {
                       const auto& s = raw_ptr->imgdata.sizes; // 未旋转有效区
                       if (ori == 6 || ori == 8 || ori == 5 || ori == 7) { libW = s.height; libH = s.width; }
                       else                                                { libW = s.width;  libH = s.height; }
                       raw_ptr->recycle();
                    }
               } catch (...) {}

               auto fit_inside = [](int& x,int& y,int& w,int& h,int W,int H){
                   // 保持中心点，尽量只缩 w/h
                    int cx = x + w/2;
                    int cy = y + h/2;

                    w = std::min(w, W);
                    h = std::min(h, H);

                    x = clampi(cx - w/2, 0, W - w);
                    y = clampi(cy - h/2, 0, H - h);
               };
               if (libW > 0 && libH > 0) fit_inside(dx,dy,dw,dh, libW, libH);
               else                      fit_inside(dx,dy,dw,dh, dispFullW, dispFullH);
               // 最后再做一次偶数对齐，避免 CFA 半像素
               sanitize_nikon_crop_even_align_neutral(dx,dy,dw,dh, (libW>0?libW:dispFullW), (libH>0?libH:dispFullH));
 

                // 5) 写入裁剪参数（注意关闭 resize 避免“再缩一次”）
                p.resize.enabled = false;
                p.crop.enabled   = true;
                p.crop.fixratio  = false;
                p.crop.x = dx - 4; p.crop.y = dy - 4; p.crop.w = dw; p.crop.h = dh;
        } 
        } 
            try_bind_dcp_exact(c, p);
            // std::printf("inputProfile = %s\n", p.icm.inputProfile.c_str());

            // p.lensProf.lcMode  = rtengine::procparams::LensProfParams::LcMode::LENSFUNAUTOMATCH;
            // p.lensProf.useDist = true; 
            // p.lensProf.useVign = false; 
            // p.lensProf.useCA   = false;
            disable_lens_all(p);
            set_icm_params(p);

            set_tone_curve_params(p);
            p.toneCurve.curve = { 2.0, 0.183649, 0.5, 0.75, 18.0, 30.0, 17.0, -45.0 };
            // if (c.ii->getMetaData()->getExpComp() > 0.0) {
            //     p.toneCurve.expcomp = c.ii->getMetaData()->getExpComp() + 1.0;
            // }
            set_dirpyr_denoise(p);
        },
        83
    );

    _model_rules.emplace_back(
        [](const DecodeCtx& c){
            return ieq(c.model, "ILCE-7RM5");
        },
        [](const DecodeCtx& c, rtengine::procparams::ProcParams& p){
                        if (is_sony_ycbcr(c.fname)) {
                const auto fname = c.fname;

                // CropBox cb = read_crop_from_exif(fname);
                CropBox cb = read_7m4_crop_from_exif(fname);
                if (!cb.ok) {
                    // std::fprintf(stderr,"[EXIF-Crop] none\n");
                } else {
                    int sensorW=0, sensorH=0;
                    infer_7m4_full_wh_from_exif_extents(fname, c.ii, sensorW, sensorH);
                    sanitize_7m4_crop_even_align(cb.x, cb.y, cb.w, cb.h, sensorW, sensorH);

                    // 3) 读 EXIF 方向
                    const int ori = read_exif_orientation(fname);

                    // 4) 拿“显示画布的真实全尺寸”（优先 LibRaw 的 sizes，再按 ori 交换）
                    int dispFullW = 0, dispFullH = 0;
                    try {
                        // LibRaw raw;
                        std::shared_ptr<LibRaw> raw_ptr = std::make_shared<LibRaw>();
                        if (check_libraw_open(raw_ptr, fname) == LIBRAW_SUCCESS) {
                            const auto& s = raw_ptr->imgdata.sizes; // 活动区尺寸（未旋转）
                            if (ori == 6 || ori == 8 || ori == 5 || ori == 7) {
                                dispFullW = s.height;
                                dispFullH = s.width; 
                            } else {
                                dispFullW = s.width;
                                dispFullH = s.height;
                            }
                            raw_ptr->recycle();
                        }
                    } catch (...) {}
                    if (dispFullW <= 0 || dispFullH <= 0) {
                        // 回退用传感器整图尺寸推估（再按 ori 交换）
                        if (ori == 6 || ori == 8 || ori == 5 || ori == 7) {
                            dispFullW = sensorH;
                            dispFullH = sensorW;
                        } else {
                            dispFullW = sensorW;
                            dispFullH = sensorH;
                        }
                    }

                    // 5) 把 sensor-space 的裁剪框映射到 display-space
                    int dx=0, dy=0, dw=0, dh=0, tmpW=0, tmpH=0;
                    map_sensor_to_display_roi(ori, sensorW, sensorH, cb.x, cb.y, cb.w, cb.h,
                                            dx,  dy,  dw,  dh,  tmpW,  tmpH);

                    // 6) 用“真实显示画布尺寸”做一次相交 + 偶数对齐，避免被后续流程再次裁掉
                    sanitize_crop_even_align(dx, dy, dw, dh, dispFullW, dispFullH);

                    // 7) 写入 p.crop（注意：这是显示坐标）
                    p.resize.enabled = false;     // 关键：避免额外缩放引发再次截断
                    p.crop.enabled   = true;
                    p.crop.fixratio  = false;
                    p.crop.x = dx; p.crop.y = dy; p.crop.w = dw; p.crop.h = dh;
                }

                disable_lens_all(p);
                p.raw.bayersensor.border = 0;
                use_fast_demosaic(p);
                p.rotate.degree = 0.0;
                p.perspective.render = false;
                p.distortion.amount = 0.0;

                try_bind_dcp_exact(c, p);
                // std::printf("inputProfile = %s\n", p.icm.inputProfile.c_str());
                p.wb.enabled = true;
                p.icm.toneCurve = true;
                p.icm.applyHueSatMap = true;
                p.icm.applyLookTable = true;
                p.icm.applyBaselineExposureOffset = true;
                p.icm.workingProfile = "ProPhoto";
                p.icm.outputProfile  = "sRGB";

                p.toneCurve.histmatching     = false;
                p.toneCurve.fromHistMatching = false;
                p.toneCurve.curveMode  = ToneCurveMode::FILMLIKE;
                p.toneCurve.curveMode2  = ToneCurveMode::FILMLIKE;
                p.toneCurve.hrenabled  = true;
                p.toneCurve.method     = "ColorPropagation";
                p.toneCurve.curve = { 2.0, 0.238327, 0.535019, 0.824903, 0.0, 100.0, 50.0, -100.0 };
                p.toneCurve.curve2 = { 2.0, 0.25, 0.488327, 0.757782, 55.0, 16.0, 38.0, -20.0 };
                set_dirpyr_denoise(p);

            } else 
            {
                const auto fname = c.fname;

                // CropBox cb = read_crop_from_exif(fname);
                CropBox cb = read_7m4_crop_from_exif(fname);
                if (!cb.ok) {
                    // std::fprintf(stderr,"[EXIF-Crop] none\n");
                } else {

                    // 1) 取“传感器坐标”的整图尺寸（EXIF外接边界 -> LibRaw -> metadata 兜底）
                        int sensorW=0, sensorH=0;
                        get_7m4_sensor_full_wh_robust(fname, c.ii, sensorW, sensorH); // 见前文提供的实现
                        if (sensorW <= 0 || sensorH <= 0) {
                            // std::fprintf(stderr,"[EXIF-Crop] full(sensor) unknown, skip\n");
                        }

                        // 2) 读取 Orientation（只读一次，保持一致）
                        const int ori = read_exif_orientation(fname);

                        // 3) 将“传感器坐标”裁剪框映射到“显示坐标”
                        int dx,dy,dw,dh, dispFullW, dispFullH;
                        map_sensor_to_display_roi(ori, sensorW, sensorH,
                                                cb.x,cb.y,cb.w,cb.h,
                                                dx,dy,dw,dh, dispFullW,dispFullH); // 见前文提供的实现

                        // 4) 只在“显示坐标”做边界钳制（不要再在传感器坐标里做偶数对齐了）
                        auto clip_display = [](int& x,int& y,int& w,int& h,int W,int H) {
                            if (x < 0) x = 0; 
                            if (y < 0) y = 0;
                            if (w < 1) w = 1; 
                            if (h < 1) h = 1;
                            if (x + w > W) w = W - x;
                            if (y + h > H) h = H - y;
                        };
                        clip_display(dx,dy,dw,dh, dispFullW,dispFullH);

                        int libW = 0, libH = 0;
                        try {
                            // LibRaw raw;
                            std::shared_ptr<LibRaw> raw_ptr = std::make_shared<LibRaw>();
                            if (check_libraw_open(raw_ptr, fname) == LIBRAW_SUCCESS) {
                                const auto& s = raw_ptr->imgdata.sizes;
                                // s.width/s.height 是“有效区”，未旋转
                                if (ori == 6 || ori == 8 || ori == 5 || ori == 7) {
                                    libW = s.height;
                                    libH = s.width;
                                } else {
                                    libW = s.width;
                                    libH = s.height;
                                }
                                raw_ptr->recycle();
                            }
                        } catch(...) {}

                        // B) 用“真实画布”吸收你的 ROI（先平移，后再缩；不要做偶数对齐）
                        auto fit_inside = [](int& x,int& y,int& w,int& h,int W,int H){
                            // 右边越界 -> 先向左平移，不够再减宽
                            int overR = (x + w) - W;
                            if (overR > 0) {
                                int shift = std::min(x, overR);
                                x -= shift;
                                overR -= shift;
                                if (overR > 0) w -= overR;
                            }
                            // 下边越界 -> 先向上平移，不够再减高
                            int overB = (y + h) - H;
                            if (overB > 0) {
                                int shift = std::min(y, overB);
                                y -= shift;
                                overB -= shift;
                                if (overB > 0) h -= overB;
                            }
                            // 左/上越界的兜底（极少见）
                            if (x < 0) { w += x; x = 0; }
                            if (y < 0) { h += y; y = 0; }
                            if (x + w > W) w = W - x;
                            if (y + h > H) h = H - y;
                            if (w < 1) w = 1;
                            if (h < 1) h = 1;
                        };

                        if (libW > 0 && libH > 0) {
                            fit_inside(dx,dy,dw,dh, libW,libH);
                        } else {
                            // 拿不到 LibRaw，就退回你已有的 dispFullW/dispFullH
                            fit_inside(dx,dy,dw,dh, dispFullW,dispFullH);
                        }

                        // 然后再写入 p.crop（保持你原来的写法）
                        p.resize.enabled = false;
                        p.crop.enabled   = true;
                        p.crop.fixratio  = false;
                        // 根据方向判断是否是竖图，并在竖图上加偏移
                        // if (ori == 6 || ori == 8) { // 竖图
                        //     p.crop.y = dy;  // 竖图加上偏移
                        // } else { // 横图或其他方向
                        //     p.crop.y = dy;  // 无偏移
                        // }
                        p.crop.x = dx;
                        p.crop.y = dy;

                        if (ori == 8)
                        {
                             p.crop.y = dy + 30;
                        } else if (ori == 6) {
                             p.crop.x = dx + 20;
                        }

                        p.crop.w = dw; 
                        p.crop.h = dh;
                    }
                disable_lens_all(p);
                p.raw.bayersensor.border = 0;
                use_fast_demosaic(p);
                p.rotate.degree = 0.0;
                p.perspective.render = false;
                p.distortion.amount = 0.0;

                try_bind_dcp_exact(c, p);
                // std::printf("inputProfile = %s\n", p.icm.inputProfile.c_str());

                p.icm.toneCurve = true;
                p.icm.applyHueSatMap = true;
                p.icm.applyLookTable = true;
                p.icm.applyBaselineExposureOffset = true;
                p.icm.workingProfile = "ProPhoto";
                p.icm.outputProfile  = "sRGB";

                p.toneCurve.histmatching     = false;
                p.toneCurve.fromHistMatching = false;
                p.toneCurve.curveMode  = ToneCurveMode::FILMLIKE;
                p.toneCurve.hrenabled  = true;
                p.toneCurve.method     = "ColorPropagation";
                p.toneCurve.curve = { 2.0, 0.25, 0.5, 0.75, 50.0, 20.0, 20.0, -35.0 };

                set_dirpyr_denoise(p);
            }
        },
        82
    );

    _model_rules.emplace_back(
        [](const DecodeCtx& c){
             return ieq(c.model, "Z 6");
        },
        [](const DecodeCtx& c, rtengine::procparams::ProcParams& p){
            const auto fname = c.fname;
            // dump_exiv2_all_keys(fname, /*contains=*/nullptr, /*onlyCanon=*/false);
            // disable_lens_all(p);
            //  const auto fname = c.fname;

        // 1) 读取 Nikon CropArea
        CropBox cb = read_nikon_crop_from_exif(fname);
        if (cb.ok) {
            // 2) 取“传感器坐标”的整图尺寸（先 EXIF/Union，再必要时回退 LibRaw）
            int sensorW = 0, sensorH = 0;
            get_sensor_full_wh_robust(fname, c.ii, sensorW, sensorH);
            if (sensorW <= 0 || sensorH <= 0) {
                libraw_get_full_wh(fname, sensorW, sensorH);
            }

            if (sensorW > 0 && sensorH > 0) {
                // 3) 读取 EXIF 方向并把传感器坐标映射到显示坐标
                const int ori = read_exif_orientation(fname);
                int dx,dy,dw,dh, dispFullW,dispFullH;
                map_nikon_sensor_to_display_roi(ori, sensorW, sensorH,
                                          cb.x,cb.y,cb.w,cb.h,
                                          dx,dy,dw,dh, dispFullW,dispFullH);

                // 4) 显示坐标下裁剪框的边界钳制 + Bayer 偶数对齐
                // sanitize_crop_even_align(dx,dy,dw,dh, dispFullW,dispFullH);

                // 4) 用“真实显示画布”（LibRaw 有效区，按方向交换宽高）把 ROI 吸进去，避免后续被裁成方形
               int libW = 0, libH = 0;
               try {
                //    LibRaw raw;
                    std::shared_ptr<LibRaw> raw_ptr = std::make_shared<LibRaw>();
                   if (check_libraw_open(raw_ptr, fname) == LIBRAW_SUCCESS) {
                       const auto& s = raw_ptr->imgdata.sizes; // 未旋转有效区
                       if (ori == 6 || ori == 8 || ori == 5 || ori == 7) { libW = s.height; libH = s.width; }
                       else                                                { libW = s.width;  libH = s.height; }
                       raw_ptr->recycle();
                   }
               } catch (...) {}

               auto fit_inside = [](int& x,int& y,int& w,int& h,int W,int H){
                   // 保持中心点，尽量只缩 w/h
                    int cx = x + w/2;
                    int cy = y + h/2;

                    w = std::min(w, W);
                    h = std::min(h, H);

                    x = clampi(cx - w/2, 0, W - w);
                    y = clampi(cy - h/2, 0, H - h);
               };
               if (libW > 0 && libH > 0) fit_inside(dx,dy,dw,dh, libW, libH);
               else                      fit_inside(dx,dy,dw,dh, dispFullW, dispFullH);
               // 最后再做一次偶数对齐，避免 CFA 半像素
               sanitize_nikon_crop_even_align_neutral(dx,dy,dw,dh, (libW>0?libW:dispFullW), (libH>0?libH:dispFullH));
 

                // 5) 写入裁剪参数（注意关闭 resize 避免“再缩一次”）
                p.resize.enabled = false;
                p.crop.enabled   = true;
                p.crop.fixratio  = false;
                p.crop.x = dx - 4; p.crop.y = dy - 4; p.crop.w = dw; p.crop.h = dh;
            }
        } 
            try_bind_dcp_exact(c, p);
            std::printf("inputProfile = %s\n", p.icm.inputProfile.c_str());

            p.lensProf.lcMode  = rtengine::procparams::LensProfParams::LcMode::LENSFUNAUTOMATCH;
            p.lensProf.useDist = true; 
            p.lensProf.useVign = true; 
            p.lensProf.useCA   = true;
            // disable_lens_all(p);
            set_icm_params(p);
            use_fast_demosaic(p);
            set_tone_curve_params(p);
            p.toneCurve.curve = { 2.0, 0.25, 0.5, 0.75, 15.0, 30.0, 15.0, -30.0 };
            // if (c.ii->getMetaData()->getExpComp() > 0.0) {
            //     p.toneCurve.expcomp = c.ii->getMetaData()->getExpComp() + 1.0;
            // }
            set_dirpyr_denoise(p);
        },
        81
    );

    _model_rules.emplace_back(
        [](const DecodeCtx& c){
             return ieq(c.model, "Z 7");
        },
        [](const DecodeCtx& c, rtengine::procparams::ProcParams& p){
            const auto fname = c.fname;
            // dump_exiv2_all_keys(fname, /*contains=*/nullptr, /*onlyCanon=*/false);
            // disable_lens_all(p);
            //  const auto fname = c.fname;

        // 1) 读取 Nikon CropArea
        CropBox cb = read_nikon_crop_from_exif(fname);
        if (cb.ok) {
            // 2) 取“传感器坐标”的整图尺寸（先 EXIF/Union，再必要时回退 LibRaw）
            int sensorW = 0, sensorH = 0;
            get_sensor_full_wh_robust(fname, c.ii, sensorW, sensorH);
            if (sensorW <= 0 || sensorH <= 0) {
                libraw_get_full_wh(fname, sensorW, sensorH);
            }

            if (sensorW > 0 && sensorH > 0) {
                // 3) 读取 EXIF 方向并把传感器坐标映射到显示坐标
                const int ori = read_exif_orientation(fname);
                int dx,dy,dw,dh, dispFullW,dispFullH;
                map_nikon_sensor_to_display_roi(ori, sensorW, sensorH,
                                          cb.x,cb.y,cb.w,cb.h,
                                          dx,dy,dw,dh, dispFullW,dispFullH);

                // 4) 显示坐标下裁剪框的边界钳制 + Bayer 偶数对齐
                // sanitize_crop_even_align(dx,dy,dw,dh, dispFullW,dispFullH);

                // 4) 用“真实显示画布”（LibRaw 有效区，按方向交换宽高）把 ROI 吸进去，避免后续被裁成方形
               int libW = 0, libH = 0;
               try {
                //    LibRaw raw;
                    std::shared_ptr<LibRaw> raw_ptr = std::make_shared<LibRaw>();
                   if (check_libraw_open(raw_ptr, fname) == LIBRAW_SUCCESS) {
                       const auto& s = raw_ptr->imgdata.sizes; // 未旋转有效区
                       if (ori == 6 || ori == 8 || ori == 5 || ori == 7) { libW = s.height; libH = s.width; }
                       else                                                { libW = s.width;  libH = s.height; }
                       raw_ptr->recycle();
                   }
               } catch (...) {}

               auto fit_inside = [](int& x,int& y,int& w,int& h,int W,int H){
                   // 保持中心点，尽量只缩 w/h
                    int cx = x + w/2;
                    int cy = y + h/2;

                    w = std::min(w, W);
                    h = std::min(h, H);

                    x = clampi(cx - w/2, 0, W - w);
                    y = clampi(cy - h/2, 0, H - h);
               };
               if (libW > 0 && libH > 0) fit_inside(dx,dy,dw,dh, libW, libH);
               else                      fit_inside(dx,dy,dw,dh, dispFullW, dispFullH);
               // 最后再做一次偶数对齐，避免 CFA 半像素
               sanitize_nikon_crop_even_align_neutral(dx,dy,dw,dh, (libW>0?libW:dispFullW), (libH>0?libH:dispFullH));
 

                // 5) 写入裁剪参数（注意关闭 resize 避免“再缩一次”）
                p.resize.enabled = false;
                p.crop.enabled   = true;
                p.crop.fixratio  = false;
                p.crop.x = dx - 4; p.crop.y = dy - 4; p.crop.w = dw; p.crop.h = dh;
            }
        } 
            try_bind_dcp_exact(c, p);
            std::printf("inputProfile = %s\n", p.icm.inputProfile.c_str());

            p.lensProf.lcMode  = rtengine::procparams::LensProfParams::LcMode::LENSFUNAUTOMATCH;
            p.lensProf.useDist = true; 
            p.lensProf.useVign = false; 
            p.lensProf.useCA   = false;
            // disable_lens_all(p);
            set_icm_params(p);
            use_fast_demosaic(p);
            set_tone_curve_params(p);
            p.toneCurve.curve = { 2.0, 0.25, 0.5, 0.75, 25.0, 25.0, 10.0, -25.0 };
            // if (c.ii->getMetaData()->getExpComp() > 0.0) {
            //     p.toneCurve.expcomp = c.ii->getMetaData()->getExpComp() + 1.0;
            // }
            set_dirpyr_denoise(p);
        },
        80
    );

    _model_rules.emplace_back(
        [](const DecodeCtx& c){
             return ieq(c.model, "Z 7 2");
        },
        [](const DecodeCtx& c, rtengine::procparams::ProcParams& p){
            const auto fname = c.fname;
            // dump_exiv2_all_keys(fname, /*contains=*/nullptr, /*onlyCanon=*/false);
            // disable_lens_all(p);
            //  const auto fname = c.fname;

        // 1) 读取 Nikon CropArea
        CropBox cb = read_nikon_crop_from_exif(fname);
        if (cb.ok) {
            // 2) 取“传感器坐标”的整图尺寸（先 EXIF/Union，再必要时回退 LibRaw）
            int sensorW = 0, sensorH = 0;
            get_sensor_full_wh_robust(fname, c.ii, sensorW, sensorH);
            if (sensorW <= 0 || sensorH <= 0) {
                libraw_get_full_wh(fname, sensorW, sensorH);
            }

            if (sensorW > 0 && sensorH > 0) {
                // 3) 读取 EXIF 方向并把传感器坐标映射到显示坐标
                const int ori = read_exif_orientation(fname);
                int dx,dy,dw,dh, dispFullW,dispFullH;
                map_nikon_sensor_to_display_roi(ori, sensorW, sensorH,
                                          cb.x,cb.y,cb.w,cb.h,
                                          dx,dy,dw,dh, dispFullW,dispFullH);

                // 4) 显示坐标下裁剪框的边界钳制 + Bayer 偶数对齐
                // sanitize_crop_even_align(dx,dy,dw,dh, dispFullW,dispFullH);

                // 4) 用“真实显示画布”（LibRaw 有效区，按方向交换宽高）把 ROI 吸进去，避免后续被裁成方形
               int libW = 0, libH = 0;
               try {
                //    LibRaw raw;
                    std::shared_ptr<LibRaw> raw_ptr = std::make_shared<LibRaw>();
                   if (check_libraw_open(raw_ptr, fname) == LIBRAW_SUCCESS) {
                       const auto& s = raw_ptr->imgdata.sizes; // 未旋转有效区
                       if (ori == 6 || ori == 8 || ori == 5 || ori == 7) { libW = s.height; libH = s.width; }
                       else                                                { libW = s.width;  libH = s.height; }
                       raw_ptr->recycle();
                   }
               } catch (...) {}

               auto fit_inside = [](int& x,int& y,int& w,int& h,int W,int H){
                   // 保持中心点，尽量只缩 w/h
                    int cx = x + w/2;
                    int cy = y + h/2;

                    w = std::min(w, W);
                    h = std::min(h, H);

                    x = clampi(cx - w/2, 0, W - w);
                    y = clampi(cy - h/2, 0, H - h);
               };
               if (libW > 0 && libH > 0) fit_inside(dx,dy,dw,dh, libW, libH);
               else                      fit_inside(dx,dy,dw,dh, dispFullW, dispFullH);
               // 最后再做一次偶数对齐，避免 CFA 半像素
               sanitize_nikon_crop_even_align_neutral(dx,dy,dw,dh, (libW>0?libW:dispFullW), (libH>0?libH:dispFullH));
 

                // 5) 写入裁剪参数（注意关闭 resize 避免“再缩一次”）
                p.resize.enabled = false;
                p.crop.enabled   = true;
                p.crop.fixratio  = false;
                p.crop.x = dx - 4; p.crop.y = dy - 4; p.crop.w = dw; p.crop.h = dh;
            }
        } 
            try_bind_dcp_exact(c, p);
            std::printf(" z 7 2  = inputProfile = %s\n", p.icm.inputProfile.c_str());

            p.lensProf.lcMode  = rtengine::procparams::LensProfParams::LcMode::LENSFUNAUTOMATCH;
            p.lensProf.useDist = true; 
            p.lensProf.useVign = true; 
            p.lensProf.useCA   = true;
            // disable_lens_all(p);
            set_icm_params(p);
            use_fast_demosaic(p);
            set_tone_curve_params(p);
            p.toneCurve.curve = { 2.0, 0.25, 0.5, 0.75, 0.0, 90.0, 70.0, -70.0 };
            // if (c.ii->getMetaData()->getExpComp() > 0.0) {
            //     p.toneCurve.expcomp = c.ii->getMetaData()->getExpComp() + 1.0;
            // }
            set_dirpyr_denoise(p);
        },
        79
    );

}

void apply_default_rule(const DecodeCtx& ctx,
                               rtengine::procparams::ProcParams& p)
{
    // TODO: 根据你需要的默认参数写这里
    // 比如：p.exposure = 默认值；p.whiteBalance = 默认值；之类
    std::printf("in apply_default_rule \n");
    disable_lens_all(p);
    p.icm.inputProfile = "(camera)";
    set_icm_params(p);
    p.raw.bayersensor.method = RAWParams::BayerSensor::getMethodString(
                RAWParams::BayerSensor::Method::AMAZE);
    p.raw.bayersensor.ccSteps = 0;
    set_tone_curve_params(p);
    p.toneCurve.curve = { 2.0, 0.25, 0.5, 0.75, 0.0, 35.0, 10.0, -30.0 };
    p.toneCurve.clampOOG = true;
    set_dirpyr_denoise(p);

}

void apply_rules(const DecodeCtx& ctx, rtengine::procparams::ProcParams& p) {
    // std::vector<const ModelRule*> ordered;
    // ordered.reserve(rules.size());
    // for (auto& r : rules) ordered.push_back(&r);
    // std::sort(rules.begin(), rules.end(),
    //           [](const ModelRule& a, const ModelRule& b){ return a.priority < b.priority; });
    bool matched = false;
    for (const auto &r : _model_rules) {
        if (r.match(ctx)) {
            r.apply(ctx, p);
            matched = true;
            break;
        }
    }

    // 没有任何规则命中，走兜底
    if (!matched) {
        apply_default_rule(ctx, p);
    }
}

// ======================= 初始化/解码/释放 =======================

#ifdef __cplusplus
extern "C"
#endif
int rawengine_init() {
    setlocale(LC_ALL, "");
    setlocale(LC_NUMERIC, "C");

    Gio::init();

    // In RawTherapee, options and paths are managed by the App singleton.
    // We set the lensfun paths and then call Options::load() which uses App::get().
    Options& options = App::get().mut_options();

#ifdef BUILD_BUNDLE
    char exname[512] = {0};
    Glib::ustring exePath;
#ifdef _WIN32
    WCHAR exnameU[512] = {0};
    GetModuleFileNameW(NULL, exnameU, 511);
    WideCharToMultiByte(CP_UTF8, 0, exnameU, -1, exname, 511, 0, 0);
    exePath = Glib::path_get_dirname(exname);

#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
        
    std::vector<char> buffer_path(size);
    if (_NSGetExecutablePath(buffer_path.data(), &size) == 0) {
        std::string path = std::string(buffer_path.data());
#if defined(OSX_DEV_BUILD)
        size_t buildPos = path.find_last_of('/');
        if (buildPos != std::string::npos) {
            exePath = path.substr(0, buildPos + 1);
        }
#else
        std::size_t pos = path.find("/Contents/");
        if (pos != std::string::npos) {
            exePath = path.substr(0, pos + 9);
        }
#endif
        std::cout << "Current working directory exePath : " << exePath.data() << std::endl; 
    }
#else
    if (readlink("/proc/self/exe", exname, 511) < 0) {
        strncpy(exname, argv0.c_str(), 511);
    }
#endif

    if (Glib::path_is_absolute(DATA_SEARCH_PATH)) argv0 = DATA_SEARCH_PATH;
    else argv0 = Glib::build_filename(exePath, DATA_SEARCH_PATH);

    if (Glib::path_is_absolute(CREDITS_SEARCH_PATH)) creditsPath = CREDITS_SEARCH_PATH;
    else creditsPath = Glib::build_filename(exePath, CREDITS_SEARCH_PATH);

    if (Glib::path_is_absolute(EXTERNAL_PATH)) externalPath = EXTERNAL_PATH;
    else externalPath = Glib::build_filename(exePath, EXTERNAL_PATH);

    if (Glib::path_is_absolute(LICENCE_SEARCH_PATH)) licensePath = LICENCE_SEARCH_PATH;
    else licensePath = Glib::build_filename(exePath, LICENCE_SEARCH_PATH);

    options.rtSettings.lensfunDbDirectory = LENSFUN_DB_PATH;
    options.rtSettings.lensfunDbBundleDirectory = LENSFUN_DB_PATH;
    // Sync paths into App
    App::get().setArgv0(argv0);
    App::get().setCreditsPath(creditsPath);
    App::get().setLicensePath(licensePath);
#else
    argv0 = DATA_SEARCH_PATH;
    creditsPath = CREDITS_SEARCH_PATH;
    licensePath = LICENCE_SEARCH_PATH;
    options.rtSettings.lensfunDbDirectory = LENSFUN_DB_PATH;
    options.rtSettings.lensfunDbBundleDirectory = LENSFUN_DB_PATH;
    App::get().setArgv0(argv0);
    App::get().setCreditsPath(creditsPath);
    App::get().setLicensePath(licensePath);
#endif

    try {
        Options::load(false);
    } catch (Options::Error& e) {
        std::cerr << "\nFATAL ERROR:\n" << e.get_msg() << std::endl;
        return -2;
    }

    if (options.is_defProfRawMissing()) {
        options.defProfRaw = DEFPROFILE_RAW;
        std::cerr << "\nThe default profile for raw photos could not be found or is not set.\n"
                     "Please check your profiles' directory, it may be missing or damaged.\n"
                     "\"" << DEFPROFILE_RAW << "\" will be used instead.\n\n";
    }
    if (options.is_bundledDefProfRawMissing()) {
        std::cerr << "\nThe bundled profile \"" << options.defProfRaw << "\" could not be found!\n"
                     "Your installation could be damaged.\n"
                     "Default internal values will be used instead.\n\n";
        options.defProfRaw = DEFPROFILE_INTERNAL;
    }

    if (options.is_defProfImgMissing()) {
        options.defProfImg = DEFPROFILE_IMG;
        std::cerr << "\nThe default profile for non-raw photos could not be found or is not set.\n"
                     "Please check your profiles' directory, it may be missing or damaged.\n"
                     "\"" << DEFPROFILE_IMG << "\" will be used instead.\n\n";
    }
    if (options.is_bundledDefProfImgMissing()) {
        std::cerr << "\nThe bundled profile " << options.defProfImg << " could not be found!\n"
                     "Your installation could be damaged.\n"
                     "Default internal values will be used instead.\n\n";
        options.defProfImg = DEFPROFILE_INTERNAL;
    }

    TIFFSetWarningHandler(nullptr);

    // 扫描外部 DCP
    external_dcp_map = find_dcp_files(externalPath.raw());

#ifndef _WIN32
    if (Glib::file_test(Glib::build_filename(options.rtdir, "cache"), Glib::FILE_TEST_IS_DIR) &&
        !Glib::file_test(options.cacheBaseDir, Glib::FILE_TEST_IS_DIR))
    {
        if (g_rename(Glib::build_filename(options.rtdir, "cache").c_str(), options.cacheBaseDir.c_str()) == -1) {
            std::cout << "g_rename " << Glib::build_filename(options.rtdir, "cache").c_str()
                      << " => " << options.cacheBaseDir.c_str() << " failed.\n";
        }
    }
#endif

    build_rules();
    return 0;
}

#ifdef __cplusplus
extern "C"
#endif
int RAWENGINE_API rawengine_decode(const char* filename, void** buffer, int* length, int* width, int* height, int preview_type, const RawEngineLensParams* lens_params)
{
    unsigned errors = 0;
    fast_export = true;
    App::get().mut_options().saveUsePathTemplate = false;

    Glib::ustring inputFile(fname_to_utf8(filename));

    rtengine::InitialImage* ii = nullptr;
    rtengine::ProcessingJob* job = nullptr;
    int errorCode = 0;
    bool isRaw = true;

    Glib::ustring ext = getExtension(inputFile);
    if (ext.lowercase()=="jpg" || ext.lowercase()=="jpeg" || ext.lowercase()=="tif"
     || ext.lowercase()=="tiff" || ext.lowercase()=="png") {
        isRaw = false;
    }

    ii = rtengine::InitialImage::load(inputFile, isRaw, 0, &errorCode, nullptr);
    if (errorCode) return RawEngineErrorLoadFail;
    if (!ii) { errors++; std::cerr << "Error loading file: " << inputFile << std::endl; }

    rtengine::procparams::ProcParams currentParams;

    // 组装上下文
    DecodeCtx ctx;
    ctx.ii = ii;
    ctx.fname = ii->getMetaData()->getFileName();
    ctx.isRaw = isRaw;
    ctx.previewType = preview_type;
    ctx.extLower = ext.lowercase();
    ctx.make  = ii->getMetaData()->getMake();
    ctx.model = ii->getMetaData()->getModel();
    std::replace(ctx.model.begin(), ctx.model.end(), '_', ' '); // 兼容 "nikon z 7_2"
    ctx.externalDcp = &external_dcp_map;

    apply_rules(ctx, currentParams);

    // 用户传入镜头参数时，覆盖规则系统设置的镜头校正
    apply_lens_override(lens_params, currentParams);

    // 预览 resize（后续规则可覆盖）
    if (preview_type != RAWENGINE_PREVIEW_TYPE_ORIGIN) {
        currentParams.resize.enabled = true;
        currentParams.resize.allowUpscaling = false;
        currentParams.resize.scale = 1.0;
        if (preview_type == RAWENGINE_PREVIEW_TYPE_3K) {
            currentParams.resize.height = 3000; 
            currentParams.resize.width = 3000;
        } else if (preview_type == RAWENGINE_PREVIEW_TYPE_2K) {
            currentParams.resize.height = 2000; 
            currentParams.resize.width = 2000;
        }
    }

    job = rtengine::ProcessingJob::create(ii, currentParams, fast_export);
    if (!job) {
        errors++; 
        std::cerr << "Error creating processing for: " << inputFile << std::endl;
        ii->decreaseRef();
        return errors > 0 ? -2 : 0;
    }

    rtengine::IImagefloat* resultImage = rtengine::processImage(job, errorCode, nullptr);
    if (!resultImage) {
        errors++; 
        std::cerr << "Error processing: " << inputFile << std::endl;
        rtengine::ProcessingJob::destroy(job);
        ii->decreaseRef();
        return errors > 0 ? -2 : 0;
    }

    // RawTherapee's IImagefloat does not have getRGBA(); implement inline using getScanline().
    errorCode = 0;
    {
        const int img_w = resultImage->getWidth();
        const int img_h = resultImage->getHeight();
        if (img_w < 1 || img_h < 1) {
            errorCode = -1;
        } else {
            const int rowLen = img_w * 3; // 8 bps RGB
            std::vector<unsigned char> rgbaVec(img_w * img_h * 4);
            std::vector<unsigned char> rowBuf(rowLen);
            try {
                unsigned char* dst = rgbaVec.data();
                for (int row = 0; row < img_h; ++row) {
                    resultImage->getScanline(row, rowBuf.data(), 8);
                    for (int j = 0; j < rowLen; j += 3) {
                        dst[0] = rowBuf[j];
                        dst[1] = rowBuf[j + 1];
                        dst[2] = rowBuf[j + 2];
                        dst[3] = 255;
                        dst += 4;
                    }
                }
                *buffer = std::malloc(rgbaVec.size());
                std::memcpy(*buffer, rgbaVec.data(), rgbaVec.size());
                if (length) *length = static_cast<int>(rgbaVec.size());
                *width  = img_w;
                *height = img_h;
            } catch (const std::exception& ex) {
                std::cerr << "GET RGBA ERROR: " << ex.what() << std::endl;
                errorCode = -1;
            }
        }
    }

    // 结果兜底：与原逻辑一致
    if (!errorCode && !currentParams.crop.enabled) {
        const auto fname = ii->getMetaData()->getFileName();
        CropBox cb = read_crop_from_exif(fname);
        if (cb.ok) {
            int fullW=0, fullH=0;
            get_sensor_full_wh_robust(fname, ii, fullW, fullH);
            int ori = read_exif_orientation(fname);
            final_fallback_fractional_crop(*width, *height,
                                           fullW, fullH,
                                           cb.x, cb.y, cb.w, cb.h,
                                           ori,
                                           buffer, length, width, height,
                                           /*max_delta_px=*/96);
    
        }
    }
    if (errorCode) { 
        errors++; 
        std::cerr << "Image To RGBA ERROR: " << errorCode << std::endl; 
    }

    ii->decreaseRef();
    delete resultImage;

    return errors > 0 ? -2 : 0;
}

#ifdef __cplusplus
extern "C"
#endif
int RAWENGINE_API rawengine_free(void* buffer) {
    std::free(buffer);
    return 0;
}

#ifdef __cplusplus
extern "C"
#endif
void RAWENGINE_API rawengine_lens_params_default(RawEngineLensParams* params) {
    if (!params) return;
    std::memset(params, 0, sizeof(RawEngineLensParams));
    params->lens_mode = RAWENGINE_LENS_MODE_AUTO;
    params->use_distortion = 1;
}

#ifdef __cplusplus
extern "C"
#endif
int RAWENGINE_API rawengine_get_cameras(RawEngineCameraInfo** cameras, int* count) {
    if (!cameras || !count) return -1;
    *cameras = nullptr;
    *count = 0;

    const rtengine::LFDatabase* db = rtengine::LFDatabase::getInstance();
    if (!db) return -1;

    std::vector<rtengine::LFCamera> cams = db->getCameras();
    int n = (int)cams.size();
    if (n == 0) return 0;

    RawEngineCameraInfo* arr = (RawEngineCameraInfo*)std::malloc(sizeof(RawEngineCameraInfo) * n);
    if (!arr) return -1;

    for (int i = 0; i < n; ++i) {
        std::string make = cams[i].getMake().raw();
        std::string model = cams[i].getModel().raw();
        std::string display = cams[i].getDisplayString().raw();
#ifdef _WIN32
        arr[i].make = _strdup(make.c_str());
        arr[i].model = _strdup(model.c_str());
        arr[i].display_string = _strdup(display.c_str());
#else
        arr[i].make = strdup(make.c_str());
        arr[i].model = strdup(model.c_str());
        arr[i].display_string = strdup(display.c_str());
#endif
    }

    *cameras = arr;
    *count = n;
    return 0;
}

#ifdef __cplusplus
extern "C"
#endif
int RAWENGINE_API rawengine_free_cameras(RawEngineCameraInfo* cameras, int count) {
    if (!cameras) return 0;
    for (int i = 0; i < count; ++i) {
        std::free((void*)cameras[i].make);
        std::free((void*)cameras[i].model);
        std::free((void*)cameras[i].display_string);
    }
    std::free(cameras);
    return 0;
}

#ifdef __cplusplus
extern "C"
#endif
int RAWENGINE_API rawengine_get_lenses(RawEngineLensInfo** lenses, int* count) {
    if (!lenses || !count) return -1;
    *lenses = nullptr;
    *count = 0;

    const rtengine::LFDatabase* db = rtengine::LFDatabase::getInstance();
    if (!db) return -1;

    std::vector<rtengine::LFLens> lens_list = db->getLenses();
    int n = (int)lens_list.size();
    if (n == 0) return 0;

    RawEngineLensInfo* arr = (RawEngineLensInfo*)std::malloc(sizeof(RawEngineLensInfo) * n);
    if (!arr) return -1;

    for (int i = 0; i < n; ++i) {
        std::string name = lens_list[i].getLens().raw();
        std::string make = lens_list[i].getMake().raw();
#ifdef _WIN32
        arr[i].lens_name = _strdup(name.c_str());
        arr[i].make = _strdup(make.c_str());
#else
        arr[i].lens_name = strdup(name.c_str());
        arr[i].make = strdup(make.c_str());
#endif
    }

    *lenses = arr;
    *count = n;
    return 0;
}

#ifdef __cplusplus
extern "C"
#endif
int RAWENGINE_API rawengine_free_lenses(RawEngineLensInfo* lenses, int count) {
    if (!lenses) return 0;
    for (int i = 0; i < count; ++i) {
        std::free((void*)lenses[i].lens_name);
        std::free((void*)lenses[i].make);
    }
    std::free(lenses);
    return 0;
}

#ifdef __cplusplus
extern "C"
#endif
int RAWENGINE_API rawengine_detect_lens(const char* filename, int* camera_index, int* lens_index) {
    if (!camera_index || !lens_index) return -1;
    *camera_index = -1;
    *lens_index   = -1;

    const rtengine::LFDatabase* db = rtengine::LFDatabase::getInstance();
    if (!db) return -1;

    // 1. 从 EXIF 读取 make / model / lens / focalLen
    Glib::ustring fname(fname_to_utf8(filename));
    std::string exifMake, exifModel, exifLens;

    try {
        auto img = read_exif_image(fname);
        if (!img.get()) return -1;
        const Exiv2::ExifData& exif = img->exifData();

        auto itMake  = exif.findKey(Exiv2::ExifKey("Exif.Image.Make"));
        auto itModel = exif.findKey(Exiv2::ExifKey("Exif.Image.Model"));
        if (itMake != exif.end())  exifMake  = itMake->toString();
        if (itModel != exif.end()) exifModel = itModel->toString();

        // 镜头名：优先 LensModel，再试 LensInfo 等常见 tag
        auto itLens = exif.findKey(Exiv2::ExifKey("Exif.Photo.LensModel"));
        if (itLens != exif.end()) {
            exifLens = itLens->toString();
        } else {
            // 兜底：遍历找 lensmodel / lenstype 关键字
            for (const auto& md : exif) {
                std::string k = lower_copy(md.key());
                if (k.find("lensmodel") != std::string::npos ||
                    k.find("lenstype") != std::string::npos) {
                    std::string v = md.toString();
                    if (!v.empty() && v != "0" && v != "Unknown") {
                        exifLens = v;
                        break;
                    }
                }
            }
        }
    } catch (...) {
        return -1;
    }

    // trim 空白
    auto trim = [](std::string& s) {
        while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back();
        size_t p = 0;
        while (p < s.size() && std::isspace((unsigned char)s[p])) ++p;
        if (p > 0) s.erase(0, p);
    };
    trim(exifMake); trim(exifModel); trim(exifLens);

    if (exifMake.empty() || exifModel.empty()) return 0; // EXIF 不足，非错误

    // 2. lensfun 自动匹配
    rtengine::LFCamera matchedCam = db->findCamera(exifMake, exifModel, true);
    if (!matchedCam) return 0;

    std::string matchedCamMake  = matchedCam.getMake().raw();
    std::string matchedCamModel = matchedCam.getModel().raw();

    // 3. 在完整列表中找相机索引
    std::vector<rtengine::LFCamera> allCams = db->getCameras();
    for (int i = 0; i < (int)allCams.size(); ++i) {
        if (allCams[i].getMake().raw()  == matchedCamMake &&
            allCams[i].getModel().raw() == matchedCamModel) {
            *camera_index = i;
            break;
        }
    }

    // 4. 镜头匹配
    if (!exifLens.empty()) {
        rtengine::LFLens matchedLens = db->findLens(matchedCam, exifLens, true);
        if (matchedLens) {
            std::string matchedLensName = matchedLens.getLens().raw();
            std::vector<rtengine::LFLens> allLenses = db->getLenses();
            for (int i = 0; i < (int)allLenses.size(); ++i) {
                if (allLenses[i].getLens().raw() == matchedLensName) {
                    *lens_index = i;
                    break;
                }
            }
        }
    }

    return 0;
}

void deleteProcParams(std::vector<rtengine::procparams::PartialProfile*>& pparams) {
    for (unsigned int i = 0; i < pparams.size(); i++) {
        pparams[i]->deleteInstance();
        delete pparams[i];
        pparams[i] = NULL;
    }
    return;
}

bool dontLoadCache(int argc, char** argv) {
    for (int iArg = 1; iArg < argc; iArg++) {
        Glib::ustring currParam(argv[iArg]);
#if ECLIPSE_ARGS
        currParam = currParam.substr(1, currParam.length() - 2);
#endif
        if (currParam.length() > 1 && currParam.at(0) == '-' && currParam.at(1) == 'q') {
            return true;
        }
    }
    return false;
}
