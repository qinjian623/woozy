#!/usr/bin/env python3
"""Standard-library-only lexical/syntactic reference checker for language v0.1.

This is a recognizer, NOT a compiler, interpreter, type checker, ownership
checker or throws checker. See ../grammar.ebnf. It deliberately accepts neutral
bracket suffixes without deciding whether they instantiate a generic or index a
value. It reports the first lexical/syntax problem per file.

Usage:
    python3 tools/check_syntax.py samples/01_basics.lang
    python3 tools/check_syntax.py --json samples/01_basics.lang
    python3 tools/check_syntax.py --self-test

Directory arguments recurse through *.lang, INCLUDING negative examples.
Exit codes: 0 = accepted / self-tests passed; 1 = rejected / test failed;
2 = command-line, filesystem, or reference-checker resource problem.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
from pathlib import Path
import sys
from typing import Optional


KEYWORDS = frozenset("""
package import as pub func let var const struct enum trait impl for in if else
switch case default try catch throw throws defer unsafe return break continue
inout move true false
""".split())
FUTURE_RESERVED = frozenset("extern async await class extends operator".split())
MODES = frozenset({"inout", "move"})
BINDINGS = frozenset({"let", "var", "const"})
ASSIGNMENTS = frozenset({"=", "+=", "-=", "*=", "/=", "%=", "<<=", ">>=", "&=", "^=", "|="})
MULTI_OPERATORS = ("<<=", ">>=", "==", "!=", "<=", ">=", "&&", "||", "<<", ">>",
                   "+=", "-=", "*=", "/=", "%=", "&=", "^=", "|=")
UNAVAILABLE_OPERATORS = ("++", "--", "->", "=>")
SINGLE_SYMBOLS = frozenset("{}()[];,.:=+-*/%&|^~!<>")
BINARY_PRECEDENCE = {
    "||": 1, "&&": 2, "|": 3, "^": 4, "&": 5,
    "==": 6, "!=": 6,
    "<": 7, "<=": 7, ">": 7, ">=": 7,
    "<<": 8, ">>": 8,
    "+": 9, "-": 9,
    "*": 10, "/": 10, "%": 10,
}
UNARY = frozenset({"+", "-", "!", "~", "move", "inout"})
LITERAL_KINDS = frozenset({"INT", "FLOAT", "STRING", "RAW", "true", "false"})


def ascii_letter(char: str) -> bool:
    return bool(char) and ("a" <= char <= "z" or "A" <= char <= "Z")


def ascii_digit(char: str) -> bool:
    return bool(char) and "0" <= char <= "9"


def id_start(char: str) -> bool:
    return char == "_" or ascii_letter(char)


def id_continue(char: str) -> bool:
    return id_start(char) or ascii_digit(char)


@dataclass(frozen=True)
class Token:
    kind: str
    text: str
    line: int
    column: int
    offset: int


class Problem(Exception):
    def __init__(self, phase: str, code: str, message: str, token: Token):
        super().__init__(message)
        self.phase = phase
        self.code = code
        self.message = message
        self.token = token

    def diagnostic(self) -> dict:
        return {"phase": self.phase, "code": self.code, "message": self.message,
                "line": self.token.line, "column": self.token.column,
                "token": self.token.text}


class Lexer:
    def __init__(self, source: str):
        self.source = source
        self.index = 0
        self.line = 1
        self.column = 1
        if source.startswith("\ufeff"):
            self.index = 1  # Ignore exactly one initial BOM, not its next column.

    def peek(self, distance: int = 0) -> str:
        pos = self.index + distance
        return self.source[pos] if pos < len(self.source) else ""

    def starts(self, text: str) -> bool:
        return self.source.startswith(text, self.index)

    def location(self, text: str = "") -> Token:
        return Token("", text, self.line, self.column, self.index)

    def advance(self) -> str:
        char = self.peek()
        if not char:
            return ""
        self.index += 1
        if char == "\n":
            self.line += 1
            self.column = 1
        else:
            self.column += 1
        return char

    def consume(self, text: str) -> None:
        for _ in text:
            self.advance()

    def error(self, code: str, message: str, token: Optional[Token] = None) -> None:
        raise Problem("lexical", code, message, token or self.location(self.peek()))

    def make(self, kind: str, start: Token) -> Token:
        return Token(kind, self.source[start.offset:self.index], start.line, start.column, start.offset)

    def check_source_characters(self) -> None:
        # These source restrictions also apply inside comments and raw strings.
        line, column = 1, 1
        for index, char in enumerate(self.source):
            if index == 0 and char == "\ufeff":
                continue
            if char == "\x00":
                self.error("L001", "literal NUL is forbidden in source; use an escape in a text string",
                           Token("", char, line, column, index))
            if char == "\r" and (index + 1 == len(self.source) or self.source[index + 1] != "\n"):
                self.error("L002", "bare CR is not a line ending; use LF or CRLF",
                           Token("", char, line, column, index))
            if 0xD800 <= ord(char) <= 0xDFFF:
                self.error("L003", "source must contain Unicode scalar values, not surrogates",
                           Token("", char, line, column, index))
            if char == "\n":
                line, column = line + 1, 1
            else:
                column += 1

    def skip_trivia(self) -> None:
        while True:
            if self.peek() and self.peek() in " \t\n\r":
                self.advance()
            elif self.starts("//"):
                while self.peek() and self.peek() not in "\n\r":
                    self.advance()
            elif self.starts("/*"):
                start = self.location("/*")
                self.consume("/*")
                while self.peek() and not self.starts("*/"):
                    self.advance()
                if not self.peek():
                    self.error("L004", "unterminated block comment", start)
                self.consume("*/")
            else:
                return

    def digit_sequence(self, allowed: str, label: str) -> str:
        start = self.location(self.peek())
        if self.peek() not in allowed or not self.peek():
            self.error("L010", f"{label} requires a digit", start)
        begin = self.index
        previous_underscore = False
        while self.peek() and (self.peek() in allowed or self.peek() == "_"):
            char = self.advance()
            if char == "_":
                if previous_underscore:
                    self.error("L011", "numeric underscores must occur singly between digits", start)
                previous_underscore = True
            else:
                previous_underscore = False
        if previous_underscore:
            self.error("L011", "a numeric underscore must be followed by a digit of that radix", start)
        return self.source[begin:self.index]

    def number(self) -> Token:
        start = self.location()
        radices = {"b": "01", "o": "01234567", "x": "0123456789abcdefABCDEF"}
        if self.peek() == "0" and self.peek(1) in radices:
            prefix = self.peek(1)
            self.advance()
            self.advance()
            self.digit_sequence(radices[prefix], "base-prefixed integer")
            if id_continue(self.peek()):
                self.error("L012", "invalid radix digit or unsupported numeric suffix", start)
            return self.make("INT", start)

        integer = self.digit_sequence("0123456789", "decimal integer")
        if len(integer.replace("_", "")) > 1 and integer.startswith("0"):
            self.error("L013", "multi-digit decimal literals cannot have a leading zero", start)
        is_float = False
        if self.peek() == "." and ascii_digit(self.peek(1)):
            is_float = True
            self.advance()
            self.digit_sequence("0123456789", "fractional part")
        if self.peek() and self.peek() in "eE":
            is_float = True
            self.advance()
            if self.peek() and self.peek() in "+-":
                self.advance()
            self.digit_sequence("0123456789", "exponent")
        if id_continue(self.peek()):
            self.error("L014", "a number cannot touch an identifier or use a type suffix", start)
        return self.make("FLOAT" if is_float else "INT", start)

    def text_string(self) -> Token:
        start = self.location('"')
        self.advance()
        while self.peek():
            char = self.peek()
            if char == '"':
                self.advance()
                return self.make("STRING", start)
            if char in "\r\n":
                self.error("L020", "text strings cannot contain a line break; use a backtick raw string", start)
            if ord(char) < 0x20 or ord(char) == 0x7F:
                self.error("L021", "literal control character in text string; use a supported escape")
            if char != "\\":
                self.advance()
                continue
            escape = self.location("\\")
            self.advance()
            escaped = self.peek()
            if escaped and escaped in 'nrt0"\\':
                self.advance()
                continue
            if escaped == "u":
                self.advance()
                if self.peek() != "{":
                    self.error("L022", "Unicode escapes use \\u{HEX}", escape)
                self.advance()
                digits = ""
                while self.peek() and self.peek() in "0123456789abcdefABCDEF":
                    digits += self.advance()
                if self.peek() != "}" or not 1 <= len(digits) <= 6:
                    self.error("L022", "Unicode escapes require 1-6 hexadecimal digits in braces", escape)
                self.advance()
                scalar = int(digits, 16)
                if scalar > 0x10FFFF or 0xD800 <= scalar <= 0xDFFF:
                    self.error("L023", "Unicode escape is not a Unicode scalar value", escape)
                continue
            self.error("L024", "unsupported string escape; see the TextString escape list in grammar.ebnf", escape)
        self.error("L025", "unterminated text string", start)
        raise AssertionError("unreachable")

    def raw_string(self) -> Token:
        start = self.location("`")
        self.advance()
        while self.peek() and self.peek() != "`":
            self.advance()
        if not self.peek():
            self.error("L026", "unterminated raw string", start)
        self.advance()
        return self.make("RAW", start)

    def scan(self) -> list[Token]:
        self.check_source_characters()
        tokens = []
        while True:
            self.skip_trivia()
            start = self.location()
            char = self.peek()
            if not char:
                tokens.append(Token("EOF", "", self.line, self.column, self.index))
                return tokens
            if id_start(char):
                self.advance()
                while id_continue(self.peek()):
                    self.advance()
                text = self.source[start.offset:self.index]
                tokens.append(self.make(text if text in KEYWORDS | FUTURE_RESERVED else "IDENT", start))
            elif ascii_digit(char):
                tokens.append(self.number())
            elif char == '"':
                tokens.append(self.text_string())
            elif char == "`":
                tokens.append(self.raw_string())
            elif char == "'":
                self.error("L030", "single-quoted literals are not part of v0.1")
            elif char == "?":
                self.error("L031", "? is not a v0.1 token; errors use throws/try/catch")
            else:
                unavailable = next((op for op in UNAVAILABLE_OPERATORS if self.starts(op)), None)
                if unavailable:
                    self.error("L032", f"operator {unavailable!r} is not part of v0.1",
                               self.location(unavailable))
                operator = next((op for op in MULTI_OPERATORS if self.starts(op)), None)
                if operator:
                    self.consume(operator)
                    tokens.append(self.make(operator, start))
                elif char in SINGLE_SYMBOLS:
                    self.advance()
                    tokens.append(self.make(char, start))
                else:
                    self.error("L033", "unsupported source character; identifiers use ASCII letters, digits and _")


class Parser:
    def __init__(self, tokens: list[Token]):
        self.tokens = tokens
        self.index = 0

    @property
    def token(self) -> Token:
        return self.tokens[self.index]

    def at(self, *kinds: str) -> bool:
        return self.token.kind in kinds

    def next_at(self, kind: str) -> bool:
        return self.index + 1 < len(self.tokens) and self.tokens[self.index + 1].kind == kind

    def take(self, kind: str) -> bool:
        if self.at(kind):
            self.index += 1
            return True
        return False

    def fail(self, message: str, code: str = "S001") -> None:
        raise Problem("syntax", code, message, self.token)

    def expect(self, kind: str) -> Token:
        if not self.at(kind):
            actual = self.token.text or "end of file"
            self.fail(f"expected {kind!r}, found {actual!r}")
        token = self.token
        self.index += 1
        return token

    def identifier(self) -> Token:
        return self.expect("IDENT")

    def parse(self) -> None:
        self.expect("package")
        self.identifier()
        self.expect(";")
        while self.take("import"):
            self.expect("STRING")
            if self.take("as"):
                self.identifier()
            self.expect(";")
        while not self.at("EOF"):
            self.top_declaration()

    def top_declaration(self) -> None:
        self.take("pub")
        if self.token.kind in BINDINGS:
            self.binding()
            self.expect(";")
        elif self.at("func"):
            self.function(prototype=False)
        elif self.take("struct"):
            self.identifier()
            self.type_parameters_optional()
            self.expect("{")
            while not self.at("}", "EOF"):
                self.take("pub")
                self.identifier()
                self.type()
                self.expect(";")
            self.expect("}")
        elif self.take("enum"):
            self.identifier()
            self.type_parameters_optional()
            self.expect("{")
            while not self.at("}", "EOF"):
                self.identifier()
                if self.take("("):
                    self.comma_list(self.payload_field, ")", allow_empty=True)
                self.expect(";")
            self.expect("}")
        elif self.take("trait"):
            self.identifier()
            self.type_parameters_optional()
            self.expect("{")
            while not self.at("}", "EOF"):
                self.take("pub")
                self.function(prototype=True)
            self.expect("}")
        elif self.take("impl"):
            self.type_parameters_optional()
            self.named_type()
            if self.take("for"):
                self.named_type()
            self.expect("{")
            while not self.at("}", "EOF"):
                self.take("pub")
                self.function(prototype=False)
            self.expect("}")
        elif self.at("import"):
            self.fail("imports must precede all ordinary declarations", "S002")
        elif self.token.kind in FUTURE_RESERVED:
            self.fail(f"{self.token.text!r} is reserved for future use, not available in v0.1", "S003")
        else:
            self.fail("expected a top-level binding, function, struct, enum, trait or impl declaration")

    def comma_list(self, item_parser, closing: str, allow_empty: bool) -> None:
        if self.take(closing):
            if not allow_empty:
                previous = self.tokens[self.index - 1]
                raise Problem("syntax", "S004", "this list cannot be empty", previous)
            return
        item_parser()
        while self.take(","):
            if self.take(closing):
                return
            item_parser()
        self.expect(closing)

    def type_parameters_optional(self) -> None:
        if self.take("["):
            self.comma_list(self.type_parameter, "]", allow_empty=False)

    def type_parameter(self) -> None:
        self.identifier()
        if self.at("IDENT"):
            self.named_type()

    def payload_field(self) -> None:
        self.identifier()
        self.type()

    def parameter(self) -> None:
        self.identifier()
        if self.token.kind in MODES:
            self.index += 1
        self.type()

    def function_type_parameter(self) -> None:
        if self.token.kind in MODES:
            self.index += 1
        self.type()

    def function(self, prototype: bool) -> None:
        self.expect("func")
        self.identifier()
        self.type_parameters_optional()
        self.expect("(")
        self.comma_list(self.parameter, ")", allow_empty=True)
        self.return_and_throws()
        if prototype:
            self.expect(";")
        else:
            self.block()

    def function_literal(self) -> None:
        self.expect("func")
        self.expect("(")
        self.comma_list(self.parameter, ")", allow_empty=True)
        self.return_and_throws()
        self.block()

    def return_and_throws(self) -> None:
        if self.at("func"):
            self.fail("a function-valued return type must be parenthesized to disambiguate throws", "S005")
        if self.at("IDENT", "("):
            self.return_type()
        if self.take("throws"):
            self.type()

    def return_type(self) -> None:
        if self.take("("):
            self.type()
            self.expect(")")
        else:
            self.named_type()

    def type(self) -> None:
        if self.take("func"):
            self.expect("(")
            self.comma_list(self.function_type_parameter, ")", allow_empty=True)
            self.return_and_throws()
        elif self.take("("):
            self.type()
            self.expect(")")
        else:
            self.named_type()

    def type_segment(self) -> None:
        self.identifier()
        if self.take("["):
            self.comma_list(self.type, "]", allow_empty=False)

    def named_type(self) -> None:
        self.type_segment()
        while self.take("."):
            self.type_segment()

    def binding(self) -> None:
        if self.token.kind not in BINDINGS:
            self.fail("expected let, var or const")
        self.index += 1
        self.identifier()
        if not self.at("="):
            self.type()
        self.expect("=")
        self.expression()

    def block(self) -> None:
        self.expect("{")
        while not self.at("}", "EOF"):
            self.statement()
        self.expect("}")

    def statement(self) -> None:
        if self.token.kind in BINDINGS:
            self.binding()
            self.expect(";")
        elif self.at("{"):
            self.block()
        elif self.at("if"):
            self.if_statement()
        elif self.at("for"):
            self.for_statement()
        elif self.take("switch"):
            self.expression()
            self.expect("{")
            while not self.at("}", "EOF"):
                if self.take("case"):
                    self.pattern()
                elif not self.take("default"):
                    self.fail("expected case or default in switch")
                self.expect(":")
                while not self.at("case", "default", "}", "EOF"):
                    self.statement()
            self.expect("}")
        elif self.take("try"):
            self.block()
            if not self.at("catch"):
                self.fail("try blocks require at least one typed catch clause", "S006")
            while self.take("catch"):
                self.identifier()
                self.type()
                self.block()
        elif self.at("defer", "unsafe"):
            self.index += 1
            self.block()
        elif self.take("return"):
            if not self.at(";"):
                self.expression()
            self.expect(";")
        elif self.take("throw"):
            self.expression()
            self.expect(";")
        elif self.at("break", "continue"):
            self.index += 1
            self.expect(";")
        else:
            self.simple_statement()
            self.expect(";")

    def simple_statement(self) -> bool:
        self.expression()
        if self.token.kind in ASSIGNMENTS:
            self.index += 1
            self.expression()
            return True
        return False

    def if_statement(self) -> None:
        self.expect("if")
        self.expression()
        self.block()
        if self.take("else"):
            if self.at("if"):
                self.if_statement()
            else:
                self.block()

    def for_statement(self) -> None:
        self.expect("for")
        if self.at("{"):
            self.block()
            return
        if self.at("IDENT") and self.next_at("in"):
            self.identifier()
            self.expect("in")
            self.expression()
            self.block()
            return
        initializer_is_not_condition = False
        if self.token.kind in BINDINGS:
            self.binding()
            initializer_is_not_condition = True
        elif not self.at(";"):
            initializer_is_not_condition = self.simple_statement()
        if self.take(";"):
            if not self.at(";"):
                self.expression()
            self.expect(";")
            if not self.at("{"):
                self.simple_statement()
            self.block()
        elif initializer_is_not_condition:
            self.fail("a for declaration or assignment initializer requires the three-part header", "S007")
        else:
            self.block()

    def pattern(self) -> None:
        if self.take("-"):
            if not self.at("INT", "FLOAT"):
                self.fail("a negative pattern must be a numeric literal")
            self.index += 1
            return
        if self.token.kind in LITERAL_KINDS:
            self.index += 1
            return
        self.identifier()
        saw_type_arguments = False
        if self.take("["):
            saw_type_arguments = True
            self.comma_list(self.type, "]", allow_empty=False)
        saw_dot = False
        final_has_type_arguments = saw_type_arguments
        while self.take("."):
            saw_dot = True
            self.identifier()
            final_has_type_arguments = False
            if self.take("["):
                final_has_type_arguments = True
                self.comma_list(self.type, "]", allow_empty=False)
        if not saw_dot:
            if saw_type_arguments or self.at("("):
                self.fail("enum variant patterns must be qualified, for example Option.Some(value)", "S008")
            return  # Bare identifier / wildcard binding.
        if final_has_type_arguments:
            self.fail("the final variant name cannot carry type arguments", "S009")
        if self.take("("):
            self.comma_list(self.pattern, ")", allow_empty=True)

    def expression(self, minimum_precedence: int = 1) -> None:
        self.unary()
        while BINARY_PRECEDENCE.get(self.token.kind, 0) >= minimum_precedence:
            precedence = BINARY_PRECEDENCE[self.token.kind]
            self.index += 1
            self.expression(precedence + 1)

    def unary(self) -> None:
        if self.token.kind in UNARY:
            self.index += 1
            self.unary()
            return
        self.primary()
        while True:
            if self.take("("):
                self.arguments()
            elif self.take("."):
                self.identifier()
            elif self.take("["):
                self.bracket_suffix()
            else:
                return

    def primary(self) -> None:
        if self.at("IDENT") or self.token.kind in LITERAL_KINDS:
            self.index += 1
        elif self.at("func"):
            self.function_literal()
        elif self.take("("):
            self.expression()
            self.expect(")")
        elif self.take("["):
            self.comma_list(self.expression, "]", allow_empty=True)
        else:
            self.fail("expected an expression")

    def arguments(self) -> None:
        if self.take(")"):
            return
        saw_named = False
        while True:
            if self.at("IDENT") and self.next_at(":"):
                saw_named = True
                self.identifier()
                self.expect(":")
            elif saw_named:
                self.fail("a positional argument cannot follow a named argument", "S010")
            self.expression()
            if not self.take(","):
                self.expect(")")
                return
            if self.take(")"):
                return

    def bracket_argument(self) -> bool:
        """Accept a Type or Expr without making a generic-vs-index decision.

        Returns True only for a syntactic type-only candidate such as func(...).
        A named/grouped named type may equally be an expression; its token span
        is left neutral. This recognizer does not build or export a typed AST.
        """
        start = self.index
        try:
            self.type()
            if self.at(",", "]"):
                return any(t.kind == "func" for t in self.tokens[start:self.index])
        except Problem:
            pass
        self.index = start
        self.expression()
        return False

    def bracket_suffix(self) -> None:
        if self.take(":"):
            if not self.at("]"):
                self.expression()
            self.expect("]")
            return
        if self.at("]"):
            self.fail("a bracket suffix cannot be empty; [] is only an array literal", "S011")
        self.bracket_argument()
        if self.take(":"):
            if not self.at("]"):
                self.expression()
            self.expect("]")
            return
        while self.take(","):
            if self.take("]"):
                return
            self.bracket_argument()
        self.expect("]")


SCOPE = {
    "checks": ["UTF-8 source restrictions", "tokens", "v0.1 compilation-unit syntax"],
    "does_not_check": ["names/imports", "types/inference", "generic constraints",
                       "throws handling", "ownership/ARC/inout", "control-flow validity",
                       "target support", "runtime behavior"],
    "first_error_per_file": True,
    "compiler": False,
}


def check_source(source: str, path: str = "<memory>") -> dict:
    parser = None
    try:
        tokens = Lexer(source).scan()
        parser = Parser(tokens)
        parser.parse()
        return {"path": path, "accepted": True, "token_count": len(tokens) - 1,
                "diagnostics": []}
    except Problem as problem:
        return {"path": path, "accepted": False, "diagnostics": [problem.diagnostic()]}
    except RecursionError:
        token = parser.token if parser is not None else Token("", "", 1, 1, 0)
        problem = Problem("checker-limit", "C001",
                          "input exceeds this reference checker's nesting limit; syntax validity is undetermined",
                          token)
        return {"path": path, "accepted": False, "diagnostics": [problem.diagnostic()]}


def check_file(path: Path) -> dict:
    try:
        source = path.read_bytes().decode("utf-8")
    except UnicodeDecodeError as error:
        return {"path": str(path), "accepted": False, "diagnostics": [{
            "phase": "encoding", "code": "L000", "message": "source is not valid UTF-8",
            "byte_offset": error.start, "line": None, "column": None, "token": ""}]}
    except OSError as error:
        return {"path": str(path), "accepted": False, "diagnostics": [{
            "phase": "io", "code": "C002", "message": str(error),
            "line": None, "column": None, "token": ""}]}
    return check_source(source, str(path))


def self_test() -> dict:
    def unit(body: str) -> str:
        return "package sample;\n" + body

    def inside(body: str) -> str:
        return unit("func check() { " + body + " }")

    cases: list[tuple[str, bool, str]] = [
        ("minimal", True, "package sample;"),
        ("leading BOM and CRLF", True, "\ufeffpackage sample;\r\nfunc main() {}\r\n"),
        ("imports and pub", True, unit('import "std/io" as io; pub func main() {}')),
        ("import without explicit alias", True, unit('import "std/os"; func main() { os.cwd(); }')),
        ("let var const", True, unit("const N i64 = 3; func f() { let x = 2; var y i64 = x; y += 1; }")),
        ("numeric forms", True, inside("let a = 0b10_01; let b = 0o7_1; let c = 0xAf_01; let d = 12_345; let e = 1.25e-2; let f = 1e3;")),
        ("unicode and raw strings", True, inside(r'let s = "a\n\r\t\0\"\\\u{1F600}";' + "let r = `a\n/* raw */\\n`;")),
        ("comments", True, "// head\npackage sample; /* outer /* ignored opener */ func f() {}"),
        ("arrays slices and precedence", True, inside("let a = [1, 2,]; let empty = []; let x = a[:]; let y = a[1:]; let z = a[:2]; let n = ~1 + 2 * 3 << 1 & 7 == 3 || false;")),
        ("generic declaration and neutral postfix", True, unit("func f[T Trait, U](x T, y U,) T { return x; } func main() { let x = f[i32, i64](1, 2); let y = table[i](x); }")),
        ("function type in bracket", True, inside("let x = factory[func(inout i32) Unit](value); let y = factory[(func(i32) i32 throws E)](value);")),
        ("function-valued return with outer throws", True, unit("func make() (func(i32) i32) throws E { return callback; }")),
        ("function-valued return with inner throws", True, unit("func make() (func(i32) i32 throws E) { return callback; }")),
        ("function literals", True, inside("let twice = func(x i32) i32 { return x * 2; }; let action func(inout i32) Unit = func(x inout i32) Unit { x += 1; };")),
        ("fallible function literal", True, inside("let checked = func(x i32) i32 throws E { if x < 0 { throw E.Bad(); } return x; }; let mapped = transform(values, func(x i32) i32 { return x + 1; });")),
        ("parameter modes", True, unit("func f(x inout i32, y move Node) { g(inout x, value: move y); }")),
        ("struct trait impl", True, unit("pub struct Point { pub x f64; y f64; } trait Size { func size(self Self) f64; } impl Size for Point { func size(self Point) f64 { return self.x; } } impl[T Trait] Vec[T] { func len(self Self) i64 { return 0; } }")),
        ("enum and patterns", True, unit("enum Option[T] { None; Some(value T); } func f(x Option[i32]) { switch x { case Option[i32].Some(value): g(value); case Option.None: return; default: return; } switch y { case -3: return; case name: return; } }")),
        ("all for forms", True, inside("for { break; } for ready { continue; } for value in values { g(value); } for var i i32 = 0; i < 3; i += 1 { g(i); } for ;; {}")),
        ("try cleanup unsafe", True, inside("defer { cleanup(); } try { f(); } catch err E { throw err; } catch other Other { return; } unsafe { f(); }")),
        ("semantic errors deliberately accepted", True, unit('func f() i32 { let x i32 = "wrong"; break; unknown(); defer { throw bad; } return x; }')),
        ("not keywords", True, unit("func auto(while i32) i32 { let fn = while; let null = fn; return null; }")),
        ("missing package", False, "func main() {}"),
        ("late import", False, unit('func f() {} import "x" as x;')),
        ("missing initializer", False, inside("var value i32;")),
        ("missing semicolon", False, inside("let value = 1")),
        ("assignment expression", False, inside("let value = (a = 1);")),
        ("unparenthesized function result", False, unit("func make() func(i32) i32 throws E { return callback; }")),
        ("named then positional", False, inside("f(x: 1, 2);")),
        ("default source parameter", False, unit("func f(x i32 = 1) {}")),
        ("enum unqualified variant", False, inside("switch x { case Some(value): return; }")),
        ("slice step", False, inside("let x = values[0:10:2];")),
        ("empty postfix bracket", False, inside("let x = f[]();")),
        ("constant type argument", False, inside("let x Array[4] = values;")),
        ("raw address unary", False, inside("let p = &value;")),
        ("raw dereference unary", False, inside("let x = *pointer;")),
        ("try without catch", False, inside("try {}")),
        ("arrow result", False, unit("func f() -> i32 { return 1; }")),
        ("question token", False, inside("let x = f()?;")),
        ("increment token", False, inside("i++;")),
        ("future keyword", False, unit("extern func f();")),
        ("character literal", False, inside("let x = 'a';")),
        ("leading decimal zero", False, inside("let x = 007;")),
        ("base underscore", False, inside("let x = 0x_FF;")),
        ("bad radix", False, inside("let x = 0b102;")),
        ("numeric suffix", False, inside("let x = 42i32;")),
        ("numeric trailing underscore", False, inside("let x = 12_;")),
        ("float no leading digits", False, inside("let x = .5;")),
        ("float no fractional digits", False, inside("let x = 1.;")),
        ("float no exponent digits", False, inside("let x = 1e+;")),
        ("invalid escape", False, inside(r'let x = "\x41";')),
        ("surrogate escape", False, inside(r'let x = "\u{D800}";')),
        ("large unicode escape", False, inside(r'let x = "\u{110000}";')),
        ("empty unicode escape", False, inside(r'let x = "\u{}";')),
        ("raw unterminated", False, inside("let x = `raw;")),
        ("comment unterminated", False, unit("/*")),
        ("literal NUL in comment", False, unit("// \x00")),
        ("bare CR", False, "package sample;\rfunc f() {}"),
        ("non-ASCII identifier", False, unit("func 函数() {}")),
    ]
    failures = []
    for name, expected, source in cases:
        result = check_source(source, "<self-test:" + name + ">")
        if result["accepted"] != expected:
            failures.append({"case": name, "expected_accepted": expected, "actual": result})
    return {"total": len(cases), "passed": len(cases) - len(failures), "failures": failures}


def main(argv: Optional[list[str]] = None) -> int:
    argument_parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    argument_parser.add_argument("paths", nargs="*", help="source files or directories (directories include every *.lang)")
    argument_parser.add_argument("--json", action="store_true", help="emit structured results and an explicit scope statement")
    argument_parser.add_argument("--self-test", action="store_true", help="run embedded lexical/syntax regression cases")
    args = argument_parser.parse_args(argv)
    if not args.paths and not args.self_test:
        argument_parser.error("provide at least one source path or --self-test")

    test_result = self_test() if args.self_test else None
    paths: list[Path] = []
    for name in args.paths:
        path = Path(name)
        if path.is_dir():
            paths.extend(sorted(path.rglob("*.lang")))
        else:
            paths.append(path)
    paths = list(dict.fromkeys(paths))
    results = [check_file(path) for path in paths]
    if args.json:
        document = {"schema": "language-v0.1-syntax-check/1", "scope": SCOPE, "files": results}
        if test_result is not None:
            document["self_test"] = test_result
        print(json.dumps(document, ensure_ascii=False, indent=2))
    else:
        print("v0.1 syntax reference checker (NOT a compiler/type/ownership/throws checker)")
        if test_result is not None:
            print(f"SELF-TEST {test_result['passed']}/{test_result['total']} passed")
            for failure in test_result["failures"]:
                print(json.dumps(failure, ensure_ascii=False))
        for result in results:
            if result["accepted"]:
                print(f"SYNTAX OK  {result['path']}")
            else:
                for diagnostic in result["diagnostics"]:
                    location = f"{result['path']}:{diagnostic.get('line') or '?'}:{diagnostic.get('column') or '?'}"
                    print(f"{location}: {diagnostic['phase']} {diagnostic['code']}: {diagnostic['message']}")
    if any(diagnostic["phase"] in {"io", "checker-limit"}
           for result in results for diagnostic in result["diagnostics"]):
        return 2
    if any(not result["accepted"] for result in results):
        return 1
    if test_result is not None and test_result["failures"]:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
