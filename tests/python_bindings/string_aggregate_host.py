"""Test-only host returning dynamic immutable str values."""


def make_text():
    # Build at runtime so this cannot be mistaken for a source StringLiteral.
    return ''.join(('宿', chr(0), '主', chr(0x1F600)))
