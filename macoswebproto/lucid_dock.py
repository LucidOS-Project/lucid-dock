from __future__ import annotations

import math
import os
import re
import shlex
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


def _iter_existing(paths: Iterable[Path]) -> list[Path]:
	return [path for path in paths if path.is_file()]


def _find_layer_shell_library() -> Path | None:
	candidates = _iter_existing(
		[
			Path('/usr/local/lib/x86_64-linux-gnu/libgtk4-layer-shell.so'),
			Path('/usr/local/lib/libgtk4-layer-shell.so'),
			Path('/usr/lib/x86_64-linux-gnu/libgtk4-layer-shell.so'),
			Path('/usr/lib/libgtk4-layer-shell.so'),
		]
	)
	if candidates:
		return candidates[0]

	for folder in [Path('/usr/local/lib/x86_64-linux-gnu'), Path('/usr/local/lib'), Path('/usr/lib/x86_64-linux-gnu'), Path('/usr/lib')]:
		if not folder.is_dir():
			continue
		for name in ('libgtk4-layer-shell.so.1', 'libgtk4-layer-shell.so.0', 'libgtk4-layer-shell.so'):
			path = folder / name
			if path.is_file():
				return path
	return None


def ensure_layer_shell_preload() -> None:
	# gtk4-layer-shell must be loaded before libwayland for proper layer-surface init.
	if os.environ.get('XDG_SESSION_TYPE') != 'wayland':
		return
	if os.environ.get('LUCID_DOCK_PRELOAD_DONE') == '1':
		return

	library_path = _find_layer_shell_library()
	if library_path is None:
		return

	ld_preload = os.environ.get('LD_PRELOAD', '')
	entries = [entry for entry in ld_preload.split(':') if entry]
	lib_str = str(library_path)
	if lib_str in entries:
		return

	new_env = os.environ.copy()
	new_env['LD_PRELOAD'] = ':'.join([lib_str, *entries]) if entries else lib_str
	new_env['LUCID_DOCK_PRELOAD_DONE'] = '1'
	os.execvpe(sys.executable, [sys.executable, *sys.argv], new_env)


ensure_layer_shell_preload()

import gi


def configure_local_gi_paths() -> None:
	# Support locally installed gtk4-layer-shell in /usr/local.
	typelib_paths = [
		Path('/usr/local/lib/x86_64-linux-gnu/girepository-1.0'),
		Path('/usr/local/lib/girepository-1.0'),
	]
	library_paths = [
		Path('/usr/local/lib/x86_64-linux-gnu'),
		Path('/usr/local/lib'),
	]

	current_typelib = [p for p in os.environ.get('GI_TYPELIB_PATH', '').split(':') if p]
	for path in typelib_paths:
		path_str = str(path)
		if path.is_dir() and path_str not in current_typelib:
			current_typelib.insert(0, path_str)
	if current_typelib:
		os.environ['GI_TYPELIB_PATH'] = ':'.join(current_typelib)

	# Keep this for subprocesses / fallback dynamic loading.
	current_ld = [p for p in os.environ.get('LD_LIBRARY_PATH', '').split(':') if p]
	for path in library_paths:
		path_str = str(path)
		if path.is_dir() and path_str not in current_ld:
			current_ld.insert(0, path_str)
	if current_ld:
		os.environ['LD_LIBRARY_PATH'] = ':'.join(current_ld)

	# GIRepository paths affect typelib/shared-lib lookup in this process.
	try:
		gi.require_version('GIRepository', '2.0')
		from gi.repository import GIRepository
		repo = GIRepository.Repository.get_default()
		for path in typelib_paths:
			if path.is_dir():
				repo.prepend_search_path(str(path))
		for path in library_paths:
			if path.is_dir():
				repo.prepend_library_path(str(path))
	except (ImportError, ValueError, AttributeError):
		pass


configure_local_gi_paths()

gi.require_version('Gtk', '4.0')
gi.require_version('Gdk', '4.0')

try:
	gi.require_version('Gtk4LayerShell', '1.0')
	from gi.repository import Gtk4LayerShell
except (ImportError, ValueError):
	Gtk4LayerShell = None

from gi.repository import Gdk, Gio, GLib, Gtk


