#include "png_image.h"
#include <algorithm>
#include <cwchar>

extern "C" __declspec(dllexport) pngtool::Image* ReadPng(int source, const wchar_t* path, wchar_t* message, int capacity) noexcept {
    try {
        pngtool::Image image;
        std::wstring error;
        HWND owner = GetActiveWindow();
        const bool ok = source == 0 ? pngtool::LoadClipboardPng(owner, image, error)
            : source == 1 ? pngtool::PickPngFile(owner, image, error)
            : pngtool::LoadPngFile(path, image, error);
        if (ok) return new pngtool::Image(std::move(image));
        if (capacity > 0) wcsncpy_s(message, capacity, error.c_str(), _TRUNCATE);
    } catch (...) {
        if (capacity > 0) wcsncpy_s(message, capacity, L"PNG could not be decoded.", _TRUNCATE);
    }
    return nullptr;
}

extern "C" __declspec(dllexport) const uint32_t* PngPixels(const pngtool::Image* image, uint32_t* width, uint32_t* height) noexcept {
    *width = image->width;
    *height = image->height;
    return image->rgba.data();
}

extern "C" __declspec(dllexport) void FreePng(pngtool::Image* image) noexcept { delete image; }
