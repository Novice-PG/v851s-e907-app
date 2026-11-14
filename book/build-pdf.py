#!/usr/bin/env python3
"""Build the book as a single HTML file and print it to PDF with Chromium.

Usage:
    python3 book/build-pdf.py
    python3 book/build-pdf.py --html-only

Requires:
    - python3-markdown (pip install markdown)
    - chromium (headless), with a CJK font installed
"""

from __future__ import annotations

import argparse
import asyncio
import base64
import html as html_lib
import json
import re
import shutil
import subprocess
import tempfile
import time
import urllib.request
from pathlib import Path

import markdown

BOOK = Path(__file__).resolve().parent
ROOT = BOOK.parent
OUT_PDF = BOOK / "from-uart-to-heterogeneous-systems.pdf"
BUILD = BOOK / ".build"

CHAPTERS = sorted(BOOK.glob("[0-9][0-9]-*.md"))
APPENDICES = sorted(BOOK.glob("appendix-*.md"))

FRONT_MATTER = """
<p class="lead">
这是一本从零开始的嵌入式系统实战书。它用一块真实的异构板子
（Allwinner V851S：A7 跑 Linux，E907 跑 Zephyr），把“从按下电源
到网页可访问”这条链路完整拆开，再带你从零做一遍。
</p>

<p>
你不需要先会驱动、设备树、蓝牙或 WebSocket。你只需要会一点 C，
会用几条 Linux 命令，并且愿意先让东西跑起来，再理解它为什么能跑。
</p>

<h2>这本书适合谁</h2>

<ul>
  <li>会一点 C，但没做过嵌入式的人；</li>
  <li>会用 Linux，但没看过启动日志、设备树、UBI 的人；</li>
  <li>想理解“两颗核怎么协作”的软件工程师；</li>
  <li>想把这套系统交付出去，而不只是做个 demo 的人。</li>
</ul>

<h2>怎么读</h2>

<table class="plain">
  <tr><th>遍数</th><th>目标</th><th>做法</th></tr>
  <tr><td>第一遍</td><td>跑起来</td><td>照着做实验，不纠结每一行</td></tr>
  <tr><td>第二遍</td><td>拆开</td><td>读代码、画数据流、回答章末问题</td></tr>
  <tr><td>第三遍</td><td>自己改</td><td>做改造任务，最后完成一个综合项目</td></tr>
</table>

<h2>三个“不”</h2>

<ol>
  <li><b>不背 API。</b>知道“为什么调用它”比记住参数重要。</li>
  <li><b>不跳过实验。</b>看懂和跑通是两件事；跑通和改对是两件事。</li>
  <li><b>不害怕报错。</b>嵌入式大部分时间在定位问题，错误信息就是教材。</li>
</ol>

<h2>约定</h2>

<ul>
  <li><code>$</code> 表示在主机执行，<code>#</code> 表示在板子执行。</li>
  <li><code>&lt;board-ip&gt;</code> 是你板子的 IP，不要照抄示例。</li>
  <li>标有“危险”的操作，先在虚拟机或备份上练。</li>
  <li>每章末尾的“验收”必须能独立完成，才算过关。</li>
</ul>
"""

