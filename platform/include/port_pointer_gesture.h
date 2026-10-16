#pragma once

// Mouse and touch gestures for pointer-driven menus and the map: hover, click
// or tap (with double clicks), drag past a slop, two-finger pan and pinch,
// wheel, and back (right click). Positions are window points; times seconds.
// Header-only and free of SDL so tests/port_pointer.cpp can drive it.
//
// One-shot results (click, back) and accumulated motion (drag, pan, pinch,
// wheel) are taken by whichever screen handles them. Anything not taken
// within kExpire is dropped, so a click in a screen without pointer support
// can't fire later in another.

#include <cmath>
#include <cstdint>

namespace PortPointer {

enum class Device : uint8_t { None, Mouse, Touch };

enum Button : uint8_t {
  kButtonLeft = 1,
  kButtonRight = 2,
  kButtonMiddle = 4,
};

struct Click {
  float x = 0.f, y = 0.f;
  Device device = Device::None;
  uint8_t button = kButtonLeft;
  int count = 1;  // 2 for a double click / double tap
};

class Recognizer {
public:
  static constexpr float kMouseSlop = 4.f;    // points
  static constexpr float kTouchSlop = 14.f;   // points
  static constexpr float kDoubleTime = 0.4f;  // seconds
  static constexpr float kDoubleDist = 16.f;  // points
  static constexpr float kExpire = 0.25f;     // seconds an untaken result is kept
  static constexpr float kPi = 3.14159265f;

  // Position of the last pointer seen, and whether it moved (or a finger
  // landed) since ClearMoved. An idle cursor doesn't move, so it never steals
  // the selection from the keyboard or pad.
  float X() const { return mX; }
  float Y() const { return mY; }
  bool HasPosition() const { return mHasPosition; }
  bool Moved() const { return mMoved; }
  void ClearMoved() { mMoved = false; }
  Device LastDevice() const { return mLast; }
  // A drag (any button or finger) is in progress past its slop.
  bool Dragging() const { return mDragging || mFingerCount >= 2; }
  // A button or finger is down.
  bool Pressed() const { return mButtons != 0 || mFingerCount > 0; }
  // Where the current (or last) press started.
  float PressX() const { return mPressX; }
  float PressY() const { return mPressY; }
  int FingerCount() const { return mFingerCount; }
  uint8_t Buttons() const { return mButtons; }

  // ---- mouse
  void MouseMove(float x, float y, float t) {
    if (mHasPosition && x == mX && y == mY && mLast == Device::Mouse) {
      return;
    }
    if (mButtons != 0) {
      const float dx = x - mX, dy = y - mY;
      if (!mDragging && Dist(x, y, mPressX, mPressY) > kMouseSlop) {
        mDragging = true;
        // The motion inside the slop counts, so the drag doesn't lag the cursor.
        AddDrag(x - mPressX, y - mPressY, mButtons, t);
      } else if (mDragging) {
        AddDrag(dx, dy, mButtons, t);
      }
    }
    SetPosition(x, y, Device::Mouse);
  }

  void MouseButton(uint8_t button, bool down, float x, float y, float t) {
    SetPosition(x, y, Device::Mouse);
    if (down) {
      if (mButtons == 0) {
        mPressX = x;
        mPressY = y;
        mDragging = false;
      }
      mButtons |= button;
      return;
    }
    if ((mButtons & button) == 0) {
      return;
    }
    mButtons &= static_cast< uint8_t >(~button);
    if (mButtons != 0) {
      return;  // a chord: the drag (or click) ends with the last button
    }
    const bool wasDrag = mDragging;
    mDragging = false;
    if (wasDrag) {
      return;
    }
    if (button == kButtonRight) {
      PostBack(t);
    } else if (button == kButtonLeft) {
      PostClick(x, y, Device::Mouse, button, t);
    }
  }

  // Wheel notches, positive away from the user (scroll up / zoom in).
  void Wheel(float notches, float t) {
    if (notches == 0.f) {
      return;
    }
    mLast = Device::Mouse;
    mWheel += notches;
    mWheelTime = Stamp(mWheelTime, t);
  }

  // ---- touch (ids are SDL finger ids; only the first two fingers count)
  void FingerDown(uint64_t id, float x, float y, float t) {
    SetPosition(x, y, Device::Touch);
    mMoved = true;
    const int slot = mFingerCount < 2 ? mFingerCount : -1;
    if (slot < 0) {
      mTapCancelled = true;
      return;
    }
    mFingers[slot] = {id, x, y};
    ++mFingerCount;
    if (mFingerCount == 1) {
      mPressX = x;
      mPressY = y;
      mDragging = false;
      mTapCancelled = false;
    } else {
      // A second finger turns a tap or one-finger drag into a pan/pinch.
      mTapCancelled = true;
      mDragging = false;
      mPrevSpan = Span();
      mPrevAngle = Angle();
      mPrevCx = 0.5f * (mFingers[0].x + mFingers[1].x);
      mPrevCy = 0.5f * (mFingers[0].y + mFingers[1].y);
    }
  }

