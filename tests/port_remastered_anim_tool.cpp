// Prints every animation PortRemasteredAnim reads from one Remastered CHPR, in the
// number format of build/mpr/anim/chpr_anim.py so the two can be diffed. It needs the
// developer's own game data, so it is not a ctest.
//
//   port_remastered_anim_tool <file.CHPR>              text dump of the animations
//   port_remastered_anim_tool --json <file.CHPR>       chpr_anim.py --json layout
//   port_remastered_anim_tool --skel <file.CHPR>       skeleton as JSON (bones, maps, inverse binds)
//   port_remastered_anim_tool --skin <file.CHPR> [frame]
//                                                      per animation, how far each joint's skin
//                                                      matrix is from identity at that frame
//   port_remastered_anim_tool --truncate <file.CHPR>   reads prefixes of the file; none may crash

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>

#include "port_remastered_anim.h"

using namespace PortRemasteredAnim;

namespace {

std::string AnimName(const Anim& a) { return a.name.empty() ? "anim" + std::to_string(a.id) : a.name; }

void PrintJson(const Character& chr) {
  std::printf("{");
  for (size_t i = 0; i < chr.anims.size(); ++i) {
    const Anim& a = chr.anims[i];
    std::printf("%s\"%s\": {\"fps\": null, \"frames\": %u, \"bones\": {", i ? ", " : "", AnimName(a).c_str(),
                a.frames);
    bool firstBone = true;
    for (size_t b = 0; b < a.bones.size(); ++b) {
      if (!a.tracked[b]) {
        continue;
      }
      std::printf("%s\"%zu\": [", firstBone ? "" : ", ", b);
      firstBone = false;
      for (uint32_t f = 0; f < a.frames; ++f) {
        const Key& k = a.bones[b][f];
        std::printf("%s[%.9g, %.9g, %.9g, %.9g, %.9g, %.9g, %.9g, %.9g, %.9g, %.9g]", f ? ", " : "",
                    double(k.rotation[0]), double(k.rotation[1]), double(k.rotation[2]), double(k.rotation[3]),
                    double(k.translation[0]), double(k.translation[1]), double(k.translation[2]),
                    double(k.scale[0]), double(k.scale[1]), double(k.scale[2]));
      }
      std::printf("]");
    }
    std::printf("}}");
  }
  std::printf("}\n");
}

void PrintSkeleton(const Character& chr) {
  std::printf("{\"error\": \"%s\", \"bones\": [", chr.skeletonError.c_str());
  for (size_t i = 0; i < chr.bones.size(); ++i) {
    const Bone& b = chr.bones[i];
    std::printf("%s{\"name\": \"%s\", \"parent\": %d, \"quat\": [%.9g, %.9g, %.9g, %.9g], \"trans\": [%.9g, %.9g, %.9g], "
                "\"scale\": [%.9g, %.9g, %.9g]}",
                i ? ", " : "", b.name.c_str(), b.parent, double(b.bind.rotation[0]), double(b.bind.rotation[1]),
                double(b.bind.rotation[2]), double(b.bind.rotation[3]), double(b.bind.translation[0]),
                double(b.bind.translation[1]), double(b.bind.translation[2]), double(b.bind.scale[0]),
                double(b.bind.scale[1]), double(b.bind.scale[2]));
  }
  std::printf("], \"anim_op_bone_to_bone\": [");
  for (size_t i = 0; i < chr.animBone.size(); ++i) {
    std::printf("%s%d", i ? ", " : "", chr.animBone[i]);
  }
  std::printf("], \"skin_joint_to_bone\": [");
  for (size_t i = 0; i < chr.jointBone.size(); ++i) {
    std::printf("%s%d", i ? ", " : "", chr.jointBone[i]);
  }
  std::printf("], \"skin_inverse_bind\": [");
  for (size_t i = 0; i < chr.inverseBind.size(); ++i) {
    std::printf("%s[", i ? ", " : "");
    for (size_t k = 0; k < 12; ++k) {
      std::printf("%s%.9g", k ? ", " : "", double(chr.inverseBind[i][k]));
    }
    std::printf("]");
  }
  std::printf("]}\n");
}

// Largest |m - identity| over the joints, and which joint.
void PrintSkin(const Character& chr, uint32_t frame) {
  static const float kIdentity[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
  std::printf("bones=%zu joints=%zu%s%s\n", chr.bones.size(), chr.jointBone.size(),
              chr.skeletonError.empty() ? "" : " skeleton error: ", chr.skeletonError.c_str());
  for (const Anim& a : chr.anims) {
    std::vector<std::array<float, 12>> pose;
    if (!SkinPose(chr, a, frame, pose)) {
      std::printf("%s: no pose at frame %u\n", AnimName(a).c_str(), frame);
      continue;
    }
    double worst = 0.0, worstRot = 0.0, worstTrans = 0.0;
    size_t worstJoint = 0;
    for (size_t k = 0; k < pose.size(); ++k) {
      double dev = 0.0, rot = 0.0, trans = 0.0;
      for (size_t i = 0; i < 12; ++i) {
        const double e = std::fabs(double(pose[k][i]) - double(kIdentity[i]));
        dev = std::max(dev, e);
        (i % 4 == 3 ? trans : rot) = std::max(i % 4 == 3 ? trans : rot, e);
      }
      if (dev >= worst) {
        worst = dev;
        worstRot = rot;
        worstTrans = trans;
        worstJoint = k;
      }
    }
    std::printf("%s frame %u: %zu joints, max |skin - I| = %.6g (rot/scale %.6g, trans %.6g) at joint %zu\n",
                AnimName(a).c_str(), frame, pose.size(), worst, worstRot, worstTrans, worstJoint);
    // The scale the skin matrices carry over the whole animation (column lengths).
    double lo = 1.0, hi = 1.0;
    std::vector<std::array<double, 2>> joint(pose.size(), {1e9, -1e9});
    for (uint32_t f = 0; f < a.frames; ++f) {
      if (!SkinPose(chr, a, f, pose)) {
        continue;
      }
      for (size_t k = 0; k < pose.size(); ++k) {
        const auto& m = pose[k];
        for (int c = 0; c < 3; ++c) {
          const double len = std::sqrt(double(m[c]) * m[c] + double(m[4 + c]) * m[4 + c] + double(m[8 + c]) * m[8 + c]);
          lo = std::min(lo, len);
          hi = std::max(hi, len);
          joint[k][0] = std::min(joint[k][0], len);
          joint[k][1] = std::max(joint[k][1], len);
        }
      }
    }
    std::printf("  scale over %u frames: %.4g..%.4g\n", a.frames, lo, hi);
    for (size_t k = 0; k < joint.size(); ++k) {
      if (std::fabs(joint[k][0] - 1.0) > 1e-3 || std::fabs(joint[k][1] - 1.0) > 1e-3) {
        std::printf("    joint %zu: %.4g..%.4g\n", k, joint[k][0], joint[k][1]);
      }
    }
  }
}

// Prefixes of the file at many sizes (and the file with one byte flipped): reading must
// either succeed or fail with a message, never crash (build with -fsanitize to check).
void Truncate(const std::vector<uint8_t>& data) {
  size_t ok = 0, failed = 0;
  auto attempt = [&](const std::vector<uint8_t>& bytes) {
    Character chr;
    std::string error;
    if (ReadCharacter(bytes, chr, error)) {
      ++ok;
      for (const Anim& a : chr.anims) {
        std::vector<std::array<float, 12>> pose;
        SkinPose(chr, a, 0, pose);
      }
    } else {
      ++failed;
    }
  };
  std::vector<size_t> sizes;
  for (size_t n = 0; n < std::min<size_t>(data.size(), 0x400); n += 7) {
    sizes.push_back(n);
  }
  for (size_t n = 0; n <= 400; ++n) {
    sizes.push_back(data.size() * n / 400);
  }
  for (size_t n = 1; n <= 64 && n < data.size(); ++n) {
    sizes.push_back(data.size() - n);
  }
  for (size_t n : sizes) {
    attempt(std::vector<uint8_t>(data.begin(), data.begin() + std::ptrdiff_t(std::min(n, data.size()))));
  }
  // Damage: a byte flipped every so often, so counts and offsets go wrong.
  std::srand(1);
  for (int round = 0; round < 300; ++round) {
    std::vector<uint8_t> copy = data;
    for (int k = 0; k < 4; ++k) {
      copy[size_t(std::rand()) % copy.size()] ^= uint8_t(1 << (std::rand() % 8));
    }
    attempt(copy);
  }
  std::printf("%zu attempts: %zu read, %zu failed cleanly\n", ok + failed, ok, failed);
}

}  // namespace

int main(int argc, char** argv) {
  enum { kText, kJson, kSkel, kSkin, kTruncate } mode = kText;
  int arg = 1;
  if (argc > 2 && std::strncmp(argv[1], "--", 2) == 0) {
    mode = !std::strcmp(argv[1], "--json")       ? kJson
           : !std::strcmp(argv[1], "--skel")     ? kSkel
           : !std::strcmp(argv[1], "--skin")     ? kSkin
           : !std::strcmp(argv[1], "--truncate") ? kTruncate
                                                 : kText;
    arg = 2;
  }
  if (argc <= arg) {
    std::fprintf(stderr, "usage: %s [--json|--skel|--skin [frame]|--truncate] <file.CHPR>\n", argv[0]);
    return 2;
  }
  std::ifstream in(argv[arg], std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "could not open '%s'\n", argv[arg]);
    return 1;
  }
  const std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (mode == kTruncate) {
    Truncate(data);
    return 0;
  }
  Character chr;
  std::string error;
  if (!ReadCharacter(data, chr, error)) {
    std::fprintf(stderr, "%s: %s\n", argv[arg], error.c_str());
    return 1;
  }
  if (mode == kJson) {
    PrintJson(chr);
    return 0;
  }
  if (mode == kSkel) {
    PrintSkeleton(chr);
    return 0;
  }
  if (mode == kSkin) {
    PrintSkin(chr, argc > arg + 1 ? uint32_t(std::strtoul(argv[arg + 1], nullptr, 10)) : 0);
    return 0;
  }
  std::printf("skinned model:");
  for (uint8_t b : chr.skinnedModel) {
    std::printf(" %02x", b);
  }
  std::printf("\n");
  for (const auto& a : chr.anims) {
    std::printf("%s fps=%g frames=%u bones=%zu\n", a.name.c_str(), double(a.fps), a.frames, a.bones.size());
    for (size_t b = 0; b < a.bones.size(); ++b) {
      for (uint32_t f = 0; f < a.frames; ++f) {
        const auto& k = a.bones[b][f];
        std::printf("  bone %zu f%02u rot=(%.6f %.6f %.6f %.6f) trans=(%g %g %g) scale=(%g %g %g)\n", b, f,
                    double(k.rotation[0]), double(k.rotation[1]), double(k.rotation[2]), double(k.rotation[3]),
                    double(k.translation[0]), double(k.translation[1]), double(k.translation[2]),
                    double(k.scale[0]), double(k.scale[1]), double(k.scale[2]));
      }
    }
  }
  return 0;
}
