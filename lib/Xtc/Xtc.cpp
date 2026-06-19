/**
 * Xtc.cpp
 *
 * Main XTC ebook class implementation
 * XTC ebook support for CrossPoint Reader
 */

#include "Xtc.h"

#include <Bitmap.h>
#include <HalStorage.h>
#include <Logging.h>

namespace {
std::string getThumbSignaturePath(const std::string& cachePath, int height) {
  return cachePath + "/thumb_" + std::to_string(height) + ".sig";
}

std::string buildThumbSignature(const xtc::XtcHeader& header, const xtc::PageInfo& pageInfo, uint32_t pageCount,
                                uint8_t bitDepth, uint64_t sourceSize, int height) {
  std::string signature;
  signature.reserve(256);
  signature += "xtc-thumb-v1\n";
  signature += "height=" + std::to_string(height) + '\n';
  signature += "pages=" + std::to_string(pageCount) + '\n';
  signature += "bitDepth=" + std::to_string(bitDepth) + '\n';
  signature += "sourceSize=" + std::to_string(sourceSize) + '\n';
  signature += "pageOffset=" + std::to_string(pageInfo.offset) + '\n';
  signature += "pageSize=" + std::to_string(pageInfo.size) + '\n';
  signature += "pageWidth=" + std::to_string(pageInfo.width) + '\n';
  signature += "pageHeight=" + std::to_string(pageInfo.height) + '\n';
  signature += "hasThumbnails=" + std::to_string(header.hasThumbnails) + '\n';
  signature += "thumbOffset=" + std::to_string(header.thumbOffset) + '\n';
  signature += "pageTableOffset=" + std::to_string(header.pageTableOffset) + '\n';
  signature += "dataOffset=" + std::to_string(header.dataOffset) + '\n';
  signature += "chapterOffset=" + std::to_string(header.chapterOffset) + '\n';
  return signature;
}

bool thumbSignatureMatches(const std::string& sigPath, const std::string& expected) {
  if (!Storage.exists(sigPath.c_str())) {
    return false;
  }

  const String current = Storage.readFile(sigPath.c_str());
  return current == expected.c_str();
}

bool writeThumbSignature(const std::string& sigPath, const std::string& signature) {
  return Storage.writeFile(sigPath.c_str(), String(signature.c_str()));
}
}  // namespace

bool Xtc::load() {
  LOG_DBG("XTC", "Loading XTC: %s", filepath.c_str());

  // Initialize parser
  parser.reset(new xtc::XtcParser());

  // Open XTC file
  xtc::XtcError err = parser->open(filepath.c_str());
  if (err != xtc::XtcError::OK) {
    LOG_ERR("XTC", "Failed to load: %s", xtc::errorToString(err));
    parser.reset();
    return false;
  }

  loaded = true;
  LOG_DBG("XTC", "Loaded XTC: %s (%lu pages)", filepath.c_str(), parser->getPageCount());
  return true;
}

bool Xtc::clearCache() const {
  if (!Storage.exists(cachePath.c_str())) {
    LOG_DBG("XTC", "Cache does not exist, no action needed");
    return true;
  }

  if (!Storage.removeDir(cachePath.c_str())) {
    LOG_ERR("XTC", "Failed to clear cache");
    return false;
  }

  LOG_DBG("XTC", "Cache cleared successfully");
  return true;
}

void Xtc::setupCacheDir() const {
  if (Storage.exists(cachePath.c_str())) {
    return;
  }

  // Create directories recursively
  for (size_t i = 1; i < cachePath.length(); i++) {
    if (cachePath[i] == '/') {
      Storage.mkdir(cachePath.substr(0, i).c_str());
    }
  }
  Storage.mkdir(cachePath.c_str());
}

std::string Xtc::getTitle() const {
  if (!loaded || !parser) {
    return "";
  }

  // Try to get title from XTC metadata first
  std::string title = parser->getTitle();
  if (!title.empty()) {
    return title;
  }

  // Fallback: extract filename from path as title
  size_t lastSlash = filepath.find_last_of('/');
  size_t lastDot = filepath.find_last_of('.');

  if (lastSlash == std::string::npos) {
    lastSlash = 0;
  } else {
    lastSlash++;
  }

  if (lastDot == std::string::npos || lastDot <= lastSlash) {
    return filepath.substr(lastSlash);
  }

  return filepath.substr(lastSlash, lastDot - lastSlash);
}

