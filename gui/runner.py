# Running porter from the GUI: one QProcess per command, its stdout read as JSON. A port (`--progress json`) is a stream of events, one object per line
# (stage, step, progress, stage_end, say, result); the other commands (`--json`) print one JSON document. Cancelling stops the whole process tree:
# a port runs dotnet, the rewriter and the NDK's clang as children of its own.
import json, os, signal, subprocess, sys

from PySide6.QtCore import QObject, QProcess, QProcessEnvironment, Signal

from porter.paths import REPO


def _porter(parent, args):   # a porter process in its own session, so cancel() can stop its children with it
    e = QProcessEnvironment.systemEnvironment()
    e.insert('PYTHONIOENCODING', 'utf-8'); e.insert('PYTHONUNBUFFERED', '1')
    p = QProcess(parent)
    p.setProgram(sys.executable); p.setArguments(['-m', 'porter', *args]); p.setWorkingDirectory(str(REPO)); p.setProcessEnvironment(e)
    if os.name != 'nt': p.setUnixProcessParameters(QProcess.UnixProcessFlag.CreateNewSession)
    return p


class Porter(QObject):
    """A long porter command (port, install) whose stdout is JSON events"""
    event = Signal(dict)
    finished = Signal(int)

    def __init__(self, parent=None):
        super().__init__(parent)
        self.p, self.buf, self.err = None, b'', []

    def running(self): return self.p is not None and self.p.state() != QProcess.NotRunning

    def start(self, args):
        self.p = _porter(self, args); self.buf, self.err = b'', []
        self.p.readyReadStandardOutput.connect(self._out)
        self.p.readyReadStandardError.connect(lambda: self.err.append(bytes(self.p.readAllStandardError()).decode('utf-8', 'replace')))
        self.p.finished.connect(self._done)
        self.p.start()

    def _out(self):
        self.buf += bytes(self.p.readAllStandardOutput())
        *lines, self.buf = self.buf.split(b'\n')
        for ln in lines:
            s = ln.decode('utf-8', 'replace').strip()
            if not s: continue
            try: ev = json.loads(s)
            except ValueError: ev = {'ev': 'say', 'msg': s}
            self.event.emit(ev if isinstance(ev, dict) else {'ev': 'say', 'msg': s})

    def _done(self, code, _status):
        if self.buf.strip(): self.buf += b'\n'; self._out()
        err = ''.join(self.err).strip()
        if code and err: self.event.emit({'ev': 'say', 'msg': '[RED] ' + err.splitlines()[-1]})   # a traceback's last line (the JSON stream has the error's message too)
        self.finished.emit(code)

    def cancel(self):
        if not self.running(): return
        if os.name == 'nt': subprocess.run(['taskkill', '/T', '/F', '/PID', str(self.p.processId())], capture_output=True)
        else: os.killpg(self.p.processId(), signal.SIGKILL)


def call_json(parent, args, done):
    """A short porter command that prints one JSON document: done(obj or None, exit code)"""
    p = _porter(parent, args)
    def fin(code, _status):
        out = bytes(p.readAllStandardOutput()).decode('utf-8', 'replace')
        try: obj = json.loads(out)
        except ValueError: obj = None
        done(obj, code); p.deleteLater()
    p.finished.connect(fin); p.start()
    return p
