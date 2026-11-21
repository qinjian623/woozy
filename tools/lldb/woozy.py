"""Load with 'command script import tools/lldb/woozy.py' in LLDB."""

import json

import lldb


def field(value, name, alternate):
    member = value.GetChildMemberWithName(name)
    return member if member.IsValid() else value.GetChildMemberWithName(alternate)


def text_summary(value, _):
    value = value.GetNonSyntheticValue()
    size = field(value, "bytes", "len").GetValueAsUnsigned()
    if not size:
        return '""'
    address = value.GetChildMemberWithName("data").GetValueAsUnsigned()
    if not address:
        return "<unavailable string data>"
    error = lldb.SBError()
    data = value.GetProcess().ReadMemory(address, min(size, 4096), error)
    if error.Fail():
        return "<unavailable string data>"
    result = json.dumps(data.decode("utf-8", errors="replace"), ensure_ascii=False)
    return result + ("…" if size > len(data) else "")


class Sequence:
    def __init__(self, value, _):
        self.value = value
        self.update()

    def update(self):
        value = self.value.GetNonSyntheticValue()
        owner = value.GetChildMemberWithName("owner")
        self.start = 0
        if owner.IsValid():
            self.start = value.GetChildMemberWithName("start").GetValueAsUnsigned()
            self.length = field(value, "length", "len").GetValueAsUnsigned()
            descriptor = owner.Dereference().GetNonSyntheticValue()
        else:
            descriptor = value
            if value.GetType().IsPointerType():
                descriptor = value.Dereference().GetNonSyntheticValue()
            self.length = field(descriptor, "length", "len").GetValueAsUnsigned()
        data = descriptor.GetChildMemberWithName("data")
        self.address = data.GetValueAsUnsigned()
        self.item = data.GetType().GetPointeeType()
        self.stride = descriptor.GetChildMemberWithName("stride").GetValueAsUnsigned()
        initialized = descriptor.GetChildMemberWithName("initialized").GetValueAsUnsigned()
        size = self.item.GetByteSize()
        self.count = self.length if (self.address and size and self.stride >= size
                                     and self.start + self.length <= initialized) else 0
        return False

    def num_children(self):
        return self.count

    def get_child_index(self, name):
        if name.startswith("[") and name.endswith("]") and name[1:-1].isdigit():
            index = int(name[1:-1])
            if index < self.count:
                return index
        return -1

    def get_child_at_index(self, index):
        if 0 <= index < self.count:
            address = self.address + (self.start + index) * self.stride
            return self.value.CreateValueFromAddress(f"[{index}]", address, self.item)
        return None


def sequence_summary(value, _):
    return f"length {Sequence(value, _).length}"


def __lldb_init_module(debugger, _):
    debugger.HandleCommand(f"type summary add -w woozy -F {__name__}.text_summary string")
    pattern = r"'^(Vec|Slice)(\[.*\]|<.*>)( *\*)?$'"
    debugger.HandleCommand(f"type summary add -w woozy -e -F {__name__}.sequence_summary -x {pattern}")
    debugger.HandleCommand(f"type synthetic add -w woozy -l {__name__}.Sequence -x {pattern}")
    debugger.HandleCommand("type category enable woozy")