CSS = r"""
@page {
  size: A4;
  margin: 19mm 16mm 20mm 16mm;
}

html { font-size: 10.5pt; }

body {
  margin: 0;
  color: #1a1a1a;
  font-family: "Noto Serif CJK SC", "Noto Serif CJK TC", serif;
  line-height: 1.7;
  text-align: justify;
  hyphens: none;
}

h1, h2, h3, h4 {
  font-family: "Noto Sans CJK SC", "Noto Sans CJK TC", sans-serif;
  color: #111;
  line-height: 1.35;
  page-break-after: avoid;
  break-after: avoid;
}

h1 {
  font-size: 22pt;
  margin: 0 0 14pt 0;
  padding-bottom: 8pt;
  border-bottom: 2.5pt solid #2b5c8a;
}

h2 {
  font-size: 14.5pt;
  margin: 20pt 0 8pt 0;
  padding-left: 7pt;
  border-left: 4pt solid #2b5c8a;
}

h3 {
  font-size: 12pt;
  margin: 14pt 0 6pt 0;
}

h4 { font-size: 11pt; margin: 12pt 0 4pt 0; }

p { margin: 0 0 7pt 0; orphans: 2; widows: 2; }

a { color: #24527a; text-decoration: none; }

code, pre, kbd, samp {
  font-family: "DejaVu Sans Mono", "Noto Sans Mono CJK SC", monospace;
}

code {
  font-size: 0.88em;
  background: #f2f4f6;
  padding: 0.5pt 3pt;
  border-radius: 2pt;
}

pre {
  font-size: 8.2pt;
  line-height: 1.45;
  background: #f6f7f9;
  border: 0.6pt solid #d9dee5;
  border-left: 3pt solid #2b5c8a;
  padding: 7pt 9pt;
  margin: 8pt 0 10pt 0;
  white-space: pre-wrap;
  word-break: break-word;
  page-break-inside: auto;
  break-inside: auto;
}

pre.keep {
  page-break-inside: avoid;
  break-inside: avoid;
}

pre code {
  background: transparent;
  padding: 0;
  font-size: 1em;
}

blockquote {
  margin: 9pt 0;
  padding: 6pt 10pt;
  color: #333;
  background: #f4f8fb;
  border-left: 3pt solid #7aa7c7;
}

blockquote p:last-child { margin-bottom: 0; }

table {
  width: 100%;
  border-collapse: collapse;
  margin: 8pt 0 11pt 0;
  font-size: 8.8pt;
  line-height: 1.5;
  page-break-inside: auto;
}

th, td {
  border: 0.5pt solid #cfd6de;
  padding: 3.2pt 5pt;
  vertical-align: top;
  text-align: left;
}

th {
  background: #e9eff5;
  font-family: "Noto Sans CJK SC", sans-serif;
  font-weight: 600;
}

tr { page-break-inside: avoid; }

ul, ol { margin: 4pt 0 8pt 0; padding-left: 18pt; }
li { margin: 2pt 0; }

hr {
  border: 0;
  border-top: 0.8pt solid #c9d1da;
  margin: 16pt 0;
}

/* ---------- cover ---------- */

.cover {
  page-break-after: always;
  break-after: page;
  height: 245mm;
  display: flex;
  flex-direction: column;
  justify-content: center;
  text-align: left;
  padding: 0 6mm;
}

.cover .kicker {
  font-family: "Noto Sans CJK SC", sans-serif;
  font-size: 10pt;
  letter-spacing: 2pt;
  color: #2b5c8a;
  margin-bottom: 18pt;
}

.cover h1 {
  font-size: 34pt;
  border: none;
  margin: 0 0 8pt 0;
  padding: 0;
  line-height: 1.2;
}

.cover .sub {
  font-family: "Noto Sans CJK SC", sans-serif;
  font-size: 15pt;
  color: #444;
  margin-bottom: 26pt;
}

.cover .line {
  width: 58mm;
  border-top: 3pt solid #2b5c8a;
  margin: 0 0 24pt 0;
}

.cover .meta {
  font-size: 10pt;
  color: #555;
  line-height: 1.9;
}

.cover .meta b { color: #222; }

/* ---------- toc ---------- */

.toc {
  page-break-after: always;
  break-after: page;
}

.toc h1 { margin-bottom: 16pt; }

.toc .cols {
  display: flex;
  gap: 10mm;
  align-items: flex-start;
}

.toc ol.col {
  list-style: none;
  padding: 0;
  margin: 0;
  flex: 1 1 0;
  min-width: 0;
}

.toc li {
  font-size: 9pt;
  line-height: 1.7;
  break-inside: avoid;
  border-bottom: 0.4pt dotted #c8d0d8;
  padding: 1.5pt 0;
}

.toc li .n {
  display: inline-block;
  width: 8mm;
  color: #2b5c8a;
  font-family: "Noto Sans CJK SC", sans-serif;
  font-size: 8.5pt;
}

.toc .part {
  font-family: "Noto Sans CJK SC", sans-serif;
  font-weight: 700;
  font-size: 10pt;
  color: #2b5c8a;
  margin: 8pt 0 3pt 0;
  break-inside: avoid;
}

/* ---------- chapters ---------- */

.chapter {
  page-break-before: always;
  break-before: page;
}

.chapter > h1 {
  margin-top: 0;
}

.chapter .chapter-label {
  font-family: "Noto Sans CJK SC", sans-serif;
  font-size: 9pt;
  letter-spacing: 1.2pt;
  color: #2b5c8a;
  margin-bottom: 3pt;
}

/* ---------- text diagrams ---------- */

.art {
  margin: 9pt 0 12pt 0;
  padding: 7pt 9pt;
  background: #f6f7f9;
  border: 0.6pt solid #d9dee5;
  border-left: 3pt solid #2b5c8a;
  page-break-inside: avoid;
  break-inside: avoid;
}

.art svg { display: block; max-width: 100%; }

.front h2 { border-left: none; padding-left: 0; color: #2b5c8a; }
.front .lead { font-size: 12pt; line-height: 1.8; }

table.plain th, table.plain td { border: none; border-bottom: 0.4pt solid #dde3e9; }
"""