BASE_WIDTH = 57.6
MAX_WIDTH = BASE_WIDTH * 2
DISTANCE_LIMIT = BASE_WIDTH * 6
BEYOND_DISTANCE_LIMIT = DISTANCE_LIMIT + 1
DISTANCE_INPUT = [
	-DISTANCE_LIMIT,
	-DISTANCE_LIMIT / 1.25,
	-DISTANCE_LIMIT / 2,
	0,
	DISTANCE_LIMIT / 2,
	DISTANCE_LIMIT / 1.25,
	DISTANCE_LIMIT,
]
WIDTH_OUTPUT = [
	BASE_WIDTH,
	BASE_WIDTH * 1.1,
	BASE_WIDTH * 1.414,
	MAX_WIDTH,
	BASE_WIDTH * 1.414,
	BASE_WIDTH * 1.1,
	BASE_WIDTH,
]

HIDDEN_DOCK_THRESHOLD = 30
BOUNCE_HEIGHT = 40
BOUNCE_DURATION = 0.4
SLIDE_DURATION = 0.24
AUTO_HIDE_ENABLED = False
PANEL_PADDING_X = 10
PANEL_PADDING_Y = 8
ITEM_GAP = 10
ITEM_SLOT_HEIGHT = 128
DOT_SIZE = 4
DIVIDER_WIDTH = 2
DIVIDER_SIDE_MARGIN = 8
WINDOW_HEIGHT = 160
WINDOW_WIDTH_FALLBACK = 1280
BOTTOM_MARGIN = 8
RUNNING_REFRESH_SECONDS = 4

CONFIG_PATH = Path(__file__).with_name('apps-config.ts')
STATE_PATH = Path(__file__).with_name('apps.svelte.ts')
DESKTOP_DIR = Path('/usr/share/applications')
ICON_THEME_DIR = Path('/usr/share/icons/Lucid-Light')
ICON_THEME_SEARCH_SUBDIRS = [
	'scalable/apps',
	'256x256/apps',
	'128x128/apps',
	'64x64/apps',
	'512x512/apps',
]
ICON_EXTENSIONS = ['svg', 'png', 'xpm']

APP_ID_ALIASES = {
	'finder': ['org.gnome.Nautilus.desktop', 'nautilus.desktop'],
	'wallpapers': ['gnome-background-panel.desktop', 'org.gnome.Settings.desktop'],
	'calculator': ['org.gnome.Calculator.desktop'],
	'calendar': ['org.gnome.Calendar.desktop', 'gnome-calendar.desktop'],
	'vscode': ['code.desktop', 'code-oss.desktop'],
	'appstore': ['org.gnome.Software.desktop', 'gnome-software.desktop'],
}

CSS = """
window {
	background: transparent;
}

.dock-root {
	background: transparent;
}

.dock-panel {
	background: rgba(255, 255, 255, 0.4);
	border-radius: 19px;
	box-shadow:
		inset 0 0 0 1px rgba(255, 255, 255, 0.28),
		0 0 0 1px rgba(20, 20, 20, 0.22),
		2px 5px 19px 7px rgba(0, 0, 0, 0.3);
}

.dock-divider {
	background: rgba(20, 20, 20, 0.3);
	border-radius: 999px;
	min-width: 2px;
}

.dock-item {
	padding: 0;
	margin: 0;
	border: none;
	background: transparent;
	box-shadow: none;
	outline: none;
}

.dock-item:hover,
.dock-item:active,
.dock-item:focus-visible {
	background: transparent;
	box-shadow: none;
	outline: none;
}

.dock-dot {
	border-radius: 999px;
	background: rgba(20, 20, 20, 0.85);
}
"""


@dataclass(slots=True)
class PrototypeConfig:
	app_id: str
	title: str
	dock_breaks_before: bool


@dataclass(slots=True)
class DesktopApp:
	app_id: str
	title: str
	desktop_id: str
	desktop_path: Path
	icon_name: str | None
	exec_line: str | None
	process_candidates: set[str]
	app_info: Gio.DesktopAppInfo | None
	dock_breaks_before: bool
	is_open: bool


class DockState:
	def __init__(self, initial_open: dict[str, bool], active_app_id: str | None) -> None:
		self.open = dict(initial_open)
		self.active_app_id = active_app_id

	def is_open(self, app_id: str) -> bool:
		return bool(self.open.get(app_id, False))

	def open_app(self, app_id: str) -> bool:
		was_open = self.is_open(app_id)
		self.open[app_id] = True
		self.active_app_id = app_id
		return was_open

	def set_open(self, app_id: str, is_open: bool) -> None:
		self.open[app_id] = is_open


