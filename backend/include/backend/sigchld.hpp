#pragma once

#ifndef _WIN32
#    include <signal.h>

extern volatile sig_atomic_t* sigchld;
#endif