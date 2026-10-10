# A static libssh (macOS) links OpenSSL::Crypto and ZLIB::ZLIB without looking for them itself.
find_package(OpenSSL QUIET COMPONENTS Crypto)
find_package(ZLIB QUIET)
find_package(LIBSSH REQUIRED)
