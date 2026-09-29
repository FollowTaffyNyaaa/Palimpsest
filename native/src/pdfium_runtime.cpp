#include "pdfium_runtime.h"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <sstream>

namespace palimpsest::detail {

#if defined(_WIN32)
namespace {

template <typename T>
bool bind(HMODULE module, const char* name, T& target, std::string& error) {
    target = reinterpret_cast<T>(GetProcAddress(module, name));
    if (target != nullptr) {
        return true;
    }
    error = std::string("PDFium is missing required export: ") + name;
    return false;
}

std::wstring env_pdfium_path() {
    wchar_t buffer[32768]{};
    const DWORD length = GetEnvironmentVariableW(L"PALIMPSEST_PDFIUM_DLL", buffer, static_cast<DWORD>(std::size(buffer)));
    if (length == 0 || length >= std::size(buffer)) {
        return {};
    }
    return std::wstring(buffer, length);
}

}  // namespace
#endif

PdfiumRuntime::~PdfiumRuntime() {
    if (initialized_ && DestroyLibrary != nullptr) {
        DestroyLibrary();
    }
#if defined(_WIN32)
    if (module_ != nullptr) {
        FreeLibrary(module_);
    }
#endif
}

bool PdfiumRuntime::load(std::string& error) {
#if !defined(_WIN32)
    error = "This bootstrap PDFium runtime currently supports Windows only.";
    return false;
#else
    if (module_ != nullptr) {
        return true;
    }

    const auto explicit_path = env_pdfium_path();
    if (!explicit_path.empty()) {
        module_ = LoadLibraryW(explicit_path.c_str());
    }
    if (module_ == nullptr) {
        module_ = LoadLibraryW(L"pdfium.dll");
    }
    if (module_ == nullptr) {
        std::ostringstream out;
        out << "Unable to load pdfium.dll (Win32 error " << GetLastError()
            << "). Run windows/scripts/build-native.ps1 so the pinned NuGet runtime is copied beside paper_inspector.exe.";
        error = out.str();
        return false;
    }

    if (!bind(module_, "FPDF_InitLibrary", InitLibrary, error) ||
        !bind(module_, "FPDF_DestroyLibrary", DestroyLibrary, error) ||
        !bind(module_, "FPDF_LoadMemDocument64", LoadMemDocument64, error) ||
        !bind(module_, "FPDF_CloseDocument", CloseDocument, error) ||
        !bind(module_, "FPDF_GetPageCount", GetPageCount, error) ||
        !bind(module_, "FPDF_LoadPage", LoadPage, error) ||
        !bind(module_, "FPDF_ClosePage", ClosePage, error) ||
        !bind(module_, "FPDF_GetPageWidthF", GetPageWidthF, error) ||
        !bind(module_, "FPDF_GetPageHeightF", GetPageHeightF, error) ||
        !bind(module_, "FPDF_GetLastError", GetLastError, error) ||
        !bind(module_, "FPDFText_LoadPage", TextLoadPage, error) ||
        !bind(module_, "FPDFText_ClosePage", TextClosePage, error) ||
        !bind(module_, "FPDFText_CountChars", TextCountChars, error) ||
        !bind(module_, "FPDFText_GetUnicode", TextGetUnicode, error) ||
        !bind(module_, "FPDFText_GetCharBox", TextGetCharBox, error) ||
        !bind(module_, "FPDFText_GetFontSize", TextGetFontSize, error) ||
        !bind(module_, "FPDFText_GetFontInfo", TextGetFontInfo, error) ||
        !bind(module_, "FPDFText_GetFontWeight", TextGetFontWeight, error) ||
        !bind(module_, "FPDFBitmap_Create", BitmapCreate, error) ||
        !bind(module_, "FPDFBitmap_Destroy", BitmapDestroy, error) ||
        !bind(module_, "FPDFBitmap_FillRect", BitmapFillRect, error) ||
        !bind(module_, "FPDFBitmap_GetBuffer", BitmapGetBuffer, error) ||
        !bind(module_, "FPDFBitmap_GetStride", BitmapGetStride, error) ||
        !bind(module_, "FPDF_RenderPageBitmap", RenderPageBitmap, error)) {
        FreeLibrary(module_);
        module_ = nullptr;
        return false;
    }

    InitLibrary();
    initialized_ = true;
    return true;
#endif
}

}  // namespace palimpsest::detail