std::string Xtc::getAuthor() const {
  if (!loaded || !parser) {
    return "";
  }

  // Try to get author from XTC metadata
  return parser->getAuthor();
}

bool Xtc::hasChapters() const {
  if (!loaded || !parser) {
    return false;
  }
  return parser->hasChapters();
}

const std::vector<xtc::ChapterInfo>& Xtc::getChapters() {
  static const std::vector<xtc::ChapterInfo> kEmpty;
  if (!loaded || !parser) {
    return kEmpty;
  }
  return parser->getChapters();
}

std::string Xtc::getCoverBmpPath() const { return cachePath + "/cover.bmp"; }

bool Xtc::generateCoverBmp() const {
  // Already generated
  if (Storage.exists(getCoverBmpPath().c_str())) {
    return true;
  }

  if (!loaded || !parser) {
    LOG_ERR("XTC", "Cannot generate cover BMP, file not loaded");
    return false;
  }

  if (parser->getPageCount() == 0) {
    LOG_ERR("XTC", "No pages in XTC file");
    return false;
  }

  // Setup cache directory
  setupCacheDir();

  // Get first page info for cover
  xtc::PageInfo pageInfo;
  if (!parser->getPageInfo(0, pageInfo)) {
    LOG_DBG("XTC", "Failed to get first page info");
    return false;
  }

  // Get bit depth
  const uint8_t bitDepth = parser->getBitDepth();

  // Allocate buffer for page data
  // XTG (1-bit): Row-major, ((width+7)/8) * height bytes
  // XTH (2-bit): Two bit planes, column-major, ((width * height + 7) / 8) * 2 bytes
  size_t bitmapSize;
  if (bitDepth == 2) {
    bitmapSize = ((static_cast<size_t>(pageInfo.width) * pageInfo.height + 7) / 8) * 2;
  } else {
    bitmapSize = ((pageInfo.width + 7) / 8) * pageInfo.height;
  }
  uint8_t* pageBuffer = static_cast<uint8_t*>(malloc(bitmapSize));
  if (!pageBuffer) {
    LOG_ERR("XTC", "Failed to allocate page buffer (%lu bytes)", bitmapSize);
    return false;
  }

  // Load first page (cover)
  size_t bytesRead = const_cast<xtc::XtcParser*>(parser.get())->loadPage(0, pageBuffer, bitmapSize);
  if (bytesRead == 0) {
    LOG_ERR("XTC", "Failed to load cover page");
    free(pageBuffer);
    return false;
  }

  // Create BMP file
  HalFile coverBmp;
  if (!Storage.openFileForWrite("XTC", getCoverBmpPath(), coverBmp)) {
    LOG_DBG("XTC", "Failed to create cover BMP file");
    free(pageBuffer);
    return false;
  }

  // Write 1-bit BMP header (top-down row order)
  BmpHeader bmpHeader;
  createBmpHeader(&bmpHeader, pageInfo.width, pageInfo.height, BmpRowOrder::TopDown);
  coverBmp.write(reinterpret_cast<const uint8_t*>(&bmpHeader), sizeof(bmpHeader));

  const uint32_t rowSize = ((pageInfo.width + 31) / 32) * 4;

  // Write bitmap data
  // BMP requires 4-byte row alignment
  const size_t dstRowSize = (pageInfo.width + 7) / 8;  // 1-bit destination row size

  if (bitDepth == 2) {
    // XTH 2-bit mode: Two bit planes, column-major order
    // - Columns scanned right to left (x = width-1 down to 0)
    // - 8 vertical pixels per byte (MSB = topmost pixel in group)
    // - First plane: Bit1, Second plane: Bit2
    // - Pixel value = (bit1 << 1) | bit2
    const size_t planeSize = (static_cast<size_t>(pageInfo.width) * pageInfo.height + 7) / 8;
    const uint8_t* plane1 = pageBuffer;                 // Bit1 plane
    const uint8_t* plane2 = pageBuffer + planeSize;     // Bit2 plane
    const size_t colBytes = (pageInfo.height + 7) / 8;  // Bytes per column

    // Allocate a row buffer for 1-bit output
    uint8_t* rowBuffer = static_cast<uint8_t*>(malloc(dstRowSize));
    if (!rowBuffer) {
      free(pageBuffer);
      return false;
    }

    for (uint16_t y = 0; y < pageInfo.height; y++) {
      memset(rowBuffer, 0xFF, dstRowSize);  // Start with all white

      for (uint16_t x = 0; x < pageInfo.width; x++) {
        // Column-major, right to left: column index = (width - 1 - x)
        const size_t colIndex = pageInfo.width - 1 - x;
        const size_t byteInCol = y / 8;
        const size_t bitInByte = 7 - (y % 8);  // MSB = topmost pixel

        const size_t byteOffset = colIndex * colBytes + byteInCol;
        const uint8_t bit1 = (plane1[byteOffset] >> bitInByte) & 1;
        const uint8_t bit2 = (plane2[byteOffset] >> bitInByte) & 1;
        const uint8_t pixelValue = (bit1 << 1) | bit2;

        // Threshold: 0=white (1); 1,2,3=black (0)
        if (pixelValue >= 1) {
          // Set bit to 0 (black) in BMP format
          const size_t dstByte = x / 8;
          const size_t dstBit = 7 - (x % 8);
          rowBuffer[dstByte] &= ~(1 << dstBit);
        }
      }

      // Write converted row
      coverBmp.write(rowBuffer, dstRowSize);

      // Pad to 4-byte boundary
      uint8_t padding[4] = {0, 0, 0, 0};
      size_t paddingSize = rowSize - dstRowSize;
      if (paddingSize > 0) {
        coverBmp.write(padding, paddingSize);
      }
    }

    free(rowBuffer);
  } else {
    // 1-bit source: write directly with proper padding
    const size_t srcRowSize = (pageInfo.width + 7) / 8;

    for (uint16_t y = 0; y < pageInfo.height; y++) {
      // Write source row
      coverBmp.write(pageBuffer + y * srcRowSize, srcRowSize);

      // Pad to 4-byte boundary
      uint8_t padding[4] = {0, 0, 0, 0};
      size_t paddingSize = rowSize - srcRowSize;
      if (paddingSize > 0) {
        coverBmp.write(padding, paddingSize);
      }
    }
  }

  free(pageBuffer);

  LOG_DBG("XTC", "Generated cover BMP: %s", getCoverBmpPath().c_str());
  return true;
}

