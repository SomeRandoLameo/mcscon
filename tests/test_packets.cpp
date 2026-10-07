#include <mcscon/packets_cb.h>
#include "check.h"
#include "packet_vectors.h"
#include <stdlib.h>
#include <string>
#include <map>
using namespace mc;

// Every random valid packet from the independent reference encoder must decode and consume exactly all bytes.
static void test_vectors() {
    std::map<std::string, int> failed; int total = 0;
    for (const PVec& pv : pvecs) {
        Reader r(pv.d, pv.len);
        bool ok = decodeByName(pv.name, r, (Version)pv.ver);
        ++total;
        if (!ok || r.left() != 0 || !r.ok()) { ++failed[std::string(pv.name) + "@" + std::to_string(pv.ver)]; }
    }
    for (auto& f : failed) printf("    mismatch: %s (%d)\n", f.first.c_str(), f.second);
    printf("    %d vectors, %zu failing packet/version pairs\n", total, failed.size());
    CHECK(failed.empty());
}
// Truncation / corruption must never crash or read out of bounds (run under ASan).
static void test_robust() {
    srand(5);
    for (const PVec& pv : pvecs) {
        for (unsigned cut = 0; cut < pv.len; cut += 1 + pv.len / 8) { Reader r(pv.d, cut); decodeByName(pv.name, r, (Version)pv.ver); }
        unsigned char buf[2048]; if (pv.len > sizeof buf) continue;
        for (int k = 0; k < 8; ++k) {
            memcpy(buf, pv.d, pv.len); if (pv.len) buf[rand() % pv.len] = (unsigned char)rand();
            Reader r(buf, pv.len); decodeByName(pv.name, r, (Version)pv.ver);
        }
    }
    CHECK(true);
}
// Negative control: proves the vectors actually pin the layout. Dropping the last byte must make decoding fail,
// except for packets whose final field is a "rest of packet" buffer (or optional trailing data).
static void test_negative_control() {
    std::map<std::string, int> survivors; int tried = 0;
    for (const PVec& pv : pvecs) {
        if (pv.len < 1) continue;
        Reader r(pv.d, pv.len - 1); ++tried;
        if (decodeByName(pv.name, r, (Version)pv.ver)) ++survivors[pv.name];
    }
    for (auto& s : survivors) printf("    survives truncation: %s (%d)\n", s.first.c_str(), s.second);
    CHECK(tried > 10000);
    for (auto& s : survivors) CHECK(s.first == "CustomPayload" || s.first == "TabComplete" || s.first == "Map");
}
int main() { RUN(test_vectors); RUN(test_robust); RUN(test_negative_control); DONE(); }