def find_icon_in_lucid_theme(icon_name: str) -> Path | None:
	if not icon_name:
		return None
	for subdir in ICON_THEME_SEARCH_SUBDIRS:
		for ext in ICON_EXTENSIONS:
			candidate = ICON_THEME_DIR / subdir / f'{icon_name}.{ext}'
			if candidate.is_file():
				return candidate
	return None


def sine_in_out(progress: float) -> float:
	return -(math.cos(math.pi * progress) - 1) / 2


def interpolate_width(distance: float) -> float:
	if distance <= DISTANCE_INPUT[0] or distance >= DISTANCE_INPUT[-1]:
		return BASE_WIDTH

	for index in range(len(DISTANCE_INPUT) - 1):
		left = DISTANCE_INPUT[index]
		right = DISTANCE_INPUT[index + 1]
		if left <= distance <= right:
			start = WIDTH_OUTPUT[index]
			end = WIDTH_OUTPUT[index + 1]
			span = right - left
			if span == 0:
				return end
			mix = (distance - left) / span
			return start + (end - start) * mix

	return BASE_WIDTH


def clean_typescript(text: str) -> str:
	return '\n'.join(line for line in text.splitlines() if not line.lstrip().startswith('//'))


def parse_prototype_config(config_path: Path) -> list[PrototypeConfig]:
	text = clean_typescript(config_path.read_text(encoding='utf-8'))
	config_blocks = {
		name: body
		for name, body in re.findall(r"const\s+(\w+)\s*=\s*create_app_config\(\{(.*?)\}\);", text, re.S)
	}

	export_match = re.search(r"export const apps_config = \{(.*?)\};", text, re.S)
	if export_match is None:
		raise RuntimeError(f'Could not parse apps_config from {config_path}')

	ordered_config: list[PrototypeConfig] = []
	for raw_line in export_match.group(1).splitlines():
		line = raw_line.strip().rstrip(',')
		if not line:
			continue

		mapping = re.match(r"'([^']+)'\s*:\s*(\w+)$", line)
		if mapping is not None:
			app_id, variable_name = mapping.groups()
		else:
			bare = re.match(r'(\w+)$', line)
			if bare is None:
				continue
			variable_name = bare.group(1)
			app_id = variable_name

		block = config_blocks.get(variable_name)
		if block is None:
			continue

		title_match = re.search(r"title:\s*(?:`([^`]*)`|'([^']*)')", block)
		title = next((group for group in title_match.groups() if group), app_id) if title_match else app_id
		ordered_config.append(
			PrototypeConfig(
				app_id=app_id,
				title=title,
				dock_breaks_before='dock_breaks_before: true' in block,
			)
		)

	return ordered_config


def parse_prototype_state(state_path: Path) -> tuple[dict[str, bool], str | None]:
	text = clean_typescript(state_path.read_text(encoding='utf-8'))
	open_match = re.search(r'open:\s*\{(.*?)\}\s*as\s*Record<AppID, boolean>', text, re.S)
	if open_match is None:
		raise RuntimeError(f'Could not parse open state from {state_path}')

	initial_open = {
		app_id: value == 'true'
		for app_id, value in re.findall(r"'?([a-zA-Z0-9-]+)'?\s*:\s*(true|false)", open_match.group(1))
	}

	active_match = re.search(r"active:\s*'([^']+)'", text)
	active_app_id = active_match.group(1) if active_match else None
	return initial_open, active_app_id


def load_keyfile_value(key_file: GLib.KeyFile, key: str) -> str | None:
	try:
		return key_file.get_string('Desktop Entry', key)
	except GLib.Error:
		return None


def load_desktop_catalog() -> dict[str, tuple[Path, str | None]]:
	catalog: dict[str, tuple[Path, str | None]] = {}
	for desktop_path in sorted(DESKTOP_DIR.glob('*.desktop')):
		key_file = GLib.KeyFile()
		try:
			key_file.load_from_file(str(desktop_path), GLib.KeyFileFlags.NONE)
		except GLib.Error:
			continue

		if load_keyfile_value(key_file, 'Type') != 'Application':
			continue

		try:
			if key_file.get_boolean('Desktop Entry', 'NoDisplay'):
				continue
		except GLib.Error:
			pass

		catalog[desktop_path.name] = (desktop_path, load_keyfile_value(key_file, 'Name'))
	return catalog


