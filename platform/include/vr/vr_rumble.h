// SPDX-License-Identifier: GPL-3.0-or-later
//
// The headset controllers' rumble, by where it comes from. While the VR pad
// owns port 0, CRumbleManager plays the game's rumble effects on three of its
// generator's ports by the part of Samus they come from, and each port pulses
// on its own: port 0 the body (landings, damage, bumps, camera shakes), port 1
// the arm cannon (charge, missiles, the recoil of charged shots and combos, the
// Wave Buster's lock), port 2 the grapple arm (firing, swinging). vr_pad.cpp
// turns each port's motor into a channel here, and the split target
// (vr_rumble_hand=split) plays the arm cannon on the gun hand only, the grapple
// arm on the off hand only and the body on both. Any other target plays the
// strongest channel on its hand(s), as one pad would.

#pragma once

#include "vr/vr_settings.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace PortVr {

// In the generator's port order (CRumbleManager).
enum class RumbleChannel : uint8_t { Body, Gun, OffHand };
inline constexpr std::size_t kRumbleChannelCount = 3;

using RumbleLevels = std::array<float, kRumbleChannelCount>;

// Each hand's amplitude (0 left, 1 right) for the channels' levels, before the
// intensity setting. The gun hand is the right one when `right_handed`.
inline std::array<float, 2> RumbleHandAmplitudes(RumbleHand target, bool right_handed,
                                                 const RumbleLevels& levels) noexcept {
    const float body = levels[static_cast<std::size_t>(RumbleChannel::Body)];
    const float gun = levels[static_cast<std::size_t>(RumbleChannel::Gun)];
    const float off_hand = levels[static_cast<std::size_t>(RumbleChannel::OffHand)];
    std::array<float, 2> hands{};
    if (target == RumbleHand::Split) {
        const std::size_t gun_hand = right_handed ? 1 : 0;
        hands[gun_hand] = std::max(body, gun);
        hands[1 - gun_hand] = std::max(body, off_hand);
        return hands;
    }
    const float all = std::max({body, gun, off_hand});
    hands[0] = target == RumbleHand::Both || target == RumbleHand::Left ? all : 0.0f;
    hands[1] = target == RumbleHand::Both || target == RumbleHand::Right ? all : 0.0f;
    return hands;
}

} // namespace PortVr
