#include "../../../runtime/native/process.hpp"
#include "../../../runtime/native/ipc.hpp"
#include "../../../runtime/native/thread.hpp"
namespace {
LangText literal(const char *s,uint64_t n) {return {reinterpret_cast<const uint8_t *>(s),n,n,nullptr};}
int64_t worker(void *p) noexcept {
  auto *memory=*static_cast<LangHandle **>(p);
  auto pid=lang_process_fork_v1();
  if(!pid) {
    auto *v=lang_vec_new_v1(0,1,nullptr,nullptr);uint8_t n=1;lang_vec_push_v1(v,&n);lang_vec_release_v1(v);
    const long code=lang_ipc_write_i64_v1(memory,64,99)==0?0:1;
    __asm__ volatile("syscall"::"a"(60L),"D"(code):"rcx","r11","memory");__builtin_trap();
  }
  int32_t status=-1;return lang_process_waitpid_v1(pid,0,&status)==pid&&status==0?0:1;
}
}
extern "C" int main() {
  LangVec *arguments,*environment;
  if(lang_process_args_v1(&arguments)!=0 || arguments->length!=2)return 1;
  auto *values=static_cast<LangText *>(arguments->data);
  if(values[1].bytes!=5 || values[1].data[0]!='p')return 2;
  lang_vec_release_v1(arguments);
  if(lang_process_environment_v1(&environment)!=0)return 3;
  auto *args=lang_vec_new_v1(0,sizeof(LangText),nullptr,nullptr);
  auto shell=literal("sh",2),option=literal("-c",2),command=literal("exit 7",6),path=literal("/bin/sh",7);
  lang_vec_push_v1(args,&shell);lang_vec_push_v1(args,&option);lang_vec_push_v1(args,&command);
  auto pid=lang_process_spawn_v1(&path,args,environment,-1,-1,-1);int32_t status=-1;
  if(pid<0 || lang_process_waitpid_v1(pid,0,&status)!=pid || status!=(7<<8))return 4;
  lang_vec_release_v1(args);lang_vec_release_v1(environment);
  LangHandle *memory;if(lang_ipc_map_anon_v1(&memory,4096,3)!=0)return 5;
  if(lang_ipc_mutex_init_v1(memory,0)!=0 || lang_ipc_sem_init_v1(memory,16,1)!=0)return 6;
  if(lang_ipc_mutex_lock_v1(memory,0)!=0 || lang_ipc_mutex_unlock_v1(memory,0)!=0)return 7;
  if(lang_ipc_sem_wait_v1(memory,16,0)!=0 || lang_ipc_sem_post_v1(memory,16)!=0)return 8;
  auto *thread=lang_thread_spawn_v1(worker,&memory,sizeof(memory),nullptr,nullptr);
  if(lang_thread_wait_v1(thread)!=0 || lang_thread_result_v1(thread)!=0)return 9;
  lang_handle_release_v1(thread);int64_t result=0;
  if(lang_ipc_read_i64_v1(&result,memory,64)!=0 || result!=99)return 10;
  if(lang_ipc_mutex_destroy_v1(memory,0)!=0 || lang_ipc_sem_destroy_v1(memory,16)!=0)return 11;
  lang_handle_release_v1(memory);return 0;
}
