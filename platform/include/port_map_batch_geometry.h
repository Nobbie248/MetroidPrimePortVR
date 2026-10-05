#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace PortMapBatch {
using Position = std::array<float, 3>;
using Color = std::array<uint8_t, 4>;
enum class Primitive { Triangles, Strip, Fan, Quads };

// Port: one aligned GX record for a fill vertex or an outline's quad corner.
// Endpoints stay in map space; Aurora expands lines after the eye projection.
struct Vertex {
  Position first;
  Position second;
  Color color;
  float signedWidth;
  float endpoint; // -1: fill, 0/1: first/second line endpoint.
};
static_assert(sizeof(Vertex) == 36 && offsetof(Vertex, second) == 12 &&
              offsetof(Vertex, color) == 24 && offsetof(Vertex, signedWidth) == 28);
inline constexpr size_t kDrawVertices = 65532; // Whole triangles, within GX's u16 count.

class Geometry {
  std::vector<Vertex> vertices_;
  uint8_t lineWidth_;
  bool lineWidthChanged_ = false;
  Color lastColor_{};
  bool hasColor_ = false;

public:
  explicit Geometry(uint8_t lineWidth = 6) : lineWidth_(lineWidth) {}
  const std::vector<Vertex>& Vertices() const { return vertices_; }
  void Reset(uint8_t lineWidth) {
    vertices_.clear();
    lineWidth_ = lineWidth;
    lineWidthChanged_ = false;
    hasColor_ = false;
  }
  uint8_t LineWidth() const { return lineWidth_; }
  bool LineWidthChanged() const { return lineWidthChanged_; }
  const Color& LastColor() const { return lastColor_; }
  bool HasColor() const { return hasColor_; }
  void SetColor(Color color) { lastColor_ = color; hasColor_ = true; }
  void SetLineWidth(float width) {
    lineWidth_ = static_cast<uint8_t>(std::clamp(width * 6.f, 0.f, 255.f));
    lineWidthChanged_ = true;
  }

  void AppendPolygon(Primitive primitive, std::span<const Position> points, Color color) {
    SetColor(color);
    const auto triangle = [&](size_t a, size_t b, size_t c) {
      for (size_t index : {a, b, c}) {
        vertices_.push_back({points[index], points[index], color, 0.f, -1.f});
      }
    };
    switch (primitive) {
    case Primitive::Triangles:
      for (size_t i = 0; i + 2 < points.size(); i += 3) { triangle(i, i + 1, i + 2); }
      break;
    case Primitive::Strip:
      for (size_t i = 2; i < points.size(); ++i) {
        if (i & 1) { triangle(i - 1, i - 2, i); }
        else { triangle(i - 2, i - 1, i); }
      }
      break;
    case Primitive::Fan:
      for (size_t i = 2; i < points.size(); ++i) { triangle(0, i - 1, i); }
      break;
    case Primitive::Quads:
      for (size_t i = 0; i + 3 < points.size(); i += 4) {
        triangle(i, i + 1, i + 2);
        triangle(i + 2, i + 3, i);
      }
      break;
    }
  }

  void AppendOutline(std::span<const Position> points, Color color) {
    SetColor(color);
    const float width = static_cast<float>(lineWidth_) / 6.f;
    // Same quad indices as Aurora's GX_LINES: 0,1,3,3,2,0.
    for (size_t i = 1; i < points.size(); ++i) {
      for (uint8_t corner : {0, 1, 3, 3, 2, 0}) {
        vertices_.push_back({points[i - 1], points[i], color,
                             corner & 1 ? width : -width, corner >= 2 ? 1.f : 0.f});
      }
    }
  }
};
} // namespace PortMapBatch