  void FingerMove(uint64_t id, float x, float y, float t) {
    const int slot = Slot(id);
    if (slot < 0) {
      return;
    }
    Finger& f = mFingers[slot];
    const float dx = x - f.x, dy = y - f.y;
    f.x = x;
    f.y = y;
    if (mFingerCount == 1) {
      SetPosition(x, y, Device::Touch);
      if (mTapCancelled) {
        return;  // the finger left over from a pinch doesn't drag
      }
      if (!mDragging && Dist(x, y, mPressX, mPressY) > kTouchSlop) {
        mDragging = true;
        AddDrag(x - mPressX, y - mPressY, kButtonLeft, t);
      } else if (mDragging) {
        AddDrag(dx, dy, kButtonLeft, t);
      }
      return;
    }
    const float cx = 0.5f * (mFingers[0].x + mFingers[1].x);
    const float cy = 0.5f * (mFingers[0].y + mFingers[1].y);
    mPanX += cx - mPrevCx;
    mPanY += cy - mPrevCy;
    mPanTime = Stamp(mPanTime, t);
    mPrevCx = cx;
    mPrevCy = cy;
    const float span = Span();
    if (mPrevSpan > 1.f && span > 1.f) {
      mPinch *= span / mPrevSpan;
      mPinchTime = Stamp(mPinchTime, t);
    }
    mPrevSpan = span;
    const float angle = Angle();
    float turn = angle - mPrevAngle;
    if (turn > kPi) {
      turn -= 2.f * kPi;
    } else if (turn <= -kPi) {
      turn += 2.f * kPi;
    }
    mTwist += turn;
    mTwistTime = Stamp(mTwistTime, t);
    mPrevAngle = angle;
    mLast = Device::Touch;
  }

  void FingerUp(uint64_t id, float x, float y, float t) {
    const int slot = Slot(id);
    if (slot < 0) {
      return;
    }
    if (slot == 0 && mFingerCount == 2) {
      mFingers[0] = mFingers[1];
    }
    --mFingerCount;
    if (mFingerCount > 0) {
      return;
    }
    const bool tap = !mDragging && !mTapCancelled;
    mDragging = false;
    mTapCancelled = false;
    SetPosition(x, y, Device::Touch);
    if (tap) {
      PostClick(x, y, Device::Touch, kButtonLeft, t);
    }
  }

  // A finger gesture the system took over (SDL_EVENT_FINGER_CANCELED).
  void CancelTouches() {
    mFingerCount = 0;
    mDragging = false;
    mTapCancelled = false;
  }

  // Esc, Android back, a scripted back.
  void Back(float t) { PostBack(t); }

  // A key or pad button: the pointer is no longer the last-used device.
  void NoteOtherDevice() { mLast = Device::None; }

  // ---- results
  bool TakeClick(Click& out) {
    if (!mHasClick) {
      return false;
    }
    out = mClick;
    mHasClick = false;
    return true;
  }
  bool PeekClick(Click& out) const {
    if (mHasClick) {
      out = mClick;
    }
    return mHasClick;
  }
  bool TakeBack() {
    const bool back = mHasBack;
    mHasBack = false;
    return back;
  }
  // Drag motion since the last take, and the buttons held during it (a touch
  // drag reports kButtonLeft).
  bool TakeDrag(float& dx, float& dy, uint8_t& buttons) {
    dx = mDragX;
    dy = mDragY;
    buttons = mDragButtons;
    const bool any = mDragX != 0.f || mDragY != 0.f;
    mDragX = mDragY = 0.f;
    mDragButtons = 0;
    mDragTime = -1.f;
    return any;
  }
  bool TakePan(float& dx, float& dy) {
    dx = mPanX;
    dy = mPanY;
    const bool any = mPanX != 0.f || mPanY != 0.f;
    mPanX = mPanY = 0.f;
    mPanTime = -1.f;
    return any;
  }
  // Multiplicative pinch since the last take: >1 fingers spread (zoom in).
  float TakePinch() {
    const float p = mPinch;
    mPinch = 1.f;
    mPinchTime = -1.f;
    return p;
  }
  // Two-finger twist since the last take, in radians, positive clockwise on
  // screen (y down).
  float TakeTwist() {
    const float a = mTwist;
    mTwist = 0.f;
    mTwistTime = -1.f;
    return a;
  }
  float TakeWheel() {
    const float w = mWheel;
    mWheel = 0.f;
    mWheelTime = -1.f;
    return w;
  }

