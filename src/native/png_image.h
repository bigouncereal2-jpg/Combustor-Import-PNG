#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pngtool {

struct Image {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint32_t> rgba;
};

bool DecodePngBytes(const uint8_t* data, size_t size, Image& image, std::wstring& error);
bool LoadPngFile(const std::wstring& path, Image& image, std::wstring& error);
bool LoadClipboardPng(HWND owner, Image& image, std::wstring& error);
bool PickPngFile(HWND owner, Image& image, std::wstring& error);

}
