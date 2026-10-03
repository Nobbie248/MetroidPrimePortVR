// The movie import on Android (see port_remastered_movie.h). There is no
// ffmpeg to start there, so the system decodes: MediaExtractor takes the MP4
// apart, MediaCodec decodes the H.264 into raw pictures, and the port's own
// encoder (port_remastered_jpeg.h) writes them as the JPEGs of a THP. No SDL in
// here: a command line test builds from this file and the two it uses.

#include "port_remastered_jpeg.h"
#include "port_remastered_movie.h"

#include <cstdio>
#include <cstring>
#include <fstream>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>

namespace PortRemastered {
namespace {

// MediaCodecInfo.CodecCapabilities: the layouts a decoder writes into a buffer.
constexpr int32_t kColorPlanar = 19;
constexpr int32_t kColorPackedPlanar = 20;
constexpr int32_t kColorSemiPlanar = 21;
constexpr int32_t kColorPackedSemiPlanar = 39;
constexpr int32_t kColorQcomSemiPlanar = 0x7FA30C00;
constexpr int32_t kColorFlexible = 0x7F420888;
// MediaFormat: COLOR_STANDARD_BT709, COLOR_RANGE_FULL.
constexpr int32_t kStandardBt709 = 1;
constexpr int32_t kRangeFull = 1;

constexpr int64_t kWaitUs = 10000;

// How the decoder lays a picture out in its buffer; it says so in a format
// that arrives before the first picture and again when it changes.
struct Layout {
  int width = 0;
  int height = 0;
  int left = 0;
  int top = 0;
  // Where each plane starts in the buffer, and the steps between its rows and
  // (for the chroma, which may be interleaved) between its samples.
  size_t lumaAt = 0;
  size_t cbAt = 0;
  size_t crAt = 0;
  int stride = 0;
  int chromaStride = 0;
  int chromaStep = 1;
  bool bt709 = true;
  bool fullRange = false;
  bool known = false;
};

// MediaImage2 (frameworks/native/headers/media_plugin/media/hardware/VideoAPI.h):
// newer decoders describe the buffer with one, under "image-data". It settles
// what the "flexible" colour format leaves open.
struct ImagePlane {
  uint32_t offset;
  int32_t columnStep;
  int32_t rowStep;
  uint32_t horizontalSubsampling;
  uint32_t verticalSubsampling;
};
struct ImageData {
  uint32_t type; // 1 = YUV
  uint32_t planes;
  uint32_t width;
  uint32_t height;
  uint32_t bitDepth;
  uint32_t bitDepthAllocated;
  ImagePlane plane[4];
};

bool ReadImageData(AMediaFormat* format, Layout& layout) {
  void* data = nullptr;
  size_t size = 0;
  ImageData image;
  if (!AMediaFormat_getBuffer(format, "image-data", &data, &size) || data == nullptr || size < sizeof(image)) {
    return false;
  }
  std::memcpy(&image, data, sizeof(image));
  const ImagePlane& y = image.plane[0];
  const ImagePlane& cb = image.plane[1];
  const ImagePlane& cr = image.plane[2];
  if (image.type != 1 || image.planes != 3 || image.bitDepth != 8 || image.bitDepthAllocated != 8 ||
      y.columnStep != 1 || y.rowStep <= 0 || y.horizontalSubsampling != 1 || y.verticalSubsampling != 1 ||
      cb.horizontalSubsampling != 2 || cb.verticalSubsampling != 2 || cr.horizontalSubsampling != 2 ||
      cr.verticalSubsampling != 2 || cb.columnStep != cr.columnStep || cb.rowStep != cr.rowStep ||
      cb.columnStep <= 0 || cb.rowStep <= 0) {
    return false;
  }
  layout.lumaAt = y.offset;
  layout.stride = y.rowStep;
  layout.cbAt = cb.offset;
  layout.crAt = cr.offset;
  layout.chromaStride = cb.rowStep;
  layout.chromaStep = cb.columnStep;
  return true;
}

bool ReadLayout(AMediaFormat* format, Layout& layout, std::string& error) {
  layout = {};
  int32_t width = 0, height = 0;
  if (!AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_WIDTH, &width) ||
      !AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_HEIGHT, &height) || width <= 0 || height <= 0) {
    error = "the decoder did not say how large its pictures are";
    return false;
  }
  // The buffer holds whole macroblocks; the picture is the crop of it.
  int32_t left = 0, top = 0, right = 0, bottom = 0;
  if (AMediaFormat_getInt32(format, "crop-left", &left) && AMediaFormat_getInt32(format, "crop-right", &right) &&
      AMediaFormat_getInt32(format, "crop-top", &top) && AMediaFormat_getInt32(format, "crop-bottom", &bottom) &&
      left >= 0 && top >= 0 && right >= left && bottom >= top && right < width && bottom < height) {
    layout.left = left;
    layout.top = top;
    layout.width = right - left + 1;
    layout.height = bottom - top + 1;
  } else {
    layout.width = width;
    layout.height = height;
  }
  if (!ReadImageData(format, layout)) {
    // Older decoders: the colour format names the layout, the planes follow
    // one another, and stride and slice height give their size.
    int32_t stride = 0, sliceHeight = 0;
    AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_STRIDE, &stride);
    AMediaFormat_getInt32(format, "slice-height", &sliceHeight);
    stride = stride > 0 ? stride : width;
    sliceHeight = sliceHeight > 0 ? sliceHeight : height;
    int32_t color = kColorFlexible;
    AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_COLOR_FORMAT, &color);
    layout.stride = stride;
    layout.cbAt = size_t(stride) * size_t(sliceHeight);
    switch (color) {
    case kColorPlanar:
    case kColorPackedPlanar:
    case kColorFlexible:
      layout.chromaStride = (stride + 1) / 2;
      layout.chromaStep = 1;
      layout.crAt = layout.cbAt + size_t(layout.chromaStride) * size_t((sliceHeight + 1) / 2);
      break;
    case kColorSemiPlanar:
    case kColorPackedSemiPlanar:
    case kColorQcomSemiPlanar:
      layout.chromaStride = stride;
      layout.chromaStep = 2;
      layout.crAt = layout.cbAt + 1;
      break;
    default: {
      char text[96];
      std::snprintf(text, sizeof(text), "this device's decoder writes a picture layout the port can't read (0x%X)",
                    unsigned(color));
      error = text;
      return false;
    }
    }
  }
  // A stream that doesn't say is taken for what Remastered's are.
  int32_t standard = kStandardBt709, range = 0;
  AMediaFormat_getInt32(format, "color-standard", &standard);
  AMediaFormat_getInt32(format, "color-range", &range);
  layout.bt709 = standard == kStandardBt709 || standard == 0;
  layout.fullRange = range == kRangeFull;
  layout.known = true;
  return true;
}

