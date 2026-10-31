# The nx2android desktop app: four tabs over porter.
#   Port   pick the game folder (porter inspect), options, Port (porter port --progress json): the stage list, the current stage's bar, the log; then open or install the result
#   Setup  the tools a port needs (porter doctor), downloading the missing ones (porter tools --download), and the saved settings (porter.config): prod.keys, the NDK,
#          dotnet, adb, the output folder, the device
#   Games  the game profiles (porter profiles --all)
#   Saves  a ported game's save between the device and the PC (porter saves): the save folders on the device, a fresh export to a folder here, an import of a
#          folder or .zip (a console's save, or an export)
import os, subprocess, sys, time
from pathlib import Path

from PySide6.QtCore import Qt, QTimer, QUrl
from PySide6.QtGui import QColor, QDesktopServices, QFont, QIcon
from PySide6.QtWidgets import (QApplication, QCheckBox, QComboBox, QFileDialog, QFormLayout, QFrame, QGridLayout, QGroupBox, QHBoxLayout, QHeaderView, QLabel, QLineEdit,
                               QMainWindow, QMessageBox, QPlainTextEdit, QProgressBar, QPushButton, QSpinBox, QSplitter, QStyle, QTableWidget, QTableWidgetItem,
                               QTabWidget, QTreeWidget, QTreeWidgetItem, QVBoxLayout, QWidget)

from porter import config
from gui.runner import Porter, call_json

LANGUAGES = [('', 'English (default)'), ('ja', 'Japanese'), ('fr', 'French'), ('de', 'German'), ('it', 'Italian'), ('es', 'Spanish'), ('nl', 'Dutch'), ('pt', 'Portuguese'),
             ('ru', 'Russian'), ('ko', 'Korean'), ('zh-Hans', 'Chinese (Simplified)'), ('zh-Hant', 'Chinese (Traditional)')]
STATUS_COLOUR = {'playable': '#2e7d32', 'wip': '#ef6c00', 'boots': '#ef6c00', 'experimental': '#ef6c00', 'ios only': '#757575'}


def hms(s):
    s = int(s or 0); return f'{s // 3600}:{s // 60 % 60:02d}:{s % 60:02d}' if s >= 3600 else f'{s // 60}:{s % 60:02d}'


def size(b):
    for u, d in (('GB', 2**30), ('MB', 2**20), ('KB', 2**10)):
        if b >= d: return f'{b / d:.1f} {u}'
    return f'{b} B'


def path_row(edit, pick):   # a line edit with a Browse button
    w = QWidget(); h = QHBoxLayout(w); h.setContentsMargins(0, 0, 0, 0)
    b = QPushButton('Browse…'); b.clicked.connect(pick)
    h.addWidget(edit, 1); h.addWidget(b)
    return w


def with_info(field, tip):   # a field with the style's information icon after it, which shows tip on hover (the field shows it too)
    w = QWidget(); h = QHBoxLayout(w); h.setContentsMargins(0, 0, 0, 0)
    i = QLabel(); i.setPixmap(QApplication.style().standardIcon(QStyle.SP_MessageBoxInformation).pixmap(16, 16)); i.setCursor(Qt.WhatsThisCursor)
    i.setToolTip(tip); field.setToolTip(tip)
    h.addWidget(field); h.addWidget(i); h.addStretch(1)
    return w


