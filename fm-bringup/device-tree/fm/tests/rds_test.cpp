// SPDX-License-Identifier: Apache-2.0
#include "RdsDecoder.h"
#include "BcmFmProtocol.h"
#include <cassert>
#include <functional>
#include <iostream>
#include <random>
using namespace bcmfm;
using Bytes = std::vector<uint8_t>;
Bytes group(uint16_t pi, uint16_t b, uint16_t c, uint16_t d) {
    return {0, uint8_t(pi>>8), uint8_t(pi), 0x10, uint8_t(b>>8), uint8_t(b),
            uint8_t(b & 0x0800 ? 0x40 : 0x20), uint8_t(c>>8), uint8_t(c),
            0x30, uint8_t(d>>8), uint8_t(d)};
}
void ps(RdsDecoder& r, const std::string& text, uint16_t pi = 0x1234, bool b = false) {
    assert(text.size() == 8);
    for (int round = 0; round < 3; ++round)
        for (int n = 0; n < 4; ++n)
            r.feed(group(pi, (b ? 0x0800 : 0) | n, b ? pi : 0,
                         (uint8_t(text[2*n]) << 8) | uint8_t(text[2*n+1])), 88000, 0);
}
int main() {
    int total = 0;
    auto test = [&](const char* name, const std::function<void()>& f) {
        f(); ++total; std::cout << "PASS RDS " << name << '\n';
    };
    test("FIFO framing and exact register permissions", [] {
        Bytes req{0x15,0xfc,3,0x80,1,240};
        assert(allowed(req.data(),req.size()));
        for (int n : {0,1,2,3,128,239,241,255}) {
            req[5]=n; assert(!allowed(req.data(),req.size()));
        }
        req[5]=240;
        for (int n=0;n<=243;++n) {
            Bytes evt{0x0e,uint8_t(6+n),1,0x15,0xfc,0,0x80,1}; evt.resize(8+n);
            Bytes data; assert(response(req,evt,&data)==(n<=240 && n%3==0));
        }
        Bytes read{0x15,0xfc,3,0x12,1,2}, shortReply{0x0e,6,1,0x15,0xfc,0,0x12,1}, data;
        assert(!response(read,shortReply,&data));
    });
    test("PS repetition and event consumption", [] {
        RdsDecoder r; r.feed(group(0x1234,0,0,0x5445),88000,0); assert(!r.pi());
        ps(r,"TEST FM "); assert(r.pi()==0x1234 && r.ps()=="TEST FM");
        assert(r.takeEvents()==RdsDecoder::kPs); assert(!r.takeEvents());
        ps(r,"TEST FM "); assert(!r.takeEvents());
    });
    test("0B C-prime identity", [] {
        RdsDecoder r; ps(r,"B RADIO ",0x2345,true); assert(r.ps()=="B RADIO");
        auto bad=group(0x3456,0x0800,0x9999,0x5858);
        for(int n=0;n<10;++n) r.feed(bad,88000,0);
        assert(r.pi()==0x2345);
    });
    test("corrected blocks accepted, uncorrectable blocks rejected", [] {
        RdsDecoder r;
        for (int round=0;round<4;++round) for(int n=0;n<4;++n) {
            auto data=group(0x1234,n,0,0x4141); data[9]|=0x04;
            r.feed(data,88000,0);
        }
        assert(r.ps()=="AAAAAAAA"); r.reset();
        for (int round=0;round<4;++round) for(int n=0;n<4;++n) {
            auto data=group(0x1234,n,0,0x4141); data[9]|=0x0c;
            r.feed(data,88000,0);
        }
        assert(r.ps().empty() && !r.pi());
    });
    test("fragmented FIFO groups and empty markers", [] {
        RdsDecoder r; auto g=group(0x1234,0,0,0x4141);
        for(int n=0;n<2;++n) {
            r.feed(Bytes(g.begin(),g.begin()+6),88000,0);
            r.feed({0x7c,0xff,0xff},88000,0);
            r.feed(Bytes(g.begin()+6,g.end()),88000,0);
        }
        assert(r.pi()==0x1234);
    });
    test("end marker stops decoding padded data", [] {
        RdsDecoder r; auto g=group(0x1234,0,0,0);
        Bytes data{0x7c,0xff,0xff};data.insert(data.end(),g.begin(),g.end());
        r.feed(data,88000,0);r.feed(data,88000,0);assert(!r.pi());
    });
    test("group ordering and malformed payloads", [] {
        RdsDecoder r; auto g=group(0x1234,0,0,0);
        std::swap(g[3],g[6]);
        for(int n=0;n<3;++n)r.feed(g,88000,0);
        assert(!r.pi());assert(!r.feed({0,1},88000,0));assert(!r.feed(Bytes(243),88000,0));
    });
    test("2A text and terminator confirmation", [] {
        RdsDecoder r;
        for (int n=0;n<3;++n) {
            r.feed(group(0x1234,0x2000,0x4865,0x6c6c),88000,0);
            r.feed(group(0x1234,0x2001,0x6f0d,0x7878),88000,0);
        }
        assert(r.text()=="Hello"); assert(r.takeEvents()&RdsDecoder::kRt);
        r.feed(group(0x1234,0x2010,0x4e65,0x770d),88000,0);
        assert(r.text().empty()); // A/B change invalidates old text immediately
        r.feed(group(0x1234,0x2010,0x4e65,0x770d),88000,0); assert(r.text()=="New");
    });
    test("2B text and full length 32/64 strings", [] {
        RdsDecoder r;
        for(int round=0;round<3;++round) for(int n=0;n<16;++n)
            r.feed(group(0x1234,0x2800|n,0x1234,0x4242),88000,0);
        assert(r.text()==std::string(32,'B'));
        for(int round=0;round<3;++round) for(int n=0;n<16;++n)
            r.feed(group(0x1234,0x2000|n,0x4141,0x4141),88000,0);
        assert(r.text()==std::string(64,'A'));
    });
    test("PI change and inactivity invalidate metadata", [] {
        RdsDecoder r; ps(r,"OLD NAME");r.takeEvents();
        r.feed(group(0x4321,0x1000,0,0),88000,0);assert(r.ps()=="OLD NAME");
        r.feed(group(0x4321,0x1000,0,0),88000,0);assert(r.ps().empty());
        assert(r.takeEvents()&RdsDecoder::kPs);
        ps(r,"NEW NAME");r.takeEvents();r.expire(15000);assert(!r.pi()&&r.ps().empty());
        assert(r.takeEvents()&RdsDecoder::kPs);
    });
    test("PS change never publishes a mixed name", [] {
        RdsDecoder r; ps(r,"OLD NAME");r.takeEvents();
        r.feed(group(0x1234,0,0,0x4e45),88000,0);assert(!r.takeEvents());
        r.feed(group(0x1234,0,0,0x4e45),88000,0);assert(r.ps()=="OLD NAME");
        ps(r,"NEW NAME");assert(r.ps()=="NEW NAME");
    });
    test("G0 character conversion rather than Latin-1 or platform charset", [] {
        const uint8_t data[]={0x91,0x99,0x8e,0xa9,0x24,0x0d,0x41};
        assert(RdsDecoder::utf8(data,sizeof(data))=="äü¡€¤");
        uint8_t controls[]={1,0x1b,0x41};assert(RdsDecoder::utf8(controls,3)=="  A");
    });
    test("complete repeated Method-A AF list", [] {
        RdsDecoder r;
        for(int n=0;n<3;++n) {
            r.feed(group(0x1234,0,0xe205,0),88000,0);
            r.feed(group(0x1234,1,0x46cd,0),88000,0);
        }
        assert(r.alternatives()==std::vector<int>({88000,94500}));
        r.feed(group(0x1234,0,0xe000,0),88000,0);assert(r.alternatives().empty());
    });
    test("LF/MF escape and other networks cannot become FM AFs", [] {
        RdsDecoder r;
        for(int n=0;n<5;++n) {
            r.feed(group(0x1234,0,0xe205,0),88000,0);
            r.feed(group(0x1234,0,0xfa46,0),88000,0);
        }
        assert(r.alternatives().empty());
        for(int n=0;n<5;++n) {
            r.feed(group(0x1234,0,0xe20a,0),88000,0);
            r.feed(group(0x1234,0,0x46cd,0),88000,0);
        }
        assert(r.alternatives().empty());
    });
    test("Method-B pairs require tuned transmitter and repetition", [] {
        RdsDecoder r;
        for(int n=0;n<3;++n) r.feed(group(0x1234,0,0x0546,0),88000,0);
        assert(r.alternatives()==std::vector<int>({88000,94500}));
        for(int n=0;n<3;++n) r.feed(group(0x1234,0,0x4760,0),88000,0);
        assert(r.alternatives().size()==2);
        r.reset(); for(int n=0;n<3;++n) r.feed(group(0x1234,0xe000,0x0546,0),88000,0);
        assert(r.alternatives().empty()); // EON describes another network
    });
    test("random malformed input is bounded", [] {
        std::mt19937 gen(7870);RdsDecoder r;
        for(int n=0;n<20000;++n) {
            Bytes data(gen()%300);for(auto& byte:data)byte=gen();
            r.feed(data,88000,n);r.expire(n);r.takeEvents();
            assert(r.ps().size()<=24 && r.text().size()<=192 && r.alternatives().size()<=25);
        }
    });
    std::cout << total << " RDS tests passed\n";
}
