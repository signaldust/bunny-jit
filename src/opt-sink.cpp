
#include "bjit.h"

using namespace bjit;

void Proc::opt_sink()
{
    livescan();
    debug();

    printf(" Sink\n");
}