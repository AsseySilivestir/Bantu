#include "event_loop.hpp"
#include <cassert>
#include <cstdio>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
using namespace bantu_loop;
static int P=0,F=0;
static void ck(const char* n, bool c){ if(c){P++;printf("  ok    %s\n",n);} else {F++;printf("  FAIL  %s\n",n);} }

// Runs the whole suite against one backend. Called for the platform's best
// backend AND for the portable poll fallback, because Linux CI and Windows use
// the latter and a bug that only shows up there is a bug that ships.
static int run(std::unique_ptr<Backend> be){
    printf("\n  -- backend: %s --\n", be->name());

    // listener
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int one=1; setsockopt(ls,SOL_SOCKET,SO_REUSEADDR,&one,sizeof one);
    sockaddr_in a{}; a.sin_family=AF_INET; a.sin_addr.s_addr=htonl(INADDR_LOOPBACK); a.sin_port=0;
    bind(ls,(sockaddr*)&a,sizeof a); listen(ls,16);
    socklen_t al=sizeof a; getsockname(ls,(sockaddr*)&a,&al);
    ck("setNonBlocking(listener)", setNonBlocking(ls));
    ck("add listener", be->add(ls,true,false));

    std::vector<Event> evs;
    ck("idle wait times out", be->wait(evs,50)==0);

    // connect
    int c = socket(AF_INET, SOCK_STREAM, 0);
    setNonBlocking(c);
    connect(c,(sockaddr*)&a,sizeof a);
    int n = be->wait(evs,1000);
    ck("listener became readable", n==1 && evs[0].fd==ls && evs[0].readable);

    int s = accept(ls,nullptr,nullptr);
    setNonBlocking(s);
    be->add(s,true,false);

    // data arrives
    const char* msg="hello";
    send(c,msg,5,0);
    n = be->wait(evs,1000);
    bool sawRead=false; for(auto&e:evs) if(e.fd==s&&e.readable) sawRead=true;
    ck("peer socket became readable", sawRead);
    char buf[64]; ssize_t got=recv(s,buf,sizeof buf,0);
    ck("payload intact", got==5 && memcmp(buf,"hello",5)==0);

    // no spurious readable once drained
    n = be->wait(evs,50);
    sawRead=false; for(auto&e:evs) if(e.fd==s&&e.readable) sawRead=true;
    ck("no readable event once drained", !sawRead);

    // writable toggling
    be->mod(s,true,true);
    n = be->wait(evs,1000);
    bool sawWrite=false; for(auto&e:evs) if(e.fd==s&&e.writable) sawWrite=true;
    ck("writable when enabled", sawWrite);
    be->mod(s,true,false);
    n = be->wait(evs,50);
    sawWrite=false; for(auto&e:evs) if(e.fd==s&&e.writable) sawWrite=true;
    ck("no writable once disabled", !sawWrite);

    // coalescing: one Event per fd even when both directions are ready
    send(c,"x",1,0);
    be->mod(s,true,true);
    usleep(50000);
    n = be->wait(evs,1000);
    int countS=0; for(auto&e:evs) if(e.fd==s) countS++;
    ck("read+write coalesce into ONE event", countS==1);

    // hangup detection
    close(c);
    n = be->wait(evs,1000);
    bool sawEnd=false; for(auto&e:evs) if(e.fd==s&&(e.error||e.readable)) sawEnd=true;
    ck("peer close is reported", sawEnd);

    // del
    ck("del removes the fd", be->del(s));
    close(s);
    n = be->wait(evs,50);
    bool stale=false; for(auto&e:evs) if(e.fd==s) stale=true;
    ck("no events after del", !stale);

    // many fds (the poll backend's swap-erase index bookkeeping)
    std::vector<int> socks;
    for(int i=0;i<200;i++){ int fd=socket(AF_INET,SOCK_STREAM,0); setNonBlocking(fd); be->add(fd,true,false); socks.push_back(fd);}
    for(size_t i=0;i<socks.size();i+=2) be->del(socks[i]);
    ck("200 add / 100 interleaved del", be->wait(evs,20)>=0);
    for(int fd:socks) { be->del(fd); close(fd); }

    close(ls);
    return 0;
}

int main(){
    run(makeBackend());
    run(std::unique_ptr<Backend>(new PollBackend()));
    printf("\n========================================\n");
    printf("  PASS: %d   FAIL: %d\n", P, F);
    printf("========================================\n");
    printf("  RESULT: %s\n", F ? "FAILURES PRESENT" : "ALL GREEN");
    return F?1:0;
}