// False when the buffer is too small for the layout it is said to have.
bool MakePicture(const Layout& layout, const uint8_t* data, size_t size, Picture& picture) {
  const size_t lumaAt = layout.lumaAt + size_t(layout.top) * size_t(layout.stride) + size_t(layout.left);
  const size_t chromaAt = size_t(layout.top / 2) * size_t(layout.chromaStride) +
                          size_t(layout.left / 2) * size_t(layout.chromaStep);
  // The last sample read of each plane; a last row need not be a whole stride long.
  const size_t lumaEnd = lumaAt + size_t(layout.height - 1) * size_t(layout.stride) + size_t(layout.width - 1);
  const size_t chromaEnd = chromaAt + size_t((layout.height + 1) / 2 - 1) * size_t(layout.chromaStride) +
                           size_t((layout.width + 1) / 2 - 1) * size_t(layout.chromaStep);
  if (layout.stride < layout.left + layout.width || lumaEnd >= size || layout.cbAt + chromaEnd >= size ||
      layout.crAt + chromaEnd >= size) {
    return false;
  }
  picture.width = layout.width;
  picture.height = layout.height;
  picture.stride = layout.stride;
  picture.y = data + lumaAt;
  picture.chromaStride = layout.chromaStride;
  picture.chromaStep = layout.chromaStep;
  picture.cb = data + layout.cbAt + chromaAt;
  picture.cr = data + layout.crAt + chromaAt;
  picture.bt709 = layout.bt709;
  picture.fullRange = layout.fullRange;
  return true;
}