# ---------------- Port ----------------
class PortTab(QWidget):
    def __init__(self, win):
        super().__init__()
        self.win, self.info, self.result, self.items, self.t0 = win, None, None, {}, 0
        self.porter = Porter(self); self.porter.event.connect(self.on_event); self.porter.finished.connect(self.on_finished)
        v = QVBoxLayout(self)

        self.folder = QLineEdit(); self.folder.setPlaceholderText('the folder with the game\'s NSP / XCI / NSZ / XCZ files (base game, update, DLC)')
        self.folder.editingFinished.connect(self.inspect)
        g = QGroupBox('Game'); gl = QVBoxLayout(g)
        gl.addWidget(path_row(self.folder, self.pick_folder))
        self.card = QLabel('Choose a game folder.'); self.card.setWordWrap(True); self.card.setTextFormat(Qt.RichText); self.card.setMinimumHeight(60)
        gl.addWidget(self.card)
        v.addWidget(g)

        o = QGroupBox('Options'); f = QFormLayout(o)
        self.app_name = QLineEdit(); self.app_name.setPlaceholderText('the game\'s name')
        self.language = QComboBox()
        for code, name in LANGUAGES: self.language.addItem(name, code)
        self.out = QLineEdit(); self.out.setPlaceholderText('the game folder')
        self.install = QCheckBox('Install on the device when done')
        self.device = QComboBox(); self.device.setMinimumWidth(260)
        refresh = QPushButton('Refresh'); refresh.clicked.connect(self.refresh_devices)
        dev = QWidget(); dh = QHBoxLayout(dev); dh.setContentsMargins(0, 0, 0, 0); dh.addWidget(self.install); dh.addWidget(self.device, 1); dh.addWidget(refresh)
        self.replace_signer = QCheckBox('Replace an app of the same package signed with another key (uninstalls it first: its saves are deleted)')
        self.package = QLineEdit(); self.package.setPlaceholderText('the profile\'s package')   # porter port --package: a test install beside the player's other build
        self.package.setToolTip('Install under another package than the profile\'s (org.nx2a.<game>.test), beside your other build of the game.\n'
                                'An app of another package has its own saves.')
        self.fps60 = QCheckBox('60 fps (the profile\'s own frame-rate cap, else the community\'s FPSLocker-Warehouse config of the game\'s build, fetched from GitHub)')
        self.touch = QCheckBox('Touch game: start with the on-screen gamepad hidden, so every touch but the menu button\'s goes to the game (the pause menu shows it again)')   # porter port --touch
        self.debug = QCheckBox('Development build (debuggable, debug information, DM_CTL key presses)')
        self.experimental = QCheckBox('Experimental: try a game that is not ported to Android yet (it may not start)')
        self.experimental.toggled.connect(lambda _: self.update_port_button())
        self.vercode = QSpinBox(); self.vercode.setRange(0, 65535); self.vercode.setSpecialValueText('none'); self.vercode.setMaximumWidth(120)   # porter port --vercode-offset (0: none, or $NX2A_VERCODE_OFFSET)
        self.vercode.setToolTip('Added to the APK\'s versionCode. Android does not install an APK over one with a higher versionCode;\n'
                                'raise this to install a rebuild of the same game version over an earlier rebuild that had an offset, and keep its saves.')
        self.headroom = QSpinBox(); self.headroom.setRange(-1, 48); self.headroom.setSpecialValueText('Auto'); self.headroom.setValue(-1); self.headroom.setSuffix(' GB')
        self.headroom.setMaximumWidth(120)   # porter port --alias-headroom (Auto: none passed)
        headroom = with_info(self.headroom, 'Extra address space (the alias region) for a game that takes its memory as address regions.\n'
                             'Only for a game that crashes and whose log shows "AllocateAddressRegion refused (0x1803)".\n'
                             'Auto: 8 GB for a game version the profile was not tested with, none for a tested one.\n'
                             'Raise it (16, 24 …) if the game still runs out. It uses no memory and does not change the speed.')
        f.addRow('App name', self.app_name); f.addRow('Game language', self.language)
        f.addRow('Output folder', path_row(self.out, self.pick_out)); f.addRow('Device', dev); f.addRow('', self.replace_signer)
        f.addRow('Package', self.package); f.addRow('versionCode offset', self.vercode); f.addRow('Extra address space', headroom)
        f.addRow('', self.fps60); f.addRow('', self.touch); f.addRow('', self.debug); f.addRow('', self.experimental)
        v.addWidget(o)

        bar = QHBoxLayout()
        self.port_btn = QPushButton('Port'); self.port_btn.setEnabled(False); self.port_btn.setMinimumHeight(36)
        f2 = self.port_btn.font(); f2.setPointSize(f2.pointSize() + 2); f2.setBold(True); self.port_btn.setFont(f2)
        self.port_btn.clicked.connect(self.start)
        self.cancel_btn = QPushButton('Cancel'); self.cancel_btn.setEnabled(False); self.cancel_btn.clicked.connect(self.cancel)
        self.open_btn = QPushButton('Open output folder'); self.open_btn.setEnabled(False); self.open_btn.clicked.connect(self.open_out)
        self.inst_btn = QPushButton('Install on device'); self.inst_btn.setEnabled(False); self.inst_btn.clicked.connect(self.install_now)
        for b in (self.port_btn, self.cancel_btn): bar.addWidget(b)
        bar.addStretch(1)
        for b in (self.open_btn, self.inst_btn): bar.addWidget(b)
        v.addLayout(bar)

        self.step = QLabel(''); self.prog = QProgressBar(); self.prog.setTextVisible(True); self.prog.setRange(0, 1000); self.prog.setValue(0)
        v.addWidget(self.step); v.addWidget(self.prog)
        split = QSplitter(Qt.Vertical)
        self.stages = QTreeWidget(); self.stages.setHeaderLabels(['', 'Stage', 'Result', 'Time']); self.stages.setRootIsDecorated(False)
        hh = self.stages.header(); hh.setSectionResizeMode(0, QHeaderView.ResizeToContents); hh.setSectionResizeMode(1, QHeaderView.ResizeToContents); hh.setSectionResizeMode(2, QHeaderView.Stretch)
        self.log = QPlainTextEdit(); self.log.setReadOnly(True); self.log.setFont(QFont('Consolas' if os.name == 'nt' else 'Menlo', 9)); self.log.setMaximumBlockCount(20000)
        split.addWidget(self.stages); split.addWidget(self.log); split.setSizes([180, 220])
        v.addWidget(split, 1)
        self.tick = QTimer(self); self.tick.timeout.connect(self.on_tick); self.tick.start(1000)
        self.refresh_devices()

    # ---- the game folder ----
    def pick_folder(self):
        d = QFileDialog.getExistingDirectory(self, 'Game folder', self.folder.text() or str(Path.home()))
        if d: self.folder.setText(d); self.inspect()

    def pick_out(self):
        d = QFileDialog.getExistingDirectory(self, 'Output folder', self.out.text() or self.folder.text() or str(Path.home()))
        if d: self.out.setText(d)

    def inspect(self):
        d = self.folder.text().strip()
        self.info = None; self.port_btn.setEnabled(False)
        if not d: return
        if not Path(d).is_dir(): self.card.setText(f'<span style="color:#c62828">Not a folder: {d}</span>'); return
        self.card.setText('Reading the game folder…')
        call_json(self, ['inspect', d, '--json'], self.on_inspect)

    def on_inspect(self, info, code):
        err = info
        self.info = info = None if info and info.get('error') else info
        if not info: self.card.setText('<span style="color:#c62828">' + ((err or {}).get('error') or 'No game found in this folder (an NSP, XCI, NSZ or XCZ with a title ID).') + '</span>'); return
        a = info.get('android') or {}
        st = a.get('status', 'ios only' if info.get('profile') else 'not supported')
        col = STATUS_COLOUR.get(st, '#c62828')
        rows = [(f'<b>{info["name"]}</b> &nbsp; <span style="color:#757575">{info["title_id"]}</span>' if info.get('name') else f'<b>{info["title_id"]}</b>') + ' &nbsp; '
                f'<span style="background:{col};color:white;padding:1px 6px">&nbsp;{st}&nbsp;</span>']
        files = [f'base game' ] + [f'update {u["version"]}' for u in info['updates']] + ([f'{len(info["dlc"])} DLC'] if info['dlc'] else [])
        rows.append('In the folder: ' + ', '.join(files) + (f' — packs <b>{info["pack"]}</b>' if info.get('pack') else ''))
        if info.get('compressed'): rows.append(f'<span style="color:#757575">NSZ / XCZ ({len(info["compressed"])}): decompressed during the port into a temporary folder, deleted once extracted</span>')
        if a.get('tested'): rows.append(f'Tested: {a["tested"]}')
        if info.get('reason'): rows.append(f'<span style="color:{"#ef6c00" if info["supported"] else "#c62828"}">{info["reason"]}</span>')
        self.card.setText('<br>'.join(rows))
        self.app_name.setPlaceholderText(info.get('name') or 'the game\'s name')
        self.package.setPlaceholderText(a.get('package') or 'the profile\'s package')
        self.update_port_button()

    def update_port_button(self):   # a supported game, or any game with Experimental ticked
        i = self.info or {}
        self.port_btn.setEnabled(bool(i.get('supported') or (i.get('experimental') and self.experimental.isChecked())) and not self.porter.running())

    def refresh_devices(self):
        cur = self.device.currentData() or config.load().get('device')
        def got(rows, code):
            self.device.clear()
            for r in rows or []:
                if r['state'] == 'device': self.device.addItem(f'{r["serial"]}  {r["info"]}', r['serial'])
            if not self.device.count(): self.device.addItem('no device connected', '')
            i = self.device.findData(cur)
            if i >= 0: self.device.setCurrentIndex(i)
        call_json(self, ['devices', '--json'], got)

    # ---- the port ----
    def start(self):
        cfg = config.load()
        if not cfg.get('keys'):
            QMessageBox.warning(self, 'prod.keys', 'Set the path of your prod.keys on the Setup tab first.'); self.win.tabs.setCurrentIndex(1); return
        args = ['port', self.folder.text().strip(), '--progress', 'json']
        if self.app_name.text().strip(): args += ['--app-name', self.app_name.text().strip()]
        if self.language.currentData(): args += ['--language', self.language.currentData()]
        if self.out.text().strip(): args += ['--out', self.out.text().strip()]
        if self.install.isChecked():
            args.append('--install')
            if self.device.currentData(): args += ['--device', self.device.currentData()]
            if self.replace_signer.isChecked(): args.append('--replace-signer')
        if self.package.text().strip(): args += ['--package', self.package.text().strip()]
        if self.vercode.value(): args += ['--vercode-offset', str(self.vercode.value())]
        if self.headroom.value() >= 0: args += ['--alias-headroom', str(self.headroom.value())]
        if self.fps60.isChecked(): args.append('--fps60')
        if self.touch.isChecked(): args.append('--touch')
        if self.debug.isChecked(): args.append('--debug')
        if self.experimental.isChecked() and not self.info.get('supported'): args.append('--experimental')
        self.run(args)

    def install_now(self):
        if not self.result: return
        args = ['install', self.result['data'], '--apk', self.result['apk'], '--progress', 'json']
        if self.device.currentData(): args += ['--device', self.device.currentData()]
        if self.replace_signer.isChecked(): args.append('--replace-signer')
        self.run(args)

    def run(self, args):
        self.stages.clear(); self.items = {}; self.log.clear(); self.prog.setValue(0); self.step.setText('starting…')
        self.result = None; self.t0 = time.time(); self.cancelled = False
        for b in (self.port_btn, self.open_btn, self.inst_btn): b.setEnabled(False)
        self.cancel_btn.setEnabled(True)
        self.append('$ porter ' + ' '.join(f'"{a}"' if ' ' in a else a for a in args))
        self.porter.start(args)

    def cancel(self):
        self.cancelled = True; self.porter.cancel(); self.append('[RED] cancelled')

    def append(self, msg):
        red = msg.startswith('[RED] ')
        self.log.appendHtml(f'<span style="color:{"#c62828" if red else "inherit"}">{msg.removeprefix("[RED] ").replace("&", "&amp;").replace("<", "&lt;").replace(chr(10), "<br>")}</span>')

    def item(self, k, name):
        it = self.items.get(k)
        if not it:
            it = QTreeWidgetItem(['…', name, '', '']); self.stages.addTopLevelItem(it); self.items[k] = it
        return it

    def on_event(self, ev):
        e = ev.get('ev')
        if e == 'stage':
            self.cur = ev; it = self.item(ev['k'], ev['name']); it.setText(0, '▶'); it.setData(0, Qt.UserRole, time.time())
            self.step.setText(f'[{ev["k"]}/{ev["n"]}] {ev["name"]}' + (f' · {ev["step"]}' if ev.get('step') else '')); self.prog.setRange(0, 0)
        elif e == 'step':
            self.step.setText(f'{ev["name"]} · {ev["step"]}'); self.prog.setRange(0, 0)
        elif e == 'progress':
            done, total = ev.get('done'), ev.get('total')
            count = '' if done is None else (f'{size(done)} / {size(total)}' if ev.get('unit') == 'B' and total else f'{done:,}' + (f' / {total:,}' if total else '') + (f' {ev["unit"]}' if ev.get('unit') else ''))
            self.step.setText(f'{ev["name"]}' + (f' · {ev["step"]}' if ev.get('step') else '') + (f' · {count}' if count else '') + (f' · {ev["note"]}' if ev.get('note') else '')
                              + (f' · ETA {hms(ev["eta"])}' if ev.get('eta') else ''))
            if ev.get('frac') is not None: self.prog.setRange(0, 1000); self.prog.setValue(int(ev['frac'] * 1000))
            else: self.prog.setRange(0, 0)
        elif e == 'stage_end':
            it = self.item(ev['k'], ev['name']); ok = ev.get('ok', True)
            it.setText(0, '✓' if ok else '✗'); it.setForeground(0, QColor('#2e7d32' if ok else '#c62828'))
            it.setText(2, ev.get('summary', '')); it.setText(3, hms(ev.get('elapsed')))
            self.prog.setRange(0, 1000); self.prog.setValue(1000 if ok else 0)
        elif e == 'say':
            self.append(ev.get('msg', ''))
        elif e == 'result':
            self.result = ev

    def on_tick(self):   # the running stage's elapsed time
        if not self.porter.running(): return
        for it in self.items.values():
            t = it.data(0, Qt.UserRole)
            if it.text(0) == '▶' and t: it.setText(3, hms(time.time() - t))

    def on_finished(self, code):
        self.cancel_btn.setEnabled(False)
        self.update_port_button()
        took = hms(time.time() - self.t0)
        if code == 0:
            self.step.setText(f'done in {took}'); self.prog.setRange(0, 1000); self.prog.setValue(1000)
            if self.result: self.open_btn.setEnabled(True); self.inst_btn.setEnabled(True)
        else:
            self.step.setText('cancelled' if self.cancelled or code == 130 else f'failed after {took} (the log says why)')
            self.prog.setRange(0, 1000); self.prog.setValue(0)

    def open_out(self):
        if self.result: QDesktopServices.openUrl(QUrl.fromLocalFile(str(Path(self.result['apk']).parent)))