std::string Xtc::getThumbBmpPath() const { return cachePath + "/thumb_[HEIGHT].bmp"; }
std::string Xtc::getThumbBmpPath(int height) const { return cachePath + "/thumb_" + std::to_string(height) + ".bmp"; }

bool Xtc::generateThumbBmp(int height) const {
  if (!loaded || !parser) {
    LOG_ERR("XTC", "Cannot generate thumb BMP, file not loaded");
    return false;
  }

  if (parser->getPageCount() == 0) {
    LOG_ERR("XTC", "No pages in XTC file");
    return false;
  }

  // Setup cache directory
  setupCacheDir();

  // Get first page info for cover
  xtc::PageInfo pageInfo;
  if (!parser->getPageInfo(0, pageInfo)) {
    LOG_DBG("XTC", "Failed to get first page info");
    return false;
  }

  // Get bit depth
  const uint8_t bitDepth = parser->getBitDepth();
  const xtc::XtcHeader& header = parser->getHeader();
  const std::string thumbPath = getThumbBmpPath(height);
  const std::string sigPath = getThumbSignaturePath(cachePath, height);
  const std::string thumbSignature =
      buildThumbSignature(header, pageInfo, parser->getPageCount(), bitDepth, parser->getSourceFileSize(), height);

  // Already generated and still matches the source file, no work needed.
  if (Storage.exists(thumbPath.c_str()) && thumbSignatureMatches(sigPath, thumbSignature)) {
    return true;
  }

  // Prefer a pre-rendered thumbnail embedded in the source file when present.
  if (parser->hasEmbeddedThumbnail()) {
    HalFile thumbBmp;
    if (Storage.openFileForWrite("XTC", thumbPath, thumbBmp)) {
      const xtc::XtcError embeddedError = parser->copyEmbeddedThumbnailTo(thumbBmp);
      thumbBmp.flush();
      const bool closedOk = thumbBmp.close();
      if (embeddedError == xtc::XtcError::OK && closedOk && writeThumbSignature(sigPath, thumbSignature)) {
        LOG_DBG("XTC", "Copied embedded thumb BMP: %s", thumbPath.c_str());
        return true;
      }
    }
    Storage.remove(thumbPath.c_str());
    Storage.remove(sigPath.c_str());
  }

  // Calculate target dimensions for thumbnail (fit within 240x400 Continue Reading card)
  int THUMB_TARGET_WIDTH = height * 0.6;
  int THUMB_TARGET_HEIGHT = height;

  // Calculate scale factor
  float scaleX = static_cast<float>(THUMB_TARGET_WIDTH) / pageInfo.width;
  float scaleY = static_cast<float>(THUMB_TARGET_HEIGHT) / pageInfo.height;
  float scale = (scaleX > scaleY) ? scaleX : scaleY;  // for cropping

  // Only scale down, never up
  if (scale >= 1.0f) {
    // Page is already small enough, just use cover.bmp.
    // Copy cover.bmp to thumb.bmp.
    if (generateCoverBmp()) {
      HalFile src, dst;
      bool copyOk = false;
      if (Storage.openFileForRead("XTC", getCoverBmpPath(), src) &&
          Storage.openFileForWrite("XTC", thumbPath, dst)) {
        uint8_t buffer[512];
        copyOk = true;
        while (src.available()) {
          size_t bytesRead = src.read(buffer, sizeof(buffer));
          if (bytesRead == 0 || dst.write(buffer, bytesRead) != bytesRead) {
            copyOk = false;
            break;
          }
        }
        dst.flush();
        copyOk = copyOk && dst.close();
        src.close();
        if (copyOk && Storage.exists(thumbPath.c_str()) && writeThumbSignature(sigPath, thumbSignature)) {
          LOG_DBG("XTC", "Copied cover to thumb (no scaling needed)");
          return true;
        }
      }
      Storage.remove(thumbPath.c_str());
      Storage.remove(sigPath.c_str());
      LOG_DBG("XTC", "Failed to copy cover to thumb (no scaling needed)");
      return false;
    }
    return false;
  }

  uint16_t thumbWidth = static_cast<uint16_t>(pageInfo.width * scale);
  uint16_t thumbHeight = static_cast<uint16_t>(pageInfo.height * scale);

  LOG_DBG("XTC", "Generating thumb BMP: %dx%d -> %dx%d (scale: %.3f)", pageInfo.width, pageInfo.height, thumbWidth,
          thumbHeight, scale);

  // XTCH pages are twice the size of XTC pages. Loading a complete 480x800
  // page needs 96 KB of contiguous heap, which is not reliably available on
  // the home screen (especially after its cover buffer has been allocated).
  // Sample both planes while streaming and retain only thumbnail-sized data.
  if (bitDepth == 2) {
    const uint32_t rowSize = (thumbWidth + 31) / 32 * 4;
    const size_t thumbSize = static_cast<size_t>(rowSize) * thumbHeight;
    const size_t planeSize = (static_cast<size_t>(pageInfo.width) * pageInfo.height + 7) / 8;
    const size_t colBytes = (pageInfo.height + 7) / 8;

    auto* srcXToDst = static_cast<int16_t*>(malloc(static_cast<size_t>(pageInfo.width) * sizeof(int16_t)));
    auto* srcYToDst = static_cast<int16_t*>(malloc(static_cast<size_t>(pageInfo.height) * sizeof(int16_t)));
    auto* sampledPlane1 = static_cast<uint8_t*>(malloc(thumbSize));
    auto* thumbData = static_cast<uint8_t*>(malloc(thumbSize));
    if (!srcXToDst || !srcYToDst || !sampledPlane1 || !thumbData) {
      LOG_ERR("XTC", "Failed to allocate XTCH thumbnail buffers (%lu bytes)", thumbSize * 2);
      free(srcXToDst);
      free(srcYToDst);
      free(sampledPlane1);
      free(thumbData);
      return false;
    }

    memset(srcXToDst, 0xFF, static_cast<size_t>(pageInfo.width) * sizeof(int16_t));
    memset(srcYToDst, 0xFF, static_cast<size_t>(pageInfo.height) * sizeof(int16_t));
    memset(sampledPlane1, 0x00, thumbSize);
    memset(thumbData, 0xFF, thumbSize);

    const uint32_t scaleInvFp = static_cast<uint32_t>(65536.0f / scale);
    for (uint16_t dstX = 0; dstX < thumbWidth; dstX++) {
      uint32_t srcX = (static_cast<uint32_t>(dstX) * scaleInvFp) >> 16;
      if (srcX >= pageInfo.width) srcX = pageInfo.width - 1;
      srcXToDst[srcX] = static_cast<int16_t>(dstX);
    }
    for (uint16_t dstY = 0; dstY < thumbHeight; dstY++) {
      uint32_t srcY = (static_cast<uint32_t>(dstY) * scaleInvFp) >> 16;
      if (srcY >= pageInfo.height) srcY = pageInfo.height - 1;
      srcYToDst[srcY] = static_cast<int16_t>(dstY);
    }

    const xtc::XtcError streamError = const_cast<xtc::XtcParser*>(parser.get())->loadPageStreaming(
        0,
        [&](const uint8_t* data, size_t size, size_t offset) {
          for (size_t i = 0; i < size; i++) {
            const size_t absoluteOffset = offset + i;
            if (absoluteOffset >= planeSize * 2) return;

            const bool secondPlane = absoluteOffset >= planeSize;
            const size_t planeOffset = secondPlane ? absoluteOffset - planeSize : absoluteOffset;
            const size_t colIndex = planeOffset / colBytes;
            if (colIndex >= pageInfo.width) continue;

            const uint16_t srcX = pageInfo.width - 1 - static_cast<uint16_t>(colIndex);
            const int16_t dstX = srcXToDst[srcX];
            if (dstX < 0) continue;

            const size_t byteInCol = planeOffset % colBytes;
            for (uint8_t bit = 0; bit < 8; bit++) {
              const size_t srcY = byteInCol * 8 + bit;
              if (srcY >= pageInfo.height) break;
              const int16_t dstY = srcYToDst[srcY];
              if (dstY < 0) continue;

              const uint8_t planeBit = (data[i] >> (7 - bit)) & 1;
              const size_t byteIndex = static_cast<size_t>(dstY) * rowSize + static_cast<uint16_t>(dstX) / 8;
              const uint8_t bitMask = 1 << (7 - (static_cast<uint16_t>(dstX) % 8));
              if (!secondPlane) {
                if (planeBit) sampledPlane1[byteIndex] |= bitMask;
                continue;
              }

              const uint8_t bit1 = (sampledPlane1[byteIndex] & bitMask) ? 1 : 0;
              const uint8_t pixelValue = (bit1 << 1) | planeBit;
              const uint8_t grayValue = (3 - pixelValue) * 85;
              uint32_t hash = static_cast<uint32_t>(dstX) * 374761393u +
                              static_cast<uint32_t>(dstY) * 668265263u;
              hash = (hash ^ (hash >> 13)) * 1274126177u;
              const int threshold = static_cast<int>(hash >> 24);
              const int adjustedThreshold = 128 + ((threshold - 128) / 2);
              if (grayValue < adjustedThreshold) thumbData[byteIndex] &= ~bitMask;
            }
          }
        });

    free(srcXToDst);
    free(srcYToDst);
    free(sampledPlane1);

    if (streamError != xtc::XtcError::OK) {
      LOG_ERR("XTC", "Failed to stream XTCH cover page for thumb: %s", xtc::errorToString(streamError));
      free(thumbData);
      return false;
    }

    HalFile thumbBmp;
    if (!Storage.openFileForWrite("XTC", thumbPath, thumbBmp)) {
      LOG_DBG("XTC", "Failed to create thumb BMP file");
      free(thumbData);
      return false;
    }

    BmpHeader bmpHeader;
    createBmpHeader(&bmpHeader, thumbWidth, thumbHeight, BmpRowOrder::TopDown);
    const bool writeOk =
        thumbBmp.write(reinterpret_cast<const uint8_t*>(&bmpHeader), sizeof(bmpHeader)) == sizeof(bmpHeader) &&
        thumbBmp.write(thumbData, thumbSize) == thumbSize;
    thumbBmp.flush();
    const bool closedOk = thumbBmp.close();
    free(thumbData);
    if (!writeOk || !closedOk) {
      Storage.remove(thumbPath.c_str());
      Storage.remove(sigPath.c_str());
      LOG_ERR("XTC", "Failed to write XTCH thumbnail BMP");
      return false;
    }

    if (!writeThumbSignature(sigPath, thumbSignature)) {
      Storage.remove(thumbPath.c_str());
      Storage.remove(sigPath.c_str());
      LOG_ERR("XTC", "Failed to write XTCH thumbnail signature");
      return false;
    }

    LOG_DBG("XTC", "Generated streamed XTCH thumb BMP (%dx%d): %s", thumbWidth, thumbHeight, thumbPath.c_str());
    return true;
  }

  // Allocate buffer for page data
  size_t bitmapSize;
  if (bitDepth == 2) {
    bitmapSize = ((static_cast<size_t>(pageInfo.width) * pageInfo.height + 7) / 8) * 2;
  } else {
    bitmapSize = ((pageInfo.width + 7) / 8) * pageInfo.height;
  }
  uint8_t* pageBuffer = static_cast<uint8_t*>(malloc(bitmapSize));
  if (!pageBuffer) {
    LOG_ERR("XTC", "Failed to allocate page buffer (%lu bytes)", bitmapSize);
    return false;
  }

  // Load first page (cover)
  size_t bytesRead = const_cast<xtc::XtcParser*>(parser.get())->loadPage(0, pageBuffer, bitmapSize);
  if (bytesRead == 0) {
    LOG_ERR("XTC", "Failed to load cover page for thumb");
    free(pageBuffer);
    return false;
  }

  // Create thumbnail BMP file - use 1-bit format for fast home screen rendering (no gray passes)
  HalFile thumbBmp;
  if (!Storage.openFileForWrite("XTC", thumbPath, thumbBmp)) {
    LOG_DBG("XTC", "Failed to create thumb BMP file");
    free(pageBuffer);
    return false;
  }

  // Write 1-bit BMP header (top-down row order)
  BmpHeader bmpHeader;
  createBmpHeader(&bmpHeader, thumbWidth, thumbHeight, BmpRowOrder::TopDown);
  if (thumbBmp.write(reinterpret_cast<const uint8_t*>(&bmpHeader), sizeof(bmpHeader)) != sizeof(bmpHeader)) {
    free(pageBuffer);
    Storage.remove(thumbPath.c_str());
    Storage.remove(sigPath.c_str());
    LOG_ERR("XTC", "Failed to write thumb BMP header");
    return false;
  }

  const uint32_t rowSize = (thumbWidth + 31) / 32 * 4;

  // Allocate row buffer for 1-bit output
  uint8_t* rowBuffer = static_cast<uint8_t*>(malloc(rowSize));
  if (!rowBuffer) {
    free(pageBuffer);
    return false;
  }

  // Fixed-point scale factor (16.16)
  uint32_t scaleInv_fp = static_cast<uint32_t>(65536.0f / scale);

  // Pre-calculate plane info for 2-bit mode
  const size_t planeSize = (bitDepth == 2) ? ((static_cast<size_t>(pageInfo.width) * pageInfo.height + 7) / 8) : 0;
  const uint8_t* plane1 = (bitDepth == 2) ? pageBuffer : nullptr;
  const uint8_t* plane2 = (bitDepth == 2) ? pageBuffer + planeSize : nullptr;
  const size_t colBytes = (bitDepth == 2) ? ((pageInfo.height + 7) / 8) : 0;
  const size_t srcRowBytes = (bitDepth == 1) ? ((pageInfo.width + 7) / 8) : 0;

  for (uint16_t dstY = 0; dstY < thumbHeight; dstY++) {
    memset(rowBuffer, 0xFF, rowSize);  // Start with all white (bit 1)

    // Calculate source Y range with bounds checking
    uint32_t srcYStart = (static_cast<uint32_t>(dstY) * scaleInv_fp) >> 16;
    uint32_t srcYEnd = (static_cast<uint32_t>(dstY + 1) * scaleInv_fp) >> 16;
    if (srcYStart >= pageInfo.height) srcYStart = pageInfo.height - 1;
    if (srcYEnd > pageInfo.height) srcYEnd = pageInfo.height;
    if (srcYEnd <= srcYStart) srcYEnd = srcYStart + 1;
    if (srcYEnd > pageInfo.height) srcYEnd = pageInfo.height;

    for (uint16_t dstX = 0; dstX < thumbWidth; dstX++) {
      // Calculate source X range with bounds checking
      uint32_t srcXStart = (static_cast<uint32_t>(dstX) * scaleInv_fp) >> 16;
      uint32_t srcXEnd = (static_cast<uint32_t>(dstX + 1) * scaleInv_fp) >> 16;
      if (srcXStart >= pageInfo.width) srcXStart = pageInfo.width - 1;
      if (srcXEnd > pageInfo.width) srcXEnd = pageInfo.width;
      if (srcXEnd <= srcXStart) srcXEnd = srcXStart + 1;
      if (srcXEnd > pageInfo.width) srcXEnd = pageInfo.width;

      // Area averaging: sum grayscale values (0-255 range)
      uint32_t graySum = 0;
      uint32_t totalCount = 0;

      for (uint32_t srcY = srcYStart; srcY < srcYEnd && srcY < pageInfo.height; srcY++) {
        for (uint32_t srcX = srcXStart; srcX < srcXEnd && srcX < pageInfo.width; srcX++) {
          uint8_t grayValue = 255;  // Default: white

          if (bitDepth == 2) {
            // XTH 2-bit mode: pixel value 0-3
            // Bounds check for column index
            if (srcX < pageInfo.width) {
              const size_t colIndex = pageInfo.width - 1 - srcX;
              const size_t byteInCol = srcY / 8;
              const size_t bitInByte = 7 - (srcY % 8);
              const size_t byteOffset = colIndex * colBytes + byteInCol;
              // Bounds check for buffer access
              if (byteOffset < planeSize) {
                const uint8_t bit1 = (plane1[byteOffset] >> bitInByte) & 1;
                const uint8_t bit2 = (plane2[byteOffset] >> bitInByte) & 1;
                const uint8_t pixelValue = (bit1 << 1) | bit2;
                // Convert 2-bit (0-3) to grayscale: 0=black, 3=white
                // pixelValue: 0=white, 1=light gray, 2=dark gray, 3=black (XTC polarity)
                grayValue = (3 - pixelValue) * 85;  // 0->255, 1->170, 2->85, 3->0
              }
            }
          } else {
            // 1-bit mode
            const size_t byteIdx = srcY * srcRowBytes + srcX / 8;
            const size_t bitIdx = 7 - (srcX % 8);
            // Bounds check for buffer access
            if (byteIdx < bitmapSize) {
              const uint8_t pixelBit = (pageBuffer[byteIdx] >> bitIdx) & 1;
              // XTC 1-bit polarity: 0=black, 1=white (same as BMP palette)
              grayValue = pixelBit ? 255 : 0;
            }
          }

          graySum += grayValue;
          totalCount++;
        }
      }

      // Calculate average grayscale and quantize to 1-bit with noise dithering
      uint8_t avgGray = (totalCount > 0) ? static_cast<uint8_t>(graySum / totalCount) : 255;

      // Hash-based noise dithering for 1-bit output
      uint32_t hash = static_cast<uint32_t>(dstX) * 374761393u + static_cast<uint32_t>(dstY) * 668265263u;
      hash = (hash ^ (hash >> 13)) * 1274126177u;
      const int threshold = static_cast<int>(hash >> 24);           // 0-255
      const int adjustedThreshold = 128 + ((threshold - 128) / 2);  // Range: 64-192

      // Quantize to 1-bit: 0=black, 1=white
      uint8_t oneBit = (avgGray >= adjustedThreshold) ? 1 : 0;

      // Pack 1-bit value into row buffer (MSB first, 8 pixels per byte)
      const size_t byteIndex = dstX / 8;
      const size_t bitOffset = 7 - (dstX % 8);
      // Bounds check for row buffer access
      if (byteIndex < rowSize) {
        if (oneBit) {
          rowBuffer[byteIndex] |= (1 << bitOffset);  // Set bit for white
        } else {
          rowBuffer[byteIndex] &= ~(1 << bitOffset);  // Clear bit for black
        }
      }
    }

    // Write row (already padded to 4-byte boundary by rowSize)
    if (thumbBmp.write(rowBuffer, rowSize) != rowSize) {
      free(rowBuffer);
      free(pageBuffer);
      Storage.remove(thumbPath.c_str());
      Storage.remove(sigPath.c_str());
      LOG_ERR("XTC", "Failed to write thumb BMP row");
      return false;
    }
  }

  free(rowBuffer);
  free(pageBuffer);

  thumbBmp.flush();
  if (!thumbBmp.close()) {
    Storage.remove(thumbPath.c_str());
    Storage.remove(sigPath.c_str());
    LOG_ERR("XTC", "Failed to close thumb BMP");
    return false;
  }

  if (!writeThumbSignature(sigPath, thumbSignature)) {
    Storage.remove(thumbPath.c_str());
    Storage.remove(sigPath.c_str());
    LOG_ERR("XTC", "Failed to write thumb signature");
    return false;
  }

  LOG_DBG("XTC", "Generated thumb BMP (%dx%d): %s", thumbWidth, thumbHeight, getThumbBmpPath(height).c_str());
  return true;
}

