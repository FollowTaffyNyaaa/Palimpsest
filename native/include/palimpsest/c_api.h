#pragma once

#include <cstddef>

#if defined(_WIN32)
#  if defined(PALIMPSEST_CORE_EXPORTS)
#    define PALIMPSEST_API __declspec(dllexport)
#  else
#    define PALIMPSEST_API __declspec(dllimport)
#  endif
#else
#  define PALIMPSEST_API
#endif

extern "C" {

PALIMPSEST_API const char* palimpsest_version();

PALIMPSEST_API int palimpsest_inspect_pdf_utf8(
    const char* pdf_path,
    const char* output_dir,
    char* error_buffer,
    std::size_t error_buffer_size);

}
