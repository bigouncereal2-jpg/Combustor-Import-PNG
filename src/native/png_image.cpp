#include "png_image.h"

#include <commdlg.h>
#include <cderr.h>
#include <shellapi.h>
#include <shlobj_core.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <new>

namespace pngtool {
namespace {

using Microsoft::WRL::ComPtr;
constexpr size_t MaxBytes = 64u * 1024u * 1024u;
constexpr uint32_t MaxDimension = 8192;
constexpr uint64_t MaxPixels = 16u * 1024u * 1024u;
static_assert(std::endian::native == std::endian::little);

struct ComScope {
    HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ~ComScope() { if (SUCCEEDED(result)) CoUninitialize(); }
    bool ready() const { return SUCCEEDED(result) || result == RPC_E_CHANGED_MODE; }
};

struct FileHandle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~FileHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};

struct ClipboardScope {
    bool opened;
    explicit ClipboardScope(HWND owner) : opened(OpenClipboard(owner) != FALSE) {}
    ~ClipboardScope() { if (opened) CloseClipboard(); }
};

struct BitmapHandle {
    HBITMAP value = nullptr;
    ~BitmapHandle() { if (value) DeleteObject(value); }
};

bool Dimensions(uint32_t width, uint32_t height, std::wstring& error) {
    if (!width || !height) { error = L"The image has no pixels."; return false; }
    if (width > MaxDimension || height > MaxDimension) { error = L"The image must be at most 8192 pixels wide and high."; return false; }
    if (uint64_t(width) * height > MaxPixels) { error = L"The image must contain at most 16 million pixels."; return false; }
    return true;
}

uint32_t Big32(const uint8_t* data) {
    return (uint32_t(data[0]) << 24) | (uint32_t(data[1]) << 16) | (uint32_t(data[2]) << 8) | data[3];
}

bool PngHeader(const uint8_t* data, size_t size, uint32_t& width, uint32_t& height, std::wstring& error) {
    constexpr uint8_t signature[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (size > MaxBytes) { error = L"PNG files must be 64 MiB or smaller."; return false; }
    if (!data || size < 33 || std::memcmp(data, signature, 8) != 0 || Big32(data + 8) != 13 || std::memcmp(data + 12, "IHDR", 4) != 0) {
        error = L"This is not a valid PNG image.";
        return false;
    }
    width = Big32(data + 16);
    height = Big32(data + 20);
    if (!Dimensions(width, height, error)) return false;
    const uint8_t depth = data[24], color = data[25];
    const bool depthOkay = (color == 0 && (depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16)) ||
        (color == 2 && (depth == 8 || depth == 16)) || (color == 3 && (depth == 1 || depth == 2 || depth == 4 || depth == 8)) ||
        ((color == 4 || color == 6) && (depth == 8 || depth == 16));
    if (!depthOkay || data[26] != 0 || data[27] != 0 || data[28] > 1) {
        error = L"The PNG header uses an unsupported or invalid format.";
        return false;
    }
    return true;
}

bool Factory(ComPtr<IWICImagingFactory>& factory, std::wstring& error) {
    HRESULT result = CoCreateInstance(CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.ReleaseAndGetAddressOf()));
    if (FAILED(result)) result = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.ReleaseAndGetAddressOf()));
    if (FAILED(result)) { error = L"Windows could not initialize the image decoder."; return false; }
    return true;
}

bool CopySource(IWICImagingFactory* factory, IWICBitmapSource* source, Image& image, std::wstring& error) {
    UINT width = 0, height = 0;
    if (FAILED(source->GetSize(&width, &height)) || !Dimensions(width, height, error)) {
        if (error.empty()) error = L"Windows could not read the image dimensions.";
        return false;
    }
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(converter.GetAddressOf())) ||
        FAILED(converter->Initialize(source, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
        error = L"Windows could not convert the image to RGBA pixels.";
        return false;
    }
    Image decoded;
    decoded.width = width;
    decoded.height = height;
    decoded.rgba.resize(size_t(width) * height);
    const UINT stride = width * 4;
    const UINT bytes = static_cast<UINT>(decoded.rgba.size() * sizeof(uint32_t));
    if (FAILED(converter->CopyPixels(nullptr, stride, bytes, reinterpret_cast<BYTE*>(decoded.rgba.data())))) {
        error = L"The image data is damaged or could not be decoded.";
        return false;
    }
    image = std::move(decoded);
    return true;
}

