"""Rejected Sources keep receiver markers and their real declaration identity."""

BOX = (
    "struct Box{items Vec[i32];}"
    "impl Box{func take(self move Self,other i32)Self{return move self;}}"
)
PAIR = (
    "struct Pair{value i32;}"
    "impl Pair{func change(self inout Self,other i32){self=Pair(value:other);}}"
)
REJECTS = {
    "inout-value-plus": (
        "impl i32{func edit(self inout Self){self+=1;}}"
        "func main(){var x i32=1;(inout (+x)).edit();}",
        "E_CAPABILITY_UNAVAILABLE",
    ),
    "no-auto-move": (
        BOX + "func main(){let x=Box(items:[1]);let y=x.take(2);}",
        "E_CAPABILITY_UNAVAILABLE",
    ),
    "no-auto-inout": (
        PAIR + "func main(){var x=Pair(value:1);x.change(2);}",
        "E_CAPABILITY_UNAVAILABLE",
    ),
    "wrong-marker": (
        PAIR + "func main(){let x=Pair(value:1);(move x).change(2);}",
        "E_CALL_ARGUMENT",
    ),
    "marker-on-ordinary": (
        "struct S{value i32;}impl S{func read(self Self)i32{return self.value;}}func main(){var x=S(value:1);let y=(inout x).read();}",
        "E_CALL_ARGUMENT",
    ),
    "let-inout": (
        PAIR + "func main(){let x=Pair(value:1);(inout x).change(2);}",
        "E_CALL_ARGUMENT",
    ),
    "borrowed-inout": (
        PAIR + "func f(x Pair){(inout x).change(2);}func main(){}",
        "E_CALL_ARGUMENT",
    ),
    "borrowed-move": (
        BOX + "func f(x Box){let y=(move x).take(2);}func main(){}",
        "E_CALL_ARGUMENT",
    ),
    "field-inout": (
        PAIR + "struct S{p Pair;}func main(){let x=S(p:Pair(value:1));(inout x.p).change(2);}",
        "E_TYPE_MISMATCH",
    ),
    "index-inout": (
        PAIR + "func main(){var xs Vec[Pair]=[Pair(value:1)];(inout xs[0]).change(xs[0].value);}",
        "E_CALL_ARGUMENT",
    ),
    "partial-move": (
        BOX + "struct S{b Box;}func main(){var x=S(b:Box(items:[1]));let y=(move x.b).take(2);}",
        "E_CAPABILITY_UNAVAILABLE",
    ),
    # Owning inout Self is supported now. The receiver capability gate still
    # rejects a value whose copy/drop contract is not represented in HIR (a
    # Task handle is owned by the executor), so this keeps testing the gate.
    "owning-inout": (
        "struct S{v Task[i32,Unit];}impl S{func edit(self inout Self){}}func main()i32{return 0;}",
        "E_CAPABILITY_UNAVAILABLE",
    ),
    "alias-read": (
        PAIR + "func main(){var x=Pair(value:1);(inout x).change(x.value);}",
        "E_CALL_ARGUMENT",
    ),
    "alias-nested": (
        PAIR
        + "func f(x inout Pair)i32{return x.value;}func main(){var x=Pair(value:1);(inout x).change(f(inout x));}",
        "E_CALL_ARGUMENT",
    ),
    "alias-duplicate": (
        "struct P{value i32;}impl P{func f(self inout Self,other inout Self){}}func main(){var x=P(value:1);(inout x).f(inout x);}",
        "E_CALL_ARGUMENT",
    ),
    "move-later-read": (
        BOX + "func main(){let x=Box(items:[1]);let y=(move x).take(x.items[0]);}",
        "E_OWNERSHIP_USE_AFTER_MOVE",
    ),
    "move-catch-read": (
        "enum E{Bad;}func fail()i32 throws E{throw E.Bad;}"
        + BOX
        + "func main(){let x=Box(items:[1]);try{let y=(move x).take(fail());}catch e E{let z=x.items[0];}}",
        "E_OWNERSHIP_USE_AFTER_MOVE",
    ),
    "defer-moved": (
        BOX + "func main(){let x=Box(items:[1]);defer{let z=x.items[0];}let y=(move x).take(2);}",
        "E_OWNERSHIP_USE_AFTER_MOVE",
    ),
    "raw-own-U": (
        "struct S{value i32;}trait Echo{func echo[U](self move U)U;}impl Echo for S{func echo[V](self move V)V{return move self;}}func main(){let s=S(value:1);let r=(move s).echo[S]();}",
        "E_CALL_ARGUMENT",
    ),
    "raw-trait-A": (
        "struct S{value i32;}trait Echo[A]{func echo(owner move A)A;}impl Echo[S] for S{func echo(owner move S)S{return move owner;}}func main(){let s=S(value:1);let r=(move s).echo();}",
        "E_CALL_ARGUMENT",
    ),
    "raw-inherent-U": (
        "struct S{value i32;}impl S{func echo[U](self move U)U{return move self;}}func main(){let s=S(value:1);let r=(move s).echo[S]();}",
        "E_CALL_ARGUMENT",
    ),
    "static-first-concrete": (
        "struct S{value i32;}impl S{func echo(owner move S)S{return move owner;}}func main(){let s=S(value:1);let r=(move s).echo();}",
        "E_CALL_ARGUMENT",
    ),
    "ambiguous-application": (
        "struct S{value i32;}trait Echo[A]{func echo(owner move Self)Self;}impl Echo[i32] for S{func echo(owner move Self)Self{return move owner;}}impl Echo[bool] for S{func echo(owner move Self)Self{return move owner;}}func main(){let s=S(value:1);let r=(move s).echo();}",
        "E_TYPE_MISMATCH",
    ),
}
