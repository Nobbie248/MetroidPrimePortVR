#pragma once

#include <string_view>
#include <vector>

#include <webgpu/webgpu_cpp.h>

namespace aurora::webgpu::gpu_prof {

// Runtime-switched per-pass GPU times for non-Tracy builds (Tracy builds always record). Only
// passes with timestampWrites from pass_writes() are timed; off costs nothing per pass.
struct Entry {
  const char* name; // static lifetime
  float msPerFrame;
  float passesPerFrame;
};
struct Result {
  std::vector<Entry> entries; // by msPerFrame, descending; averages over the last 60 frames
  float totalMs = 0.f;        // sum of all passes
  float spanMs = 0.f;         // first begin to last end
  uint32_t frames = 0;
  bool supported = false;
};
void set_enabled(bool on);
bool supported();
Result results();

void initialize();
void shutdown();

void frame_begin(const wgpu::CommandEncoder& encoder);
void frame_end(const wgpu::CommandEncoder& encoder);
void after_submit();
const wgpu::PassTimestampWrites* pass_writes(std::string_view name);

class Zone {
public:
  Zone(const wgpu::CommandEncoder& encoder, std::string_view name);
  ~Zone();
  Zone(const Zone&) = delete;
  Zone& operator=(const Zone&) = delete;

private:
  const wgpu::CommandEncoder* m_encoder = nullptr;
  uint32_t m_endQuery = 0;
};

} // namespace aurora::webgpu::gpu_prof
