// SPDX-License-Identifier: GPL-3.0-only
// persistent helper threads for per-frame pixel passes. HUD keep and the FSR 3 colour conversions used to
// create three std::threads for every generated frame (several hundred thread creations a second at 120 fps).
// run(n, grain, f) splits [0,n) into four ranges aligned to grain; the caller does the last range itself.
// One caller at a time per pool.
#ifndef PK_THREAD_POOL_H
#define PK_THREAD_POOL_H
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <algorithm>
#include <cstdint>
struct PkPool{
 static constexpr int kHelpers=3;
 std::mutex m;std::condition_variable go,done;std::thread threads[kHelpers];
 std::function<void(size_t,size_t)> task;size_t total=0,grain=1;uint64_t generation=0;int pending=0;bool stop=false,started=false,failed=false;
 ~PkPool(){if(!started)return;{std::lock_guard<std::mutex> l(m);stop=true;}go.notify_all();for(auto& t:threads)if(t.joinable())t.join();}
 void range(int i,size_t& a,size_t& b)const{size_t q=(total/(kHelpers+1)+grain-1)/grain*grain;a=std::min(total,q*i);b=i==kHelpers?total:std::min(total,q*(i+1));}
 void start(){started=true;try{for(int i=0;i<kHelpers;i++)threads[i]=std::thread([this,i]{uint64_t seen=0;for(;;){std::function<void(size_t,size_t)> f;size_t a,b;
  {std::unique_lock<std::mutex> l(m);go.wait(l,[&]{return stop||generation!=seen;});if(stop)return;seen=generation;f=task;range(i,a,b);}
  if(a<b)f(a,b);{std::lock_guard<std::mutex> l(m);if(--pending==0)done.notify_one();}}});}
  catch(...){{std::lock_guard<std::mutex> l(m);stop=true;}go.notify_all();for(auto& t:threads)if(t.joinable())t.join();failed=true;}}
 void run(size_t n,size_t g,const std::function<void(size_t,size_t)>& f){
  if(!started)start();
  if(failed){f(0,n);return;} // no helper threads: run inline
  {std::lock_guard<std::mutex> l(m);task=f;total=n;grain=g?g:1;pending=kHelpers;++generation;}go.notify_all();
  size_t a,b;range(kHelpers,a,b);if(a<b)f(a,b);
  std::unique_lock<std::mutex> l(m);done.wait(l,[&]{return pending==0;});task=nullptr;
 }
};
#endif
