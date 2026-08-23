#!/usr/bin/env python3
"""
Lucid Dock: a PyQt6 animated desktop dock for GNOME/Ubuntu.

Features:
- Glass-like dock panel with rounded corners and transparency.
- Hover magnification for icons.
- GNOME pinned apps via `gsettings get org.gnome.shell favorite-apps`.
- Running GUI app detection via psutil.
- Running app indicator dot under icons.
- First icon triggers GNOME Show Applications via D-Bus eval.
- X11 EWMH hints for dock type and struts.

Dependencies:
- PyQt6
- psutil
"""

from __future__ import annotations

import ast
import concurrent.futures
import os
import re
import shlex
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Set

import psutil
from PyQt6.QtCore import (
    QEasingCurve,
    QEvent,
    QPoint,
    QRect,
    QSize,
    Qt,
    QTimer,
    QVariantAnimation,
    pyqtSignal,
)
from PyQt6.QtGui import (
    QColor,
    QEnterEvent,
    QFont,
    QIcon,
    QLinearGradient,
    QPainter,
    QPainterPath,
    QPen,
    QPixmap,
)
from PyQt6.QtWidgets import (
    QApplication,
    QGraphicsDropShadowEffect,
    QHBoxLayout,
    QPushButton,
    QSizePolicy,
    QVBoxLayout,
    QWidget,
)


DESKTOP_DIRS = [
    Path.home() / ".local/share/applications",
    Path("/usr/local/share/applications"),
    Path("/usr/share/applications"),
]

ICON_DIRS = [
    Path.home() / ".local/share/icons",
    Path("/usr/share/icons/hicolor"),
    Path("/usr/share/icons/Adwaita"),
    Path("/usr/share/pixmaps"),
]


@dataclass
class DesktopEntry:
    desktop_id: str
    name: str
    exec_cmd: str
    icon_name: str
    startup_wm_class: str
    path: Path


def run_command(args: List[str]) -> str:
    try:
        out = subprocess.check_output(args, text=True, stderr=subprocess.DEVNULL)
        return out.strip()
    except (subprocess.CalledProcessError, FileNotFoundError):
        return ""


def parse_desktop_file(path: Path) -> Optional[DesktopEntry]:
    if not path.exists() or path.suffix != ".desktop":
        return None

    in_entry = False
    fields: Dict[str, str] = {}

    try:
        text = path.read_text(encoding="utf-8", errors="ignore")
    except OSError:
        return None

    for raw_line in text.splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("[") and line.endswith("]"):
            in_entry = line == "[Desktop Entry]"
            continue
        if not in_entry or "=" not in line:
            continue

        key, value = line.split("=", 1)
        key = key.strip()
        if key in {"Name", "Exec", "Icon", "StartupWMClass", "NoDisplay", "Hidden"}:
            fields[key] = value.strip()

    if fields.get("NoDisplay", "false").lower() == "true":
        return None
    if fields.get("Hidden", "false").lower() == "true":
        return None

    name = fields.get("Name", path.stem)
    exec_cmd = fields.get("Exec", "")
    icon_name = fields.get("Icon", "")
    startup_wm_class = fields.get("StartupWMClass", "")

    return DesktopEntry(
        desktop_id=path.name,
        name=name,
        exec_cmd=exec_cmd,
        icon_name=icon_name,
        startup_wm_class=startup_wm_class,
        path=path,
    )


def load_desktop_entries() -> Dict[str, DesktopEntry]:
    entries: Dict[str, DesktopEntry] = {}
    for base in DESKTOP_DIRS:
        if not base.exists():
            continue
        for desktop_file in base.glob("*.desktop"):
            entry = parse_desktop_file(desktop_file)
            if entry:
                entries[entry.desktop_id] = entry
    return entries


def get_favorite_desktop_ids() -> List[str]:
    raw = run_command(["gsettings", "get", "org.gnome.shell", "favorite-apps"])
    if not raw:
        return []

    # GNOME format looks like: ['org.gnome.Nautilus.desktop', 'firefox.desktop']
    try:
        values = ast.literal_eval(raw)
        if isinstance(values, list):
            return [str(v) for v in values if str(v).endswith(".desktop")]
    except (ValueError, SyntaxError):
        pass

    found = re.findall(r"[A-Za-z0-9._+-]+\.desktop", raw)
    return found