# ---------------- Setup ----------------
class SetupTab(QWidget):
    FIELDS = [('keys', 'prod.keys', 'file'), ('ndk', 'Android NDK folder', 'dir'), ('dotnet', 'dotnet', 'file'), ('adb', 'adb', 'file'),
              ('out', 'Default output folder', 'dir'), ('device', 'Device (adb serial)', 'text')]

    def __init__(self, win):
        super().__init__()
        self.win = win
        v = QVBoxLayout(self)
        g = QGroupBox('Requirements'); gl = QVBoxLayout(g)
        self.table = QTableWidget(0, 3); self.table.setHorizontalHeaderLabels(['', 'Requirement', 'Found']); self.table.verticalHeader().setVisible(False)
        self.table.setEditTriggers(QTableWidget.NoEditTriggers); self.table.setSelectionMode(QTableWidget.NoSelection)
        hh = self.table.horizontalHeader(); hh.setSectionResizeMode(0, QHeaderView.ResizeToContents); hh.setSectionResizeMode(1, QHeaderView.ResizeToContents); hh.setSectionResizeMode(2, QHeaderView.Stretch)
        gl.addWidget(self.table)
        note = QLabel('A port needs the <b>Android NDK r27c</b>, the <b>.NET SDK 10</b> and, to install from here, Android <b>platform-tools</b>. '
                      '<b>Download missing tools</b> fetches them from Google and Microsoft (about 1 GB) into your nx2android folder; or install them yourself and set their paths below. '
                      'Your <b>prod.keys</b> must come from your own Switch. Keys and game files never leave your computer except to your own device.')
        note.setWordWrap(True); note.setTextFormat(Qt.RichText); gl.addWidget(note)
        dl = QHBoxLayout(); self.dl_btn = QPushButton('Download missing tools'); self.dl_btn.clicked.connect(self.download)
        self.dl_step = QLabel(''); self.dl_prog = QProgressBar(); self.dl_prog.setRange(0, 1000); self.dl_prog.setVisible(False)
        dl.addWidget(self.dl_btn); dl.addWidget(self.dl_step, 1); dl.addWidget(self.dl_prog, 1); gl.addLayout(dl)
        self.fetch = Porter(self); self.fetch.event.connect(self.on_fetch); self.fetch.finished.connect(self.fetched)
        v.addWidget(g)
        s = QGroupBox('Settings'); f = QFormLayout(s); self.edits = {}
        for key, label, kind in self.FIELDS:
            e = QLineEdit(); self.edits[key] = e
            e.setPlaceholderText({'ndk': 'found automatically', 'dotnet': 'found automatically', 'adb': 'found automatically', 'out': 'the game folder', 'device': 'the only connected device'}.get(key, ''))
            f.addRow(label, e if kind == 'text' else path_row(e, lambda _=False, k=key, kd=kind: self.pick(k, kd)))
        row = QHBoxLayout(); save = QPushButton('Save and check'); save.clicked.connect(self.save); row.addStretch(1); row.addWidget(save)
        f.addRow('', row)
        v.addWidget(s); v.addStretch(1)
        self.load()

    # ---- downloading the missing tools ----
    def download(self):
        self.dl_btn.setEnabled(False); self.dl_step.setText('reading the download lists…')
        call_json(self, ['tools', '--json'], self.confirm_download)

    def confirm_download(self, items, code):
        self.dl_btn.setEnabled(True); self.dl_step.setText('')
        if items is None: QMessageBox.warning(self, 'Download', 'Could not read the download lists from Google and Microsoft. Check the internet connection and try again.'); return
        if not items: QMessageBox.information(self, 'Download', 'Nothing to download: every tool is there.'); return
        rows = ''.join(f'<li><b>{it["name"]}</b> {it["version"]}, {size(it["size"])}, from {it["url"].split("/")[2]} (<a href="{it["license"]}">license</a>)</li>' for it in items)
        box = QMessageBox(QMessageBox.Question, 'Download missing tools', f'<p>Download and unpack into your nx2android folder:</p><ul>{rows}</ul>'
                          '<p>They come from their makers under their own licenses: downloading them means you accept those.</p>', QMessageBox.Yes | QMessageBox.No, self)
        box.setTextFormat(Qt.RichText)
        if box.exec() != QMessageBox.Yes: return
        self.dl_btn.setEnabled(False); self.dl_prog.setVisible(True); self.dl_prog.setRange(0, 0); self.dl_errors = []
        self.fetch.start(['tools', '--download', '--progress', 'json'])

    def on_fetch(self, ev):
        e = ev.get('ev')
        if e == 'stage': self.dl_step.setText(f'[{ev["k"]}/{ev["n"]}] {ev["name"]}'); self.dl_prog.setRange(0, 0)
        elif e == 'progress':
            done, total = ev.get('done'), ev.get('total')
            self.dl_step.setText(f'{ev["name"]} · {ev.get("step") or ""}' + (f' · {size(done)} / {size(total)}' if ev.get('unit') == 'B' and total and done is not None else ''))
            if ev.get('frac') is not None: self.dl_prog.setRange(0, 1000); self.dl_prog.setValue(int(ev['frac'] * 1000))
            else: self.dl_prog.setRange(0, 0)
        elif e == 'say' and ev.get('msg', '').startswith('[RED] '): self.dl_errors.append(ev['msg'].removeprefix('[RED] '))

    def fetched(self, code):
        self.dl_btn.setEnabled(True); self.dl_prog.setVisible(False)
        self.dl_step.setText('the tools are ready' if code == 0 else 'the download failed')
        if code: QMessageBox.warning(self, 'Download', '\n'.join(self.dl_errors) or 'The download failed.')
        self.load()

    def pick(self, key, kind):
        e = self.edits[key]
        p = QFileDialog.getExistingDirectory(self, key, e.text()) if kind == 'dir' else QFileDialog.getOpenFileName(self, key, e.text())[0]
        if p: e.setText(p)

    def load(self):
        for k, v in config.load().items():
            if k in self.edits: self.edits[k].setText(v)
        self.check()

    def save(self):
        c = config.load()
        for k, e in self.edits.items(): c[k] = e.text().strip()
        config.save(c); self.check()

    def check(self):
        def got(rows, code):
            self.table.setRowCount(0)
            for r in rows or []:
                i = self.table.rowCount(); self.table.insertRow(i)
                mark = QTableWidgetItem('✓' if r['ok'] else ('✗' if r['required'] else '–'))
                mark.setForeground(QColor('#2e7d32' if r['ok'] else '#c62828' if r['required'] else '#ef6c00'))
                self.table.setItem(i, 0, mark); self.table.setItem(i, 1, QTableWidgetItem(r['name']))
                self.table.setItem(i, 2, QTableWidgetItem(r['detail'] + (f'  →  {r["fix"]}' if r['fix'] and not r['ok'] else '')))
            self.table.resizeRowsToContents()
            self.win.set_ready(bool(rows) and all(r['ok'] or not r['required'] for r in rows))
        call_json(self, ['doctor', '--json'], got)