TOC_PARTS = [
    ("第 0 部分　起跑线", range(0, 5)),
    ("第 1 部分　硬件与启动", range(5, 11)),
    ("第 2 部分　异构双核", range(11, 18)),
    ("第 3 部分　A7 Linux", range(18, 26)),
    ("第 4 部分　多端", range(26, 32)),
    ("第 5 部分　工程化", range(32, 38)),
    ("第 6 部分　综合项目", range(38, 41)),
]


def chapter_number(path: Path) -> int | None:
    m = re.match(r"(\d{2})-", path.name)
    return int(m.group(1)) if m else None


def chapter_title(text: str, path: Path) -> str:
    for line in text.splitlines():
        if line.startswith("# "):
            return line[2:].strip()
    return path.stem


GOAL_BLOCK = re.compile(r"^(> 本章目标[^\n]*)\n(?=> \d+\.)", re.MULTILINE)

# Characters that carry the drawing, plus arrows that must stay centered
# in their cell.
BOX = set("┌┐└┘─│├┤┬┴┼")
ARROWS = set("►◄▲▼→←↑↓↔")
WIDE = set("（），：、。“”")

CELL_FONT = 9.0          # pt, ASCII cell font size
CELL_RATIO = 0.6017      # DejaVu Sans Mono advance / em


def char_width(ch: str) -> int:
    """Terminal cell width of a character."""
    if ch in BOX:
        return 1
    import unicodedata

    return 2 if unicodedata.east_asian_width(ch) in "WF" else 1


def is_cjk(ch: str) -> bool:
    return char_width(ch) == 2



VERT = set("│┌┐└┘├┤┬┴┼")


def _columns(line: str) -> list[int]:
    cols, c = [], 0
    for ch in line:
        cols.append(c)
        c += char_width(ch)
    return cols


def _vertical_columns(lines: list[str]) -> list[int]:
    """Columns where vertical borders actually are, ignoring 1-2 cell drift.

    The source diagrams were typed next to CJK text, so some borders are
    one cell off.  The true border wins the vote.
    """
    counts: dict[int, int] = {}
    for line in lines:
        for col, ch in zip(_columns(line), line):
            if ch in VERT:
                counts[col] = counts.get(col, 0) + 1
    if not counts:
        return []
    cands = sorted(counts)
    clusters: list[list[int]] = [[cands[0]]]
    for c in cands[1:]:
        if c - clusters[-1][-1] <= 1:
            clusters[-1].append(c)
        else:
            clusters.append([c])
    return [max(cl, key=lambda c: (counts[c], c)) for cl in clusters
            if max(counts[c] for c in cl) >= 2]


def snap_diagram(text: str) -> str:
    """Move drifting vertical borders onto the consensus column."""
    lines = text.rstrip("\n").split("\n")
    cols = _vertical_columns(lines)
    if not cols:
        return text
    fixed = []
    for line in lines:
        s = line
        guard = 0
        while guard < 64:
            guard += 1
            changed = False
            for i, ch in enumerate(s):
                if ch not in VERT:
                    continue
                col = sum(char_width(c) for c in s[:i])
                target = min(cols, key=lambda c: abs(c - col))
                if target == col or abs(target - col) > 3:
                    continue
                delta = target - col
                if delta > 0:
                    fill = "─" if i > 0 and s[i - 1] == "─" else " "
                    s = s[:i] + fill * delta + s[i:]
                else:
                    j = i
                    while j > 0 and s[j - 1] in " ─" and i - j < -delta:
                        j -= 1
                    if i - j == 0:
                        continue
                    s = s[:j] + s[i:]
                changed = True
                break
            if not changed:
                break
        fixed.append(s)
    return "\n".join(fixed)