struct Session {
  int fd = -1;
  AMediaExtractor* extractor = nullptr;
  AMediaFormat* track = nullptr;
  AMediaCodec* codec = nullptr;
  bool started = false;

  ~Session() {
    if (codec != nullptr) {
      if (started) {
        AMediaCodec_stop(codec);
      }
      AMediaCodec_delete(codec);
    }
    if (track != nullptr) {
      AMediaFormat_delete(track);
    }
    if (extractor != nullptr) {
      AMediaExtractor_delete(extractor);
    }
    if (fd >= 0) {
      close(fd);
    }
  }
};

bool Open(const std::string& mp4, Session& session, std::string& error) {
  session.fd = open(mp4.c_str(), O_RDONLY | O_CLOEXEC);
  struct stat info {};
  if (session.fd < 0 || fstat(session.fd, &info) != 0) {
    error = "cannot read the movie taken from the image";
    return false;
  }
  session.extractor = AMediaExtractor_new();
  if (session.extractor == nullptr ||
      AMediaExtractor_setDataSourceFd(session.extractor, session.fd, 0, info.st_size) != AMEDIA_OK) {
    error = "the system could not read the movie";
    return false;
  }
  const char* mime = nullptr;
  const size_t tracks = AMediaExtractor_getTrackCount(session.extractor);
  for (size_t i = 0; i < tracks && session.track == nullptr; ++i) {
    AMediaFormat* format = AMediaExtractor_getTrackFormat(session.extractor, i);
    if (format != nullptr && AMediaFormat_getString(format, AMEDIAFORMAT_KEY_MIME, &mime) && mime != nullptr &&
        std::strncmp(mime, "video/", 6) == 0) {
      AMediaExtractor_selectTrack(session.extractor, i);
      session.track = format;
    } else if (format != nullptr) {
      AMediaFormat_delete(format);
    }
  }
  if (session.track == nullptr) {
    error = "the movie has no picture";
    return false;
  }
  // Pictures into buffers the port can read, not onto a surface.
  AMediaFormat_setInt32(session.track, AMEDIAFORMAT_KEY_COLOR_FORMAT, kColorFlexible);
  session.codec = AMediaCodec_createDecoderByType(mime);
  if (session.codec == nullptr) {
    error = std::string("this device has no decoder for ") + mime;
    return false;
  }
  if (AMediaCodec_configure(session.codec, session.track, nullptr, nullptr, 0) != AMEDIA_OK ||
      AMediaCodec_start(session.codec) != AMEDIA_OK) {
    error = "this device's decoder refused the movie";
    return false;
  }
  session.started = true;
  return true;
}

} // namespace

// Nothing to look for: every Android has the decoder. The name is the one the
// import's notes use.
std::string FindFfmpeg() { return "MediaCodec"; }