def resolve_icon(icon_name: str, size: int = 64) -> QIcon:
    if not icon_name:
        return QIcon.fromTheme("application-x-executable")

    icon_path = Path(icon_name)
    if icon_path.is_file():
        return QIcon(str(icon_path))

    theme_icon = QIcon.fromTheme(icon_name)
    if not theme_icon.isNull():
        return theme_icon

    candidates = [icon_name]
    if "." not in icon_name:
        candidates.extend([f"{icon_name}.png", f"{icon_name}.svg", f"{icon_name}.xpm"])

    for base in ICON_DIRS:
        if not base.exists():
            continue
        # Search common size paths and fallback recursive scan.
        scan_dirs = [
            base / f"{size}x{size}/apps",
            base / "scalable/apps",
            base,
        ]
        for scan in scan_dirs:
            if not scan.exists():
                continue
            for c in candidates:
                p = scan / c
                if p.is_file():
                    return QIcon(str(p))
        for c in candidates:
            for p in base.rglob(c):
                if p.is_file():
                    return QIcon(str(p))

    return QIcon.fromTheme("application-x-executable")


def sanitize_exec(exec_cmd: str) -> str:
    if not exec_cmd:
        return ""
    tokenized = shlex.split(exec_cmd)
    cleaned = [t for t in tokenized if not t.startswith("%")]
    if not cleaned:
        return ""
    return cleaned[0]


def process_name_tokens(proc: psutil.Process) -> Set[str]:
    tokens: Set[str] = set()

    try:
        name = proc.name().lower().strip()
        if name:
            tokens.add(Path(name).stem)
    except (psutil.AccessDenied, psutil.NoSuchProcess, psutil.ZombieProcess):
        pass

    try:
        cmdline = proc.cmdline()
    except (psutil.AccessDenied, psutil.NoSuchProcess, psutil.ZombieProcess):
        cmdline = []

    if cmdline:
        exe0 = Path(cmdline[0]).name.lower().strip()
        if exe0:
            tokens.add(Path(exe0).stem)
        for arg in cmdline[1:]:
            if arg.startswith("-"):
                continue
            basename = Path(arg).name.lower().strip()
            if basename and "/" not in basename:
                tokens.add(Path(basename).stem)

    ignore = {
        "gnome-shell",
        "xwayland",
        "xorg",
        "systemd",
        "ibus-daemon",
        "pipewire",
        "wireplumber",
        "dbus-daemon",
    }
    return {t for t in tokens if t and t not in ignore}


def running_gui_tokens() -> Set[str]:
    tokens: Set[str] = set()
    for proc in psutil.process_iter(attrs=[], ad_value=None):
        tokens |= process_name_tokens(proc)
    return tokens


def is_entry_running(entry: DesktopEntry, run_tokens: Set[str]) -> bool:
    candidates: Set[str] = set()
    candidates.add(Path(entry.desktop_id).stem.lower())

    exec_bin = sanitize_exec(entry.exec_cmd)
    if exec_bin:
        candidates.add(Path(exec_bin).name.lower())
        candidates.add(Path(exec_bin).stem.lower())

    if entry.startup_wm_class:
        candidates.add(entry.startup_wm_class.lower())
        candidates.add(Path(entry.startup_wm_class).stem.lower())

    for c in list(candidates):
        candidates.add(c.replace(".desktop", ""))
        candidates.add(c.replace("-", ""))
        candidates.add(c.replace("_", ""))

    normalized_run = set(run_tokens)
    for t in run_tokens:
        normalized_run.add(t.replace("-", ""))
        normalized_run.add(t.replace("_", ""))

    return bool(candidates & normalized_run)


