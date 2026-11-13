// Test-only structural dump of the existing C++ project loader.
#include "lang/modules/project.hpp"
#include <json-c/json.h>
#include <iostream>
#include <stdexcept>
#include <string>
using namespace lang;
using namespace lang::modules;
using J = json_object *;
J s(const std::string &v) { return json_object_new_string_len(v.data(), int(v.size())); }
J n(uint64_t v) { return json_object_new_uint64(v); }
J b(bool v) { return json_object_new_boolean(v); }
J obj() { return json_object_new_object(); }
J array() { return json_object_new_array(); }
void put(J j, const char *k, J v) { json_object_object_add(j, k, v); }
void push(J j, J v) { json_object_array_add(j, v); }
template<class R, class F> J list(const R &r, F f) {
  J a=array(); for (const auto &v:r) push(a,f(v)); return a;
}
J strings(const auto &r) { return list(r,[](const auto &v){return s(v);}); }
J indices(const auto &r) { return list(r,[](const auto &v){if constexpr(requires{v.value;}) return n(v.value);else return n(v);}); }
J optional_index(const auto &v) { if(!v)return nullptr; if constexpr(requires{v->value;})return n(v->value);else return n(*v); }
std::string hex(std::string_view v) {
  static constexpr char digits[]="0123456789abcdef";
  std::string out;out.reserve(v.size()*2);
  for(unsigned char c:v){out+=digits[c>>4];out+=digits[c&15];}return out;
}
J span(SourceSpan v,const SourceManager &sm) {
  J j=obj();put(j,"snapshot",n(v.snapshot.value));put(j,"path",s(sm.get(v.snapshot)->path));
  put(j,"begin",n(v.begin_byte));put(j,"end",n(v.end_byte));return j;
}
J source_dump(const SourceSnapshot &v) {
  J j=obj();put(j,"file_id",n(v.file.value));put(j,"revision_id",n(v.revision.value));
  put(j,"id",n(v.id.value));put(j,"path",s(v.path));put(j,"raw_hex",s(hex(v.original)));
  put(j,"valid",b(v.valid()));put(j,"normalized_hex",v.valid()?s(hex(v.normalized())):nullptr);
  if(v.error()) {J e=obj();put(e,"byte",n(v.error()->byte));put(e,"length",n(v.error()->length));put(e,"message",s(v.error()->what()));put(j,"error",e);}else put(j,"error",nullptr);
  return j;
}
J diagnostic(const Diagnostic &v,const SourceManager &sm) {
  J j=obj();put(j,"code",s(v.code));put(j,"phase",s(v.phase));put(j,"severity",s(v.severity));put(j,"message",s(v.message));
  put(j,"primary",v.primary?span(*v.primary,sm):nullptr);
  put(j,"related",list(v.related,[&](const auto &x){J z=obj();put(z,"span",span(x.span,sm));put(z,"message",s(x.message));return z;}));
  put(j,"fixits",list(v.fixits,[&](const auto &x){J z=obj();put(z,"span",span(x.span,sm));put(z,"replacement",s(x.replacement));put(z,"revision",n(x.revision.value));return z;}));
  put(j,"notes",strings(v.notes));put(j,"expected_type",v.expected_type?s(*v.expected_type):nullptr);put(j,"actual_type",v.actual_type?s(*v.actual_type):nullptr);return j;
}
J c_ref(const CTypeRef &v) {
  J j=obj();put(j,"kind",n(unsigned(v.kind)));put(j,"scalar",s(v.scalar));put(j,"writable",b(v.writable));put(j,"named",n(v.named));
  put(j,"args",list(v.args,[](const auto &a){return c_ref(a);}));return j;
}
J export_dump(const ForeignExport &v) {
  J j=obj();put(j,"name",s(v.name));put(j,"identity",s(v.identity));put(j,"kind",n(unsigned(v.kind)));put(j,"bundle",n(v.bundle));
  put(j,"declaration",optional_index(v.declaration));put(j,"candidates",indices(v.candidates));put(j,"c_type",optional_index(v.c_type));put(j,"c_callable",optional_index(v.c_callable));put(j,"c_type_ref",v.c_type_ref?c_ref(*v.c_type_ref):nullptr);return j;
}
J project_dump(const Project &v,const SourceManager &sm) {
  J j=obj();put(j,"root",s(v.root.string()));put(j,"package_name",s(v.package_name));put(j,"backend",s(v.backend));put(j,"word_bits",n(v.word_bits));put(j,"hosted",b(v.hosted));put(j,"entry",n(v.entry.value));
  // C++ owns files per module. The Wz arena appends all files before following
  // imports, so ascending module ID and within-module order reconstruct it.
  J files=array(),mods=array();size_t file_index=0;
  for(const auto &m:v.modules){
    J z=obj();put(z,"id",n(m.id.value));put(z,"logical_path",s(m.logical_path));put(z,"identity",s(m.identity));put(z,"package_name",s(m.package_name));
    J refs=array();for(const auto &f:m.files){push(refs,n(file_index++));J a=obj();put(a,"source",n(f->source->id.value));put(a,"root",n(f->root.value));put(a,"nodes",n(f->nodes.size()));put(a,"tokens",n(f->tokens.size()));put(a,"recovered",b(f->recovered));push(files,a);}put(z,"files",refs);
    put(z,"imports",list(m.imports,[&](const auto &x){J a=obj();put(a,"file",n(x.file));put(a,"node",n(x.node.value));put(a,"target",n(x.target.value));put(a,"alias",s(x.alias));put(a,"span",span(x.span,sm));return a;}));
    put(z,"foreign_exports",list(m.foreign_exports,[](const auto &x){return export_dump(x);}));
    J binding=nullptr;if(m.binding){binding=obj();put(binding,"bundle",n(m.binding->bundle));put(binding,"index",n(m.binding->index));}put(z,"binding",binding);put(z,"c_binding",optional_index(m.c_binding));push(mods,z);
  }
  put(j,"files",files);put(j,"modules",mods);
  put(j,"bindings",list(v.bindings,[&](const auto &x){J a=obj();put(a,"path",s(x.path.string()));put(a,"sha256",s(x.sha256));put(a,"source",span(x.source,sm));put(a,"schema_version",n(x.schema_version));put(a,"contract",s(x.contract));put(a,"modules",list(x.modules,[&](const auto &q){J z=obj();put(z,"logical_path",s(q.logical_path));put(z,"host_module",s(q.host_module));put(z,"provider",s(q.provider));put(z,"source",span(q.source,sm));return z;}));put(a,"declarations",indices(x.declarations));return a;}));
  put(j,"foreign_bindings",list(v.foreign_bindings,[&](const auto &x){J a=obj();put(a,"id",n(x.id.value));put(a,"bundle",n(x.bundle));put(a,"source",span(x.source,sm));put(a,"json_pointer",s(x.json_pointer));put(a,"identity",s(x.identity));put(a,"module",s(x.module));put(a,"name",s(x.name));put(a,"stable_identity",s(x.stable_identity));put(a,"detail_kind",n(x.detail.index()));return a;}));
  put(j,"c_bindings",list(v.c_bindings,[&](const auto &x){J a=obj();put(a,"path",s(x.path.string()));put(a,"sha256",s(x.sha256));put(a,"source",span(x.source,sm));put(a,"schema_version",n(x.schema_version));put(a,"contract",s(x.contract));put(a,"abi",s(x.abi));put(a,"target",s(x.target));put(a,"requires_hosted",b(x.requires_hosted));put(a,"types",indices(x.types));put(a,"callables",indices(x.callables));put(a,"modules",list(x.modules,[&](const auto &q){J z=obj();put(z,"logical_path",s(q.logical_path));put(z,"provider",s(q.provider));put(z,"library",s(q.library));put(z,"source",span(q.source,sm));return z;}));return a;}));
  put(j,"c_types",list(v.c_types,[&](const auto &x){J a=obj();put(a,"id",n(x.id));put(a,"bundle",n(x.bundle));put(a,"source",span(x.source,sm));put(a,"identity",s(x.identity));put(a,"module",s(x.module));put(a,"name",s(x.name));put(a,"kind",n(unsigned(x.kind)));return a;}));
  put(j,"c_callables",list(v.c_callables,[&](const auto &x){J a=obj();put(a,"id",n(x.id));put(a,"bundle",n(x.bundle));put(a,"source",span(x.source,sm));put(a,"identity",s(x.identity));put(a,"module",s(x.module));put(a,"name",s(x.name));put(a,"stable_identity",s(x.stable_identity));return a;}));
  put(j,"c_libraries",strings(v.c_libraries));put(j,"c_library_dirs",list(v.c_library_dirs,[](const auto &x){return s(x.string());}));return j;
}
int main(int argc,char **argv) {
  try {
    if(argc!=4)throw std::runtime_error("usage: oracle MANIFEST STD_SOURCE_OR_DASH STD_BINDING_OR_DASH");
    SourceManager sm;ProjectOptions options{argv[1],{}, {}};
    if(std::string(argv[2])!="-")options.std_source_root=argv[2];
    if(std::string(argv[3])!="-")options.std_binding_bundle=argv[3];
    auto r=load_project(options,sm);J j=obj();
    const char *phases[]={"complete","recovered","failed","cancelled"};const char *failures[]={"source","unsupported","environment","internal"};
    put(j,"state",s(phases[unsigned(r.state)]));put(j,"failure",s(failures[unsigned(r.failure)]));put(j,"has_value",b(bool(r.value)));put(j,"success",b(r.success()));put(j,"exit_code",n(unsigned(r.exit_code())));
    put(j,"diagnostics",list(r.diagnostics,[&](const auto &x){return diagnostic(x,sm);}));
    J sources=array();for(uint64_t id=1;;++id){try{push(sources,source_dump(*sm.get(SourceSnapshotId(id))));}catch(const std::out_of_range &){break;}}put(j,"sources",sources);
    put(j,"project",r.value?project_dump(*r.value,sm):nullptr);
    std::cout<<json_object_to_json_string_ext(j,JSON_C_TO_STRING_PLAIN)<<'\n';json_object_put(j);return 0;
  }catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 99;}
}