def resolve_desktop_entry(prototype: PrototypeConfig, catalog: dict[str, tuple[Path, str | None]]) -> Path | None:
	for desktop_id in APP_ID_ALIASES.get(prototype.app_id, []):
		if desktop_id in catalog:
			return catalog[desktop_id][0]

	title_lower = prototype.title.casefold()
	for desktop_id, (desktop_path, desktop_name) in catalog.items():
		if desktop_id.removesuffix('.desktop').casefold() == prototype.app_id.casefold():
			return desktop_path
		if desktop_name and desktop_name.casefold() == title_lower:
			return desktop_path

	for _, (desktop_path, desktop_name) in catalog.items():
		name_match = desktop_name.casefold() if desktop_name else ''
		if title_lower in name_match or prototype.app_id.replace('-', ' ') in name_match:
			return desktop_path

	return None


def load_dock_apps(config_path: Path, state_path: Path) -> tuple[list[DesktopApp], DockState]:
	prototype_config = parse_prototype_config(config_path)
	initial_open, active_app_id = parse_prototype_state(state_path)
	state = DockState(initial_open, active_app_id)
	catalog = load_desktop_catalog()

	apps: list[DesktopApp] = []
	for prototype in prototype_config:
		desktop_path = resolve_desktop_entry(prototype, catalog)
		if desktop_path is None:
			continue

		key_file = GLib.KeyFile()
		try:
			key_file.load_from_file(str(desktop_path), GLib.KeyFileFlags.NONE)
		except GLib.Error:
			continue

		apps.append(
			DesktopApp(
				app_id=prototype.app_id,
				title=load_keyfile_value(key_file, 'Name') or prototype.title,
				desktop_id=desktop_path.name,
				desktop_path=desktop_path,
				icon_name=load_keyfile_value(key_file, 'Icon'),
				exec_line=load_keyfile_value(key_file, 'Exec'),
				process_candidates=exec_candidates(load_keyfile_value(key_file, 'Exec'), desktop_path.name),
				app_info=Gio.DesktopAppInfo.new_from_filename(str(desktop_path)),
				dock_breaks_before=prototype.dock_breaks_before,
				is_open=state.is_open(prototype.app_id),
			)
		)

	return apps, state


def get_gnome_favorite_desktop_ids() -> list[str]:
	settings = Gio.Settings.new('org.gnome.shell')
	try:
		favorites = list(settings.get_strv('favorite-apps'))
	except GLib.Error:
		favorites = []

	return [item for item in favorites if item.endswith('.desktop')]


def load_desktop_info(desktop_path: Path) -> tuple[str, str | None, str | None]:
	key_file = GLib.KeyFile()
	key_file.load_from_file(str(desktop_path), GLib.KeyFileFlags.NONE)
	title = load_keyfile_value(key_file, 'Name') or desktop_path.stem
	icon_name = load_keyfile_value(key_file, 'Icon')
	exec_line = load_keyfile_value(key_file, 'Exec')
	return title, icon_name, exec_line


def collect_running_process_names() -> set[str]:
	names: set[str] = set()
	proc_root = Path('/proc')
	for pid_dir in proc_root.iterdir():
		if not pid_dir.name.isdigit():
			continue

		comm = pid_dir / 'comm'
		cmdline = pid_dir / 'cmdline'
		try:
			if comm.exists():
				name = comm.read_text(encoding='utf-8', errors='ignore').strip()
				if name:
					names.add(name)

			if cmdline.exists():
				raw = cmdline.read_bytes()
				if raw:
					first = raw.split(b'\0', 1)[0].decode('utf-8', errors='ignore').strip()
					if first:
						names.add(Path(first).name)
		except OSError:
			continue

	return names


def exec_candidates(exec_line: str | None, desktop_id: str) -> set[str]:
	candidates = {desktop_id.removesuffix('.desktop')}
	if not exec_line:
		return candidates

	try:
		tokens = shlex.split(exec_line)
	except ValueError:
		tokens = exec_line.split()

	if not tokens:
		return candidates

	binary = tokens[0]
	if binary:
		candidates.add(Path(binary).name)
		candidates.add(Path(binary).stem)

	return candidates


