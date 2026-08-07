/*
 * LeafGFX Generic Image Loader
 *
 * Adds PNG/GIF/JPEG support via stb_image while keeping BMP support
 * through the existing native loader.
 */

#include "leafgfx_bmp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#include "../../src/stb_image.h"

static bool has_extension_ci(const char* path, const char* ext) {
    if (!path || !ext) {
        return false;
    }

    size_t path_len = strlen(path);
    size_t ext_len = strlen(ext);
    if (path_len < ext_len) {
        return false;
    }

    const char* tail = path + (path_len - ext_len);
    for (size_t i = 0; i < ext_len; ++i) {
        char a = tail[i];
        char b = ext[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) {
            return false;
        }
    }
    return true;
}

static bool looks_like_bmp_path(const char* path) {
    return has_extension_ci(path, ".bmp");
}

static bool looks_like_stbi_path(const char* path) {
    return has_extension_ci(path, ".png") ||
           has_extension_ci(path, ".gif") ||
           has_extension_ci(path, ".jpg") ||
           has_extension_ci(path, ".jpeg");
}

static FILE* open_image_file_best_effort(const char* path, char* resolved, size_t resolved_len) {
    if (!path || !path[0]) {
        return NULL;
    }

    if (resolved && resolved_len > 0) {
        resolved[0] = '\0';
    }

    FILE* fp = fopen(path, "rb");
    if (fp) {
        if (resolved && resolved_len > 0) {
            snprintf(resolved, resolved_len, "%s", path);
        }
        return fp;
    }

    if (path[0] != '/') {
        char with_slash[512];
        snprintf(with_slash, sizeof(with_slash), "/%s", path);
        fp = fopen(with_slash, "rb");
        if (fp) {
            if (resolved && resolved_len > 0) {
                snprintf(resolved, resolved_len, "%s", with_slash);
            }
            return fp;
        }
    } else {
        fp = fopen(path + 1, "rb");
        if (fp) {
            if (resolved && resolved_len > 0) {
                snprintf(resolved, resolved_len, "%s", path + 1);
            }
            return fp;
        }
    }

    return NULL;
}

static gfx_bmp_result_t read_entire_file(const char* path, uint8_t** out_data, size_t* out_size) {
    if (!path || !out_data || !out_size) {
        return GFX_BMP_ERROR_INVALID_PARAMETER;
    }

    *out_data = NULL;
    *out_size = 0;

    char resolved[512];
    FILE* fp = open_image_file_best_effort(path, resolved, sizeof(resolved));
    if (!fp) {
        return GFX_BMP_ERROR_FILE_NOT_FOUND;
    }

    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (file_size <= 0 || file_size > 128L * 1024L * 1024L) {
        fclose(fp);
        return GFX_BMP_ERROR_INVALID_FILE;
    }

    uint8_t* data = (uint8_t*)malloc((size_t)file_size);
    if (!data) {
        fclose(fp);
        return GFX_BMP_ERROR_OUT_OF_MEMORY;
    }

    size_t bytes_read = fread(data, 1, (size_t)file_size, fp);
    fclose(fp);
    if (bytes_read != (size_t)file_size) {
        free(data);
        return GFX_BMP_ERROR_READ_ERROR;
    }

    *out_data = data;
    *out_size = (size_t)file_size;
    return GFX_BMP_SUCCESS;
}

static gfx_bmp_result_t decode_with_stbi(const uint8_t* data, size_t size, gfx_image_t** image) {
    if (!data || !image || size == 0) {
        return GFX_BMP_ERROR_INVALID_PARAMETER;
    }

    *image = NULL;

    int w = 0;
    int h = 0;
    int n = 0;
    stbi_uc* pixels = stbi_load_from_memory(data, (int)size, &w, &h, &n, 4);
    if (!pixels || w <= 0 || h <= 0) {
        if (pixels) {
            stbi_image_free(pixels);
        }
        return GFX_BMP_ERROR_INVALID_FILE;
    }

    gfx_image_t* out = gfx_image_create((uint32_t)w, (uint32_t)h, 0);
    if (!out) {
        stbi_image_free(pixels);
        return GFX_BMP_ERROR_OUT_OF_MEMORY;
    }

    const size_t px_count = (size_t)w * (size_t)h;
    for (size_t i = 0; i < px_count; ++i) {
        uint8_t r = pixels[i * 4 + 0];
        uint8_t g = pixels[i * 4 + 1];
        uint8_t b = pixels[i * 4 + 2];
        uint8_t a = pixels[i * 4 + 3];
        out->pixels[i] = ((uint32_t)a << 24) |
                         ((uint32_t)r << 16) |
                         ((uint32_t)g << 8) |
                         (uint32_t)b;
    }

    stbi_image_free(pixels);
    *image = out;
    return GFX_BMP_SUCCESS;
}

gfx_bmp_result_t gfx_image_load_memory(const uint8_t* data, size_t size,
                                       const char* path_hint, gfx_image_t** image) {
    if (!data || !image) {
        return GFX_BMP_ERROR_INVALID_PARAMETER;
    }

    if (path_hint && looks_like_bmp_path(path_hint)) {
        return gfx_image_load_bmp_memory(data, size, image);
    }

    if (path_hint && looks_like_stbi_path(path_hint)) {
        return decode_with_stbi(data, size, image);
    }

    gfx_bmp_result_t bmp = gfx_image_load_bmp_memory(data, size, image);
    if (bmp == GFX_BMP_SUCCESS) {
        return bmp;
    }

    return decode_with_stbi(data, size, image);
}

gfx_bmp_result_t gfx_image_load(const char* path, gfx_image_t** image) {
    if (!path || !image) {
        return GFX_BMP_ERROR_INVALID_PARAMETER;
    }

    *image = NULL;

    if (looks_like_bmp_path(path)) {
        return gfx_image_load_bmp(path, image);
    }

    uint8_t* data = NULL;
    size_t size = 0;
    gfx_bmp_result_t read_result = read_entire_file(path, &data, &size);
    if (read_result != GFX_BMP_SUCCESS) {
        return read_result;
    }

    gfx_bmp_result_t result = gfx_image_load_memory(data, size, path, image);
    free(data);
    return result;
}
