include(FetchContent)

set(BUILD_SHARED_LIBS OFF)
set(BUILD_TESTING OFF)

set(PCRE2_BUILD_TESTS OFF)
set(PCRE2_BUILD_PCRE2GREP OFF)
set(PCRE2_SUPPORT_JIT ON CACHE BOOL "JIT for tokenizer presegmentation" FORCE)
set(PCRE2_SHOW_REPORT OFF)
FetchContent_Declare(pcre2
    # Tiktoken's pretokenizer uses Unicode 16 character properties.
    URL https://github.com/PCRE2Project/pcre2/releases/download/pcre2-10.47/pcre2-10.47.tar.gz
    URL_HASH SHA256=c08ae2388ef333e8403e670ad70c0a11f1eed021fd88308d7e02f596fcd9dc16
    EXCLUDE_FROM_ALL)

foreach(feature IN ITEMS CATALOG DOCS HTML HTTP ICONV ICU MODULES PROGRAMS PYTHON READLINE TESTS XINCLUDE)
    set(LIBXML2_WITH_${feature} OFF)
endforeach()
FetchContent_Declare(libxml2
    URL https://codeload.github.com/GNOME/libxml2/tar.gz/refs/tags/v2.15.4
    URL_HASH SHA256=bb01bd9ac9a7403f9706816fd6d3e7888041d32b0211075feb20e755d2a9f29d
    EXCLUDE_FROM_ALL)

FetchContent_Declare(cmark
    URL https://codeload.github.com/commonmark/cmark/tar.gz/refs/tags/0.31.2
    URL_HASH SHA256=f9bc5ca38bcb0b727f0056100fac4d743e768872e3bacec7746de28f5700d697
    EXCLUDE_FROM_ALL)

set(JSONCONS_BUILD_TESTS OFF)
FetchContent_Declare(jsoncons
    URL https://codeload.github.com/danielaparker/jsoncons/tar.gz/refs/tags/v1.10.0
    URL_HASH SHA256=c829350a2eece662beb378143e2ab1c1cbb07d703687bf168626665a8a7d8bc1
    EXCLUDE_FROM_ALL)
FetchContent_Declare(tomlplusplus
    URL https://codeload.github.com/marzer/tomlplusplus/tar.gz/refs/tags/v3.4.0
    URL_HASH SHA256=8517f65938a4faae9ccf8ebb36631a38c1cadfb5efa85d9a72e15b9e97d25155
    EXCLUDE_FROM_ALL)

FetchContent_Declare(sqlite_amalgamation
    URL https://www.sqlite.org/2026/sqlite-amalgamation-3540100.zip
    URL_HASH SHA256=0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9
    SOURCE_SUBDIR no-cmake
    EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(pcre2 libxml2 cmark jsoncons tomlplusplus sqlite_amalgamation)

find_package(Threads REQUIRED)
add_library(syntwine_sqlite STATIC "${sqlite_amalgamation_SOURCE_DIR}/sqlite3.c")
target_include_directories(syntwine_sqlite SYSTEM PUBLIC "${sqlite_amalgamation_SOURCE_DIR}")
target_compile_definitions(syntwine_sqlite PRIVATE SQLITE_OMIT_LOAD_EXTENSION)
target_link_libraries(syntwine_sqlite PRIVATE Threads::Threads)

if(APPLE)
    # The SDK supplies the system HTTP client with Keychain trust.
    find_package(CURL REQUIRED)
else()
    set(BUILD_CURL_EXE OFF)
    set(BUILD_LIBCURL_DOCS OFF)
    set(BUILD_MISC_DOCS OFF)
    set(ENABLE_CURL_MANUAL OFF)
    set(CURL_DISABLE_INSTALL ON)
    set(CURL_USE_PKGCONFIG OFF)
    set(CURL_USE_LIBPSL OFF)
    set(CURL_USE_LIBSSH2 OFF)
    foreach(codec IN ITEMS ZLIB BROTLI ZSTD)
        set(CURL_${codec} OFF CACHE STRING "HTTP telemetry does not negotiate compressed responses" FORCE)
    endforeach()
    set(USE_NGHTTP2 OFF)
    set(USE_LIBIDN2 OFF)
    foreach(protocol IN ITEMS DICT FILE FTP GOPHER IMAP IPFS LDAP LDAPS MQTT POP3 RTSP SMTP TELNET TFTP WEBSOCKETS)
        set(CURL_DISABLE_${protocol} ON)
    endforeach()
    if(WIN32)
        set(CURL_USE_SCHANNEL ON)
        set(CURL_USE_OPENSSL OFF)
    else()
        find_package(OpenSSL 3 REQUIRED)
        set(CURL_USE_OPENSSL ON)
        set(CURL_CA_FALLBACK ON CACHE BOOL "Use the OS default trust store" FORCE)
        install(FILES "${CMAKE_CURRENT_LIST_DIR}/licenses/openssl.txt" DESTINATION syntwine/licenses)
    endif()
    FetchContent_Declare(curl
        URL https://curl.se/download/curl-8.23.0.tar.xz
        URL_HASH SHA256=8dadd7d2536a314f982ad05a651b713f426bf7d02cc2c76cffb9796f2218e6c5
        EXCLUDE_FROM_ALL)
    FetchContent_MakeAvailable(curl)
    install(FILES "${curl_SOURCE_DIR}/COPYING" DESTINATION syntwine/licenses RENAME curl.txt)
endif()

install(FILES "${pcre2_SOURCE_DIR}/LICENCE.md" DESTINATION syntwine/licenses RENAME pcre2.txt)
install(FILES "${libxml2_SOURCE_DIR}/Copyright" DESTINATION syntwine/licenses RENAME libxml2.txt)
install(FILES "${cmark_SOURCE_DIR}/COPYING" DESTINATION syntwine/licenses RENAME cmark.txt)
install(FILES "${jsoncons_SOURCE_DIR}/LICENSE" DESTINATION syntwine/licenses RENAME jsoncons.txt)
install(FILES "${tomlplusplus_SOURCE_DIR}/LICENSE" DESTINATION syntwine/licenses RENAME tomlplusplus.txt)
install(FILES "${CMAKE_CURRENT_LIST_DIR}/licenses/pybind11.txt" DESTINATION syntwine/licenses)
