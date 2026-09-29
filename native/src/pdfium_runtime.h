#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace palimpsest::detail {

using FPDF_DOCUMENT = void*;
using FPDF_PAGE = void*;
using FPDF_TEXTPAGE = void*;
using FPDF_BITMAP = void*;

class PdfiumRuntime {
public:
    PdfiumRuntime() = default;
    ~PdfiumRuntime();
    PdfiumRuntime(const PdfiumRuntime&) = delete;
    PdfiumRuntime& operator=(const PdfiumRuntime&) = delete;

    bool load(std::string& error);
    [[nodiscard]] bool loaded() const { return module_ != nullptr; }

    void (*InitLibrary)(){};
    void (*DestroyLibrary)(){};
    FPDF_DOCUMENT (*LoadMemDocument64)(const void*, std::size_t, const char*){};
    void (*CloseDocument)(FPDF_DOCUMENT){};
    int (*GetPageCount)(FPDF_DOCUMENT){};
    FPDF_PAGE (*LoadPage)(FPDF_DOCUMENT, int){};
    void (*ClosePage)(FPDF_PAGE){};
    float (*GetPageWidthF)(FPDF_PAGE){};
    float (*GetPageHeightF)(FPDF_PAGE){};
    unsigned long (*GetLastError)(){};

    FPDF_TEXTPAGE (*TextLoadPage)(FPDF_PAGE){};
    void (*TextClosePage)(FPDF_TEXTPAGE){};
    int (*TextCountChars)(FPDF_TEXTPAGE){};
    unsigned int (*TextGetUnicode)(FPDF_TEXTPAGE, int){};
    int (*TextGetCharBox)(FPDF_TEXTPAGE, int, double*, double*, double*, double*){};
    double (*TextGetFontSize)(FPDF_TEXTPAGE, int){};
    unsigned long (*TextGetFontInfo)(FPDF_TEXTPAGE, int, void*, unsigned long, int*){};
    int (*TextGetFontWeight)(FPDF_TEXTPAGE, int){};

    FPDF_BITMAP (*BitmapCreate)(int, int, int){};
    void (*BitmapDestroy)(FPDF_BITMAP){};
    void (*BitmapFillRect)(FPDF_BITMAP, int, int, int, int, std::uint32_t){};
    void* (*BitmapGetBuffer)(FPDF_BITMAP){};
    int (*BitmapGetStride)(FPDF_BITMAP){};
    void (*RenderPageBitmap)(FPDF_BITMAP, FPDF_PAGE, int, int, int, int, int, int){};

private:
#if defined(_WIN32)
    HMODULE module_{};
#else
    void* module_{};
#endif
    bool initialized_{false};
};

}  // namespace palimpsest::detail