# ---------------- Games ----------------
class GamesTab(QWidget):
    def __init__(self):
        super().__init__()
        v = QVBoxLayout(self)
        self.table = QTableWidget(0, 4); self.table.setHorizontalHeaderLabels(['Status', 'Game', 'Title ID', 'Tested on'])
        self.table.verticalHeader().setVisible(False); self.table.setEditTriggers(QTableWidget.NoEditTriggers); self.table.setSortingEnabled(True)
        hh = self.table.horizontalHeader(); hh.setSectionResizeMode(1, QHeaderView.Stretch); hh.setSectionResizeMode(2, QHeaderView.ResizeToContents); hh.setSectionResizeMode(3, QHeaderView.Stretch)
        lab = QLabel('Games ported to Android. <b>playable</b>, <b>wip</b> (plays, with parts missing) and <b>boots</b> ones can be ported; any other game can still be tried with Experimental on the Port tab.')
        lab.setWordWrap(True); lab.setTextFormat(Qt.RichText)
        v.addWidget(lab); v.addWidget(self.table)
        call_json(self, ['profiles', '--json'], self.got)

    def got(self, rows, code):
        rows = sorted(rows or [], key=lambda r: r['name'].lower())
        self.table.setSortingEnabled(False); self.table.setRowCount(len(rows))
        for i, r in enumerate(rows):
            st = QTableWidgetItem(r['status']); st.setForeground(QColor(STATUS_COLOUR.get(r['status'], '#000')))
            for j, it in enumerate((st, QTableWidgetItem(r['name']), QTableWidgetItem(r['id']), QTableWidgetItem(r['tested']))): self.table.setItem(i, j, it)
        self.table.setSortingEnabled(True)