bool GlobalBytes(HANDLE handle, std::vector<uint8_t>& bytes, std::wstring& error) {
    if (!handle) { error = L"The clipboard image is unavailable."; return false; }
    const SIZE_T size = GlobalSize(static_cast<HGLOBAL>(handle));
    if (!size || size > MaxBytes) { error = L"The clipboard image is empty or larger than 64 MiB."; return false; }
    const void* data = GlobalLock(static_cast<HGLOBAL>(handle));
    if (!data) { error = L"Windows could not read the clipboard image."; return false; }
    try { bytes.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size); }
    catch (...) { GlobalUnlock(static_cast<HGLOBAL>(handle)); throw; }
    GlobalUnlock(static_cast<HGLOBAL>(handle));
    return true;
}

bool IsPngPath(const std::wstring& path) {
    return path.size() >= 4 && _wcsicmp(path.c_str() + path.size() - 4, L".png") == 0;
}

bool DropPath(const std::vector<uint8_t>& bytes, std::wstring& path, std::wstring& error) {
    if (bytes.size() < sizeof(DROPFILES)) { error = L"The clipboard file list is invalid."; return false; }
    DROPFILES drop{};
    std::memcpy(&drop, bytes.data(), sizeof(drop));
    if (drop.pFiles < sizeof(drop) || drop.pFiles >= bytes.size()) { error = L"The clipboard file list is invalid."; return false; }
    const uint8_t* start = bytes.data() + drop.pFiles;
    const size_t available = bytes.size() - drop.pFiles;
    if (drop.fWide) {
        size_t i = 0;
        for (; i + 1 < available; i += 2) {
            const wchar_t character = wchar_t(uint16_t(start[i]) | (uint16_t(start[i + 1]) << 8));
            if (!character) break;
            if (path.size() == 32767) { error = L"The clipboard file path is too long."; return false; }
            path.push_back(character);
        }
        if (path.empty() || i + 3 >= available || start[i] || start[i + 1] || start[i + 2] || start[i + 3]) {
            error = L"Copy a single PNG file before pasting.";
            return false;
        }
    } else {
        size_t length = 0;
        while (length < available && start[length]) ++length;
        if (!length || length + 1 >= available || start[length + 1] != 0 || length > 32767) { error = L"Copy a single PNG file before pasting."; return false; }
        const int needed = MultiByteToWideChar(CP_ACP, 0, reinterpret_cast<const char*>(start), static_cast<int>(length), nullptr, 0);
        if (!needed) { error = L"The clipboard file path could not be read."; return false; }
        path.resize(needed);
        if (!MultiByteToWideChar(CP_ACP, 0, reinterpret_cast<const char*>(start), static_cast<int>(length), path.data(), needed)) { error = L"The clipboard file path could not be read."; return false; }
    }
    if (!IsPngPath(path)) { error = L"Copy a single .png file before pasting."; return false; }
    return true;
}

bool ContiguousMask(uint32_t mask) {
    if (!mask) return false;
    const uint32_t value = mask >> std::countr_zero(mask);
    return (value & (value + 1)) == 0;
}

uint32_t Channel(uint32_t pixel, uint32_t mask, uint32_t fallback) {
    if (!mask) return fallback;
    const unsigned shift = std::countr_zero(mask);
    const uint32_t maximum = mask >> shift;
    return static_cast<uint32_t>((uint64_t((pixel & mask) >> shift) * 255 + maximum / 2) / maximum);
}

