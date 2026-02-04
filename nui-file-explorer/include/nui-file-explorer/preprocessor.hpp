#pragma once

#define STRINGIZE(x) #x
#define STRINGIZE_EXPANDED(x) STRINGIZE(x)

#ifndef BROWSER_ENGINE
#    define BROWSER_ENGINE undefined
#endif