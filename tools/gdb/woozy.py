"""Load with 'source tools/gdb/woozy.py' in GDB."""

import gdb
import gdb.printing


def type_names(value):
    current = value.type
    names = {current.name, current.tag}
    current = current.strip_typedefs()
    names.update((current.name, current.tag))
    if current.code == gdb.TYPE_CODE_PTR:
        target = current.target().strip_typedefs()
        names.update((target.name, target.tag))
    return [name for name in names if name]


def length(value):
    fields = {field.name for field in value.type.strip_typedefs().fields()}
    return int(value["length"] if "length" in fields else value["len"])


class TextPrinter:
    def __init__(self, value):
        self.value = value

    def display_hint(self):
        return "string"

    def to_string(self):
        fields = {field.name for field in self.value.type.strip_typedefs().fields()}
        size = int(self.value["bytes"] if "bytes" in fields else self.value["len"])
        if not size:
            return ""
        data = self.value["data"].cast(gdb.lookup_type("char").pointer())
        if not int(data):
            return "<unavailable string data>"
        return data.lazy_string(encoding="UTF-8", length=size)


class SequencePrinter:
    def __init__(self, descriptor, start, count):
        self.descriptor = descriptor
        self.start = start
        self.count = count

    def display_hint(self):
        return "array"

    def to_string(self):
        return f"length {self.count}"

    def children(self):
        if not self.count or self.descriptor is None:
            return
        # Vec elements use the runtime stride. Slice keeps the same backing
        # descriptor and starts inside its initialized prefix.
        data = self.descriptor["data"]
        item_type = data.type.strip_typedefs().target()
        stride = int(self.descriptor["stride"])
        initialized = int(self.descriptor["initialized"])
        if (item_type.code == gdb.TYPE_CODE_VOID or not int(data) or
                stride < max(1, item_type.sizeof) or self.start + self.count > initialized):
            return
        raw = data.cast(gdb.lookup_type("unsigned char").pointer())
        for index in range(self.count):
            item = raw + (self.start + index) * stride
            yield f"[{index}]", item.cast(data.type).dereference()


def woozy_printer(value):
    try:
        if value.is_optimized_out:
            return None
        names = type_names(value)
        if "string" in names:
            return TextPrinter(value)
        if any(name.startswith(("Slice[", "Slice<")) for name in names):
            count = length(value)
            owner = value["owner"]
            return SequencePrinter(owner.dereference() if int(owner) else None,
                                   int(value["start"]), count)
        if any(name.startswith(("Vec[", "Vec<")) for name in names):
            if not int(value):
                return None
            descriptor = value.dereference()
            return SequencePrinter(descriptor, 0, length(descriptor))
    except (gdb.error, TypeError, ValueError):
        return None
    return None


woozy_printer.name = "woozy"
woozy_printer.enabled = True
gdb.printing.register_pretty_printer(gdb.current_progspace(), woozy_printer,
                                    replace=True)
