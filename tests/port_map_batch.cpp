#include "port_map_batch_geometry.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {
void Check(bool condition, const char* message) {
  if (!condition) { std::fprintf(stderr, "map batch: %s\n", message); std::abort(); }
}
}

int main() {
  using namespace PortMapBatch;
  const Position points[] = {{0, 0, 0}, {1, 0, 0}, {2, 0, 0}, {3, 0, 0}, {4, 0, 0}};
  const Color fill{10, 20, 30, 128}, line{200, 100, 50, 64};
  for (auto primitive : {Primitive::Triangles, Primitive::Strip, Primitive::Fan, Primitive::Quads}) {
    Geometry batch;
    batch.AppendPolygon(primitive, points, fill);
    const std::vector<int> expected = primitive == Primitive::Triangles ? std::vector<int>{0, 1, 2}
        : primitive == Primitive::Strip ? std::vector<int>{0, 1, 2, 2, 1, 3, 2, 3, 4}
        : primitive == Primitive::Fan ? std::vector<int>{0, 1, 2, 0, 2, 3, 0, 3, 4}
        : std::vector<int>{0, 1, 2, 2, 3, 0};
    Check(batch.Vertices().size() == expected.size(), "polygon triangle count");
    for (size_t i = 0; i < expected.size(); ++i) {
      const Vertex& vertex = batch.Vertices()[i];
      Check(vertex.first == points[expected[i]] && vertex.second == vertex.first,
            "topology matches GX winding");
      Check(vertex.endpoint == -1.f && vertex.signedWidth == 0.f && vertex.color == fill,
            "fill metadata and alpha");
    }
  }
  Geometry ordered(9); // GX width in sixths of a logical pixel.
  ordered.AppendPolygon(Primitive::Triangles, std::span(points, 3), fill);
  ordered.AppendOutline(std::span(points, 3), line);
  ordered.AppendPolygon(Primitive::Triangles, std::span(points + 2, 3), fill);
  Check(ordered.Vertices().size() == 18, "fill outline fill sequence length");
  const int corners[] = {0, 1, 3, 3, 2, 0};
  for (int segment = 0; segment < 2; ++segment) {
    for (int i = 0; i < 6; ++i) {
      const Vertex& vertex = ordered.Vertices()[3 + segment * 6 + i];
      Check(vertex.first == points[segment] && vertex.second == points[segment + 1],
            "each outline segment keeps both endpoints");
      Check(vertex.endpoint == (corners[i] >= 2 ? 1.f : 0.f) &&
                vertex.signedWidth == (corners[i] & 1 ? 1.5f : -1.5f) && vertex.color == line,
            "line quad matches original expansion, colour, and inherited width");
    }
  }
  Check(ordered.Vertices()[15].endpoint == -1.f && ordered.Vertices()[15].color == fill,
        "second fill stays after outlines");
  ordered.SetLineWidth(2.29f);
  Check(ordered.LineWidth() == 13 && ordered.LineWidthChanged(), "width quantizes like GX");
  ordered.Reset(6);
  Check(ordered.Vertices().empty() && !ordered.LineWidthChanged() && !ordered.HasColor(),
        "flush reset forgets state changes");
  ordered.AppendOutline(std::span(points, 2), line);
  ordered.AppendOutline(std::span(points + 3, 2), fill);
  Check(ordered.Vertices().size() == 12 && ordered.Vertices()[6].first == points[3],
        "separate strips never connect endpoints");
  ordered.Reset(6);
  for (size_t i = 0; i < kDrawVertices / 3 + 2; ++i) {
    ordered.AppendPolygon(Primitive::Triangles, std::span(points, 3), fill);
  }
  Check(kDrawVertices % 3 == 0 && kDrawVertices <= 65535 &&
            ordered.Vertices().size() == kDrawVertices + 6 &&
            ordered.Vertices()[kDrawVertices].first == points[0],
        "draw count boundary preserves whole triangles and order");
  std::puts("minimap geometry checks passed");
}
