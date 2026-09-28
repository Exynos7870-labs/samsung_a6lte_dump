// Host mock: record route shutdowns; this is NOT an audio HAL test.
#pragma once
#include <atomic>
#include <cassert>
#include <utils/String8.h>
extern std::atomic<int> audioStops;
namespace android {
struct AudioSystem {
    static int setParameters(int output, const String8& value) {
        assert(output == 0 && value == "l_fmradio_mode=off");
        ++audioStops;
        return 0;
    }
};
}
