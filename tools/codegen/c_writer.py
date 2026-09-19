"""
C Code Writer utility.
Pure Python 3 standard library.
"""

import sys
sys.dont_write_bytecode = True

class CWriter:
    def __init__(self, indent_str="  "):
        self.indent_str = indent_str
        self.indent_level = 0
        self.lines = []

    def indent(self):
        self.indent_level += 1

    def dedent(self):
        if self.indent_level > 0:
            self.indent_level -= 1

    def line(self, text=""):
        if text.strip() == "":
            self.lines.append("")
        else:
            self.lines.append(f"{self.indent_str * self.indent_level}{text}")

    def block_start(self, header=""):
        if header:
            self.line(f"{header} {{")
        else:
            self.line("{")
        self.indent()

    def block_end(self, suffix=""):
        self.dedent()
        self.line(f"}}{suffix}")

    def get_code(self) -> str:
        return "\n".join(self.lines) + "\n"