uint32_t Xtc::getPageCount() const {
  if (!loaded || !parser) {
    return 0;
  }
  return parser->getPageCount();
}

uint16_t Xtc::getPageWidth() const {
  if (!loaded || !parser) {
    return 0;
  }
  return parser->getWidth();
}

uint16_t Xtc::getPageHeight() const {
  if (!loaded || !parser) {
    return 0;
  }
  return parser->getHeight();
}

uint8_t Xtc::getBitDepth() const {
  if (!loaded || !parser) {
    return 1;  // Default to 1-bit
  }
  return parser->getBitDepth();
}

size_t Xtc::loadPage(uint32_t pageIndex, uint8_t* buffer, size_t bufferSize) const {
  if (!loaded || !parser) {
    return 0;
  }
  return const_cast<xtc::XtcParser*>(parser.get())->loadPage(pageIndex, buffer, bufferSize);
}

xtc::XtcError Xtc::loadPageStreaming(uint32_t pageIndex,
                                     std::function<void(const uint8_t* data, size_t size, size_t offset)> callback,
                                     size_t chunkSize) const {
  if (!loaded || !parser) {
    return xtc::XtcError::FILE_NOT_FOUND;
  }
  return const_cast<xtc::XtcParser*>(parser.get())->loadPageStreaming(pageIndex, callback, chunkSize);
}

uint8_t Xtc::calculateProgress(uint32_t currentPage) const {
  if (!loaded || !parser || parser->getPageCount() == 0) {
    return 0;
  }
  return static_cast<uint8_t>((currentPage + 1) * 100 / parser->getPageCount());
}

xtc::XtcError Xtc::getLastError() const {
  if (!parser) {
    return xtc::XtcError::FILE_NOT_FOUND;
  }
  return parser->getLastError();
}