bool DibV5(const std::vector<uint8_t>& bytes, Image& image, std::wstring& error) {
    if (bytes.size() < sizeof(BITMAPV5HEADER)) { error = L"The clipboard bitmap header is incomplete."; return false; }
    BITMAPV5HEADER header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    if (header.bV5Size != sizeof(header) || header.bV5Planes != 1 || header.bV5Width <= 0 || header.bV5Height == 0 ||
        (header.bV5BitCount != 24 && header.bV5BitCount != 32) ||
        (header.bV5Compression != BI_RGB && header.bV5Compression != BI_BITFIELDS) ||
        (header.bV5BitCount == 24 && header.bV5Compression != BI_RGB) || header.bV5ProfileSize != 0 || header.bV5ProfileData != 0 || header.bV5ClrUsed > 256) {
        error = L"Copy the image as PNG, or choose a PNG file. This clipboard bitmap format is not supported.";
        return false;
    }
    const uint32_t width = static_cast<uint32_t>(header.bV5Width);
    const uint64_t absoluteHeight = header.bV5Height < 0 ? uint64_t(-int64_t(header.bV5Height)) : uint64_t(header.bV5Height);
    if (absoluteHeight > std::numeric_limits<uint32_t>::max() || !Dimensions(width, static_cast<uint32_t>(absoluteHeight), error)) return false;
    const uint32_t height = static_cast<uint32_t>(absoluteHeight);
    const uint64_t stride = ((uint64_t(width) * header.bV5BitCount + 31) / 32) * 4;
    const uint64_t offset = uint64_t(header.bV5Size) + uint64_t(header.bV5ClrUsed) * sizeof(RGBQUAD);
    if (offset > bytes.size() || stride * height > bytes.size() - offset) { error = L"The clipboard bitmap pixels are incomplete."; return false; }
    uint32_t red = header.bV5RedMask, green = header.bV5GreenMask, blue = header.bV5BlueMask, alpha = header.bV5AlphaMask;
    if (!red && !green && !blue) { red = 0x00ff0000; green = 0x0000ff00; blue = 0x000000ff; }
    if (!ContiguousMask(red) || !ContiguousMask(green) || !ContiguousMask(blue) || (alpha && !ContiguousMask(alpha)) ||
        (red & green) || (red & blue) || (green & blue) || (alpha & (red | green | blue))) { error = L"The clipboard bitmap has invalid color masks."; return false; }
    Image decoded{width, height, std::vector<uint32_t>(size_t(width) * height)};
    for (uint32_t y = 0; y < height; ++y) {
        const uint32_t sourceY = header.bV5Height < 0 ? y : height - y - 1;
        const uint8_t* row = bytes.data() + offset + stride * sourceY;
        for (uint32_t x = 0; x < width; ++x) {
            uint32_t pixel;
            if (header.bV5BitCount == 24) {
                pixel = row[x * 3 + 2] | (uint32_t(row[x * 3 + 1]) << 8) | (uint32_t(row[x * 3]) << 16) | 0xff000000;
            } else {
                uint32_t source;
                std::memcpy(&source, row + size_t(x) * 4, sizeof(source));
                pixel = Channel(source, red, 0) | (Channel(source, green, 0) << 8) | (Channel(source, blue, 0) << 16) | (Channel(source, alpha, 255) << 24);
            }
            decoded.rgba[size_t(y) * width + x] = pixel;
        }
    }
    image = std::move(decoded);
    return true;
}

bool BitmapImage(HBITMAP bitmap, Image& image, std::wstring& error) {
    ComScope apartment;
    if (!apartment.ready()) { error = L"Windows could not initialize image decoding."; return false; }
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmap> source;
    if (!Factory(factory, error)) return false;
    if (FAILED(factory->CreateBitmapFromHBITMAP(bitmap, nullptr, WICBitmapIgnoreAlpha, source.GetAddressOf()))) { error = L"Windows could not read the clipboard bitmap."; return false; }
    return CopySource(factory.Get(), source.Get(), image, error);
}

}

bool DecodePngBytes(const uint8_t* data, size_t size, Image& image, std::wstring& error) {
    error.clear();
    try {
        uint32_t width = 0, height = 0;
        if (!PngHeader(data, size, width, height, error)) return false;
        ComScope apartment;
        if (!apartment.ready()) { error = L"Windows could not initialize PNG decoding."; return false; }
        ComPtr<IWICImagingFactory> factory;
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        if (!Factory(factory, error)) return false;
        if (FAILED(factory->CreateStream(stream.GetAddressOf())) || FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(data), static_cast<DWORD>(size))) ||
            FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, decoder.GetAddressOf()))) { error = L"The PNG is damaged or could not be decoded."; return false; }
        GUID container{};
        if (FAILED(decoder->GetContainerFormat(&container)) || !IsEqualGUID(container, GUID_ContainerFormatPng) || FAILED(decoder->GetFrame(0, frame.GetAddressOf()))) { error = L"Windows could not read the PNG image."; return false; }
        UINT decodedWidth = 0, decodedHeight = 0;
        if (FAILED(frame->GetSize(&decodedWidth, &decodedHeight)) || decodedWidth != width || decodedHeight != height) { error = L"The PNG dimensions do not match its header."; return false; }
        return CopySource(factory.Get(), frame.Get(), image, error);
    } catch (const std::bad_alloc&) { error = L"There is not enough memory to import this image."; return false; }
}

