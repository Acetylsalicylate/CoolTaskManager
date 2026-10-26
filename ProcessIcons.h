#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <shellapi.h>

#include <cstdint>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

#include <GLFW/glfw3.h>  // OpenGL entry points for the icon textures

#ifndef GL_CLAMP_TO_EDGE  // OpenGL 1.2 constant, missing from the 1.1 header shipped with GLFW
#define GL_CLAMP_TO_EDGE 0x812F
#endif

#include "imgui.h"

// Small shell icons of process executables, kept as OpenGL textures and shared between rows: processes that run the
// same executable (chrome.exe, svchost.exe, ...) only load one icon.
//
// Like every other GL resource here, the textures belong to the rendering context: release() must be called while the
// context is still current (main.cpp does that right before shutting the backends down).
class ProcessIconCache {
public:
    ProcessIconCache() = default;
    ~ProcessIconCache() { release(); }

    ProcessIconCache(const ProcessIconCache&) = delete;
    ProcessIconCache& operator=(const ProcessIconCache&) = delete;

    // Returns a texture of the executable's icon, or nullptr when the icon could not be obtained (no path, protected
    // process, unsupported icon format). Failures are remembered, so a missing icon is not retried every refresh.
    ImTextureID get(const std::string& utf8_path) {
        if (utf8_path.empty()) return nullptr;

        if (const auto it = characters_.find(utf8_path); it != characters_.end()) return it->second;

        ImTextureID texture = load(utf8_path);
        characters_.emplace(utf8_path, texture);
        return texture;
    }

    void release() {
        for (const auto& [path, texture] : characters_) {
            if (texture != nullptr) {
                const GLuint id = static_cast<GLuint>(reinterpret_cast<std::intptr_t>(texture));
                glDeleteTextures(1, &id);
            }
        }
        characters_.clear();
    }

private:
    static std::wstring toWide(const std::string& utf8) {
        const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
        if (size <= 1) return {};
        std::wstring wide(static_cast<std::size_t>(size), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, wide.data(), size);
        wide.pop_back();
        return wide;
    }

    static ImTextureID load(const std::string& utf8_path) {
        const std::wstring path = toWide(utf8_path);
        if (path.empty()) return nullptr;

        SHFILEINFOW info{};
        if (SHGetFileInfoW(path.c_str(), 0, &info, sizeof(info), SHGFI_ICON | SHGFI_SMALLICON) == 0 ||
            info.hIcon == nullptr) {
            return nullptr;
        }

        int width = 0;
        int height = 0;
        std::vector<unsigned char> pixels;
        const bool converted = iconToRgba(info.hIcon, width, height, pixels);
        DestroyIcon(info.hIcon);
        if (!converted) return nullptr;

        GLuint texture = 0;
        glGenTextures(1, &texture);
        if (texture == 0) return nullptr;

        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        glBindTexture(GL_TEXTURE_2D, 0);

        return reinterpret_cast<ImTextureID>(static_cast<std::intptr_t>(texture));
    }

    // Reads the icon's colour bitmap through GetDIBits: shell icons are 32-bit with an alpha channel, which keeps the
    // anti-aliased edges intact. Older 24-bit or monochrome icons get their transparency from the AND-mask instead.
    static bool iconToRgba(HICON icon, int& width, int& height, std::vector<unsigned char>& rgba) {
        ICONINFO icon_info{};
        if (GetIconInfo(icon, &icon_info) == FALSE) return false;

        BITMAP color_bitmap{};
        const bool has_color = icon_info.hbmColor != nullptr &&
                               GetObjectW(icon_info.hbmColor, sizeof(color_bitmap), &color_bitmap) != 0;
        const int w = has_color ? color_bitmap.bmWidth : 0;
        const int h = has_color ? color_bitmap.bmHeight : 0;
        if (w <= 0 || h <= 0 || w > 256 || h > 256) {
            if (icon_info.hbmColor) DeleteObject(icon_info.hbmColor);
            if (icon_info.hbmMask) DeleteObject(icon_info.hbmMask);
            return false;
        }

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = w;
        bmi.bmiHeader.biHeight = h;  // GetDIBits always returns bottom-up rows
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        HDC dc = CreateCompatibleDC(nullptr);
        if (dc == nullptr) {
            DeleteObject(icon_info.hbmColor);
            DeleteObject(icon_info.hbmMask);
            return false;
        }

        std::vector<unsigned char> bgra(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4, 0);
        const int copied = GetDIBits(dc, icon_info.hbmColor, 0, static_cast<UINT>(h), bgra.data(), &bmi, DIB_RGB_COLORS);
        if (copied == 0) {
            DeleteDC(dc);
            DeleteObject(icon_info.hbmColor);
            DeleteObject(icon_info.hbmMask);
            return false;
        }

        bool has_alpha = false;
        for (std::size_t i = 3; i < bgra.size(); i += 4) {
            if (bgra[i] != 0) {
                has_alpha = true;
                break;
            }
        }

        // 1bpp AND-mask: a set bit means "transparent".
        std::vector<unsigned char> mask;
        const int mask_stride = ((w + 31) / 32) * 4;
        if (!has_alpha && icon_info.hbmMask != nullptr) {
            BITMAPINFO mask_info{};
            mask_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            mask_info.bmiHeader.biWidth = w;
            mask_info.bmiHeader.biHeight = h;
            mask_info.bmiHeader.biPlanes = 1;
            mask_info.bmiHeader.biBitCount = 1;
            mask_info.bmiHeader.biCompression = BI_RGB;

            mask.assign(static_cast<std::size_t>(mask_stride) * static_cast<std::size_t>(h), 0);
            if (GetDIBits(dc, icon_info.hbmMask, 0, static_cast<UINT>(h), mask.data(), &mask_info, DIB_RGB_COLORS) == 0) {
                mask.clear();
            }
        }

        DeleteDC(dc);
        DeleteObject(icon_info.hbmColor);
        DeleteObject(icon_info.hbmMask);

        rgba.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4, 0);
        for (int y = 0; y < h; ++y) {
            const int source_row = h - 1 - y;  // flip bottom-up to top-down
            for (int x = 0; x < w; ++x) {
                const std::size_t source = (static_cast<std::size_t>(source_row) * w + x) * 4;
                const std::size_t target = (static_cast<std::size_t>(y) * w + x) * 4;

                unsigned char alpha = has_alpha ? bgra[source + 3] : 0;
                if (!has_alpha && !mask.empty()) {
                    const unsigned char byte = mask[static_cast<std::size_t>(source_row) * mask_stride + x / 8];
                    const bool transparent = (byte & (0x80u >> (x % 8))) != 0;
                    alpha = transparent ? 0 : 255;
                } else if (!has_alpha) {
                    alpha = 255;
                }

                rgba[target + 0] = bgra[source + 2];  // BGRA -> RGBA
                rgba[target + 1] = bgra[source + 1];
                rgba[target + 2] = bgra[source + 0];
                rgba[target + 3] = alpha;
            }
        }

        width = w;
        height = h;
        return true;
    }

    std::unordered_map<std::string, ImTextureID> characters_;
};
