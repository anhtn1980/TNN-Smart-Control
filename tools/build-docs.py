#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
build-docs.py — Sinh bản HTML đọc đẹp từ các file Markdown của dự án.

Mục đích: cho phép đọc tài liệu (.md) bằng trình duyệt cho dễ nhìn, MÀ KHÔNG
sửa tay HTML — .md vẫn là nguồn duy nhất. Chạy lại script mỗi khi .md đổi.

Đặc điểm:
- KHÔNG phụ thuộc thư viện ngoài (chỉ cần python3) — chạy offline, không pip.
- Hỗ trợ subset GitHub-Flavored Markdown mà tài liệu dự án dùng:
  tiêu đề, bảng, danh sách (có lồng), code fence/inline, **đậm**, *nghiêng*,
  ~~gạch~~, [link](url), trích dẫn >, đường kẻ ---.
- Xuất ra docs-html/*.html (self-contained, mở bằng file:// được) + index.html.

Dùng:  python3 tools/build-docs.py
"""

import html
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT_DIR = ROOT / "docs-html"

# Các file .md ở thư mục gốc sẽ được chuyển. (Bỏ qua file trong thư mục con như tools/.)
def find_md_files():
    return sorted(p for p in ROOT.glob("*.md") if p.is_file())


# ───────────────────────── INLINE ─────────────────────────
_LINK_RE = re.compile(r"\[([^\]]+)\]\(([^)]+)\)")


def render_inline(text):
    """Chuyển markdown inline -> HTML. Bảo vệ `code` khỏi các luật khác."""
    # Tách theo đoạn code inline `...`; phần lẻ (index chẵn) là text thường,
    # phần index lẻ là nội dung code.
    parts = text.split("`")
    out = []
    for i, seg in enumerate(parts):
        if i % 2 == 1:
            out.append("<code>" + html.escape(seg) + "</code>")
        else:
            out.append(_render_text_segment(seg))
    return "".join(out)


def _render_text_segment(seg):
    seg = html.escape(seg)
    # link: [text](url) — escape lại ở trên nên url đã an toàn
    def _link(m):
        label, url = m.group(1), m.group(2)
        return '<a href="{}" target="_blank" rel="noopener">{}</a>'.format(url, label)
    seg = _LINK_RE.sub(_link, seg)
    # đậm trước, nghiêng sau
    seg = re.sub(r"\*\*([^*]+)\*\*", r"<strong>\1</strong>", seg)
    seg = re.sub(r"~~([^~]+)~~", r"<del>\1</del>", seg)
    seg = re.sub(r"(?<!\*)\*([^*\n]+)\*(?!\*)", r"<em>\1</em>", seg)
    return seg


def slugify(text):
    s = re.sub(r"<[^>]+>", "", text).strip().lower()
    s = re.sub(r"[^\w\s-]", "", s, flags=re.UNICODE)
    s = re.sub(r"[\s]+", "-", s)
    return s or "section"


# ───────────────────────── BLOCK ─────────────────────────
def md_to_html(md):
    lines = md.split("\n")
    out = []
    i = 0
    n = len(lines)

    def flush_para(buf):
        if buf:
            out.append("<p>" + render_inline(" ".join(buf).strip()) + "</p>")
            buf.clear()

    para = []

    while i < n:
        line = lines[i]
        stripped = line.strip()

        # fenced code block
        if stripped.startswith("```"):
            flush_para(para)
            i += 1
            code = []
            while i < n and not lines[i].strip().startswith("```"):
                code.append(lines[i])
                i += 1
            i += 1  # bỏ dòng ``` đóng
            out.append("<pre><code>" + html.escape("\n".join(code)) + "</code></pre>")
            continue

        # blank line
        if stripped == "":
            flush_para(para)
            i += 1
            continue

        # horizontal rule
        if re.fullmatch(r"(-{3,}|\*{3,}|_{3,})", stripped):
            flush_para(para)
            out.append("<hr>")
            i += 1
            continue

        # heading
        m = re.match(r"(#{1,6})\s+(.*)", stripped)
        if m:
            flush_para(para)
            level = len(m.group(1))
            content = render_inline(m.group(2).strip())
            sid = slugify(m.group(2))
            out.append("<h{0} id=\"{1}\">{2}</h{0}>".format(level, sid, content))
            i += 1
            continue

        # table: dòng có '|' và dòng kế là separator |---|
        if "|" in line and i + 1 < n and re.match(r"^\s*\|?[\s:|-]+\|?\s*$", lines[i + 1]) and "-" in lines[i + 1]:
            flush_para(para)
            i = _emit_table(lines, i, out)
            continue

        # blockquote
        if stripped.startswith(">"):
            flush_para(para)
            quote = []
            while i < n and lines[i].strip().startswith(">"):
                quote.append(re.sub(r"^\s*>\s?", "", lines[i]))
                i += 1
            out.append("<blockquote>" + md_to_html("\n".join(quote)) + "</blockquote>")
            continue

        # list (bullet hoặc số)
        if re.match(r"^\s*([-*+]|\d+\.)\s+", line):
            flush_para(para)
            i = _emit_list(lines, i, out)
            continue

        # paragraph
        para.append(stripped)
        i += 1

    flush_para(para)
    return "\n".join(out)


def _emit_table(lines, i, out):
    header = _split_row(lines[i])
    sep = _split_row(lines[i + 1])
    aligns = []
    for c in sep:
        c = c.strip()
        if c.startswith(":") and c.endswith(":"):
            aligns.append("center")
        elif c.endswith(":"):
            aligns.append("right")
        elif c.startswith(":"):
            aligns.append("left")
        else:
            aligns.append("")
    i += 2
    rows = []
    n = len(lines)
    while i < n and "|" in lines[i] and lines[i].strip() != "":
        rows.append(_split_row(lines[i]))
        i += 1

    def cell(tag, val, idx):
        a = aligns[idx] if idx < len(aligns) else ""
        style = ' style="text-align:{}"'.format(a) if a else ""
        return "<{0}{1}>{2}</{0}>".format(tag, style, render_inline(val.strip()))

    out.append("<table>")
    out.append("<thead><tr>" + "".join(cell("th", c, j) for j, c in enumerate(header)) + "</tr></thead>")
    out.append("<tbody>")
    for r in rows:
        out.append("<tr>" + "".join(cell("td", c, j) for j, c in enumerate(r)) + "</tr>")
    out.append("</tbody></table>")
    return i


def _split_row(line):
    line = line.strip()
    if line.startswith("|"):
        line = line[1:]
    if line.endswith("|"):
        line = line[:-1]
    # tách theo | nhưng tôn trọng \| (hiếm trong tài liệu này)
    return [c.replace("\\|", "|") for c in line.split("|")]


def _emit_list(lines, i, out):
    """Xử lý danh sách có lồng theo mức thụt đầu dòng."""
    n = len(lines)
    # thu thập các dòng thuộc danh sách (đến khi gặp dòng trống + không tiếp tục, hoặc block khác)
    items = []  # (indent, ordered, content)
    while i < n:
        line = lines[i]
        m = re.match(r"^(\s*)([-*+]|\d+\.)\s+(.*)", line)
        if m:
            indent = len(m.group(1).expandtabs(4))
            ordered = bool(re.match(r"\d+\.", m.group(2)))
            items.append((indent, ordered, m.group(3)))
            i += 1
        elif line.strip() == "":
            # cho phép dòng trống nếu dòng kế vẫn là list item
            if i + 1 < n and re.match(r"^\s*([-*+]|\d+\.)\s+", lines[i + 1]):
                i += 1
                continue
            break
        else:
            break
    _render_list_items(items, 0, out)
    return i


def _render_list_items(items, pos, out):
    """Render đệ quy theo mức thụt; trả về vị trí kế tiếp."""
    if pos >= len(items):
        return pos
    base_indent = items[pos][0]
    ordered = items[pos][1]
    out.append("<ol>" if ordered else "<ul>")
    while pos < len(items):
        indent, od, content = items[pos]
        if indent < base_indent:
            break
        if indent > base_indent:
            # danh sách con — gắn vào item trước đó
            pos = _render_list_items(items, pos, out)
            continue
        out.append("<li>" + render_inline(content.strip()))
        # nếu item kế thụt sâu hơn -> sublist nằm trong <li> này
        if pos + 1 < len(items) and items[pos + 1][0] > base_indent:
            pos = _render_list_items(items, pos + 1, out)
            out.append("</li>")
        else:
            out.append("</li>")
            pos += 1
    out.append("</ol>" if ordered else "</ul>")
    return pos


# ───────────────────────── PAGE ─────────────────────────
CSS = """
:root{--bg:#0d1117;--panel:#161b22;--border:#30363d;--fg:#e6edf3;--muted:#8b949e;
--accent:#58a6ff;--code:#79c0ff;--codebg:#161b22}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);
font-family:-apple-system,Segoe UI,Roboto,Helvetica,Arial,sans-serif;
line-height:1.7;font-size:16px}
.wrap{max-width:880px;margin:0 auto;padding:32px 20px 80px}
.nav{max-width:880px;margin:0 auto;padding:14px 20px;border-bottom:1px solid var(--border)}
.nav a{color:var(--muted);text-decoration:none;font-size:14px}
.nav a:hover{color:var(--accent)}
h1,h2,h3,h4,h5,h6{line-height:1.3;margin:1.6em 0 .6em;font-weight:600}
h1{font-size:1.9em;border-bottom:1px solid var(--border);padding-bottom:.3em}
h2{font-size:1.5em;border-bottom:1px solid var(--border);padding-bottom:.3em}
h3{font-size:1.25em}
a{color:var(--accent)}
p{margin:.7em 0}
ul,ol{padding-left:1.6em;margin:.6em 0}
li{margin:.25em 0}
code{background:var(--codebg);color:var(--code);padding:.15em .4em;border-radius:5px;
font-family:SFMono-Regular,Consolas,Liberation Mono,monospace;font-size:.88em;
border:1px solid var(--border)}
pre{background:var(--codebg);border:1px solid var(--border);border-radius:8px;
padding:14px 16px;overflow:auto}
pre code{background:none;border:none;padding:0;color:var(--fg)}
blockquote{border-left:3px solid var(--accent);margin:.8em 0;padding:.2em 14px;
color:var(--muted);background:var(--panel);border-radius:0 6px 6px 0}
hr{border:none;border-top:1px solid var(--border);margin:1.8em 0}
table{border-collapse:collapse;width:100%;margin:1em 0;font-size:.95em;
display:block;overflow-x:auto}
th,td{border:1px solid var(--border);padding:8px 12px;text-align:left}
th{background:var(--panel);font-weight:600}
tr:nth-child(even) td{background:rgba(255,255,255,.02)}
.gen{color:var(--muted);font-size:12px;margin-top:40px;border-top:1px solid var(--border);
padding-top:14px}
"""

PAGE = """<!DOCTYPE html>
<html lang="vi"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>{title}</title>
<style>{css}</style></head>
<body>
<div class="nav">{nav}</div>
<div class="wrap">
{body}
<div class="gen">Tự sinh từ <code>{src}</code> bằng <code>tools/build-docs.py</code> — không sửa tay file này.</div>
</div>
</body></html>
"""


def build():
    md_files = find_md_files()
    if not md_files:
        print("Không tìm thấy file .md nào ở thư mục gốc.")
        return 1

    OUT_DIR.mkdir(exist_ok=True)
    nav_links = ['<a href="index.html">🏠 Mục lục</a>'] + [
        '<a href="{}.html">{}</a>'.format(p.stem, p.name) for p in md_files
    ]
    nav = " &nbsp;·&nbsp; ".join(nav_links)

    for p in md_files:
        md = p.read_text(encoding="utf-8")
        body = md_to_html(md)
        page = PAGE.format(title=p.name, css=CSS, nav=nav, body=body, src=p.name)
        (OUT_DIR / (p.stem + ".html")).write_text(page, encoding="utf-8")
        print("✓ {} -> docs-html/{}.html".format(p.name, p.stem))

    # index
    items = "\n".join(
        '<li><a href="{}.html">{}</a></li>'.format(p.stem, p.name) for p in md_files
    )
    index_body = "<h1>📚 Tài liệu TNN Smart Control</h1><ul>{}</ul>".format(items)
    index = PAGE.format(title="Mục lục tài liệu", css=CSS, nav=nav,
                        body=index_body, src="*.md")
    (OUT_DIR / "index.html").write_text(index, encoding="utf-8")
    print("✓ index.html")
    print("\nXong. Mở docs-html/index.html bằng trình duyệt để đọc.")
    return 0


if __name__ == "__main__":
    sys.exit(build())