def load_system_dock_apps() -> tuple[list[DesktopApp], DockState]:
	catalog = load_desktop_catalog()
	favorites = get_gnome_favorite_desktop_ids()
	running_names = collect_running_process_names()

	apps: list[DesktopApp] = []
	for desktop_id in favorites:
		record = catalog.get(desktop_id)
		if record is None:
			continue

		desktop_path, _ = record
		try:
			title, icon_name, exec_line = load_desktop_info(desktop_path)
		except GLib.Error:
			continue

		app_id = desktop_id.removesuffix('.desktop')
		is_open = bool(exec_candidates(exec_line, desktop_id) & running_names)

		apps.append(
			DesktopApp(
				app_id=app_id,
				title=title,
				desktop_id=desktop_id,
				desktop_path=desktop_path,
				icon_name=icon_name,
				exec_line=exec_line,
				process_candidates=exec_candidates(exec_line, desktop_id),
				app_info=Gio.DesktopAppInfo.new_from_filename(str(desktop_path)),
				dock_breaks_before=False,
				is_open=is_open,
			)
		)

	initial_open = {app.app_id: app.is_open for app in apps}
	active_app = next((app.app_id for app in apps if app.is_open), apps[0].app_id if apps else None)
	state = DockState(initial_open, active_app)
	return apps, state


class DockDivider(Gtk.Box):
	def __init__(self, panel_height: int) -> None:
		super().__init__()
		self.add_css_class('dock-divider')
		self.set_size_request(DIVIDER_WIDTH, int(panel_height * 0.6))


