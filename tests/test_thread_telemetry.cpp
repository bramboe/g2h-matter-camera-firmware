#include "../app/src/g2h_thread_radio.hpp"
#include <cassert>
#include <iostream>
#include <map>
int main(){
 using namespace g2h;RadioDataset d;
 std::vector<std::string> n={"| Role | RLOC16 | Age | Avg RSSI | Last RSSI |R|D|N| Extended MAC |", "+------+--------+-----+----------+-----------+-+-+-+------------------+", "| R | 0xc800 | 2 | -29 | -30 |1|1|1| 9a91556102c39ddb |"};
 assert(ParseRadioTable(n,"neighbor",d)&&d.count==1&&d.Fresh());
 auto bad=n;bad.pop_back();assert(ParseRadioTable(bad,"neighbor",d)&&d.count==0); // Empty table is complete.
 bad=n;bad[2].pop_back();assert(!ParseRadioTable(bad,"neighbor",d));
 bad=n;bad[2].replace(bad[2].find("-29"),3,"-129");assert(!ParseRadioTable(bad,"neighbor",d));
 bad=n;bad.push_back("Error 7: InvalidArgs");assert(!ParseRadioTable(bad,"neighbor",d));
 bad=n;bad.insert(bad.begin(),"neighbor table");assert(!ParseRadioTable(bad,"neighbor",d));
 std::vector<std::string> child={"| ID | RLOC16 | Timeout | Age | LQ In | C_VN |R|D|N|Ver|CSL|QMsgCnt|Suprvsn| Extended MAC |", "+----+--------+---------+-----+-------+------+-+-+-+---+---+-------+-------+------------------+", "| 1 | 0xc801 | 240 | 24 | 3 | 131 |1|0|0|3|0|0|0|4ecede68435358ac|"};
 assert(ParseRadioTable(child,"child",d)&&d.count==1);
 std::vector<std::string> routers={"| ID | RLOC16 | Next Hop | Path Cost | LQ In | LQ Out | Age | Extended MAC | Link |", "+----+--------+----------+-----------+-------+--------+-----+------------------+------+", "| 49 | 0xc400 | 63 | 0 | 3 | 3 | 0 | faa1c03908e2dbf2 | 1 |"};
 assert(ParseRadioTable(routers,"router",d)&&d.count==1);
 bad=routers;bad[2].replace(bad[2].find("| 3 |"),5,"| 4 |");assert(!ParseRadioTable(bad,"router",d));
 RadioDataset freshNeighbors,staleChildren;
 assert(ParseRadioTable(n,"neighbor",freshNeighbors));assert(ParseRadioTable(child,"child",staleChildren));
 staleChildren.sampled-=std::chrono::seconds(91);assert(freshNeighbors.Fresh()&&!staleChildren.Fresh());
 auto full=n;while(full.size()<66)full.push_back(n.back());assert(ParseRadioTable(full,"neighbor",d)&&d.count==64);
 // Complete counts do not require one oversized Matter string. Preview
 // truncates only at whole lines and makes the omission explicit.
 const auto preview=RadioTablePreview(d.table);assert(d.count==64&&preview.size()<=kRadioPreviewLimit);
 assert(preview.find(kRadioPreviewTruncated)!=std::string::npos);
 assert(RadioTablePreview("small\ntable")=="small\ntable");
 assert(RadioTablePreview(std::string(2000,'x'))==kRadioPreviewTruncated);
 auto atBoundary=std::string(kRadioPreviewLimit,'a');assert(RadioTablePreview(atBoundary)==atBoundary);
 // Reject oversized numeric cells and table data instead of partial topology.
 bad=n;bad[2].insert(bad[2].find("-29"),13000,'0');assert(!ParseRadioTable(bad,"neighbor",d));
 bad=n;while(bad.size()<67)bad.push_back(n.back());assert(!ParseRadioTable(bad,"neighbor",d));
 RadioCounters a,b;assert(ParseRadioCounters({"TxTotal: 4294967295"," RxTotal: 4"," TxRetry: 1"},"100",a));
 assert(ParseRadioCounters({"TxTotal: 3","RxTotal: 5","TxRetry: 1"},"200",b));
 assert(!RadioCounterDelta(a,b,a.tx,b.tx)); // Wrap/reset is unknown, never a huge rate.
 assert(!RadioCounterDelta(b,a,b.rx,a.rx)); // Radio reboot/uptime regression.
 a.sampled-=std::chrono::seconds(91);assert(!a.Fresh()&&!RadioCounterDelta(a,b,0,5));
 assert(!ParseRadioCounters({"TxTotal: 4294967296","RxTotal: 0","TxRetry: 0"},"1",a));
 assert(!ParseRadioCounters({"TxTotal: 1","TxTotal: 2","RxTotal: 0","TxRetry: 0"},"1",a));
 assert(!ParseRadioCounters({"TxTotal: 1","RxTotal: 0"},"1",a));
 // Real byte-stream fixture: require matching echo, ignore old logs, optional
 // unsupported commands must not discard the seven core fields.
 int master=posix_openpt(O_RDWR|O_NOCTTY);assert(master>=0&&!grantpt(master)&&!unlockpt(master));
 std::string slave=ptsname(master),lock="/tmp/g2h-radio-telemetry-"+std::to_string(getpid());int keep=open(slave.c_str(),O_RDWR|O_NOCTTY);assert(keep>=0);
 std::map<std::string,std::string> v={{"state","router"},{"channel","25"},{"networkname","test-thread"},{"panid","0x421b"},{"extpanid","0123456789abcdef"},{"extaddr","fedcba9876543210"},{"rloc16","a400"},{"ifconfig","up"}};
 std::thread emu([&]{std::string q;for(unsigned i=0;i<12;){char c;assert(read(master,&c,1)==1);if(c=='\r')continue;if(c!='\n'){q+=c;continue;}if(q.empty())continue;
  std::string payload=v.count(q)?v[q]+"\r\nDone":"Error 7: InvalidArgs";
  if(q=="neighbor table"){payload.clear();for(auto &line:n)payload+=line+"\r\n";payload+="Done";}
  std::string answer="old log\r\nDone\r\n> "+q+"\r\n"+payload+"\r\n> ";assert(write(master,answer.data(),answer.size())==ssize_t(answer.size()));q.clear();++i;}});
 ThreadRadio radio(slave,lock);assert(radio.PollOnce());auto s=radio.Snapshot();assert(s.Fresh()&&s.role=="router"&&s.enabled&&*s.enabled&&s.neighbors.Fresh()&&s.neighbors.count==1&&!s.children.Fresh()&&!s.routers.Fresh()&&!s.counters.Fresh());emu.join();close(keep);close(master);
 // A truncated mandatory response must time out within the documented budget.
 master=posix_openpt(O_RDWR|O_NOCTTY);assert(master>=0&&!grantpt(master)&&!unlockpt(master));slave=ptsname(master);keep=open(slave.c_str(),O_RDWR|O_NOCTTY);assert(keep>=0);
 std::thread truncated([&]{char c;while(read(master,&c,1)==1&&c!='\n'){}std::string response="state\r\nrouter\r\n";assert(write(master,response.data(),response.size())==ssize_t(response.size()));});
 ThreadRadio incomplete(slave,lock+"-truncated");auto began=std::chrono::steady_clock::now();assert(!incomplete.PollOnce());auto elapsed=std::chrono::steady_clock::now()-began;
 assert(elapsed>=std::chrono::seconds(2)&&elapsed<std::chrono::seconds(4));assert(!incomplete.Snapshot().Fresh());truncated.join();close(keep);close(master);
 std::cout<<"THREAD_TELEMETRY_OK\n";
}
