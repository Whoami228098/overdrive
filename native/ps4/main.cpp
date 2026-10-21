// Overdrive store — PS4 native entry (OpenOrbis).
// v0: skeleton that boots, prints a boot log and idles; the real
// ps5-react native backend (host_platform port) lands on top of this.
#include <cstdio>
#include <thread>
#include <chrono>

extern "C" int main(int argc, char* argv[])
{
    (void)argc; (void)argv;
    printf("[overdrive-ps4] boot\n");
    printf("[overdrive-ps4] skeleton ok, idling\n");
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(60));
    }
    return 0;
}
