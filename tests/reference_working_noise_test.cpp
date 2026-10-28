#include "../rtengine/raw_noise_model.h"
#include <cstdlib>
#include <iostream>
#include <limits>

using namespace rtengine::raw_noise;
static unsigned checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::exit(1); } } while (0)

int main()
{
    Model m;
    m.profile.matched = true;
    m.profile.make = "synthetic";
    m.profile.a = {{65535, 131070, 196605, 262140}};
    m.profile.b = {{65535.0*65535, 2*65535.0*65535, 3*65535.0*65535, 4*65535.0*65535}};
    m.renderMetadataValid = m.colorMatrixValid = true;
    m.renderSkip = 1;
    m.decoderBlackDN.fill(1024);
    m.decoderWhiteDN.fill(1024+65535);
    m.preprocessingScale = {{1, 2, 3, 4}};
    m.renderMultiplier = {{2, 3, 4}};
    m.cameraToWorking = {{{{1,0,0}},{{0,1,0}},{{0,0,1}}}};
    const auto r = referenceWorkingNoise(m);
    CHECK(r.referenceUsable && !m.valid && !m.sensor.valid);
    CHECK(r.stage3Gain == 1);
    for (unsigned c = 0; c < 4; ++c) {
        CHECK(r.normalizedA[c] == c+1 && r.normalizedB[c] == c+1);
    }
    CHECK(r.cameraToWorking[0][0] == 2 && r.cameraToWorking[1][1] == 9 && r.cameraToWorking[2][2] == 12);
    m.cameraToWorking = {{{{1,2,3}},{{0,1,4}},{{5,6,0}}}};
    const auto mixed = referenceWorkingNoise(m);
    CHECK(mixed.referenceUsable);
    const double gains[3] = {2,9,12};
    for (unsigned i=0;i<3;++i) for (unsigned j=0;j<3;++j)
        CHECK(mixed.cameraToWorking[i][j] == m.cameraToWorking[i][j]*gains[j]);
    const std::array<uint32_t,5> dims{{6000,4000,6000,4000,2}};
    auto hash = [&](const Model& n) { return referenceNoiseHash(n, referenceWorkingNoise(n), dims, 1); };
    const auto h = hash(m);
    CHECK(h == hash(m));
    for (unsigned c=0;c<4;++c) {
        auto n=m; n.profile.a[c]+=1; CHECK(hash(n)!=h);
        n=m; n.profile.b[c]+=1; CHECK(hash(n)!=h);
        n=m; n.preprocessingScale[c]+=1; CHECK(hash(n)!=h);
    }
    auto n=m; n.profile.mode="other"; CHECK(hash(n)!=h);
    n=m;
    for (unsigned c=0;c<4;++c) n.decoderWhiteDN[c]=n.decoderBlackDN[c]+65535.0/2;
    const auto halfSpan=referenceWorkingNoise(n);
    CHECK(halfSpan.referenceUsable && halfSpan.normalizedA==mixed.normalizedA && halfSpan.normalizedB==mixed.normalizedB);
    for (unsigned i=0;i<3;++i) for (unsigned j=0;j<3;++j)
        CHECK(halfSpan.cameraToWorking[i][j]==mixed.cameraToWorking[i][j]*0.5);
    n=m; n.profile.model="other"; CHECK(hash(n)!=h);
    n=m; n.workingProfile="other"; CHECK(hash(n)!=h);
    for (unsigned c=0;c<4;++c) {
        n=m; n.decoderWhiteDN[c]+=1; CHECK(hash(n)!=h);
        n=m; n.decoderBlackDN[c]+=1; CHECK(hash(n)!=h);
    }
    for (unsigned c=0;c<3;++c) {
        n=m; n.renderMultiplier[c]+=1; CHECK(hash(n)!=h);
        for (unsigned j=0;j<3;++j) { n=m; n.cameraToWorking[c][j]+=0.1; CHECK(hash(n)!=h); }
    }
    n=m; n.profile.generic=true; CHECK(hash(n)!=h);
    n=m; n.profile.iso=100; CHECK(hash(n)!=h);
    auto d=dims; ++d[2]; CHECK(referenceNoiseHash(m,mixed,d,1)!=h);
    for (unsigned i=0;i<5;++i) { d=dims; ++d[i]; CHECK(referenceNoiseHash(m,mixed,d,1)!=h); }
    CHECK(referenceNoiseHash(m,mixed,dims,0.5)!=h);
    CHECK(!referenceWorkingNoise(m,true).referenceUsable);
    CHECK(referenceWorkingNoise(m,true).normalizedA == mixed.normalizedA);
    n=m; n.renderSkip=2; CHECK(!referenceWorkingNoise(n).referenceUsable);
    for (const char* why : {"dcp_transform_not_certified_linear", "icc_transform_not_certified_linear"}) {
        n=m; n.colorMatrixValid=false; n.colorReason=why;
        const auto f=referenceWorkingNoise(n);
        CHECK(!f.referenceUsable && f.reason==why && f.normalizedB==mixed.normalizedB);
        CHECK(f.cameraToWorking == ReferenceWorkingNoise{}.cameraToWorking);
    }
    for (unsigned c=0;c<4;++c) {
        n=m; n.decoderWhiteDN[c]=n.decoderBlackDN[c]; CHECK(!referenceWorkingNoise(n).referenceUsable);
        n=m; n.decoderBlackDN[c]=std::numeric_limits<double>::quiet_NaN(); CHECK(!referenceWorkingNoise(n).referenceUsable);
        n=m; n.decoderWhiteDN[c]=std::numeric_limits<double>::infinity(); CHECK(!referenceWorkingNoise(n).referenceUsable);
        n=m; n.preprocessingScale[c]=0; CHECK(!referenceWorkingNoise(n).referenceUsable);
    }
    n=m; n.cameraToWorking[2]=n.cameraToWorking[1]; CHECK(!referenceWorkingNoise(n).referenceUsable);
    n=m; n.cameraToWorking[0][0]=std::numeric_limits<double>::infinity(); CHECK(!referenceWorkingNoise(n).referenceUsable);
    n=m; n.renderMultiplier[1]=std::numeric_limits<double>::quiet_NaN(); CHECK(!referenceWorkingNoise(n).referenceUsable);
    n=m; n.profile.a[0]=-1; CHECK(!referenceWorkingNoise(n).referenceUsable);
    n=m; n.renderMetadataValid=false; CHECK(!referenceWorkingNoise(n).referenceUsable);
    n=m; n.preprocessingScale[0]=std::numeric_limits<double>::max(); CHECK(!referenceWorkingNoise(n).referenceUsable);
    n=m; n.decoderWhiteDN[0]=std::numeric_limits<double>::max();
    n.decoderBlackDN[0]=-std::numeric_limits<double>::max(); CHECK(!referenceWorkingNoise(n).referenceUsable);
    n=m; n.profile.b[0]=std::numeric_limits<double>::quiet_NaN(); CHECK(!referenceWorkingNoise(n).referenceUsable);
    n=m; n.profile.a[0]=n.profile.b[0]=0; CHECK(!referenceWorkingNoise(n).referenceUsable);
    n=m; n.profile.a[0]=0; CHECK(referenceWorkingNoise(n).referenceUsable);
    n=m; n.profile.b[0]=0; CHECK(referenceWorkingNoise(n).referenceUsable);
    n=m; n.profile.matched=false; CHECK(!referenceWorkingNoise(n).referenceUsable);
    std::cout << "PASS reference_working_noise_test " << checks << " synthetic checks\n";
}
