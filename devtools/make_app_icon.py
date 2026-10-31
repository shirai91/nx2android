# The desktop app's icon from docs/images/icon.svg: gui/icon.png (the window icon, 256 px) and packaging/nx2android.ico (the Windows exe, 16-256 px).   make_app_icon.py
# Run it again after changing icon.svg. Needs PySide6 (requirements.txt) and Pillow.
import io, sys
from pathlib import Path
from PIL import Image
from PySide6.QtCore import QBuffer, QIODevice
from PySide6.QtGui import QGuiApplication, QImage, QPainter, Qt
from PySide6.QtSvg import QSvgRenderer

ROOT = Path(__file__).resolve().parent.parent
app = QGuiApplication(sys.argv)
svg = QSvgRenderer(str(ROOT / 'docs/images/icon.svg'))


def render(px):
    img = QImage(px, px, QImage.Format_ARGB32); img.fill(Qt.transparent)
    p = QPainter(img); p.setRenderHint(QPainter.Antialiasing); svg.render(p); p.end()
    buf = QBuffer(); buf.open(QIODevice.WriteOnly); img.save(buf, 'PNG')
    return Image.open(io.BytesIO(bytes(buf.data()))).convert('RGBA')


big = render(256)
big.save(ROOT / 'gui/icon.png')
big.save(ROOT / 'packaging/nx2android.ico', sizes=[(s, s) for s in (16, 24, 32, 48, 64, 128, 256)])
print(ROOT / 'gui/icon.png', ROOT / 'packaging/nx2android.ico')
