/*
 *  This file is part of RawTherapee.
 *
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

#include <glib.h>
#include <glibmm/convert.h>
#include <glibmm/miscutils.h>

#include "pathutils.h"


Glib::ustring removeExtension (const Glib::ustring& filename)
{

    Glib::ustring bname = Glib::path_get_basename(filename);
    size_t lastdot = bname.find_last_of ('.');
    size_t lastwhitespace = bname.find_last_of (" \t\f\v\n\r");

    if (lastdot != bname.npos && (lastwhitespace == bname.npos || lastdot > lastwhitespace)) {
        return filename.substr (0, filename.size() - (bname.size() - lastdot));
    } else {
        return filename;
    }
}

Glib::ustring getExtension (const Glib::ustring& filename)
{

    Glib::ustring bname = Glib::path_get_basename(filename);
    size_t lastdot = bname.find_last_of ('.');
    size_t lastwhitespace = bname.find_last_of (" \t\f\v\n\r");

    if (lastdot != bname.npos && (lastwhitespace == bname.npos || lastdot > lastwhitespace)) {
        return filename.substr (filename.size() - (bname.size() - lastdot) + 1, filename.npos);
    } else {
        return "";
    }
}


// For an unknown reason, Glib::filename_to_utf8 doesn't work on reliably Windows,
// so we're using Glib::filename_to_utf8 for Linux/Apple and Glib::locale_to_utf8 for Windows.
Glib::ustring fname_to_utf8(const std::string &fname)
{
#ifdef _WIN32

    // PhotoEditor 侧统一以 UTF-8 传入路径（QString::toUtf8()），g_fopen 在 Windows 上也期望 UTF-8。
    // 旧实现无条件 locale_to_utf8，会把已经是 UTF-8 的非 ASCII 路径当成本地(GBK)编码再转一次，
    // 结果中文/非 ASCII 路径的 RAW 打不开（换成纯英文路径才正常）。
    // 因此：入参本身就是合法 UTF-8 时直接透传；仅在确实不是 UTF-8（老式本地编码）时才回退旧逻辑。
    if (fname.empty() || g_utf8_validate(fname.c_str(), -1, nullptr)) {
        return Glib::ustring(fname);
    }

    try {
        return Glib::locale_to_utf8(fname);
    } catch (Glib::Error&) {
        return Glib::convert_with_fallback(fname, "UTF-8", "ISO-8859-1", "?");
    }

#else

    return Glib::filename_to_utf8(fname);

#endif
}
