#include <mcscon.h>
#include <cstdio>

int main() {
    McBot* bot = mc_bot_new(MC_V1_12_2);
    std::printf("C++: bot %s\n", bot ? "created" : "failed");
    mc_bot_free(bot);
    return bot ? 0 : 1;
}
