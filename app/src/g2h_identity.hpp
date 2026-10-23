#pragma once
#include <string>
#include <vector>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

namespace g2h {
struct Identity {
    unsigned pin=0,discriminator=0;
    std::string serial,unique;
    static bool Number(const std::string &s,unsigned &value){
        if(s.empty()||s.size()>8)return false;value=0;
        for(char c:s){if(c<'0'||c>'9')return false;value=value*10+unsigned(c-'0');}return true;
    }
    static bool ValidPin(unsigned pin){
        if(!pin||pin>99999998||pin==12345678||pin==87654321)return false;
        for(unsigned d=1;d<=9;++d)if(pin==d*11111111u)return false;
        return true;
    }
    bool Load(const char *path){
        const int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return false;
        struct stat st{};
        if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_uid!=geteuid()||(st.st_mode&077)!=0||st.st_nlink!=1||st.st_size<1||st.st_size>256){close(fd);return false;}
        char data[257]{};ssize_t used=0;
        while(used<st.st_size){ssize_t n=read(fd,data+used,st.st_size-used);if(n<=0){close(fd);return false;}used+=n;}
        close(fd);std::vector<std::string> lines;std::string line;
        for(ssize_t n=0;n<used;++n){if(data[n]=='\n'){lines.push_back(line);line.clear();}else if(data[n]!='\0'&&line.size()<96)line+=data[n];else return false;}
        if(!line.empty()||lines.size()!=5||lines[0]!="G2H_CAMERA_IDENTITY_V1"||!Number(lines[1],pin)||!ValidPin(pin)||!Number(lines[2],discriminator)||discriminator>4095)return false;
        serial=lines[3];unique=lines[4];
        if(serial.size()!=23||serial.compare(0,7,"g2hcam-")||unique.size()!=32)return false;
        for(char c:serial.substr(7)+unique)if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return false;
        return true;
    }
};
}
