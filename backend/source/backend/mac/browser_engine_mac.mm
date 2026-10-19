#include <backend/browser_engine.hpp>

#import <Foundation/Foundation.h>

std::string browserEngineDescription()
{
    NSString* const version =
        [[NSBundle bundleWithIdentifier:@"com.apple.WebKit"] objectForInfoDictionaryKey:@"CFBundleShortVersionString"];
    if (version == nil)
        return "WebKit";
    return std::string{"WebKit "} + version.UTF8String;
}