class DockItem(Gtk.Button):
	def __init__(self, app: DesktopApp, state: DockState, launch_callback) -> None:
		super().__init__()
		self.app = app
		self.state = state
		self.launch_callback = launch_callback
		self.current_width = BASE_WIDTH
		self.pixel_width = int(round(BASE_WIDTH))
		self.bounce_offset = 0.0
		self.bounce_started_at: float | None = None

		self.add_css_class('dock-item')
		self.set_can_focus(False)
		self.connect('clicked', self._on_clicked)

		self.fixed = Gtk.Fixed()
		self.set_child(self.fixed)

		self.image = self._build_icon()
		self.fixed.put(self.image, 0, 0)

		self.dot = Gtk.Box()
		self.dot.add_css_class('dock-dot')
		self.dot.set_size_request(DOT_SIZE, DOT_SIZE)
		self.dot.set_visible(state.is_open(app.app_id))
		self.fixed.put(self.dot, 0, 0)

		self.apply_width(BASE_WIDTH)

	def _build_icon(self) -> Gtk.Image:
		# 1. Absolute file path from desktop entry
		if self.app.icon_name and Path(self.app.icon_name).is_file():
			image = Gtk.Image.new_from_file(self.app.icon_name)
			image.set_pixel_size(int(round(BASE_WIDTH)))
			return image

		# 2. Direct search in Lucid-Light theme directory
		icon_name = self.app.icon_name or ''
		theme_file = find_icon_in_lucid_theme(icon_name)
		if theme_file is None:
			# Also try the stem in case icon_name has an extension
			stem = Path(icon_name).stem
			if stem and stem != icon_name:
				theme_file = find_icon_in_lucid_theme(stem)
		if theme_file is not None:
			image = Gtk.Image.new_from_file(str(theme_file))
			image.set_pixel_size(int(round(BASE_WIDTH)))
			return image

		# 3. Fallback to GTK icon name resolution
		if icon_name:
			image = Gtk.Image.new_from_icon_name(icon_name)
		else:
			image = Gtk.Image.new_from_icon_name('application-x-executable')
		image.set_pixel_size(int(round(BASE_WIDTH)))
		return image

	def _on_clicked(self, _button: Gtk.Button) -> None:
		was_open = self.state.open_app(self.app.app_id)
		self.app.is_open = True
		self.set_open_indicator(True)
		self.launch_callback(self.app)
		if not was_open:
			self.start_bounce()

	def set_open_indicator(self, is_open: bool) -> None:
		self.dot.set_visible(is_open)

	def start_bounce(self) -> None:
		self.bounce_started_at = time.monotonic()

	def update_bounce(self, now: float) -> bool:
		if self.bounce_started_at is None:
			return False

		progress = min(1.0, (now - self.bounce_started_at) / BOUNCE_DURATION)
		if progress < 0.5:
			local_progress = sine_in_out(progress * 2)
			self.bounce_offset = -BOUNCE_HEIGHT * local_progress
		else:
			local_progress = sine_in_out((progress - 0.5) * 2)
			self.bounce_offset = -BOUNCE_HEIGHT * (1 - local_progress)

		if progress >= 1.0:
			self.bounce_started_at = None
			self.bounce_offset = 0.0

		self._update_internal_layout()
		return self.bounce_started_at is not None

	def apply_width(self, width: float) -> None:
		pixel_width = max(1, int(round(width)))
		self.current_width = float(pixel_width)
		if pixel_width != self.pixel_width:
			self.pixel_width = pixel_width
			self.set_size_request(pixel_width, ITEM_SLOT_HEIGHT)
			self.fixed.set_size_request(pixel_width, ITEM_SLOT_HEIGHT)
			self.image.set_pixel_size(pixel_width)
		self._update_internal_layout()

	def _update_internal_layout(self) -> None:
		pixel_width = int(round(self.current_width))
		# Bottom-anchor icons so they grow upward from a fixed baseline (macOS style).
		# 13px is reserved below the icon for the running-state dot.
		base_y = ITEM_SLOT_HEIGHT - pixel_width - 13
		image_y = max(0, base_y + int(round(self.bounce_offset)))
		indicator_x = max(0, (pixel_width - DOT_SIZE) // 2)
		indicator_y = ITEM_SLOT_HEIGHT - 12
		self.fixed.move(self.image, 0, image_y)
		self.fixed.move(self.dot, indicator_x, indicator_y)


class DockWindow(Gtk.ApplicationWindow):
	def __init__(self, app: Gtk.Application, dock_apps: list[DesktopApp], state: DockState) -> None:
		super().__init__(application=app, title='Lucid Dock')
		self.state = state
		self.dock_apps = dock_apps
		self.current_mouse_x: float | None = None
		self.hidden_progress = 0.0
		self.hidden_target = 0.0
		self.slide_started_at: float | None = None
		self.slide_from = 0.0
		self.panel_x = 0
		self.panel_y = 0
		self.panel_content_width = 0
		self.frame_source: int | None = None
		self.running_refresh_source: int | None = None
		self.layout_dirty = False
		self.layer_shell_active = False

		self.set_decorated(False)
		self.set_resizable(False)
		self.set_can_focus(False)
		self.set_default_size(WINDOW_WIDTH_FALLBACK, WINDOW_HEIGHT)

		if Gtk4LayerShell is not None:
			Gtk4LayerShell.init_for_window(self)
			# OVERLAY keeps the dock above all application windows
			Gtk4LayerShell.set_layer(self, Gtk4LayerShell.Layer.OVERLAY)
			# Use a full-width layer surface and center the panel within it.
			Gtk4LayerShell.set_anchor(self, Gtk4LayerShell.Edge.BOTTOM, True)
			Gtk4LayerShell.set_anchor(self, Gtk4LayerShell.Edge.LEFT, True)
			Gtk4LayerShell.set_anchor(self, Gtk4LayerShell.Edge.RIGHT, True)
			Gtk4LayerShell.set_margin(self, Gtk4LayerShell.Edge.BOTTOM, BOTTOM_MARGIN)
			Gtk4LayerShell.set_margin(self, Gtk4LayerShell.Edge.LEFT, 0)
			Gtk4LayerShell.set_margin(self, Gtk4LayerShell.Edge.RIGHT, 0)
			Gtk4LayerShell.set_keyboard_mode(self, Gtk4LayerShell.KeyboardMode.ON_DEMAND)
			self.layer_shell_active = True

		provider = Gtk.CssProvider()
		provider.load_from_data(CSS.encode('utf-8'))
		Gtk.StyleContext.add_provider_for_display(
			Gdk.Display.get_default(),
			provider,
			Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION,
		)

		self.root = Gtk.Fixed()
		self.root.add_css_class('dock-root')
		self.set_child(self.root)

		self.panel = Gtk.Fixed()
		self.panel.add_css_class('dock-panel')
		self.root.put(self.panel, 0, 0)

		self.items: list[DockItem] = []
		self.entries: list[tuple[str, DockItem | DockDivider]] = []
		self.items_by_app_id: dict[str, DockItem] = {}
		for dock_app in dock_apps:
			if dock_app.dock_breaks_before and self.items:
				divider = DockDivider(ITEM_SLOT_HEIGHT)
				self.entries.append(('divider', divider))
				self.panel.put(divider, 0, 0)

			item = DockItem(dock_app, state, self.launch_desktop_app)
			self.items.append(item)
			self.items_by_app_id[dock_app.app_id] = item
			self.entries.append(('item', item))
			self.panel.put(item, 0, 0)

		motion = Gtk.EventControllerMotion.new()
		motion.connect('motion', self.on_motion)
		motion.connect('leave', self.on_leave)
		self.root.add_controller(motion)

		GLib.idle_add(self.queue_layout_once)
		self.running_refresh_source = GLib.timeout_add_seconds(RUNNING_REFRESH_SECONDS, self.refresh_running_state)

	def get_primary_monitor_geometry(self) -> Gdk.Rectangle:
		display = Gdk.Display.get_default()
		if display is None:
			return Gdk.Rectangle()

		surface = self.get_surface()
		if surface is not None:
			monitor = display.get_monitor_at_surface(surface)
			if monitor is not None:
				return monitor.get_geometry()

		monitors = display.get_monitors()
		if monitors.get_n_items() > 0:
			primary = monitors.get_item(0)
			if primary is not None:
				return primary.get_geometry()

		return Gdk.Rectangle()

	def update_window_geometry(self, panel_width: int, panel_height: int) -> tuple[int, int]:
		if self.layer_shell_active:
			# Let layer-shell/compositor control surface size; read real allocation.
			window_width = self.get_width()
			if window_width <= 0:
				geometry = self.get_primary_monitor_geometry()
				window_width = geometry.width if geometry.width > 0 else WINDOW_WIDTH_FALLBACK
			window_height = self.get_height()
			if window_height <= 0:
				window_height = WINDOW_HEIGHT
		else:
			window_width = max(panel_width, 1)
			self.set_default_size(window_width, WINDOW_HEIGHT)
			window_height = WINDOW_HEIGHT
		return window_width, window_height

	def refresh_running_state(self) -> bool:
		if self.current_mouse_x is not None:
			return True

		running_names = collect_running_process_names()
		for dock_app in self.dock_apps:
			is_open = bool(dock_app.process_candidates & running_names)
			dock_app.is_open = is_open
			self.state.set_open(dock_app.app_id, is_open)
			item = self.items_by_app_id.get(dock_app.app_id)
			if item is not None:
				item.set_open_indicator(is_open)

		return True

	def queue_layout_once(self) -> bool:
		self.queue_layout()
		return False

	def on_motion(self, _controller: Gtk.EventControllerMotion, x: float, y: float) -> None:
		self.current_mouse_x = x
		if AUTO_HIDE_ENABLED:
			distance_from_bottom = max(0.0, self.get_height() - y)
			self.set_hidden(distance_from_bottom > HIDDEN_DOCK_THRESHOLD)
		self.queue_layout()

	def on_leave(self, _controller: Gtk.EventControllerMotion) -> None:
		self.current_mouse_x = None
		if AUTO_HIDE_ENABLED:
			self.set_hidden(True)
		self.queue_layout()

	def set_hidden(self, hidden: bool) -> None:
		target = 1.0 if hidden else 0.0
		if math.isclose(target, self.hidden_target, abs_tol=1e-4):
			return
		self.hidden_target = target
		self.slide_started_at = time.monotonic()
		self.slide_from = self.hidden_progress
		self.ensure_frame_loop()

	def queue_layout(self) -> None:
		self.layout_dirty = True
		self.ensure_frame_loop()

	def ensure_frame_loop(self) -> None:
		if self.frame_source is None:
			# Tick callback is synced to the monitor refresh rate (e.g. 60/120/144Hz).
			self.frame_source = self.add_tick_callback(self.on_tick)

	def on_tick(self, _widget: Gtk.Widget, _frame_clock: Gdk.FrameClock) -> bool:
		now = time.monotonic()
		animations_running = False

		if self.slide_started_at is not None:
			progress = min(1.0, (now - self.slide_started_at) / SLIDE_DURATION)
			eased = sine_in_out(progress)
			self.hidden_progress = self.slide_from + (self.hidden_target - self.slide_from) * eased
			if progress >= 1.0:
				self.hidden_progress = self.hidden_target
				self.slide_started_at = None
			else:
				animations_running = True

		for item in self.items:
			animations_running = item.update_bounce(now) or animations_running

		if animations_running or self.slide_started_at is not None or self.layout_dirty:
			self.layout_panel()
			self.layout_dirty = False

		if animations_running or self.slide_started_at is not None:
			return True

		if self.frame_source is not None:
			self.remove_tick_callback(self.frame_source)
			self.frame_source = None
		return False

	def launch_desktop_app(self, dock_app: DesktopApp) -> None:
		if dock_app.app_info is None:
			return
		try:
			dock_app.app_info.launch([], None)
		except GLib.Error:
			return

	def compute_item_widths(self) -> list[float]:
		if self.current_mouse_x is None:
			return [BASE_WIDTH for _ in self.items]

		pointer_x = self.current_mouse_x - self.panel_x
		if pointer_x < -DISTANCE_LIMIT or pointer_x > self.panel_content_width + DISTANCE_LIMIT:
			return [BASE_WIDTH for _ in self.items]

		widths = [item.current_width for item in self.items]
		if not widths:
			return widths

		for _ in range(2):
			cursor = PANEL_PADDING_X
			new_widths: list[float] = []
			item_index = 0
			for kind, entry in self.entries:
				if kind == 'divider':
					cursor += DIVIDER_SIDE_MARGIN * 2 + DIVIDER_WIDTH
					continue

				current_width = widths[item_index]
				center_x = cursor + current_width / 2
				new_widths.append(interpolate_width(pointer_x - center_x))
				cursor += current_width + ITEM_GAP
				item_index += 1
			widths = new_widths

		return widths

	def layout_panel(self) -> None:
		widths = self.compute_item_widths()
		for item, width in zip(self.items, widths, strict=False):
			item.apply_width(width)

		panel_width = PANEL_PADDING_X * 2
		item_count = 0
		for kind, entry in self.entries:
			if kind == 'divider':
				panel_width += DIVIDER_SIDE_MARGIN * 2 + DIVIDER_WIDTH
			else:
				panel_width += int(round(entry.current_width)) + ITEM_GAP
				item_count += 1
		if item_count > 0:
			panel_width -= ITEM_GAP  # no trailing gap after last item
		self.panel_content_width = max(0, panel_width - PANEL_PADDING_X * 2)

		panel_height = ITEM_SLOT_HEIGHT + PANEL_PADDING_Y * 2
		window_width, window_height = self.update_window_geometry(panel_width, panel_height)
		allocated_width = self.get_width()
		if allocated_width > 0:
			window_width = allocated_width
		allocated_height = self.get_height()
		if allocated_height > 0:
			window_height = allocated_height

		self.panel_x = max(0, (window_width - panel_width) // 2)
		visible_y = window_height - panel_height - BOTTOM_MARGIN
		self.panel_y = int(round(visible_y + panel_height * self.hidden_progress))

		self.panel.set_size_request(panel_width, panel_height)
		self.root.move(self.panel, self.panel_x, self.panel_y)

		cursor = PANEL_PADDING_X
		divider_y = PANEL_PADDING_Y + int(panel_height * 0.2)
		item_index = 0
		for kind, entry in self.entries:
			if kind == 'divider':
				self.panel.move(entry, cursor + DIVIDER_SIDE_MARGIN, divider_y)
				cursor += DIVIDER_SIDE_MARGIN * 2 + DIVIDER_WIDTH
				continue

			item_width = int(round(widths[item_index]))
			self.panel.move(entry, cursor, PANEL_PADDING_Y)
			cursor += item_width + ITEM_GAP
			item_index += 1


class LucidDockApplication(Gtk.Application):
	def __init__(self) -> None:
		super().__init__(application_id='dev.lucidos.Dock', flags=Gio.ApplicationFlags.NON_UNIQUE)
		self._window: DockWindow | None = None
		self.connect('activate', self._on_activate)

	def _on_activate(self, _app: Gtk.Application) -> None:
		try:
			if self._window is not None:
				self._window.present()
				return
			dock_apps, state = load_system_dock_apps()
			self._window = DockWindow(self, dock_apps, state)
			self._window.present()
		except Exception:
			import traceback
			traceback.print_exc()
			self.quit()


def main() -> int:
	app = LucidDockApplication()
	return app.run(None)


if __name__ == '__main__':
	raise SystemExit(main())