def render_art_svg(text: str) -> str:
    """Typeset a text diagram on a fixed character grid.

    Box-drawing characters become real vector lines (glyphs from monospace
    fonts leave gaps and never line up with CJK text).  Every character is
    placed at its own column, so widths cannot drift.
    """
    text = snap_diagram(text)
    lines = text.rstrip("\n").split("\n")
    cols = max((sum(char_width(c) for c in line) for line in lines), default=0)
    if cols == 0:
        return ""

    s = CELL_FONT
    if cols * CELL_RATIO * s > 480:            # shrink very wide diagrams
        s = max(6.5, 480 / (cols * CELL_RATIO))
    w = CELL_RATIO * s                          # one cell
    h = 1.42 * s                                # one row
    width = cols * w
    height = len(lines) * h

    out = [
        f'<svg class="art-svg" xmlns="http://www.w3.org/2000/svg" '
        f'width="{width:.2f}pt" height="{height:.2f}pt" '
        f'viewBox="0 0 {width:.2f} {height:.2f}">'
    ]

    # ---- box-drawing characters as vector segments -------------------
    seg = {
        "─": "LR", "│": "UD",
        "┌": "RD", "┐": "LD", "└": "RU", "┘": "LU",
        "├": "UDR", "┤": "UDL", "┬": "LRD", "┴": "LRU", "┼": "LRUD",
    }
    path = []
    for r, line in enumerate(lines):
        col = 0
        for ch in line:
            if ch in seg:
                cx, cy = col * w, r * h
                xm, ym = cx + w / 2, cy + h / 2
                x0, x1, y0, y1 = cx, cx + w, cy, cy + h
                for d in seg[ch]:
                    if d == "L":
                        path.append(f"M{x0:.2f} {ym:.2f}H{xm:.2f}")
                    elif d == "R":
                        path.append(f"M{xm:.2f} {ym:.2f}H{x1:.2f}")
                    elif d == "U":
                        path.append(f"M{xm:.2f} {y0:.2f}V{ym:.2f}")
                    else:
                        path.append(f"M{xm:.2f} {ym:.2f}V{y1:.2f}")
            col += char_width(ch)
    if path:
        out.append(
            f'<path d="{" ".join(path)}" fill="none" stroke="#1a1a1a" '
            f'stroke-width="{max(0.45, s * 0.06):.2f}" '
            f'stroke-linecap="butt" shape-rendering="geometricPrecision"/>'
        )

    # ---- text runs ----------------------------------------------------
    for r, line in enumerate(lines):
        y = r * h + h / 2
        col = 0
        run = ""
        run_col = 0
        run_cjk = False
        for ch in line:
            cw = char_width(ch)
            if ch in BOX:
                if run.strip():
                    out.append(_text_run(run, run_col, y, run_cjk, w))
                run = ""
                col += cw
                continue
            if ch == " ":
                if run.strip():
                    out.append(_text_run(run, run_col, y, run_cjk, w))
                run = ""
                col += cw
                continue
            if not run:
                run = ch
                run_col = col
                run_cjk = is_cjk(ch)
            elif is_cjk(ch) != run_cjk:
                out.append(_text_run(run, run_col, y, run_cjk, w))
                run, run_col, run_cjk = ch, col, is_cjk(ch)
            else:
                run += ch
            col += cw
        if run.strip():
            out.append(_text_run(run, run_col, y, run_cjk, w))

    out.append("</svg>")
    return "".join(out)


def _text_run(run: str, col: int, y: float, cjk: bool, cell: float) -> str:
    x = col * cell
    if cjk:
        size = cell * 1.9
        return (
            f'<text x="{x:.2f}" y="{y:.2f}" font-size="{size:.2f}" '
            f'font-family="Noto Sans CJK SC, Noto Sans CJK TC, sans-serif" '
            f'text-anchor="start" dominant-baseline="central" '
            f'fill="#1a1a1a">{html_lib.escape(run)}</text>'
        )
    return (
        f'<text x="{x:.2f}" y="{y:.2f}" font-size="{CELL_FONT:.2f}" '
        f'font-family="DejaVu Sans Mono, monospace" '
        f'text-anchor="start" dominant-baseline="central" '
        f'fill="#1a1a1a" xml:space="preserve">{html_lib.escape(run)}</text>'
    )


PRE_RE = re.compile(
    r'<pre><code(?: class="language-([^"]*)")?>(.*?)</code></pre>', re.S
)
STRUCTURAL_RE = re.compile(r" {2,}|[│─┌┐└┘├┤┬┴┼]")


