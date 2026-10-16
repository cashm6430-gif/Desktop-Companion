#pragma once

#include <QString>

// One source of truth for the pet's right-click menu look, shared by PetWindow
// and the preview tool so the styling can be checked without a live tray.
//
// Note on the submenu arrow: a stylesheet that overrides QMenu::item makes Qt
// stop drawing the default submenu indicator, so QMenu::right-arrow is given an
// explicit image. tools/tray_preview.cpp renders this to a PNG to verify it.
namespace tray_style {

inline QString menuStyleSheet() {
    return QStringLiteral(
        "QMenu { background:#ffffff; border:1px solid #d9e0ea; border-radius:10px; padding:6px; }"
        "QMenu::item { padding:7px 34px 7px 16px; border-radius:7px; color:#2b3a4b; }"
        "QMenu::item:selected { background:#e8f0fb; color:#1f5fbf; }"
        "QMenu::item:disabled { color:#9aa7b4; }"
        "QMenu::separator { height:1px; background:#e7edf4; margin:5px 10px; }"
        "QMenu::right-arrow { width:14px; height:14px; margin-right:10px; "
        "  image:url(:/peticons/chevron-right.png); }");
}

} // namespace tray_style
