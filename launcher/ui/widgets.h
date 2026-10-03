// SPDX-License-Identifier: GPL-3.0-or-later
//
// The launcher's building blocks, after PrimedGun's ConnectStack helpers
// (DolphinQt/MainWindow.cpp): the label / slider / spin box / -+ row, the
// image that scales with the window, and the filter that keeps the mouse
// wheel from changing a setting while the page scrolls.

#pragma once

#include <QLabel>
#include <QObject>
#include <QPixmap>
#include <QWidget>

#include <functional>

class QDoubleSpinBox;
class QSlider;

namespace PrimedGunLauncher {

class WheelBlocker final : public QObject {
public:
  static WheelBlocker* Instance();

protected:
  bool eventFilter(QObject* watched, QEvent* event) override;

private:
  using QObject::QObject;
};

class FloatRow final : public QWidget {
public:
  // `onChange` runs for every edit by the player, never for SetValue.
  FloatRow(const QString& label, double min, double max, double step,
           std::function<void(float)> onChange, QWidget* tag, QWidget* parent = nullptr);
  void SetValue(double value);

private:
  QSlider* m_slider = nullptr;
  QDoubleSpinBox* m_spin = nullptr;
  double m_step = 1.0;
};

class ScaledImageLabel final : public QLabel {
public:
  explicit ScaledImageLabel(QWidget* parent = nullptr);
  void SetSourcePixmap(const QPixmap& pixmap);

protected:
  void resizeEvent(QResizeEvent* event) override;

private:
  void UpdateScaledPixmap();
  QPixmap m_source;
};

} // namespace PrimedGunLauncher
