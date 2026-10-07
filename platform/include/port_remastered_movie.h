#pragma once

// Remastered's menu movies, as movies the original game plays.
//
// A Remastered movie (FMV0) is an MP4 behind a 32 byte form header: H.264,
// 1600x900 at 60 frames a second. The game plays THP, which is a JPEG per
// frame, so a movie is decoded once at import and written again as THP under
// the disc's file name ("Video/01_startloop.thp"), where a mod's file takes
// the disc's place. The decoding is ffmpeg's: the port starts the `ffmpeg`
// program and reads JPEGs from its output, so nothing of ffmpeg is linked in.
// Without one the import leaves the disc's movies alone.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <string>
#include <vector>

namespace PortRemastered {

// A Remastered movie by its asset id (as IdToString prints it), and the disc
// files it stands in for: the disc has three copies of some transitions.
struct Movie {
  const char* id;
  std::vector<const char*> names;
};
const std::vector<Movie>& Movies();

// What the movies are written as. The game's decoder runs on the main thread
// and keeps up with this size at 30 frames a second, the disc's own rate, but
// not at 60.
struct MovieFormat {
  int width = 1600;
  int height = 900;
  int fps = 30;
};

// "1280x720@30", "1280x720" or "@60". False when `text` is none of them.
bool ParseMovieFormat(const std::string& text, MovieFormat& format);

// The MP4 inside an FMV0 asset. False when `data` is not one.
bool MovieStream(const uint8_t* data, size_t size, size_t& offset, size_t& length);

// Cuts a stream of JPEGs, as ffmpeg's image2pipe writes them, into pictures,
// each handed over the way THP stores it: with the scan's FF 00 stuffing
// removed, which is how the console's decoder expects it.
class JpegSplitter {
public:
  using Sink = std::function<bool(const std::vector<uint8_t>&)>;

  // No picture of a movie comes near this; a stream that runs past it without
  // ending one is not a JPEG stream, and holding it all would only use memory.
  static constexpr size_t kMaxPicture = size_t(16) << 20;

  // False when the bytes are not JPEGs, a picture runs past kMaxPicture, or
  // the sink returned false.
  bool Feed(const uint8_t* data, size_t size, const Sink& sink);
  // True when the stream ended between two pictures.
  bool Idle() const { return m_start == m_data.size(); }

private:
  // Splits off every whole picture in m_data from m_start on.
  bool Split(const Sink& sink);

  std::vector<uint8_t> m_data;
  size_t m_start = 0;     // the picture being parsed starts here; before it is done with
  size_t m_pos = 0;       // parsed up to here
  size_t m_scanStart = 0; // 0 while in the header segments
};

// Writes a video-only THP a frame at a time. `out` must be seekable: the header
// and the first frame record sizes only known at the end.
class ThpWriter {
public:
  ThpWriter(std::ostream& out, int width, int height, float fps);

  void Add(const std::vector<uint8_t>& jpeg);
  // False when there were no frames or the stream failed.
  bool Finish();
  int Frames() const { return m_frames; }

private:
  void Flush(uint32_t nextSize);

  std::ostream& m_out;
  int m_width;
  int m_height;
  float m_fps;
  std::vector<uint8_t> m_held; // the last frame, waiting for the size of the next
  uint32_t m_heldPrev = 0;
  uint32_t m_firstSize = 0;
  uint32_t m_lastSize = 0;
  uint32_t m_maxSize = 0;
  uint32_t m_total = 0;
  int m_frames = 0;
};

// The ffmpeg to use: MP_FFMPEG, one next to the executable, or the one on the
// path. Empty when none of them runs.
std::string FindFfmpeg();

// Decodes `mp4` (a file) with `ffmpeg` and writes it as THP to `thp`.
// `cancelled` is asked between frames.
bool ConvertMovie(const std::string& ffmpeg, const std::string& mp4, const std::string& thp, const MovieFormat& format,
                  const std::function<bool()>& cancelled, int& frames, std::string& error);

} // namespace PortRemastered