def create_lucid_icon(size: int = 64) -> QIcon:
    pix = QPixmap(size, size)
    pix.fill(Qt.GlobalColor.transparent)

    painter = QPainter(pix)
    painter.setRenderHint(QPainter.RenderHint.Antialiasing)

    grad = QLinearGradient(0, 0, size, size)
    grad.setColorAt(0.0, QColor("#4AF3C8"))
    grad.setColorAt(0.5, QColor("#31C7FF"))
    grad.setColorAt(1.0, QColor("#8EEA7E"))

    path = QPainterPath()
    path.addRoundedRect(2, 2, size - 4, size - 4, size * 0.26, size * 0.26)
    painter.fillPath(path, grad)

    pen = QPen(QColor(255, 255, 255, 230))
    pen.setWidth(max(2, size // 11))
    painter.setPen(pen)
    painter.drawLine(int(size * 0.35), int(size * 0.2), int(size * 0.35), int(size * 0.78))
    painter.drawLine(int(size * 0.35), int(size * 0.78), int(size * 0.72), int(size * 0.78))

    painter.end()
    return QIcon(pix)


class DockButton(QPushButton):
    def __init__(self, icon: QIcon, tooltip: str, parent: Optional[QWidget] = None) -> None:
        super().__init__(parent)
        self.base_size = 56
        self.base_icon_size = 34
        self._scale = 1.0
        self._running = False
        self._pinned = False

        self.setToolTip(tooltip)
        self.setIcon(icon)
        self.setIconSize(QSize(self.base_icon_size, self.base_icon_size))
        self.setFixedSize(self.base_size, self.base_size)
        self.setCursor(Qt.CursorShape.PointingHandCursor)
        self.setFlat(True)
        self.setSizePolicy(QSizePolicy.Policy.Fixed, QSizePolicy.Policy.Fixed)

        self.anim = QVariantAnimation(self)
        self.anim.setDuration(180)
        self.anim.setEasingCurve(QEasingCurve.Type.OutCubic)
        self.anim.valueChanged.connect(lambda v: self.set_scale(float(v)))

        self.setStyleSheet(
            """
            QPushButton {
                border: none;
                border-radius: 16px;
                background: rgba(255, 255, 255, 22);
            }
            QPushButton:hover {
                background: rgba(255, 255, 255, 38);
            }
            """
        )

    def set_running(self, running: bool) -> None:
        self._running = running
        self.update()

    def set_pinned(self, pinned: bool) -> None:
        self._pinned = pinned
        self.update()

    def enterEvent(self, event: QEnterEvent) -> None:
        self.animate_scale(1.22)
        super().enterEvent(event)

    def leaveEvent(self, event: QEvent) -> None:
        self.animate_scale(1.0)
        super().leaveEvent(event)

    def animate_scale(self, target: float) -> None:
        self.anim.stop()
        self.anim.setStartValue(self._scale)
        self.anim.setEndValue(target)
        self.anim.start()

    def get_scale(self) -> float:
        return self._scale

    def set_scale(self, value: float) -> None:
        self._scale = max(1.0, min(1.28, value))
        icon_side = int(self.base_icon_size * self._scale)
        self.setIconSize(QSize(icon_side, icon_side))
        self.update()

    def paintEvent(self, event) -> None:  # type: ignore[override]
        super().paintEvent(event)

        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)

        if self._running:
            glow_color = QColor("#61F8D4") if self._pinned else QColor("#7AB8FF")
            painter.setPen(Qt.PenStyle.NoPen)
            painter.setBrush(glow_color)
            dot_w = max(6, self.width() // 7)
            dot_h = max(5, self.height() // 12)
            x = (self.width() - dot_w) // 2
            y = self.height() - dot_h - 5
            painter.drawRoundedRect(QRect(x, y, dot_w, dot_h), dot_h / 2, dot_h / 2)

        painter.end()


class LucidDock(QWidget):
    run_tokens_ready = pyqtSignal(object)

    def __init__(self) -> None:
        super().__init__()

        self.desktop_entries = load_desktop_entries()
        self.favorites = get_favorite_desktop_ids()

        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        self.setWindowFlags(
            Qt.WindowType.FramelessWindowHint
            | Qt.WindowType.Tool
            | Qt.WindowType.WindowStaysOnTopHint
            | Qt.WindowType.WindowDoesNotAcceptFocus
        )

        self._buttons: List[tuple[DockButton, Optional[DesktopEntry], bool]] = []
        self._executor = concurrent.futures.ThreadPoolExecutor(max_workers=1)
        self._scan_future: Optional[concurrent.futures.Future[Set[str]]] = None
        self._last_run_tokens: Set[str] = set()
        self.run_tokens_ready.connect(self._apply_run_tokens)
        self._build_ui()
        self._rebuild_icons()
        self._schedule_running_scan()

        self.running_timer = QTimer(self)
        self.running_timer.setInterval(2500)
        self.running_timer.timeout.connect(self._tick_running_update)
        self.running_timer.start()

        self.favorites_timer = QTimer(self)
        self.favorites_timer.setInterval(6000)
        self.favorites_timer.timeout.connect(self._refresh_favorites)
        self.favorites_timer.start()

        self._position_bottom_center()

        # Delay EWMH hints until the native window exists.
        QTimer.singleShot(150, self._apply_ewmh_hints)

    def _build_ui(self) -> None:
        outer = QVBoxLayout(self)
        outer.setContentsMargins(12, 10, 12, 14)

        self.panel = QWidget(self)
        self.panel.setObjectName("dockPanel")
        self.panel_layout = QHBoxLayout(self.panel)
        self.panel_layout.setContentsMargins(18, 10, 18, 10)
        self.panel_layout.setSpacing(8)

        self.panel.setStyleSheet(
            """
            QWidget#dockPanel {
                background: rgba(248, 252, 255, 28);
                border: 1px solid rgba(255, 255, 255, 70);
                border-radius: 26px;
            }
            """
        )

        shadow = QGraphicsDropShadowEffect(self.panel)
        shadow.setBlurRadius(32)
        shadow.setOffset(0, 7)
        shadow.setColor(QColor(0, 0, 0, 80))
        self.panel.setGraphicsEffect(shadow)

        outer.addWidget(self.panel)

    def _clear_layout(self) -> None:
        while self.panel_layout.count():
            item = self.panel_layout.takeAt(0)
            if item is None:
                continue
            widget = item.widget()
            if widget is not None:
                widget.deleteLater()

    def _new_button(self, icon: QIcon, tooltip: str) -> DockButton:
        btn = DockButton(icon, tooltip, self.panel)
        self.panel_layout.addWidget(btn)
        return btn

    def _rebuild_icons(self) -> None:
        self._clear_layout()
        self._buttons.clear()

        lucid_btn = self._new_button(create_lucid_icon(), "Lucid: Show Applications")
        lucid_btn.clicked.connect(self._show_applications)
        self._buttons.append((lucid_btn, None, True))

        favorites_ids = [d for d in self.favorites if d in self.desktop_entries]

        for desktop_id in favorites_ids:
            entry = self.desktop_entries[desktop_id]
            icon = resolve_icon(entry.icon_name)
            btn = self._new_button(icon, entry.name)
            btn.set_pinned(True)
            btn.clicked.connect(lambda _=False, e=entry: self._launch_entry(e))
            self._buttons.append((btn, entry, True))

        # Include running apps that are not pinned.
        run_tokens = self._last_run_tokens
        unpinned_running = []
        for entry in self.desktop_entries.values():
            if entry.desktop_id in favorites_ids:
                continue
            if is_entry_running(entry, run_tokens):
                unpinned_running.append(entry)
            if len(unpinned_running) >= 8:
                break

        for entry in unpinned_running:
            icon = resolve_icon(entry.icon_name)
            btn = self._new_button(icon, f"{entry.name} (running)")
            btn.set_pinned(False)
            btn.clicked.connect(lambda _=False, e=entry: self._launch_entry(e))
            self._buttons.append((btn, entry, False))

        self._refresh_running_state(run_tokens=run_tokens)
        self.adjustSize()
        self._position_bottom_center()

    def _tick_running_update(self) -> None:
        self._refresh_running_state()
        self._schedule_running_scan()

    def _refresh_favorites(self) -> None:
        latest = get_favorite_desktop_ids()
        if latest != self.favorites:
            self.favorites = latest
            self._rebuild_icons()

    def _refresh_running_state(self, run_tokens: Optional[Set[str]] = None) -> None:
        if run_tokens is None:
            run_tokens = self._last_run_tokens

        for button, entry, _pinned in self._buttons:
            if entry is None:
                button.set_running(False)
                continue
            button.set_running(is_entry_running(entry, run_tokens))

    def _schedule_running_scan(self) -> None:
        if self._scan_future is not None and not self._scan_future.done():
            return

        self._scan_future = self._executor.submit(running_gui_tokens)
        self._scan_future.add_done_callback(self._on_scan_complete)

    def _on_scan_complete(self, future: concurrent.futures.Future[Set[str]]) -> None:
        try:
            run_tokens = future.result()
        except Exception:
            return
        self.run_tokens_ready.emit(run_tokens)

    def _apply_run_tokens(self, run_tokens: Set[str]) -> None:
        if run_tokens == self._last_run_tokens:
            return
        self._last_run_tokens = run_tokens
        self._refresh_running_state(run_tokens=run_tokens)

        favorites = {d for d in self.favorites if d in self.desktop_entries}
        has_unpinned_running = any(
            is_entry_running(entry, run_tokens)
            for entry in self.desktop_entries.values()
            if entry.desktop_id not in favorites
        )
        if has_unpinned_running:
            self._rebuild_icons()

    def _launch_entry(self, entry: DesktopEntry) -> None:
        # Prefer gtk-launch with desktop id, fallback to gio launch with full path.
        try:
            subprocess.Popen(["gtk-launch", entry.desktop_id], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            return
        except FileNotFoundError:
            pass

        try:
            subprocess.Popen(["gio", "launch", str(entry.path)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        except FileNotFoundError:
            pass

    def _show_applications(self) -> None:
        cmd = [
            "dbus-send",
            "--session",
            "--type=method_call",
            "--dest=org.gnome.Shell",
            "/org/gnome/Shell",
            "org.gnome.Shell.Eval",
            "string:Main.shellDBusService.ShowApplications()",
        ]
        subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def _position_bottom_center(self) -> None:
        screen = QApplication.primaryScreen()
        if not screen:
            return

        geom = screen.geometry()
        size = self.sizeHint()
        x = geom.x() + (geom.width() - size.width()) // 2
        y = geom.y() + geom.height() - size.height() - 8
        self.move(QPoint(max(0, x), max(0, y)))

    def resizeEvent(self, event) -> None:  # type: ignore[override]
        super().resizeEvent(event)
        self._position_bottom_center()

    def _apply_ewmh_hints(self) -> None:
        # Works on X11; on Wayland this may be ignored by compositor policy.
        if os.environ.get("XDG_SESSION_TYPE", "").lower() == "wayland":
            return

        wid_hex = hex(int(self.winId()))
        screen = QApplication.primaryScreen()
        if not screen:
            return

        geom = screen.geometry()
        reserve = max(64, self.height() + 8)
        # left, right, top, bottom
        strut = f"0, 0, 0, {reserve}"
        # left, right, top, bottom, left_start_y, left_end_y, right_start_y,
        # right_end_y, top_start_x, top_end_x, bottom_start_x, bottom_end_x
        strut_partial = (
            f"0, 0, 0, {reserve}, 0, 0, 0, 0, 0, 0, {geom.x()}, {geom.x() + geom.width() - 1}"
        )

        cmds = [
            [
                "xprop",
                "-id",
                wid_hex,
                "-f",
                "_NET_WM_WINDOW_TYPE",
                "32a",
                "-set",
                "_NET_WM_WINDOW_TYPE",
                "_NET_WM_WINDOW_TYPE_DOCK",
            ],
            [
                "xprop",
                "-id",
                wid_hex,
                "-f",
                "_NET_WM_STRUT",
                "32cccc",
                "-set",
                "_NET_WM_STRUT",
                strut,
            ],
            [
                "xprop",
                "-id",
                wid_hex,
                "-f",
                "_NET_WM_STRUT_PARTIAL",
                "32cccccccccccc",
                "-set",
                "_NET_WM_STRUT_PARTIAL",
                strut_partial,
            ],
        ]

        for cmd in cmds:
            subprocess.run(cmd, check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def main() -> int:
    app = QApplication(sys.argv)
    app.setApplicationName("Lucid Dock")

    # Slightly softer text for the dock tooltip and labels.
    font = QFont()
    font.setPointSize(10)
    app.setFont(font)

    dock = LucidDock()
    dock.show()

    # Re-apply hints after first show in case WM delayed mapping.
    QTimer.singleShot(800, dock._apply_ewmh_hints)

    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