  // Drop results nobody took within kExpire.
  void Expire(float t) {
    if (mHasClick && t - mClickTime > kExpire) mHasClick = false;
    if (mHasBack && t - mBackTime > kExpire) mHasBack = false;
    if (mDragTime >= 0.f && t - mDragTime > kExpire) {
      mDragX = mDragY = 0.f;
      mDragButtons = 0;
      mDragTime = -1.f;
    }
    if (mPanTime >= 0.f && t - mPanTime > kExpire) {
      mPanX = mPanY = 0.f;
      mPanTime = -1.f;
    }
    if (mPinchTime >= 0.f && t - mPinchTime > kExpire) {
      mPinch = 1.f;
      mPinchTime = -1.f;
    }
    if (mTwistTime >= 0.f && t - mTwistTime > kExpire) {
      mTwist = 0.f;
      mTwistTime = -1.f;
    }
    if (mWheelTime >= 0.f && t - mWheelTime > kExpire) {
      mWheel = 0.f;
      mWheelTime = -1.f;
    }
  }

  // Forget presses and pending results (focus loss, a screen change).
  void Reset() {
    mButtons = 0;
    mFingerCount = 0;
    mDragging = false;
    mTapCancelled = false;
    mHasClick = mHasBack = false;
    mDragX = mDragY = mPanX = mPanY = mWheel = mTwist = 0.f;
    mDragButtons = 0;
    mPinch = 1.f;
    mDragTime = mPanTime = mPinchTime = mWheelTime = mTwistTime = -1.f;
  }

private:
  struct Finger {
    uint64_t id = 0;
    float x = 0.f, y = 0.f;
  };

  static float Dist(float ax, float ay, float bx, float by) { return std::hypot(ax - bx, ay - by); }
  // The time of the first untaken contribution: expiry counts from it.
  static float Stamp(float stamp, float t) { return stamp >= 0.f ? stamp : t; }

  int Slot(uint64_t id) const {
    for (int i = 0; i < mFingerCount; ++i) {
      if (mFingers[i].id == id) return i;
    }
    return -1;
  }
  float Angle() const {
    return std::atan2(mFingers[1].y - mFingers[0].y, mFingers[1].x - mFingers[0].x);
  }
  float Span() const {
    return Dist(mFingers[0].x, mFingers[0].y, mFingers[1].x, mFingers[1].y);
  }
  void SetPosition(float x, float y, Device device) {
    if (!mHasPosition || x != mX || y != mY || mLast != device) {
      mMoved = true;
    }
    mX = x;
    mY = y;
    mHasPosition = true;
    mLast = device;
  }
  void AddDrag(float dx, float dy, uint8_t buttons, float t) {
    mDragX += dx;
    mDragY += dy;
    mDragButtons |= buttons;
    mDragTime = Stamp(mDragTime, t);
  }
  void PostClick(float x, float y, Device device, uint8_t button, float t) {
    int count = 1;
    if (mLastClickTime >= 0.f && t - mLastClickTime <= kDoubleTime && mLastClickDevice == device &&
        Dist(x, y, mLastClickX, mLastClickY) <= kDoubleDist && mLastClickCount == 1) {
      count = 2;
    }
    mClick = {x, y, device, button, count};
    mHasClick = true;
    mClickTime = t;
    mLastClickTime = t;
    mLastClickX = x;
    mLastClickY = y;
    mLastClickDevice = device;
    mLastClickCount = count;
  }
  void PostBack(float t) {
    mHasBack = true;
    mBackTime = t;
  }

  float mX = 0.f, mY = 0.f;
  bool mHasPosition = false;
  bool mMoved = false;
  Device mLast = Device::None;

  uint8_t mButtons = 0;
  float mPressX = 0.f, mPressY = 0.f;
  bool mDragging = false;

  Finger mFingers[2];
  int mFingerCount = 0;
  bool mTapCancelled = false;
  float mPrevSpan = 0.f, mPrevAngle = 0.f, mPrevCx = 0.f, mPrevCy = 0.f;

  Click mClick;
  bool mHasClick = false;
  float mClickTime = 0.f;
  float mLastClickTime = -1.f, mLastClickX = 0.f, mLastClickY = 0.f;
  Device mLastClickDevice = Device::None;
  int mLastClickCount = 0;

  bool mHasBack = false;
  float mBackTime = 0.f;

  float mDragX = 0.f, mDragY = 0.f;
  uint8_t mDragButtons = 0;
  float mDragTime = -1.f;
  float mPanX = 0.f, mPanY = 0.f, mPanTime = -1.f;
  float mPinch = 1.f, mPinchTime = -1.f;
  float mTwist = 0.f, mTwistTime = -1.f;
  float mWheel = 0.f, mWheelTime = -1.f;
};

} // namespace PortPointer