def is_diagram(code: str, lang: str | None) -> bool:
    """Diagrams are text blocks that rely on the character grid."""
    if lang not in (None, "text"):
        return False
    if any(c in code for c in BOX):
        return True
    lines = code.splitlines()
    if any(STRUCTURAL_RE.search(line) for line in lines) and any(
        is_cjk(c) for c in code
    ):
        return True
    return False


def md_convert(text: str) -> str:
    # A blockquote list needs a blank quoted line before it, otherwise
    # Python-Markdown renders "本章目标： 1. ... 2. ..." as one paragraph.
    text = GOAL_BLOCK.sub(r"\1\n>\n", text)
    md = markdown.Markdown(
        extensions=["extra", "tables", "fenced_code", "toc", "sane_lists"],
        extension_configs={"toc": {"permalink": False}},
    )
    html = md.convert(text)

    def replace_pre(m: re.Match) -> str:
        lang = m.group(1)
        code = html_lib.unescape(m.group(2))
        if is_diagram(code, lang):
            svg = render_art_svg(code)
            if svg:
                return f'<div class="art">{svg}</div>'
        block = m.group(0)
        # keep short code blocks on one page; long ones may still split
        if block.count("\n") <= 24:
            return block.replace("<pre>", '<pre class="keep">', 1)
        return block

    return PRE_RE.sub(replace_pre, html)


def slug_for(path: Path) -> str:
    n = chapter_number(path)
    if n is not None:
        return f"ch-{n:02d}"
    m = re.match(r"appendix-([a-z])-", path.name)
    return f"ap-{m.group(1)}" if m else path.stem


def toc_html() -> str:
    by_num = {chapter_number(p): p for p in CHAPTERS if chapter_number(p) is not None}
    rows = []
    for part, nums in TOC_PARTS:
        rows.append(f'<div class="part">{part}</div>')
        for n in nums:
            p = by_num.get(n)
            if not p:
                continue
            title = re.sub(r"^第\s*\d+\s*章\s*", "", chapter_title(p.read_text(), p))
            rows.append(
                f'<li><a href="#{slug_for(p)}"><span class="n">{n:02d}</span>{title}</a></li>'
            )
    rows.append('<div class="part">附录</div>')
    for p in APPENDICES:
        letter = p.name.split("-")[1].upper()
        title = re.sub(r"^附录\s*[A-Z]\s*[：:]\s*", "", chapter_title(p.read_text(), p))
        rows.append(
            f'<li><a href="#{slug_for(p)}"><span class="n">{letter}</span>{title}</a></li>'
        )
    # balance the two columns; only split at a part heading so a part
    # never starts at the bottom of the left column
    breaks = [i for i, r in enumerate(rows) if 'class="part"' in r and i > 0]
    mid = min(breaks, key=lambda i: abs(i - len(rows) / 2))
    left = "\n".join(rows[:mid])
    right = "\n".join(rows[mid:])
    return (
        '<div class="toc"><h1>目录</h1>'
        f'<div class="cols"><ol class="col">{left}</ol>'
        f'<ol class="col">{right}</ol></div></div>'
    )


def build_html(include_toc: bool = True) -> str:
    body = []
    body.append(
        """
<div class="cover">
  <div class="kicker">EMBEDDED SYSTEMS · FROM ZERO</div>
  <h1>从一根串口<br>到异构系统</h1>
  <div class="sub">基于 V851S、Zephyr 与 Linkr Bee 的从零实战</div>
  <div class="line"></div>
  <div class="meta">
    <b>版本</b>　第一版草稿（对应代码 v12，2026-11-14）<br>
    <b>主线</b>　A7 Linux + E907 Zephyr + UART3 + Web/BLE<br>
    <b>读者</b>　会一点 C、会用几条 Linux 命令<br>
    <b>目标</b>　能独立设计、移植、调试、交付
  </div>
</div>
"""
    )
    body.append('<section class="front" id="front"><h1>写在前面</h1>' + FRONT_MATTER + "</section>")
    if include_toc:
        body.append(toc_html())
    for p in CHAPTERS + APPENDICES:
        text = p.read_text(encoding="utf-8")
        title = chapter_title(text, p)
        # drop the first H1; we render our own label + title
        text = re.sub(r"^# .*\n", "", text, count=1)
        html = md_convert(text)
        label = "附录 " + slug_for(p).split("-")[-1].upper() if p in APPENDICES else f"第 {chapter_number(p)} 章"
        title = re.sub(r"^第\s*\d+\s*章\s*", "", title)
        title = re.sub(r"^附录\s*[A-Z]\s*[：:]\s*", "", title)
        body.append(
            f'<section class="chapter" id="{slug_for(p)}">'
            f'<div class="chapter-label">{label}</div>'
            f"<h1>{title}</h1>{html}</section>"
        )
    html = f"""<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<title>从一根串口到异构系统</title>
<style>{CSS}</style>
</head>
<body>
{''.join(body)}
</body>
</html>
"""
    return html