bool ConvertMovie(const std::string&, const std::string& mp4, const std::string& thp, const MovieFormat& format,
                  const std::function<bool()>& cancelled, int& frames, std::string& error) {
  frames = 0;
  Session session;
  if (!Open(mp4, session, error)) {
    return false;
  }
  std::ofstream out(thp, std::ios::binary | std::ios::trunc);
  if (!out) {
    error = "cannot write the movie";
    return false;
  }
  ThpWriter writer(out, format.width, format.height, float(format.fps));
  MovieEncoder encoder(format);
  FramePacer pacer(format.fps);
  JpegSplitter splitter;
  const JpegSplitter::Sink sink = [&](const std::vector<uint8_t>& jpeg) {
    writer.Add(jpeg);
    return bool(out);
  };
  Layout layout;
  std::vector<uint8_t> jpeg;
  bool ok = true;
  bool inputDone = false;
  bool outputDone = false;
  bool haveFirst = false;
  int64_t firstTime = 0;
  // A decoder that stops answering must not hang the import.
  int idle = 0;
  while (ok && !outputDone) {
    if (cancelled && cancelled()) {
      ok = false;
      error = "cancelled";
      break;
    }
    if (!inputDone) {
      const ssize_t index = AMediaCodec_dequeueInputBuffer(session.codec, kWaitUs);
      if (index >= 0) {
        size_t capacity = 0;
        uint8_t* buffer = AMediaCodec_getInputBuffer(session.codec, size_t(index), &capacity);
        const ssize_t size = buffer != nullptr ? AMediaExtractor_readSampleData(session.extractor, buffer, capacity) : -1;
        if (size < 0) {
          AMediaCodec_queueInputBuffer(session.codec, size_t(index), 0, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
          inputDone = true;
        } else {
          AMediaCodec_queueInputBuffer(session.codec, size_t(index), 0, size_t(size),
                                       uint64_t(AMediaExtractor_getSampleTime(session.extractor)), 0);
          AMediaExtractor_advance(session.extractor);
        }
      }
    }
    AMediaCodecBufferInfo info{};
    const ssize_t index = AMediaCodec_dequeueOutputBuffer(session.codec, &info, kWaitUs);
    if (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
      AMediaFormat* now = AMediaCodec_getOutputFormat(session.codec);
      ok = now != nullptr && ReadLayout(now, layout, error);
      if (now != nullptr) {
        AMediaFormat_delete(now);
      } else {
        error = "the decoder did not describe its pictures";
      }
      idle = 0;
      continue;
    }
    if (index < 0) {
      // Nothing yet, or a notice that needs no answer. Five hundred of those
      // in a row (five seconds at least) is a decoder that will not finish.
      if (index != AMEDIACODEC_INFO_TRY_AGAIN_LATER && index != AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
        ok = false;
        error = "this device's decoder failed on the movie";
      } else if (++idle > 500) {
        ok = false;
        error = "this device's decoder stopped answering";
      }
      continue;
    }
    idle = 0;
    if (info.size > 0 && (info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) == 0) {
      size_t capacity = 0;
      const uint8_t* buffer = AMediaCodec_getOutputBuffer(session.codec, size_t(index), &capacity);
      Picture picture;
      if (buffer == nullptr || !layout.known || size_t(info.offset) + size_t(info.size) > capacity ||
          !MakePicture(layout, buffer + info.offset, size_t(info.size), picture)) {
        ok = false;
        error = "this device's decoder handed over a picture the port can't read";
      } else {
        if (!haveFirst) {
          haveFirst = true;
          firstTime = info.presentationTimeUs;
        }
        const int copies = pacer.Copies(info.presentationTimeUs - firstTime);
        if (copies > 0) {
          encoder.Encode(picture, jpeg);
          for (int i = 0; i < copies && ok; ++i) {
            if (!splitter.Feed(jpeg.data(), jpeg.size(), sink)) {
              ok = false;
              error = "cannot write the movie";
            }
          }
        }
      }
    }
    if ((info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0) {
      outputDone = true;
    }
    AMediaCodec_releaseOutputBuffer(session.codec, size_t(index), false);
  }
  if (ok && !writer.Finish()) {
    ok = false;
    error = writer.Frames() == 0 ? "the decoder gave no pictures" : "cannot write the movie";
  }
  frames = writer.Frames();
  out.close();
  if (!ok) {
    std::remove(thp.c_str());
  }
  return ok;
}

} // namespace PortRemastered