# ---------------- Saves ----------------
class SavesTab(QWidget):
    def __init__(self):
        super().__init__()
        self.busy = None
        v = QVBoxLayout(self)
        lab = QLabel('A ported game\'s save between the device and this PC. The app keeps its exports in its files folder on the device (<code>Android/data/&lt;package&gt;/files/saves</code>), '
                     'one folder each, the save\'s files as they are: the layout a console\'s save tools copy, so a console\'s save folder imports as it is. '
                     'The game\'s pause menu has Export save and Import save too. Exporting or importing here stops the app on the device; an import exports the save it replaces there first.')
        lab.setWordWrap(True); lab.setTextFormat(Qt.RichText); v.addWidget(lab)
        row = QHBoxLayout(); self.app = QComboBox(); self.app.currentIndexChanged.connect(self.refresh)
        r = QPushButton('Refresh'); r.clicked.connect(self.load_apps)
        row.addWidget(QLabel('Game on the device:')); row.addWidget(self.app, 1); row.addWidget(r); v.addLayout(row)
        self.table = QTableWidget(0, 4); self.table.setHorizontalHeaderLabels(['Save folder', 'Made', 'Files', 'Size'])
        self.table.verticalHeader().setVisible(False); self.table.setEditTriggers(QTableWidget.NoEditTriggers); self.table.setSelectionBehavior(QTableWidget.SelectRows)
        self.table.setSelectionMode(QTableWidget.SingleSelection); self.table.horizontalHeader().setSectionResizeMode(0, QHeaderView.Stretch)
        v.addWidget(self.table, 1)
        b = QHBoxLayout()
        self.b_export = QPushButton('Export the save to the PC…'); self.b_export.clicked.connect(self.export_new)
        self.b_copy = QPushButton('Copy the chosen folder to the PC…'); self.b_copy.clicked.connect(self.export_chosen)
        self.b_import = QPushButton('Import a save folder…'); self.b_import.clicked.connect(lambda: self.import_(False))
        self.b_zip = QPushButton('Import a .zip…'); self.b_zip.clicked.connect(lambda: self.import_(True))
        for w in (self.b_export, self.b_copy, self.b_import, self.b_zip): b.addWidget(w)
        v.addLayout(b)
        self.status = QLabel(''); self.status.setWordWrap(True); v.addWidget(self.status)
        self.load_apps()

    def pkg(self): return self.app.currentData() or ''

    def set_busy(self, msg, show=True):   # msg: what runs (None: nothing); its line goes when it ends, a result written over it stays
        if msg and show: self.status.setText(msg)
        elif not msg and self.busy and self.status.text() == self.busy: self.status.setText('')
        self.busy = msg
        for w in (self.b_export, self.b_copy, self.b_import, self.b_zip, self.app): w.setEnabled(not msg and (w is self.app or bool(self.pkg())))

    def load_apps(self):
        def got(rows, code):
            cur = self.pkg(); self.app.blockSignals(True); self.app.clear()
            for pk in rows if isinstance(rows, list) else []: self.app.addItem(pk, pk)
            if not self.app.count(): self.app.addItem('no ported game on the device', '')
            i = self.app.findData(cur); self.app.setCurrentIndex(max(i, 0)); self.app.blockSignals(False)
            self.set_busy(None); self.refresh()
            if not isinstance(rows, list): self.status.setText(f'[RED] {(rows or {}).get("error", "porter saves apps failed")}')
        self.set_busy('looking for the device…'); call_json(self, ['saves', 'apps', '--json'], got)

    def refresh(self, show=True):
        self.table.setRowCount(0)
        if not self.pkg(): return
        def got(rows, code):
            self.set_busy(None)
            if not isinstance(rows, list): self.status.setText(f'[RED] {(rows or {}).get("error", "porter saves list failed")}'); return
            self.table.setRowCount(len(rows))
            for i, r in enumerate(rows):
                cells = (r['name'] + ('  (+ device save)' if r['device'] else ''), time.strftime('%Y-%m-%d %H:%M', time.localtime(r['time'])), str(r['files']), size(r['bytes']))
                for j, c in enumerate(cells):
                    it = QTableWidgetItem(c); it.setData(Qt.UserRole, r['name']); self.table.setItem(i, j, it)
            if not rows: self.status.setText('No save folders on the device yet: export one (here, or from the game\'s pause menu).')
        self.set_busy('reading the save folders…', show); call_json(self, ['saves', 'list', '--package', self.pkg(), '--json'], got)

    def done(self, what):
        def got(res, code):
            self.set_busy(None)
            if code or not isinstance(res, dict) or res.get('error'): self.status.setText(f'[RED] {what} failed: {(res or {}).get("error", "see porter saves for the reason")}')
            elif 'folder' in res: self.status.setText(f'Exported to {res["folder"]}'); QDesktopServices.openUrl(QUrl.fromLocalFile(str(Path(res['folder']).parent)))
            else: self.status.setText(f'Imported {res.get("name")}' + (f'; the save it replaced is {res["backup"]} on the device' if res.get('backup') else '') + '. Open the game on the device.')
            if not code: self.refresh(show=False)
        return got

    def export_new(self):
        d = QFileDialog.getExistingDirectory(self, 'Folder to put the save in')
        if not d: return
        self.set_busy(f'exporting {self.pkg()}\'s save (the app is started on the device to copy it out)…')
        call_json(self, ['saves', 'export', d, '--package', self.pkg(), '--json'], self.done('the export'))

    def export_chosen(self):
        it = self.table.item(self.table.currentRow(), 0) if self.table.currentRow() >= 0 else None
        if not it: self.status.setText('Choose a save folder in the list first.'); return
        d = QFileDialog.getExistingDirectory(self, 'Folder to put the save in')
        if not d: return
        self.set_busy(f'copying {it.data(Qt.UserRole)}…')
        call_json(self, ['saves', 'export', d, '--name', it.data(Qt.UserRole), '--package', self.pkg(), '--json'], self.done('the copy'))

    def import_(self, zipped):
        src = QFileDialog.getOpenFileName(self, 'The save as a .zip', '', 'Zip archives (*.zip)')[0] if zipped else QFileDialog.getExistingDirectory(self, 'The save folder (its files at the top)')
        if not src: return
        if QMessageBox.question(self, 'Import the save', f'{Path(src).name} becomes {self.pkg()}\'s save on the device. The app is stopped; the save it replaces is exported '
                                'on the device first (a folder ending -before-import). Go on?') != QMessageBox.Yes: return
        self.set_busy(f'importing {Path(src).name}…')
        call_json(self, ['saves', 'import', src, '--package', self.pkg(), '--json'], self.done('the import'))


class Window(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle('nx2android'); self.resize(980, 820)
        self.tabs = QTabWidget()
        self.port = PortTab(self); self.setup = SetupTab(self); self.games = GamesTab(); self.saves = SavesTab()
        self.tabs.addTab(self.port, 'Port'); self.tabs.addTab(self.setup, 'Setup'); self.tabs.addTab(self.games, 'Games'); self.tabs.addTab(self.saves, 'Saves')
        self.setCentralWidget(self.tabs)
        self.first = True

    def set_ready(self, ok):   # the first check: a missing requirement opens the Setup tab
        if self.first and not ok: self.tabs.setCurrentWidget(self.setup)
        self.first = False


def main(argv):
    app = QApplication(argv); app.setApplicationName('nx2android'); app.setStyle('Fusion')
    app.setWindowIcon(QIcon(str(Path(__file__).parent / 'icon.png')))   # docs/images/icon.svg, rendered by devtools/make_app_icon.py
    w = Window(); w.show()
    return app.exec()
