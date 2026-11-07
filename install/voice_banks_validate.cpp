// Read-only target qualification helper. Does not change a bank or native setting.
#include "../app/src/g2h_voice_banks.hpp"
#include "../app/src/g2h_vendor_control.hpp"
#include <cstdio>
int main(){
    g2h::VoiceBanks banks;std::string reply;
    if(!banks.Valid()||banks.Current()!=g2h::language::Id::English||!g2h::VendorExchange("misc language",reply))return 1;
    const auto first=reply.find_first_not_of(" \t\r\n"),last=reply.find_last_not_of(" \t\r\n");
    if(first==reply.npos||reply.substr(first,last-first+1)!="en")return 1;
    puts("VOICE_BANKS_VALID_EN");return 0;
}
