/*  This file is part of YUView - The YUV player with advanced analytics toolset
*   <https://github.com/IENT/YUView>
*   Copyright (C) 2015  Institut für Nachrichtentechnik, RWTH Aachen University, GERMANY
*
*   This program is free software; you can redistribute it and/or modify
*   it under the terms of the GNU General Public License as published by
*   the Free Software Foundation; either version 3 of the License, or
*   (at your option) any later version.
*
*   In addition, as a special exception, the copyright holders give
*   permission to link the code of portions of this program with the
*   OpenSSL library under certain conditions as described in each
*   individual source file, and distribute linked combinations including
*   the two.
*   
*   You must obey the GNU General Public License in all respects for all
*   of the code used other than OpenSSL. If you modify file(s) with this
*   exception, you may extend this exception to your version of the
*   file(s), but you are not obligated to do so. If you do not wish to do
*   so, delete this exception statement from your version. If you delete
*   this exception statement from all source files in the program, then
*   also delete it here.
*
*   This program is distributed in the hope that it will be useful,
*   but WITHOUT ANY WARRANTY; without even the implied warranty of
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
*   GNU General Public License for more details.
*
*   You should have received a copy of the GNU General Public License
*   along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include "hmDecoder.h"

#include <cstring>
#include <QCoreApplication>
#include <QDir>
#include "typedef.h"

// Debug the decoder ( 0:off 1:interactive deocder only 2:caching decoder only 3:both)
#define LIBhmDecoder_DEBUG_OUTPUT 0
#if LIBhmDecoder_DEBUG_OUTPUT && !NDEBUG
#include <QDebug>
#if LIBhmDecoder_DEBUG_OUTPUT == 1
#define DEBUG_hmDecoder if(!isCachingDecoder) qDebug
#elif LIBhmDecoder_DEBUG_OUTPUT == 2
#define DEBUG_hmDecoder if(isCachingDecoder) qDebug
#elif LIBhmDecoder_DEBUG_OUTPUT == 3
#define DEBUG_hmDecoder if (isCachingDecoder) qDebug("c:"); else qDebug("i:"); qDebug
#endif
#else
#define DEBUG_hmDecoder(fmt,...) ((void)0)
#endif

hmDecoderFunctions::hmDecoderFunctions() { memset(this, 0, sizeof(*this)); }

hmDecoder::hmDecoder(int signalID, bool cachingDecoder) :
  decoderError(false),
  parsingError(false),
  internalsSupported(false),
  predAndResiSignalsSupported(false)
{
  // Try to load the decoder library (.dll on Windows, .so on Linux, .dylib on Mac)
  loadDecoderLibrary();

  decoder = nullptr;
  retrieveStatistics = false;
  statsCacheCurPOC = -1;
  isCachingDecoder = cachingDecoder;
  stateReadingFrames = false;
  currentHMPic = nullptr;

  // Set the signal to decode (if supported)
  if (predAndResiSignalsSupported && signalID >= 0 && signalID <= 3)
    decodeSignal = signalID;
  else
    decodeSignal = 0;

  // The buffer holding the last requested frame (and its POC). (Empty when constructing this)
  // When using the zoom box the getOneFrame function is called frequently so we
  // keep this buffer to not decode the same frame over and over again.
  currentOutputBufferFrameIndex = -1;

  if (decoderError)
    // There was an internal error while loading/initializing the decoder. Abort.
    return;
  
  // Allocate a decoder
  allocateNewDecoder();
}

bool hmDecoder::openFile(QString fileName, hmDecoder *otherDecoder)
{ 
  // Open the file, decode the first frame and return if this was successfull.
  if (otherDecoder)
    parsingError = !annexBFile.openFile(fileName, false, &otherDecoder->annexBFile);
  else
    parsingError = !annexBFile.openFile(fileName);
  if (!decoderError)
    decoderError &= (!loadYUVFrameData(0).isEmpty());
  return !parsingError && !decoderError;
}

void hmDecoder::setError(const QString &reason)
{
  decoderError = true;
  errorString = reason;
}

QFunctionPointer hmDecoder::resolve(const char *symbol)
{
  QFunctionPointer ptr = library.resolve(symbol);
  if (!ptr) setError(QStringLiteral("Error loading the libde265 library: Can't find function %1.").arg(symbol));
  return ptr;
}

template <typename T> T hmDecoder::resolve(T &fun, const char *symbol)
{
  return fun = reinterpret_cast<T>(resolve(symbol));
}

template <typename T> T hmDecoder::resolveInternals(T &fun, const char *symbol)
{
  return fun = reinterpret_cast<T>(library.resolve(symbol));
}

void hmDecoder::loadDecoderLibrary()
{
  // Try to load the libde265 library from the current working directory
  // Unfortunately relative paths like this do not work: (at least on windows)
  // library.setFileName(".\\libde265");

  // If the file name is not set explicitly, QLibrary will try to open
  // the libde265.so file first. Since this has been compiled for linux
  // it will fail and not even try to open the libde265.dylib.
  // On windows and linux ommitting the extension works
  QStringList const libNames =
        is_Q_OS_MAC ?
           QStringList() << "libHMDecoder.dylib" :
           QStringList() << "libHMDecoder";

  QStringList const libPaths = QStringList()
      << QDir::currentPath() + "/%1"
      << QDir::currentPath() + "/libde265/%1"
      << QCoreApplication::applicationDirPath() + "/%1"
      << QCoreApplication::applicationDirPath() + "/libde265/%1"
      << "%1"; // Try the system directories.

  bool libLoaded = false;
  for (auto &libName : libNames)
  {
    for (auto &libPath : libPaths)
    {
      library.setFileName(libPath.arg(libName));
      libraryPath = libPath.arg(libName);
      libLoaded = library.load();
      if (libLoaded)
        break;
    }
    if (libLoaded)
      break;
  }

  if (!libLoaded)
  {
    libraryPath.clear();
    return setError(library.errorString());
  }

  DEBUG_hmDecoder("hmDecoder::loadDecoderLibrary - %s", libraryPath);

  // Get/check function pointers
  if (!resolve(libHMDec_new_decoder, "libHMDec_new_decoder")) return;
  if (!resolve(libHMDec_free_decoder, "libHMDec_free_decoder")) return;
  if (!resolve(libHMDec_set_SEI_Check, "libHMDec_set_SEI_Check")) return;
  if (!resolve(libHMDec_set_max_temporal_layer, "libHMDec_set_max_temporal_layer")) return;
  if (!resolve(libHMDec_push_nal_unit, "libHMDec_push_nal_unit")) return;

  if (!resolve(libHMDec_get_picture, "libHMDec_get_picture")) return;
  if (!resolve(libHMDEC_get_POC, "libHMDEC_get_POC")) return;
  if (!resolve(libHMDEC_get_picture_width, "libHMDEC_get_picture_width")) return;
  if (!resolve(libHMDEC_get_picture_height, "libHMDEC_get_picture_height")) return;
  if (!resolve(libHMDEC_get_picture_stride, "libHMDEC_get_picture_stride")) return;
  if (!resolve(libHMDEC_get_image_plane, "libHMDEC_get_image_plane")) return;
  if (!resolve(libHMDEC_get_chroma_format, "libHMDEC_get_chroma_format")) return;

  if (!resolve(libHMDEC_get_internal_bit_depth, "libHMDEC_get_internal_bit_depth")) return;
  
  // Get pointers to the internals/statistics functions (if present)
  if (!resolve(libHMDEC_get_internal_info, "libHMDEC_get_internal_info")) return;

  // All interbals functions were successfully retrieved
  internalsSupported = true;
  DEBUG_hmDecoder("hmDecoder::loadDecoderLibrary - statistics internals found");

  return;
  
  // Get pointers to the functions for retrieving prediction/residual signals
  
  // The prediction and residual signal can be obtained
  predAndResiSignalsSupported = true;
  DEBUG_hmDecoder("hmDecoder::loadDecoderLibrary - prediction/residual internals found");
}

void hmDecoder::allocateNewDecoder()
{
  if (decoder != nullptr)
    return;

  DEBUG_hmDecoder("hmDecoder::allocateNewDecoder - decodeSignal %d", decodeSignal);

  // Set some decoder parameters
  libHMDec_set_SEI_Check(decoder, true);
  libHMDec_set_max_temporal_layer(decoder, -1);

  // Create new decoder object
  decoder = libHMDec_new_decoder();
  
  // Set retrieval of the right component
  if (predAndResiSignalsSupported)
  {
    // TODO. Or is this even possible?
  }
}

void hmDecoder::setDecodeSignal(int signalID)
{
  if (!predAndResiSignalsSupported)
    return;

  DEBUG_hmDecoder("hmDecoder::setDecodeSignal old %d new %d", decodeSignal, signalID);

  if (signalID != decodeSignal)
  {
    // A different signal was selected
    decodeSignal = signalID;

    // We will have to decode the current frame again to get the internals/statistics
    // This can be done like this:
    currentOutputBufferFrameIndex = -1;
    // Now the next call to loadYUVFrameData will load the frame again...
  }
}

QByteArray hmDecoder::loadYUVFrameData(int frameIdx)
{
  // At first check if the request is for the frame that has been requested in the
  // last call to this function.
  if (frameIdx == currentOutputBufferFrameIndex)
  {
    assert(!currentOutputBuffer.isEmpty()); // Must not be empty or something is wrong
    return currentOutputBuffer;
  }

  DEBUG_hmDecoder("hmDecoder::loadYUVFrameData Start request %d", frameIdx);

  // We have to decode the requested frame.
  bool seeked = false;
  QList<QByteArray> parameterSets;
  if ((int)frameIdx < currentOutputBufferFrameIndex || currentOutputBufferFrameIndex == -1)
  {
    // The requested frame lies before the current one. We will have to rewind and start decoding from there.
    int seekFrameIdx = annexBFile.getClosestSeekableFrameNumber(frameIdx);

    DEBUG_hmDecoder("hmDecoder::loadYUVFrameData Seek to %d", seekFrameIdx);
    parameterSets = annexBFile.seekToFrameNumber(seekFrameIdx);
    currentOutputBufferFrameIndex = seekFrameIdx - 1;
    seeked = true;
  }
  else if (frameIdx > currentOutputBufferFrameIndex+2)
  {
    // The requested frame is not the next one or the one after that. Maybe it would be faster to seek ahead in the bitstream and start decoding there.
    // Check if there is a random access point closer to the requested frame than the position that we are at right now.
    int seekFrameIdx = annexBFile.getClosestSeekableFrameNumber(frameIdx);
    if (seekFrameIdx > currentOutputBufferFrameIndex)
    {
      // Yes we can (and should) seek ahead in the file
      DEBUG_hmDecoder("hmDecoder::loadYUVFrameData Seek to %d", seekFrameIdx);
      parameterSets = annexBFile.seekToFrameNumber(seekFrameIdx);
      currentOutputBufferFrameIndex = seekFrameIdx - 1;
      seeked = true;
    }
  }

  if (seeked)
  {
    // Reset the decoder and feed the parameter sets to it.
    // Then start normal decoding

    if (parameterSets.size() == 0)
      return QByteArray();

    // Delete decoder
    libHMDec_error err = libHMDec_free_decoder(decoder);
    if (err != LIBHMDEC_OK)
    {
      // Freeing the decoder failed.
      if (decError != err)
        decError = err;
      return QByteArray();
    }

    decoder = nullptr;

    // Create new decoder
    allocateNewDecoder();

    // Feed the parameter sets to the decoder
    bool bNewPicture;
    bool checkOutputPictures;
    for (QByteArray ps : parameterSets)
    {
      err = libHMDec_push_nal_unit(decoder, (uint8_t*)ps.data(), ps.size(), false, bNewPicture, checkOutputPictures);
      DEBUG_hmDecoder("hmDecoder::loadYUVFrameData pushed parameter NAL length %d%s%s", ps.length(), bNewPicture ? " bNewPicture" : "", checkOutputPictures ? " checkOutputPictures" : "");
    }
  }

  // Perform the decoding right now blocking the main thread.
  // Decode frames until we receive the one we are looking for.
  while (true)
  {
    // Decoding with the HM library works like this:
    // Push a NAL unit to the decoder. If bNewPicture is set, we will have to push it to the decoder again.
    // If checkOutputPictures is set, we can see if there is one (or more) pictures that can be read.

    if (!stateReadingFrames)
    {
      bool bNewPicture;
      bool checkOutputPictures = false;
      // The picture pointer will be invalid when we push the next NAL unit to the decoder.
      currentHMPic = nullptr;

      if (!lastNALUnit.isEmpty())
      {
        libHMDec_push_nal_unit(decoder, lastNALUnit, lastNALUnit.length(), false, bNewPicture, checkOutputPictures);
        DEBUG_hmDecoder("hmDecoder::loadYUVFrameData pushed last NAL length %d%s%s", lastNALUnit.length(), bNewPicture ? " bNewPicture" : "", checkOutputPictures ? " checkOutputPictures" : "");
        // bNewPicture should now be false
        assert(!bNewPicture);
        lastNALUnit.clear();
      }
      else
      {
        // Get the next NAL unit
        QByteArray nalUnit = annexBFile.getNextNALUnit();
        assert(nalUnit.length() > 0);
        bool endOfFile = annexBFile.atEnd();

        libHMDec_push_nal_unit(decoder, nalUnit, nalUnit.length(), endOfFile, bNewPicture, checkOutputPictures);
        DEBUG_hmDecoder("hmDecoder::loadYUVFrameData pushed next NAL length %d%s%s", nalUnit.length(), bNewPicture ? " bNewPicture" : "", checkOutputPictures ? " checkOutputPictures" : "");
        
        if (bNewPicture)
          // Save the NAL unit
          lastNALUnit = nalUnit;
      }
      
      if (checkOutputPictures)
        stateReadingFrames = true;
    }

    if (stateReadingFrames)
    {
      // Try to read pictures
      libHMDec_picture *pic = libHMDec_get_picture(decoder);
      while (pic != nullptr)
      {
        // We recieved a picture
        currentOutputBufferFrameIndex++;
        currentHMPic = pic;

        // First update the chroma format and frame size
        pixelFormat = libHMDEC_get_chroma_format(pic);
        nrBitsC0 = libHMDEC_get_internal_bit_depth(LIBHMDEC_LUMA);
        frameSize = QSize(libHMDEC_get_picture_width(pic, LIBHMDEC_LUMA), libHMDEC_get_picture_height(pic, LIBHMDEC_LUMA));
        
        if (currentOutputBufferFrameIndex == frameIdx)
        {
          // This is the frame that we want to decode

          // Put image data into buffer
          copyImgToByteArray(pic, currentOutputBuffer);

          if (retrieveStatistics)
          {
            // Get the statistics from the image and put them into the statistics cache
            cacheStatistics(pic);

            // The cache now contains the statistics for iPOC
            statsCacheCurPOC = currentOutputBufferFrameIndex;
          }

          // Picture decoded
          DEBUG_hmDecoder("hmDecoder::loadYUVFrameData decoded the requested frame %d - POC %d", currentOutputBufferFrameIndex, libHMDEC_get_POC(pic));

          return currentOutputBuffer;
        }
        else
        {
          DEBUG_hmDecoder("hmDecoder::loadYUVFrameData decoded the unrequested frame %d - POC %d", currentOutputBufferFrameIndex, libHMDEC_get_POC(pic));
        }

        // Try to get another picture
        pic = libHMDec_get_picture(decoder);
      }
    }
    
    stateReadingFrames = false;
  }
  
  return QByteArray();
}

#if SSE_CONVERSION
void hmDecoder::copyImgToByteArray(libHMDec_picture *src, byteArrayAligned &dst)
#else
void hmDecoder::copyImgToByteArray(libHMDec_picture *src, QByteArray &dst)
#endif
{
  // How many image planes are there?
  int nrPlanes = (pixelFormat == LIBHMDEC_CHROMA_400) ? 1 : 3;

  // At first get how many bytes we are going to write
  int nrBytes = 0;
  int stride;
  for (int c = 0; c < nrPlanes; c++)
  {
    libHMDec_ColorComponent component = (c == 0) ? LIBHMDEC_LUMA : (c == 1) ? LIBHMDEC_CHROMA_U : LIBHMDEC_CHROMA_V;

    int width = libHMDEC_get_picture_width(src, component);
    int height = libHMDEC_get_picture_height(src, component);
    int nrBytesPerSample = (libHMDEC_get_internal_bit_depth(LIBHMDEC_LUMA) > 8) ? 2 : 1;

    nrBytes += width * height * nrBytesPerSample;
  }

  DEBUG_hmDecoder("hmDecoder::copyImgToByteArray nrBytes %d", nrBytes);

  // Is the output big enough?
  if (dst.capacity() < nrBytes)
    dst.resize(nrBytes);

  // We can now copy from src to dst
  char* dst_c = dst.data();
  for (int c = 0; c < nrPlanes; c++)
  {
    libHMDec_ColorComponent component = (c == 0) ? LIBHMDEC_LUMA : (c == 1) ? LIBHMDEC_CHROMA_U : LIBHMDEC_CHROMA_V;

    const short* img_c = nullptr;
    img_c = libHMDEC_get_image_plane(src, component);
    stride = libHMDEC_get_picture_stride(src, component);
    
    if (img_c == nullptr)
      return;

    int width = libHMDEC_get_picture_width(src, component);
    int height = libHMDEC_get_picture_height(src, component);
    int nrBytesPerSample = (libHMDEC_get_internal_bit_depth(LIBHMDEC_LUMA) > 8) ? 2 : 1;
    size_t size = width * nrBytesPerSample;

    for (int y = 0; y < height; y++)
    {
      for (int x = 0; x < width; x++)
      {
        dst_c[x] = (char)img_c[x];
      }
      img_c += stride;
      dst_c += size;
    }
  }
}

/* Convert the de265_chroma format to a YUVCPixelFormatType and return it.
*/
yuvPixelFormat hmDecoder::getYUVPixelFormat()
{
  if (pixelFormat == LIBHMDEC_CHROMA_400)
    return yuvPixelFormat(YUV_420, nrBitsC0);
  else if (pixelFormat == LIBHMDEC_CHROMA_420)
    return yuvPixelFormat(YUV_420, nrBitsC0);
  else if (pixelFormat == LIBHMDEC_CHROMA_422)
    return yuvPixelFormat(YUV_422, nrBitsC0);
  else if (pixelFormat == LIBHMDEC_CHROMA_444)
    return yuvPixelFormat(YUV_444, nrBitsC0);
  return yuvPixelFormat();
}

