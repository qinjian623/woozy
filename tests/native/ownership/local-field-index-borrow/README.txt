Read-only indexed fields rooted in an owning value-struct local borrow the
local's Vec handle. The inout call in the same full expression and the call
index keep the older receiver-copy path. No writes through a field receiver
are part of this fixture.
