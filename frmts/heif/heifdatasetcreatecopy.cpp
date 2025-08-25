/******************************************************************************
 *
 * Project:  HEIF Driver
 * Author:   Brad Hards <bradh@frogmouth.net>
 *
 ******************************************************************************
 * Copyright (c) 2024, Brad Hards <bradh@frogmouth.net>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "heifdataset.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <vector>

#include "include_libheif.h"

#include "cpl_error.h"

#ifdef HAS_CUSTOM_FILE_WRITER

// Same default as libheif encoder example
constexpr int DEFAULT_QUALITY = 50;

static CPLErr mapColourInterpretation(GDALColorInterp colourInterpretation,
                                      heif_channel *channel)
{
    switch (colourInterpretation)
    {
        case GCI_RedBand:
            *channel = heif_channel_R;
            return CE_None;
        case GCI_GreenBand:
            *channel = heif_channel_G;
            return CE_None;
        case GCI_BlueBand:
            *channel = heif_channel_B;
            return CE_None;
        case GCI_AlphaBand:
            *channel = heif_channel_Alpha;
            return CE_None;
        default:
            return CE_Failure;
    }
}

static heif_compression_format getCompressionType(CSLConstList papszOptions)
{
    const char *pszValue = CSLFetchNameValue(papszOptions, "CODEC");
    if (pszValue == nullptr)
    {
        return heif_compression_HEVC;
    }
    if (strcmp(pszValue, "HEVC") == 0)
    {
        return heif_compression_HEVC;
    }
#if LIBHEIF_HAVE_VERSION(1, 7, 0)
    if (strcmp(pszValue, "AV1") == 0)
    {
        return heif_compression_AV1;
    }
#endif
#if LIBHEIF_HAVE_VERSION(1, 17, 0)
    if (strcmp(pszValue, "JPEG") == 0)
    {
        return heif_compression_JPEG;
    }
#endif
#if LIBHEIF_HAVE_VERSION(1, 17, 0)
    if (strcmp(pszValue, "JPEG2000") == 0)
    {
        return heif_compression_JPEG2000;
    }
#endif
#if LIBHEIF_HAVE_VERSION(1, 16, 0)
    if (strcmp(pszValue, "UNCOMPRESSED") == 0)
    {
        return heif_compression_uncompressed;
    }
#endif
#if LIBHEIF_HAVE_VERSION(1, 18, 0)
    if (strcmp(pszValue, "VVC") == 0)
    {
        return heif_compression_VVC;
    }
#endif
    CPLError(CE_Warning, CPLE_IllegalArg,
             "CODEC=%s value not recognised, ignoring.", pszValue);
    return heif_compression_HEVC;
}

static void setEncoderParameters(heif_encoder *encoder,
                                 CSLConstList papszOptions)
{
    const char *pszValue = CSLFetchNameValue(papszOptions, "QUALITY");
    int nQuality = DEFAULT_QUALITY;
    if (pszValue != nullptr)
    {
        nQuality = atoi(pszValue);
        if ((nQuality < 0) || (nQuality > 100))
        {
            CPLError(CE_Warning, CPLE_IllegalArg,
                     "QUALITY=%s value not recognised, ignoring.", pszValue);
            nQuality = DEFAULT_QUALITY;
        }
    }
    heif_encoder_set_lossy_quality(encoder, nQuality);
}

heif_error GDALHEIFDataset::VFS_WriterCallback(struct heif_context *,
                                               const void *data, size_t size,
                                               void *userdata)
{
    VSILFILE *fp = static_cast<VSILFILE *>(userdata);
    size_t bytesWritten = VSIFWriteL(data, 1, size, fp);
    heif_error result;
    if (bytesWritten == size)
    {
        result.code = heif_error_Ok;
        result.subcode = heif_suberror_Unspecified;
        result.message = "Success";
    }
    else
    {
        result.code = heif_error_Encoding_error;
        result.subcode = heif_suberror_Cannot_write_output_data;
        result.message = "Not all data written";
    }
    return result;
}

heif_error encodeRGBImage(GDALDataset *poSrcDS, heif_context *ctx,
                          heif_encoder *encoder)
{
    heif_image *image;

    heif_error err =
        heif_image_create(poSrcDS->GetRasterXSize(), poSrcDS->GetRasterYSize(),
                          heif_colorspace_RGB, heif_chroma_444, &image);
    if (err.code)
    {
        heif_context_free(ctx);
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Failed to create libheif input image.\n");
        return err;
    }

    for (auto &&poBand : poSrcDS->GetBands())
    {
        if (poBand->GetRasterDataType() != GDT_Byte)
        {
            heif_image_release(image);
            heif_context_free(ctx);
            CPLError(CE_Failure, CPLE_AppDefined, "Unsupported data type.");
            return err;
        }
        heif_channel channel;
        auto mapError =
            mapColourInterpretation(poBand->GetColorInterpretation(), &channel);
        if (mapError != CE_None)
        {
            heif_image_release(image);
            heif_context_free(ctx);
            CPLError(CE_Failure, CPLE_NotSupported,
                     "Driver does not support bands other than RGBA yet.");
            return err;
        }
        err = heif_image_add_plane(image, channel, poSrcDS->GetRasterXSize(),
                                   poSrcDS->GetRasterYSize(), 8);
        if (err.code)
        {
            heif_image_release(image);
            heif_context_free(ctx);
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Failed to add image plane to libheif input image.");
            return err;
        }
        int stride;
        uint8_t *p = heif_image_get_plane(image, channel, &stride);
        auto eErr = poBand->RasterIO(
            GF_Read, 0, 0, poSrcDS->GetRasterXSize(), poSrcDS->GetRasterYSize(),
            p, poSrcDS->GetRasterXSize(), poSrcDS->GetRasterYSize(), GDT_Byte,
            0, stride, nullptr);

        if (eErr != CE_None)
        {
            heif_image_release(image);
            heif_context_free(ctx);
            return err;
        }
    }

    // TODO: set options based on creation options
    heif_encoding_options *encoding_options = nullptr;

    heif_image_handle *out_image_handle;

    heif_context_encode_image(ctx, image, encoder, encoding_options,
                              &out_image_handle);

    heif_image_release(image);

    heif_image_handle_release(out_image_handle);
    heif_encoding_options_free(encoding_options);

    return err;
}

heif_error encodeEachBandAsGray(GDALDataset *poSrcDS, heif_context *ctx,
                                heif_encoder *encoder)
{
    int width = poSrcDS->GetRasterXSize();
    int height = poSrcDS->GetRasterYSize();
    heif_channel channel = heif_channel_Y;
    heif_error err;
    for (auto &&poBand : poSrcDS->GetBands())
    {
        heif_image *image;
        GDALDataType gdalType = poBand->GetRasterDataType();
        int bitDepth = GDALGetDataTypeSizeBits(gdalType);

        err = heif_image_create(
            poSrcDS->GetRasterXSize(), poSrcDS->GetRasterYSize(),
            heif_colorspace_monochrome, heif_chroma_monochrome, &image);
        if (err.code)
        {
            heif_context_free(ctx);
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Failed to create libheif input image.\n");
            return err;
        }

        err = heif_image_add_plane(image, channel, width, height, bitDepth);
        if (err.code)
        {
            heif_image_release(image);
            heif_context_free(ctx);
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Failed to add image plane to libheif input image.");
            return err;
        }

        int stride;
        uint8_t *p = heif_image_get_plane(image, channel, &stride);
        auto eErr = poBand->RasterIO(
            GF_Read, 0, 0, poSrcDS->GetRasterXSize(), poSrcDS->GetRasterYSize(),
            p, poSrcDS->GetRasterXSize(), poSrcDS->GetRasterYSize(), gdalType,
            0, stride, nullptr);

        if (eErr != CE_None)
        {
            heif_image_release(image);
            heif_context_free(ctx);
            return {heif_error_Usage_error, heif_suberror_Unspecified,
                    "Failed to read from source dataset."};
        }

        // TODO: set options based on creation options
        heif_encoding_options *encoding_options = nullptr;

        heif_image_handle *out_image_handle;

        heif_context_encode_image(ctx, image, encoder, encoding_options,
                                  &out_image_handle);

        heif_image_release(image);

        heif_image_handle_release(out_image_handle);
        heif_encoding_options_free(encoding_options);
    }

    return {heif_error_Ok, heif_suberror_Unspecified, "Success"};
}

/************************************************************************/
/*                             CreateCopy()                             */
/************************************************************************/
GDALDataset *
GDALHEIFDataset::CreateCopy(const char *pszFilename, GDALDataset *poSrcDS, int,
                            CPL_UNUSED char **papszOptions,
                            CPL_UNUSED GDALProgressFunc pfnProgress,
                            CPL_UNUSED void *pProgressData)
{
    if (pfnProgress == nullptr)
        pfnProgress = GDALDummyProgress;

    // TODO: more sanity checks

    heif_context *ctx = heif_context_alloc();
    heif_encoder *encoder;
    heif_compression_format codec = getCompressionType(papszOptions);
    struct heif_error err;
    err = heif_context_get_encoder_for_format(ctx, codec, &encoder);
    if (err.code)
    {
        heif_context_free(ctx);
        // TODO: get the error message and printf
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Failed to create libheif encoder.");
        return nullptr;
    }

    setEncoderParameters(encoder, papszOptions);

    int nBands = poSrcDS->GetRasterCount();
    switch (nBands)
    {
        case 0:
        case 1:
        case 2:
            return nullptr;
        case 3:
        case 4:
            err = encodeRGBImage(poSrcDS, ctx, encoder);
            break;
        default:
            err = encodeEachBandAsGray(poSrcDS, ctx, encoder);
            break;  //TODO:
    }
    if (err.code)
    {
        return nullptr;
    }

    // TODO: set properties on output image
    heif_encoder_release(encoder);

    VSILFILE *fp = VSIFOpenL(pszFilename, "wb");
    if (fp == nullptr)
    {
        ReportError(pszFilename, CE_Failure, CPLE_OpenFailed,
                    "Unable to create file.");
        heif_context_free(ctx);
        return nullptr;
    }
    heif_writer writer;
    writer.writer_api_version = 1;
    writer.write = VFS_WriterCallback;
    heif_context_write(ctx, &writer, fp);
    VSIFCloseL(fp);

    heif_context_free(ctx);

    return GDALDataset::Open(pszFilename);
}
#endif
