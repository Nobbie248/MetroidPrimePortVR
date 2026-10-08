// SPDX-License-Identifier: GPL-3.0-or-later
//
// PrimedGun's launcher look, from DolphinQt/MainWindow.cpp ConnectStack, and
// the dark palette its Dolphin build ran under (DolphinQt/Settings.cpp). Added
// here: the "not active yet" tag and a disabled button colour, for the Setup
// buttons that wait while the game runs.

#pragma once

#include <QColor>
#include <QPalette>
#include <QString>

namespace PrimedGunLauncher::Style {

inline const QString kPage = QStringLiteral(R"(
    QWidget { background: #101215; color: #edf0f4; font-family: Consolas, monospace; font-size: 12px; }
    QFrame#PrimedGunPanel, QTabWidget::pane { background: #121519; border: 1px solid #353a43; border-radius: 5px; }
    QLabel#PrimedGunTitle, QLabel#PrimedGunSection { color: #f0a12a; }
    QLabel#PrimedGunNotice { color: #f0a12a; font-size: 14px; font-weight: bold; }
    QLabel#PrimedGunMuted { color: #858b94; }
    QLabel#PrimedGunTag { color: #6d737c; font-style: italic; }
    QLabel#PrimedGunBad { color: #ff5b45; }
    QLabel#PrimedGunGood { color: #38d86f; }
    QPushButton { background: #242a33; border: 1px solid #242a33; border-radius: 4px; color: #edf0f4; padding: 5px 10px; }
    QPushButton:hover { background: #2d3440; border-color: #3b4553; }
    QPushButton:pressed { background: #303844; border-color: #c2802e; color: #f0a12a; }
    QPushButton:disabled { color: #666d76; }
    QTabBar::tab { background: #20262d; color: #edf0f4; padding: 5px 12px; border-top-left-radius: 4px; border-top-right-radius: 4px; margin-right: 2px; }
    QTabBar::tab:selected { background: #34404b; }
    QTabBar::tab:hover { background: #2b333d; }
    QCheckBox::indicator { width: 18px; height: 18px; border-radius: 4px; background: #242a33; }
    QCheckBox::indicator:checked { background: #d38a2d; }
    QRadioButton::indicator { width: 18px; height: 18px; border-radius: 9px; background: #20242b; }
    QRadioButton::indicator:checked { background: #f0a12a; }
    QSlider::groove:horizontal { height: 22px; border-radius: 4px; background: #202329; }
    QSlider::handle:horizontal { width: 12px; margin: 2px 0; border-radius: 5px; background: #d38a2d; }
    QDoubleSpinBox { background: #202329; border: 1px solid #202329; color: #edf0f4; border-radius: 4px; padding: 4px; }
    QScrollArea { border: 1px solid #353a43; border-radius: 5px; background: #121519; }
  )");

inline const QString kGameButton = QStringLiteral(R"(
    QPushButton {
      background-color: #242a33;
      border: 1px solid #343c49;
      border-radius: 4px;
      color: #edf0f4;
      min-height: 24px;
      padding: 4px 12px;
    }
    QPushButton:hover {
      background-color: #2d3440;
      border-color: #c2802e;
      color: #ffffff;
    }
    QPushButton:pressed {
      background-color: #303844;
      border-color: #c2802e;
      color: #c2802e;
    }
    QPushButton:disabled {
      background-color: #1f2126;
      border-color: #1f2126;
      color: #666d76;
    }
  )");

inline const QString kPrimaryGameButton = kGameButton + QStringLiteral(R"(
    QPushButton {
      background-color: #263342;
      border-color: #3e4c5f;
    }
  )");

inline QPalette DarkPalette(QPalette palette) {
  palette.setColor(QPalette::Window, QColor(19, 21, 24));
  palette.setColor(QPalette::WindowText, QColor(218, 222, 226));
  palette.setColor(QPalette::Base, QColor(19, 21, 24));
  palette.setColor(QPalette::AlternateBase, QColor(31, 33, 38));
  palette.setColor(QPalette::PlaceholderText, QColor(125, 132, 139));
  palette.setColor(QPalette::Text, QColor(218, 222, 226));
  palette.setColor(QPalette::Button, QColor(31, 33, 38));
  palette.setColor(QPalette::ButtonText, QColor(238, 241, 244));
  palette.setColor(QPalette::BrightText, QColor(194, 128, 46));
  palette.setColor(QPalette::Highlight, QColor(194, 128, 46));
  palette.setColor(QPalette::HighlightedText, QColor(19, 21, 24));
  palette.setColor(QPalette::Link, QColor(194, 128, 46));
  palette.setColor(QPalette::LinkVisited, QColor(194, 128, 46));
  return palette;
}

} // namespace PrimedGunLauncher::Style