async def cdp_print(html_path: Path, pdf_path: Path) -> None:
    import websockets

    chrome = shutil.which("chromium") or shutil.which("chromium-browser")
    if not chrome:
        raise SystemExit("chromium not found")

    port = 9223
    profile = Path(tempfile.mkdtemp(prefix="book-pdf-"))
    proc = await asyncio.create_subprocess_exec(
        chrome,
        "--headless=new",
        "--no-sandbox",
        "--disable-gpu",
        "--disable-dev-shm-usage",
        f"--remote-debugging-port={port}",
        f"--user-data-dir={profile}",
        "about:blank",
        stdout=asyncio.subprocess.DEVNULL,
        stderr=asyncio.subprocess.DEVNULL,
    )

    try:
        for _ in range(100):
            try:
                with urllib.request.urlopen(f"http://127.0.0.1:{port}/json/list", timeout=0.5) as r:
                    pages = json.load(r)
                if pages:
                    break
            except Exception:
                await asyncio.sleep(0.1)
        else:
            raise SystemExit("chromium did not expose the debugging port")

        ws_url = pages[0]["webSocketDebuggerUrl"]
        async with websockets.connect(ws_url, max_size=200 * 1024 * 1024) as ws:
            mid = 0
            pending = {}

            async def call(method, params=None):
                nonlocal mid
                mid += 1
                my = mid
                await ws.send(json.dumps({"id": my, "method": method, "params": params or {}}))
                while True:
                    msg = json.loads(await ws.recv())
                    if msg.get("id") == my:
                        if "error" in msg:
                            raise RuntimeError(f"{method}: {msg['error']}")
                        return msg.get("result", {})
                    if "method" in msg:
                        pending[msg["method"]] = msg

            await call("Page.enable")
            await call("Runtime.enable")
            url = html_path.resolve().as_uri()
            await call("Page.navigate", {"url": url})
            for _ in range(900):
                res = await call(
                    "Runtime.evaluate",
                    {"expression": "document.readyState", "returnByValue": True},
                )
                if res.get("result", {}).get("value") == "complete":
                    break
                await asyncio.sleep(0.2)
            else:
                raise SystemExit("page load timed out")

            # wait for fonts (best effort, with a bound)
            await call(
                "Runtime.evaluate",
                {
                    "expression": "document.fonts.ready.then(() => true)",
                    "awaitPromise": True,
                    "returnByValue": True,
                },
            )

            footer = (
                '<div style="width:100%;font-size:8px;color:#666;'
                'font-family:Noto Sans CJK SC,sans-serif;text-align:center;'
                'padding:0 12mm;">'
                "<span class=pageNumber></span> / <span class=totalPages></span>"
                "</div>"
            )
            header = "<div></div>"
            params = {
                "landscape": False,
                "displayHeaderFooter": True,
                "printBackground": True,
                "paperWidth": 8.27,
                "paperHeight": 11.69,
                "marginTop": 0.55,
                "marginBottom": 0.6,
                "marginLeft": 0.63,
                "marginRight": 0.63,
                "headerTemplate": header,
                "footerTemplate": footer,
                "preferCSSPageSize": False,
                "generateDocumentOutline": True,
                "generateTaggedPDF": True,
            }
            result = await call("Page.printToPDF", params)
            pdf_path.write_bytes(base64.b64decode(result["data"]))
            print(f"wrote {pdf_path} ({pdf_path.stat().st_size:,} bytes)")
    finally:
        proc.terminate()
        try:
            await proc.wait()
        except Exception:
            pass
        shutil.rmtree(profile, ignore_errors=True)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--html-only", action="store_true")
    args = ap.parse_args()

    BUILD.mkdir(exist_ok=True)
    html = build_html()
    html_path = BUILD / "book.html"
    html_path.write_text(html, encoding="utf-8")
    print(f"wrote {html_path} ({len(html):,} chars, {len(CHAPTERS)} chapters, {len(APPENDICES)} appendices)")
    if not args.html_only:
        asyncio.run(cdp_print(html_path, OUT_PDF))


if __name__ == "__main__":
    main()
