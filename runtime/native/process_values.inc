// Common argument, environment, and exec-vector handling. The including
// platform supplies the environment lock and the process entry point.
namespace {
void lock() noexcept;
void unlock() noexcept;

uint64_t startup_argc;
const char *const *startup_argv;
const char *const *startup_environment;
LangVec *environment;
struct Guard {Guard() noexcept {lock();} ~Guard(){unlock();}};
uint64_t size(const char *s) noexcept {uint64_t n=0;while(s[n])++n;return n;}
void copy(void *out,const void *in,uint64_t n) noexcept {
  auto *a=static_cast<volatile uint8_t *>(out);
  auto *b=static_cast<const volatile uint8_t *>(in);
  for(uint64_t i=0;i<n;++i)a[i]=b[i];
}
uint64_t add(uint64_t a,uint64_t b) noexcept {
  if(b>~uint64_t{0}-a)lang_native_panic_v1(6,0,0,0);
  return a+b;
}
void *allocate(uint64_t n,uint64_t alignment=8) noexcept {
  auto *p=lang_alloc_v1(n,alignment);
  if(!p)lang_native_panic_v1(5,0,0,0);
  return p;
}
void retain_vec(void *p) {lang_vec_retain_v1(*static_cast<LangVec **>(p));}
void drop_vec(void *p) {lang_vec_release_v1(*static_cast<LangVec **>(p));}
void retain_text(void *p) {auto *s=static_cast<LangText *>(p);if(s->owner)lang_handle_retain_v1(s->owner);}
void drop_text(void *p) {auto *s=static_cast<LangText *>(p);if(s->owner)lang_handle_release_v1(s->owner);}
struct Bytes {const uint8_t *data;uint64_t length;};
int64_t byte_view(const LangVec *v,Bytes &out) noexcept {
  if(!v || v->stride!=1 || v->initialized!=v->length || v->copy || v->drop ||
     (v->length && !v->data))return -22;
  out={static_cast<const uint8_t *>(v->data),v->length};return 0;
}
int64_t text_view(const LangText *v,Bytes &out) noexcept {
  if(!v || (v->bytes && !v->data))return -22;
  out={v->data,v->bytes};return 0;
}
bool nul(Bytes b) noexcept {for(uint64_t i=0;i<b.length;++i)if(!b.data[i])return true;return false;}
int64_t key(Bytes b) noexcept {
  if(!b.length || nul(b))return -22;
  for(uint64_t i=0;i<b.length;++i)if(b.data[i]=='=')return -22;
  return 0;
}
LangVec *bytes(Bytes b) noexcept {
  auto *v=lang_vec_new_v1(b.length,1,nullptr,nullptr);
  copy(v->data,b.data,b.length);v->initialized=b.length;return v;
}
LangVec *nested() noexcept {return lang_vec_new_v1(0,sizeof(LangVec *),retain_vec,drop_vec);}
void append(LangVec *v,Bytes b) noexcept {auto *row=bytes(b);lang_vec_push_v1(v,&row);}
void load_environment() noexcept {
  if(environment)return;
  environment=nested();
  if(startup_environment)for(auto *p=startup_environment;*p;++p)
    append(environment,{reinterpret_cast<const uint8_t *>(*p),size(*p)});
}
LangVec *clone_bytes(const LangVec *v) noexcept {
  auto *result=nested();
  auto **rows=static_cast<LangVec **>(v->data);
  for(uint64_t i=0;i<v->length;++i)append(result,{static_cast<const uint8_t *>(rows[i]->data),rows[i]->length});
  return result;
}
int64_t to_texts(LangVec **out,const LangVec *input) noexcept {
  auto *result=lang_vec_new_v1(0,sizeof(LangText),retain_text,drop_text);
  auto **rows=static_cast<LangVec **>(input->data);
  for(uint64_t i=0;i<input->length;++i) {
    LangText s;
    if(lang_text_from_bytes_v1(&s,rows[i])<0) {lang_vec_release_v1(result);return -84;}
    lang_vec_push_v1(result,&s);
  }
  *out=result;return 0;
}
bool matches(const LangVec *row,Bytes name) noexcept {
  if(row->length<=name.length)return false;
  auto *p=static_cast<const uint8_t *>(row->data);
  for(uint64_t i=0;i<name.length;++i)if(p[i]!=name.data[i])return false;
  return p[name.length]=='=';
}
int64_t getenv_bytes(LangVec **out,Bytes name) noexcept {
  if(!out)return -14;
  *out=nullptr;
  auto status=key(name);if(status<0)return status;
  Guard guard;load_environment();
  auto **rows=static_cast<LangVec **>(environment->data);
  for(uint64_t i=0;i<environment->length;++i)if(matches(rows[i],name)) {
    *out=bytes({static_cast<const uint8_t *>(rows[i]->data)+name.length+1,rows[i]->length-name.length-1});
    return 1;
  }
  return 0;
}
int64_t setenv(Bytes name,Bytes value,int32_t overwrite,bool remove) noexcept {
  auto status=key(name);if(status<0)return status;
  if(!remove && nul(value))return -22;
  Guard guard;load_environment();
  auto **rows=static_cast<LangVec **>(environment->data);
  bool found=false;
  for(uint64_t i=0;i<environment->length;++i)if(matches(rows[i],name))found=true;
  if(found && !overwrite && !remove)return 0;
  // A new snapshot removes every duplicate key, including inherited duplicates.
  auto *next=nested();
  for(uint64_t i=0;i<environment->length;++i)if(!matches(rows[i],name))
    append(next,{static_cast<const uint8_t *>(rows[i]->data),rows[i]->length});
  if(!remove) {
    const auto n=add(add(name.length,1),value.length);
    auto *row=lang_vec_new_v1(n,1,nullptr,nullptr);
    auto *p=static_cast<uint8_t *>(row->data);
    copy(p,name.data,name.length);p[name.length]='=';copy(p+name.length+1,value.data,value.length);
    row->initialized=n;lang_vec_push_v1(next,&row);
  }
  lang_vec_release_v1(environment);environment=next;return 0;
}
int64_t row(const LangVec *v,uint64_t i,bool raw,Bytes &out) noexcept {
  const uint64_t stride=raw?sizeof(LangVec *):sizeof(LangText);
  if(!v || v->stride!=stride || v->initialized!=v->length ||
     (v->length && !v->data) || i>=v->length)return -22;
  auto *p=static_cast<const uint8_t *>(v->data)+i*stride;
  return raw?byte_view(*reinterpret_cast<LangVec *const *>(p),out):text_view(reinterpret_cast<const LangText *>(p),out);
}
struct Exec {
  char *storage=nullptr,*path=nullptr;
  char **arguments=nullptr,**environment=nullptr;
  ~Exec(){lang_free_v1(storage);}
  int64_t prepare(Bytes file,const LangVec *args,const LangVec *env,bool raw) noexcept {
    if(!file.length || nul(file))return -22;
    uint64_t n=add(file.length,1);
    const uint64_t stride=raw?sizeof(LangVec *):sizeof(LangText);
    for(auto *v:{args,env}) {
      if(!v || v->stride!=stride || v->initialized!=v->length || (v->length&&!v->data))return -22;
      if(v->length>(~uint64_t{0}/sizeof(char *))-1)lang_native_panic_v1(6,0,0,0);
      n=add(n,(v->length+1)*sizeof(char *));
      for(uint64_t i=0;i<v->length;++i) {
        Bytes b;auto status=row(v,i,raw,b);if(status<0)return status;
        if(nul(b))return -22;
        n=add(n,add(b.length,1));
      }
    }
    storage=static_cast<char *>(allocate(n));
    arguments=reinterpret_cast<char **>(storage);
    environment=arguments+args->length+1;
    char *p=reinterpret_cast<char *>(environment+env->length+1);
    path=p;copy(p,file.data,file.length);p+=file.length;*p++=0;
    unsigned index=0;
    for(auto *v:{args,env}) {
      auto **pointers=index++?environment:arguments;
      for(uint64_t i=0;i<v->length;++i) {
        Bytes b;row(v,i,raw,b);pointers[i]=p;copy(p,b.data,b.length);p+=b.length;*p++=0;
      }
      pointers[v->length]=nullptr;
    }
    return 0;
  }
};
} // namespace

