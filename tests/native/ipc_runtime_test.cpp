#include "../../runtime/native/ipc.hpp"
#include "../../runtime/native/process.hpp"
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>

namespace {
void need(bool b,const char *m){if(!b)throw std::runtime_error(m);}
void wait_ok(int64_t pid){int32_t s=-1;need(pid>0&&lang_process_waitpid_v1(pid,0,&s)==pid&&s==0,"child failed");}
void child_need(bool ok){if(!ok)_exit(88);}
LangText text(const std::string &s){return {reinterpret_cast<const uint8_t *>(s.data()),s.size(),s.size(),nullptr};}
void mapping(){
  LangHandle *h=nullptr;need(lang_ipc_map_anon_v1(&h,4096,3)==0,"map failed");need(lang_ipc_length_v1(h)==4096,"length differs");
  need(lang_ipc_write_i64_v1(h,80,123)==0,"i64 write failed");int64_t n=0;need(lang_ipc_read_i64_v1(&n,h,80)==0&&n==123,"i64 read differs");
  need(lang_ipc_read_i64_v1(&n,h,4090)==-22&&lang_ipc_read_i64_v1(&n,h,81)==-22&&lang_ipc_write_i32_v1(h,~uint64_t{0},1)==-22,"bounds/alignment accepted");
  bool changed=false;need(lang_ipc_atomic_compare_v1(&changed,h,80,123,9)==0&&changed,"CAS failed");
  need(lang_ipc_atomic_add_v1(&n,h,80,3)==0&&n==9,"atomic add differs");
  need(lang_ipc_atomic_load_v1(&n,h,80)==0&&n==12,"atomic load differs");
  uint8_t a=7;auto *v=lang_vec_new_v1(1,1,nullptr,nullptr);lang_vec_init_v1(v,&a);
  need(lang_ipc_write_bytes_v1(h,4095,v,0,1)==0&&lang_ipc_write_bytes_v1(h,4095,v,0,2)==-22,"byte bounds differ");LangVec *out;
  need(lang_ipc_read_bytes_v1(&out,h,4095,1)==0&&out->length==1&&*static_cast<uint8_t *>(out->data)==7,"bytes differ");lang_vec_release_v1(out);lang_vec_release_v1(v);
  lang_handle_retain_v1(h);lang_handle_release_v1(h);need(lang_ipc_read_i64_v1(&n,h,80)==0,"ARC alias lost map");lang_handle_release_v1(h);
  need(lang_ipc_map_anon_v1(&h,0,3)==-22&&lang_ipc_map_anon_v1(&h,4096,4)==-22,"bad mapping accepted");
  need(lang_ipc_map_anon_v1(&h,4096,1)==0,"readonly map failed");need(lang_ipc_write_i32_v1(h,0,1)==-13,"readonly write accepted");lang_handle_release_v1(h);
}
void shared(){
  LangHandle *h;need(lang_ipc_map_anon_v1(&h,4096,3)==0,"map failed");
  need(lang_ipc_mutex_init_v1(h,0)==0&&lang_ipc_cond_init_v1(h,16)==0&&lang_ipc_sem_init_v1(h,32,0)==0,"sync init failed");
  need(lang_ipc_mutex_init_v1(h,0)==-17,"double init accepted");
  need(lang_ipc_sem_trywait_v1(h,32)==-11&&lang_ipc_sem_wait_v1(h,32,1000000)==-110,"semaphore timeout differs");
  need(lang_ipc_mutex_lock_v1(h,0)==0,"lock failed");need(lang_ipc_cond_wait_v1(h,16,h,0,1000000)==-110,"condition timeout differs");need(lang_ipc_mutex_unlock_v1(h,0)==0,"timeout lost mutex");
  constexpr unsigned children=4,iterations=4000;int64_t pids[children];
  for(unsigned c=0;c<children;++c){pids[c]=lang_process_fork_v1();if(!pids[c]){
    for(unsigned i=0;i<iterations;++i){int64_t n;child_need(lang_ipc_mutex_lock_v1(h,0)==0);child_need(lang_ipc_read_i64_v1(&n,h,64)==0);child_need(lang_ipc_write_i64_v1(h,64,n+1)==0);child_need(lang_ipc_mutex_unlock_v1(h,0)==0);child_need(lang_ipc_atomic_add_v1(&n,h,72,1)==0);}
    child_need(lang_ipc_sem_post_v1(h,32)==0);_exit(0);
  }}
  for(unsigned c=0;c<children;++c)need(lang_ipc_sem_wait_v1(h,32,5000000000)==0,"shared semaphore stuck");
  for(auto pid:pids)wait_ok(pid);
  int64_t n;need(lang_ipc_read_i64_v1(&n,h,64)==0&&n==children*iterations,"shared mutex lost update");need(lang_ipc_atomic_load_v1(&n,h,72)==0&&n==children*iterations,"shared atomic lost update");
  auto pid=lang_process_fork_v1();if(!pid){child_need(lang_ipc_mutex_lock_v1(h,0)==0);child_need(lang_ipc_sem_post_v1(h,32)==0);for(;;){child_need(lang_ipc_read_i64_v1(&n,h,80)==0);if(n)break;child_need(lang_ipc_cond_wait_v1(h,16,h,0,5000000000)==0);}child_need(lang_ipc_mutex_unlock_v1(h,0)==0);_exit(0);}
  need(lang_ipc_sem_wait_v1(h,32,5000000000)==0,"condition waiter not ready");
  need(lang_ipc_mutex_lock_v1(h,0)==0,"lock failed");need(lang_ipc_write_i64_v1(h,80,1)==0,"write failed");need(lang_ipc_cond_broadcast_v1(h,16)==0,"broadcast failed");need(lang_ipc_mutex_unlock_v1(h,0)==0,"unlock failed");wait_ok(pid);
  // Force sequence rollover, then keep two processes exchanging the mutex
  // and condition. The shared condition never stores stack pointers.
  need(lang_ipc_write_i32_v1(h,24,-2)==0,"sequence setup failed");
  pid=lang_process_fork_v1();
  if(!pid) {
    for(int64_t i=0;i<1000;++i) {
      child_need(lang_ipc_mutex_lock_v1(h,0)==0);
      do {child_need(lang_ipc_read_i64_v1(&n,h,88)==0);if(n==2*i)break;
          child_need(lang_ipc_cond_wait_v1(h,16,h,0,5000000000)==0);}while(true);
      child_need(lang_ipc_write_i64_v1(h,88,2*i+1)==0);
      child_need(lang_ipc_cond_broadcast_v1(h,16)==0);
      child_need(lang_ipc_mutex_unlock_v1(h,0)==0);
    }
    _exit(0);
  }
  for(int64_t i=0;i<1000;++i) {
    need(lang_ipc_mutex_lock_v1(h,0)==0,"ping lock failed");
    do {need(lang_ipc_read_i64_v1(&n,h,88)==0,"ping read failed");if(n==2*i+1)break;
        need(lang_ipc_cond_wait_v1(h,16,h,0,5000000000)==0,"ping wait failed");}while(true);
    need(lang_ipc_write_i64_v1(h,88,2*i+2)==0,"ping write failed");
    need(lang_ipc_cond_signal_v1(h,16)==0,"ping signal failed");
    need(lang_ipc_mutex_unlock_v1(h,0)==0,"ping unlock failed");
  }
  wait_ok(pid);
  need(lang_ipc_mutex_destroy_v1(h,0)==0&&lang_ipc_cond_destroy_v1(h,16)==0&&lang_ipc_sem_destroy_v1(h,32)==0,"destroy failed");lang_handle_release_v1(h);
}
void named(){
  std::string name="/woozy-ipc-"+std::to_string(getpid());auto t=text(name);
  auto fd=lang_ipc_shm_open_v1(&t,2|64|128,0600);need(fd>=0,"shm open failed");need(ftruncate(fd,4096)==0,"truncate failed");
  LangHandle *a,*b;need(lang_ipc_map_file_v1(&a,fd,4096,0,3)==0,"file map failed");need(lang_ipc_map_file_v1(&b,fd,4096,0,3)==0,"file alias failed");close(fd);
  need(lang_ipc_shm_unlink_v1(&t)==0,"shm unlink failed");need(lang_ipc_write_i32_v1(a,0,42)==0,"file write failed");int32_t n;need(lang_ipc_read_i32_v1(&n,b,0)==0&&n==42,"file mappings do not share");lang_handle_release_v1(a);lang_handle_release_v1(b);
  for(const auto &s:{std::string("bad"),std::string("/a/b"),std::string("/.."),std::string("/.")}){auto bad=text(s);need(lang_ipc_shm_open_v1(&bad,2|64,0600)==-22,"unsafe shm name accepted");}
}
}
int main(){try{mapping();shared();named();std::cout<<"ipc runtime: ok\n";}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}
