#include "core/file_sys/npdm_layout.h"
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

using Bytes = std::vector<std::uint8_t>;
static void Word(Bytes& b, std::size_t at, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) b[at + i] = std::uint8_t(value >> (8 * i));
}
static Bytes Fixture(bool nx2) {
    Bytes b(0x800);
    std::memcpy(b.data(), "META", 4);
    Word(b, 0x70, 0x600); Word(b, 0x74, 0x200);
    Word(b, 0x78, 0x80); Word(b, 0x7c, 0x500);
    const std::size_t sig = nx2 ? 0x300 : 0x200;
    std::memcpy(b.data() + 0x80 + sig, "ACID", 4);
    Word(b, 0x80 + sig + 0x20, std::uint32_t(sig + 0x40));
    Word(b, 0x80 + sig + 0x24, 0x2c);
    std::memcpy(b.data() + 0x600, "ACI0", 4);
    Word(b, 0x620, 0x40); Word(b, 0x624, 0x1c);
    Word(b, 0x630, 0x80); Word(b, 0x634, 0x10);
    return b;
}
int main(int argc, char** argv) {
    unsigned checks = 0, errors = 0;
    const auto check = [&](bool good) { ++checks; errors += !good; };
    for (bool nx2 : {false, true}) {
        auto b = Fixture(nx2);
        const auto v = FileSys::InspectNpdmLayout(b);
        check(v && v->switch2 == nx2 && v->aci_fah == 0x640 &&
              v->acid_fac == (nx2 ? 0x3c0u : 0x2c0u) && v->aci_kac == 0x680);
        for (std::size_t size = 0; size < b.size(); ++size)
            check(!FileSys::InspectNpdmLayout(std::span(b).first(size)));
        b = Fixture(nx2); Word(b, 0x78, 0xfffffff0u); check(!FileSys::InspectNpdmLayout(b));
        b = Fixture(nx2); Word(b, 0x630, 0xfffffff0u); check(!FileSys::InspectNpdmLayout(b));
        b = Fixture(nx2); Word(b, 0x634, 3); check(!FileSys::InspectNpdmLayout(b));
        b = Fixture(nx2); b[0] = 0; check(!FileSys::InspectNpdmLayout(b));
        b = Fixture(nx2); b[0x600] = 0; check(!FileSys::InspectNpdmLayout(b));
        b = Fixture(nx2); Word(b, 0x624, 0x10000); check(!FileSys::InspectNpdmLayout(b));
    }
    if (argc == 2) {
        std::ifstream f(argv[1], std::ios::binary);
        if (!f) { std::puts("INPUT_OPEN_FAILED"); return 2; }
        Bytes b{std::istreambuf_iterator<char>(f), {}};
        const auto v = FileSys::InspectNpdmLayout(b);
        check(v && v->switch2);
        if (v) std::printf("REAL_NPDM switch2=%d acid_body=0x%zx fac=0x%zx fah=0x%zx kac=0x%zx kac_size=%zu\n",
                            v->switch2, v->acid_body, v->acid_fac, v->aci_fah, v->aci_kac, v->aci_kac_size);
    }
    std::printf("NPDM_LAYOUT checks=%u failures=%u\n", checks, errors);
    return errors ? 1 : 0;
}
