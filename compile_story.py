#!/usr/bin/env python3
"""
compile_story.py -- compile story.xml into dist/story.bin

Usage:  python3 compile_story.py story.xml dist/story.bin

Output format (read by Story.cpp with read_chunk<>, see read_write_chunk.hpp):
  chunk "hdr0"  : one Header  { u32 start_node; u32 timeout_node; u32 clock; }
  chunk "str0"  : byte pool; every string below is a [begin,end) range into it
  chunk "node"  : Node[]     (see NODE_FMT below; must match Story.hpp exactly)
  chunk "styl"  : StyleRun[] (see STYLE_FMT below)

Style runs use *byte* offsets relative to the start of their node's text.
The text is restricted to ASCII so bytes == characters.

Authoring shortcut: a <look label="..."> element inside a <node> is a dead end
("look around", "remember", "try something that doesn't work"). The compiler
turns it into a real node holding the <look>'s paragraphs plus a single
"Back." choice (cost 0) to the parent, and adds a choice with the given label
to the parent. Attributes cost / if / unless / set apply to that choice.
"""
import struct
import sys
import xml.etree.ElementTree as ET

FLAG_BOLD, FLAG_PULSE, FLAG_SHAKE, FLAG_WAVE = 1, 2, 4, 8
TAG_FLAGS = {"b": FLAG_BOLD, "pulse": FLAG_PULSE, "shake": FLAG_SHAKE, "wave": FLAG_WAVE}

# color palette for <c name="..."> (r, g, b); None = "no color override"
PALETTE = {
    "red":     (235, 90, 80),
    "paper":   (222, 205, 160),
    "hat":     (240, 200, 90),
    "mop":     (200, 150, 100),
    "machine": (120, 220, 150),
    "guard":   (120, 170, 240),
    "pigeon":  (185, 175, 205),
}

MAX_CHOICES = 4
MAX_FLAGS = 32

# struct layouts -- keep in sync with Story.hpp
HEADER_FMT = "<III"
CHOICE_FMT = "<IIIIIII"            # label_begin, label_end, target, require, forbid, set, cost
NODE_FMT = "<IIIIIIIIIII" + CHOICE_FMT.lstrip("<") * MAX_CHOICES
# name_begin, name_end, form_begin, form_end, text_begin, text_end,
# style_begin, style_end, set_flags, is_ending, choice_count, choices[4]
STYLE_FMT = "<IIBBBB"              # begin, end, flags, r, g, b