extern "C" void lang_process_init_v1(uint64_t argc,const char *const *argv,const char *const *env) noexcept {
  startup_argc=argc;startup_argv=argv;startup_environment=env;
}
extern "C" int64_t lang_process_args_bytes_v1(LangVec **out) noexcept {
  if(!out)return -14;
  auto *result=nested();
  for(uint64_t i=0;i<startup_argc;++i)append(result,{reinterpret_cast<const uint8_t *>(startup_argv[i]),size(startup_argv[i])});
  *out=result;return 0;
}
extern "C" int64_t lang_process_args_v1(LangVec **out) noexcept {
  if(!out)return -14;
  LangVec *raw;lang_process_args_bytes_v1(&raw);auto status=to_texts(out,raw);lang_vec_release_v1(raw);return status;
}
extern "C" int64_t lang_process_environment_bytes_v1(LangVec **out) noexcept {
  if(!out)return -14;
  Guard guard;load_environment();*out=clone_bytes(environment);return 0;
}
extern "C" int64_t lang_process_environment_v1(LangVec **out) noexcept {
  if(!out)return -14;
  Guard guard;load_environment();return to_texts(out,environment);
}
extern "C" int64_t lang_process_getenv_bytes_v1(LangVec **out,const LangVec *name) noexcept {
  if(!out)return -14;
  *out=nullptr;
  Bytes n;auto status=byte_view(name,n);return status<0?status:getenv_bytes(out,n);
}
extern "C" int64_t lang_process_getenv_v1(LangText *out,const LangText *name) noexcept {
  if(!out)return -14;
  *out={};Bytes n;auto status=text_view(name,n);if(status<0)return status;
  LangVec *v;status=getenv_bytes(&v,n);
  if(status==1) {auto valid=lang_text_from_bytes_v1(out,v);lang_vec_release_v1(v);if(valid<0)return -84;}
  return status;
}
extern "C" int64_t lang_process_setenv_v1(const LangText *name,const LangText *value,int32_t overwrite) noexcept {
  Bytes n,v;auto status=text_view(name,n);if(!status)status=text_view(value,v);return status<0?status:setenv(n,v,overwrite,false);
}
extern "C" int64_t lang_process_setenv_bytes_v1(const LangVec *name,const LangVec *value,int32_t overwrite) noexcept {
  Bytes n,v;auto status=byte_view(name,n);if(!status)status=byte_view(value,v);return status<0?status:setenv(n,v,overwrite,false);
}
extern "C" int64_t lang_process_unsetenv_v1(const LangText *name) noexcept {
  Bytes n;auto status=text_view(name,n);return status<0?status:setenv(n,{},1,true);
}
extern "C" int64_t lang_process_unsetenv_bytes_v1(const LangVec *name) noexcept {
  Bytes n;auto status=byte_view(name,n);return status<0?status:setenv(n,{},1,true);
}