bool LoadPngFile(const std::wstring& path, Image& image, std::wstring& error) {
    error.clear();
    try {
        FileHandle file;
        file.value = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (file.value == INVALID_HANDLE_VALUE) { error = L"The PNG file could not be opened."; return false; }
        LARGE_INTEGER size{};
        if (GetFileType(file.value) != FILE_TYPE_DISK || !GetFileSizeEx(file.value, &size) || size.QuadPart <= 0 || uint64_t(size.QuadPart) > MaxBytes) { error = L"Choose a PNG file no larger than 64 MiB."; return false; }
        std::vector<uint8_t> bytes(static_cast<size_t>(size.QuadPart));
        size_t read = 0;
        while (read < bytes.size()) {
            DWORD actual = 0;
            if (!ReadFile(file.value, bytes.data() + read, static_cast<DWORD>(bytes.size() - read), &actual, nullptr) || !actual) { error = L"The PNG file could not be read completely."; return false; }
            read += actual;
        }
        return DecodePngBytes(bytes.data(), bytes.size(), image, error);
    } catch (const std::bad_alloc&) { error = L"There is not enough memory to import this image."; return false; }
}

bool LoadClipboardPng(HWND owner, Image& image, std::wstring& error) {
    error.clear();
    try {
        enum class Kind { Png, File, Dib, Bitmap } kind;
        std::vector<uint8_t> bytes;
        std::wstring path;
        BitmapHandle bitmap;
        {
            ClipboardScope clipboard(owner);
            if (!clipboard.opened) { error = L"The clipboard is busy. Try again."; return false; }
            const UINT png = RegisterClipboardFormatW(L"PNG");
            if (png && IsClipboardFormatAvailable(png)) {
                kind = Kind::Png;
                if (!GlobalBytes(GetClipboardData(png), bytes, error)) return false;
            } else if (IsClipboardFormatAvailable(CF_HDROP)) {
                kind = Kind::File;
                if (!GlobalBytes(GetClipboardData(CF_HDROP), bytes, error) || !DropPath(bytes, path, error)) return false;
            } else if (IsClipboardFormatAvailable(CF_DIBV5)) {
                kind = Kind::Dib;
                if (!GlobalBytes(GetClipboardData(CF_DIBV5), bytes, error)) return false;
            } else if (IsClipboardFormatAvailable(CF_BITMAP)) {
                kind = Kind::Bitmap;
                const auto borrowed = static_cast<HBITMAP>(GetClipboardData(CF_BITMAP));
                BITMAP info{};
                if (!borrowed || GetObjectW(borrowed, sizeof(info), &info) != sizeof(info) || info.bmWidth <= 0 || info.bmHeight <= 0 || !Dimensions(info.bmWidth, info.bmHeight, error)) {
                    if (error.empty()) error = L"The clipboard bitmap could not be read.";
                    return false;
                }
                bitmap.value = static_cast<HBITMAP>(CopyImage(borrowed, IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION));
                if (!bitmap.value) { error = L"Windows could not copy the clipboard bitmap."; return false; }
            } else { error = L"The clipboard does not contain a PNG image, a single PNG file, or a supported bitmap."; return false; }
        }
        switch (kind) {
            case Kind::Png: return DecodePngBytes(bytes.data(), bytes.size(), image, error);
            case Kind::File: return LoadPngFile(path, image, error);
            case Kind::Dib: return DibV5(bytes, image, error);
            case Kind::Bitmap: return BitmapImage(bitmap.value, image, error);
        }
    } catch (const std::bad_alloc&) { error = L"There is not enough memory to import this image."; }
    return false;
}

bool PickPngFile(HWND owner, Image& image, std::wstring& error) {
    error.clear();
    try {
        std::vector<wchar_t> path(32768);
        OPENFILENAMEW picker{};
        picker.lStructSize = sizeof(picker);
        picker.hwndOwner = owner;
        picker.lpstrFilter = L"PNG images (*.png)\0*.png\0All files (*.*)\0*.*\0\0";
        picker.lpstrFile = path.data();
        picker.nMaxFile = static_cast<DWORD>(path.size());
        picker.lpstrTitle = L"Import PNG";
        picker.lpstrDefExt = L"png";
        picker.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_DONTADDTORECENT;
        if (!GetOpenFileNameW(&picker)) {
            const DWORD code = CommDlgExtendedError();
            if (code) error = code == FNERR_BUFFERTOOSMALL ? L"The selected file path is too long." : L"The PNG file picker could not be opened.";
            return false;
        }
        return LoadPngFile(path.data(), image, error);
    } catch (const std::bad_alloc&) { error = L"There is not enough memory to open the PNG file picker."; return false; }
}

}