class Compiler:
    def __init__(self):
        self.pool = bytearray()
        self.flag_bits = {}

    def intern(self, s):
        """append s to the string pool; return (begin, end) byte range"""
        data = s.encode("ascii")  # raises on non-ASCII -- this game only renders ASCII glyphs
        begin = len(self.pool)
        self.pool += data
        return begin, len(self.pool)

    def flags_mask(self, attr):
        mask = 0
        if not attr:
            return mask
        for name in attr.split(","):
            name = name.strip()
            if name not in self.flag_bits:
                if len(self.flag_bits) >= MAX_FLAGS:
                    raise SystemExit(f"too many flags (max {MAX_FLAGS})")
                self.flag_bits[name] = len(self.flag_bits)
            mask |= 1 << self.flag_bits[name]
        return mask

    # ---- paragraph text + inline styles ----

    def collect_segments(self, elem, flags, color, out):
        """flatten a <p> into [(text, flags, color)] segments, resolving nesting"""
        if elem.text:
            out.append((elem.text, flags, color))
        for child in elem:
            cflags, ccolor = flags, color
            if child.tag in TAG_FLAGS:
                cflags |= TAG_FLAGS[child.tag]
            elif child.tag == "c":
                name = child.get("name")
                if name not in PALETTE:
                    raise SystemExit(f"unknown color '{name}' (known: {', '.join(PALETTE)})")
                ccolor = PALETTE[name]
            else:
                raise SystemExit(f"unknown inline tag <{child.tag}>")
            self.collect_segments(child, cflags, ccolor, out)
            if child.tail:
                out.append((child.tail, flags, color))

    def paragraph(self, p):
        """returns (text, [(begin, end, flags, color)]) with whitespace collapsed"""
        segments = []
        self.collect_segments(p, 0, None, segments)
        text = ""
        runs = []
        for raw, flags, color in segments:
            begin = len(text)
            for ch in raw:
                if ch.isspace():
                    if text and not text.endswith(" "):
                        text += " "
                else:
                    text += ch
            end = len(text)
            if end > begin and (flags or color):
                runs.append([begin, end, flags, color])
        # strip trailing space (leading is impossible: the first space is never emitted)
        if text.endswith(" "):
            text = text[:-1]
            for r in runs:
                r[1] = min(r[1], len(text))
        return text, [r for r in runs if r[1] > r[0]]

    # ---- whole story ----

    def expand_looks(self, node_elems):
        """rewrite <look> children into (choice in parent, generated node); see module docstring"""
        generated = []
        for n in node_elems:
            count = 0
            for i, child in enumerate(list(n)):
                if child.tag != "look":
                    continue
                count += 1
                gen_id = f"{n.get('id')}.look{count}"
                gen = ET.Element("node", {"id": gen_id, "form": n.get("form", "")})
                for para in child.findall("p"):
                    gen.append(para)
                back = ET.SubElement(gen, "choice", {"to": n.get("id"), "cost": "0"})
                back.text = child.get("back", "Back.")
                generated.append(gen)
                attrs = {"to": gen_id, "cost": child.get("cost", "0")}
                for key in ("if", "unless", "set"):
                    if child.get(key):
                        attrs[key] = child.get(key)
                choice = ET.Element("choice", attrs)
                choice.text = child.get("label")
                if not choice.text:
                    raise SystemExit(f"node '{n.get('id')}': <look> needs a label attribute")
                n.remove(child)
                n.insert(i, choice)
        return node_elems + generated

    def compile(self, root):
        if root.tag != "story":
            raise SystemExit("root element must be <story>")
        node_elems = self.expand_looks(list(root.findall("node")))
        ids = [n.get("id") for n in node_elems]
        if len(set(ids)) != len(ids) or None in ids:
            raise SystemExit("every <node> needs a unique id")
        index = {nid: i for i, nid in enumerate(ids)}

        def lookup(nid, where):
            if nid not in index:
                raise SystemExit(f"{where}: unknown node '{nid}'")
            return index[nid]

        header = struct.pack(HEADER_FMT,
                             lookup(root.get("start"), "<story start>"),
                             lookup(root.get("timeout"), "<story timeout>"),
                             int(root.get("clock", "10")))

        nodes = bytearray()
        styles = bytearray()
        n_styles = 0
        reachable = set()
        for i, n in enumerate(node_elems):
            name = self.intern(n.get("id"))
            form = self.intern(n.get("form", ""))
            paragraphs = [self.paragraph(p) for p in n.findall("p")]
            text = ""
            style_begin = n_styles
            for ptext, runs in paragraphs:
                if text:
                    text += "\n"
                offset = len(text)
                text += ptext
                for b, e, flags, color in runs:
                    r, g, bb = color if color else (0, 0, 0)
                    styles += struct.pack(STYLE_FMT, offset + b, offset + e,
                                          flags | (16 if color else 0), r, g, bb)
                    n_styles += 1
            text_range = self.intern(text)
            is_ending = 1 if n.get("ending", "false").lower() == "true" else 0
            choices = list(n.findall("choice"))
            if len(choices) > MAX_CHOICES:
                raise SystemExit(f"node '{ids[i]}': more than {MAX_CHOICES} choices")
            if is_ending and choices:
                raise SystemExit(f"node '{ids[i]}': endings cannot have choices")
            if not is_ending and not choices:
                raise SystemExit(f"node '{ids[i]}': no choices and not an ending")
            packed_choices = b""
            for c in choices:
                label = self.intern(" ".join((c.text or "").split()))
                target = lookup(c.get("to"), f"node '{ids[i]}' choice")
                reachable.add(target)
                packed_choices += struct.pack(CHOICE_FMT, label[0], label[1], target,
                                              self.flags_mask(c.get("if")),
                                              self.flags_mask(c.get("unless")),
                                              self.flags_mask(c.get("set")),
                                              int(c.get("cost", "1")))
            packed_choices += struct.pack(CHOICE_FMT, *([0] * 7)) * (MAX_CHOICES - len(choices))
            nodes += struct.pack("<IIIIIIIIIII", name[0], name[1], form[0], form[1],
                                 text_range[0], text_range[1], style_begin, n_styles,
                                 self.flags_mask(n.get("set")), is_ending, len(choices))
            nodes += packed_choices

        reachable.add(index[root.get("start")])
        reachable.add(index[root.get("timeout")])
        for nid, i in index.items():
            if i not in reachable:
                print(f"warning: node '{nid}' is unreachable", file=sys.stderr)

        assert len(nodes) == struct.calcsize(NODE_FMT) * len(node_elems)
        return header, bytes(self.pool), bytes(nodes), bytes(styles), len(node_elems), n_styles


def write_chunk(out, magic, data):
    assert len(magic) == 4
    out.write(magic.encode("ascii") + struct.pack("<I", len(data)) + data)


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    src, dst = sys.argv[1], sys.argv[2]
    root = ET.parse(src).getroot()
    comp = Compiler()
    header, pool, nodes, styles, n_nodes, n_styles = comp.compile(root)
    with open(dst, "wb") as out:
        write_chunk(out, "hdr0", header)
        write_chunk(out, "str0", pool)
        write_chunk(out, "node", nodes)
        write_chunk(out, "styl", styles)
    print(f"{dst}: {n_nodes} nodes, {n_styles} style runs, {len(pool)} bytes of text, "
          f"{len(comp.flag_bits)} flags ({', '.join(comp.flag_bits)})")


if __name__ == "__main__":
    main()