void hmDecoder::cacheStatistics(libHMDec_picture *img)
{
  if (!wrapperInternalsSupported())
    return;

  DEBUG_hmDecoder("hmDecoder::cacheStatistics POC %d", libHMDEC_get_POC(img));

  // Clear the local statistics cache
  curPOCStats.clear();

  // Get all the statistics
  // TODO: Could we only retrieve the statistics that are active/displayed?
  for (int i = 0; i <= LIBHMDEC_TU_COEFF_ENERGY_CR; i++)
  {
    libHMDec_info_type type = (libHMDec_info_type)i;
    std::vector<libHMDec_BlockValue> *stats = libHMDEC_get_internal_info(decoder, img, type);
    if (stats != nullptr)
      for (libHMDec_BlockValue b : (*stats))
        curPOCStats[i].addBlockValue(b.x, b.y, b.w, b.h, b.value);
  }
}

statisticsData hmDecoder::getStatisticsData(int frameIdx, int typeIdx)
{
  DEBUG_hmDecoder("hmDecoder::getStatisticsData %s", retrieveStatistics ? "" : "staistics retrievel avtivated");
  if (!retrieveStatistics)
    retrieveStatistics = true;

  if (frameIdx != statsCacheCurPOC)
  {
    if (currentOutputBufferFrameIndex == frameIdx && currentHMPic != NULL)
    {
      // We don't have to decode everything again if we still have a valid pointer to the picture
      cacheStatistics(currentHMPic);
      // The cache now contains the statistics for iPOC
      statsCacheCurPOC = currentOutputBufferFrameIndex;

      return curPOCStats[typeIdx];
    }
    else if (currentOutputBufferFrameIndex == frameIdx)
      // We will have to decode the current frame again to get the internals/statistics
      // This can be done like this:
      currentOutputBufferFrameIndex++;

    loadYUVFrameData(frameIdx);
  }

  return curPOCStats[typeIdx];
}

bool hmDecoder::reloadItemSource()
{
  if (decoderError)
    // Nothing is working, so there is nothing to reset.
    return false;

  // Reset the hmDecoder variables/buffers.
  decError = LIBHMDEC_OK;
  statsCacheCurPOC = -1;
  currentOutputBufferFrameIndex = -1;

  // Re-open the input file. This will reload the bitstream as if it was completely unknown.
  QString fileName = annexBFile.absoluteFilePath();
  parsingError = annexBFile.openFile(fileName);
  return parsingError;
}