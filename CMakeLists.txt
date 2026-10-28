cmake_minimum_required(VERSION 3.10)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
project(Alphabox VERSION 1.2.0)

# Single-config generators (Makefiles, Ninja) default to an empty build type,
# which compiles without optimization; default to Release instead.
get_property(ALPHABOX_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT ALPHABOX_MULTI_CONFIG AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING
      "Build type (Debug, Release, RelWithDebInfo, MinSizeRel)" FORCE)
endif()

# The JIT (asmjit, the third_party/asmjit submodule) is the default where it
# has an emitter, x86-64 and AArch64, and the submodule is checked out
# (git submodule update --init third_party/asmjit). Elsewhere, and with MSVC,
# where it has never been built, the default is the interpreter.
# -DES40_DISABLE_ASMJIT=ON builds the interpreter anywhere; =OFF insists on
# the JIT and stops when asmjit is missing.
set(ALPHABOX_JIT_DEFAULT_OFF ON)
if (NOT MSVC AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|aarch64|arm64|ARM64)$")
    if (EXISTS ${CMAKE_SOURCE_DIR}/third_party/asmjit/CMakeLists.txt)
        set(ALPHABOX_JIT_DEFAULT_OFF OFF)
    elseif (NOT DEFINED ES40_DISABLE_ASMJIT)
        message(WARNING "asmjit is not checked out, so this is an interpreter "
            "build; for the JIT: git submodule update --init third_party/asmjit")
    endif()
endif()
option(ES40_DISABLE_ASMJIT "Disable the JIT (asmjit)" ${ALPHABOX_JIT_DEFAULT_OFF})
# Hypervisor.framework support (`alphabox hvprobe`, `alphabox hvtest`, and
# the ALPHABOX_HV=1 runtime; see docs/hypervisor.md): macOS on Apple silicon
# only, needs the JIT's asmjit for the in-VM vectors, and the binary is
# then signed ad hoc with the hypervisor entitlement.
option(ALPHABOX_HVF "Build the Hypervisor.framework runtime (macOS/arm64)" OFF)

# Source files. One glob per source directory: a NEW directory must be added
# both here and to target_include_directories below, or its .cpp files are
# silently left out of the build.
file(GLOB srcs
    src/*.cpp
    src/cpu/*.cpp
    src/cpu/ev7/*.cpp
    src/debug/*.cpp
    src/hv/*.cpp
    src/system/*.cpp
    src/chipsets/*.cpp
    src/chipsets/tsunami/*.cpp
    src/chipsets/marvel/*.cpp
    src/chipsets/titan/*.cpp
    src/common/*.cpp
    src/devices/isa/*.cpp
    src/devices/pci/*.cpp
    src/devices/pci/sym53c8xx/*.cpp
    src/devices/common/*.cpp
    src/devices/pci/i8255x/*.cpp
    src/devices/pci/isp1040/*.cpp
    src/devices/pci/bridge/*.cpp
    src/devices/pci/tulip/*.cpp
    src/devices/pci/es137x/*.cpp
    src/devices/pci/ehci/*.cpp
    src/devices/pci/virtio/*.cpp
    src/platforms/*.cpp
    src/platforms/es40/*.cpp
    src/platforms/ds10/*.cpp
    src/platforms/ds20e/*.cpp
    src/platforms/ds20l/*.cpp
    src/platforms/es47/*.cpp
    src/platforms/es80/*.cpp
    src/platforms/gs1280/*.cpp
    src/platforms/es45/*.cpp
    src/platforms/ds25/*.cpp
    src/platforms/ds15/*.cpp
    src/devices/storage/*.cpp
    src/devices/video/*.cpp
    src/devices/video/s3/*.cpp
    src/devices/video/cirrus/*.cpp
    src/devices/video/mach64/*.cpp
    src/devices/video/permedia2/*.cpp
    src/devices/video/virge/*.cpp
    src/devices/video/tga/*.cpp
    src/devices/video/radeon/*.cpp
    src/devices/video/radeon/r100/*.cpp
    src/devices/net/*.cpp
    src/devices/usb/*.cpp
    src/gui/*.cpp)

add_executable(alphabox ${srcs} src/Main.cpp)
# Headers are included unqualified ("System.hpp"), so every source directory
# is on the include path and a moved file needs no include changes.
target_include_directories(alphabox PRIVATE
    src src/cpu src/cpu/ev7 src/debug src/hv src/system src/common
    src/chipsets src/chipsets/tsunami src/chipsets/marvel src/chipsets/titan
    src/devices/isa src/devices/pci src/devices/pci/sym53c8xx
    src/devices/common src/devices/pci/i8255x src/devices/pci/bridge
    src/devices/pci/isp1040 src/devices/pci/tulip src/devices/pci/es137x
    src/devices/pci/ehci src/devices/pci/virtio
    src/platforms src/platforms/es40 src/platforms/ds10 src/platforms/ds20e
    src/platforms/ds20l src/platforms/es47 src/platforms/es45
    src/platforms/ds25 src/platforms/ds15 src/platforms/es80
    src/platforms/gs1280
    src/devices/storage
    src/devices/video src/devices/video/s3 src/devices/video/cirrus
    src/devices/video/mach64 src/devices/video/permedia2
    src/devices/video/virge src/devices/video/tga src/devices/video/radeon
    src/devices/video/radeon/r100
    src/devices/net src/devices/usb
    src/gui ${CMAKE_BINARY_DIR}/src)

if (NOT ES40_DISABLE_ASMJIT)
    if (NOT EXISTS ${CMAKE_SOURCE_DIR}/third_party/asmjit/CMakeLists.txt)
        message(FATAL_ERROR "asmjit missing: check out the submodule\n"
            "  git submodule update --init third_party/asmjit\n"
            "or configure with -DES40_DISABLE_ASMJIT=ON for an interpreter build")
    endif()
    set(ASMJIT_NO_INSTALL ON)
    add_subdirectory(third_party/asmjit)
    target_sources(alphabox PRIVATE src/jit/jitengine.cpp)
    target_compile_definitions(alphabox PRIVATE ES40_JIT)
    target_include_directories(alphabox PRIVATE ${CMAKE_SOURCE_DIR}/third_party/asmjit/src)
    target_link_libraries(alphabox asmjit)
    message(STATUS "asmjit JIT enabled")
endif()

if (ALPHABOX_HVF)
    if (NOT APPLE OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64" OR ES40_DISABLE_ASMJIT)
        message(FATAL_ERROR "ALPHABOX_HVF needs macOS on Apple silicon and -DES40_DISABLE_ASMJIT=OFF")
    endif()
    target_compile_definitions(alphabox PRIVATE ALPHABOX_HVF)
    target_link_libraries(alphabox "-framework Hypervisor")
    add_custom_command(TARGET alphabox POST_BUILD
        COMMAND codesign --sign - --force --entitlements ${CMAKE_SOURCE_DIR}/src/hv/alphabox.entitlements $<TARGET_FILE:alphabox>
        COMMENT "Signing alphabox ad hoc with the hypervisor entitlement")
    message(STATUS "Hypervisor.framework runtime enabled")
endif()

# Path to additional CMake modules
set(CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)

if (CMAKE_GENERATOR MATCHES "Visual Studio")
  add_definitions(-DNOMINMAX -D_WIN32_WINNT=0x0A00 -DLANG_CXX11 -DCOMPILER_MSVC -D__VERSION__=\"MSVC\")
  add_definitions(-DWIN32 -DOS_WIN -D_MBCS -DWIN64 -DWIN32_LEAN_AND_MEAN -DPLATFORM_WINDOWS)
  add_definitions(/bigobj /nologo /EHsc /GF /FC /MP /Gm-)
  # Suppress warnings to reduce build log size.
  add_definitions(/wd4267 /wd4244 /wd4800 /wd4503 /wd4554 /wd4996 /wd4348 /wd4018)
  add_definitions(/wd4099 /wd4146 /wd4267 /wd4305 /wd4307)
  add_definitions(/wd4715 /wd4722 /wd4723 /wd4838 /wd4309 /wd4334)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /MP")
endif()

find_package(Threads REQUIRED)
target_link_libraries(alphabox Threads::Threads ${CMAKE_DL_LIBS})

# Configuration options
include(CheckSymbolExists)
include(CheckIncludeFile)
include(CheckLibraryExists)
include(CheckTypeSize)
include(CheckIncludeFiles)
include(TestBigEndian)
include(CheckLargeFiles)

TEST_BIG_ENDIAN(IS_BIG_ENDIAN)
if(IS_BIG_ENDIAN)
    set(ES40_BIG_ENDIAN 1)
else()
    set(ES40_LITTLE_ENDIAN 1)
endif()

check_symbol_exists(alarm "unistd.h" HAVE_ALARM)
check_include_file("arpa/inet.h" HAVE_ARPA_INET_H)
check_include_file("arpa/telnet.h" HAVE_ARPA_TELNET_H)
check_symbol_exists(atexit "stdlib.h" HAVE_ATEXIT)
check_include_file("ctype.h" HAVE_CTYPE_H)
check_include_file("errno.h" HAVE_ERRNO_H)
check_include_file("fcntl.h" HAVE_FCNTL_H)
check_symbol_exists(fopen "stdio.h" HAVE_FOPEN)
check_symbol_exists(fopen64 "stdio.h" HAVE_FOPEN64)
check_symbol_exists(fork "unistd.h" HAVE_FORK)
check_symbol_exists(fseek "stdio.h" HAVE_FSEEK)
check_symbol_exists(fseeko "stdio.h" HAVE_FSEEKO)
check_symbol_exists(fseeko64 "stdio.h" HAVE_FSEEKO64)
check_symbol_exists(ftell "stdio.h" HAVE_FTELL)
check_symbol_exists(ftello "stdio.h" HAVE_FTELLO)
check_symbol_exists(ftello64 "stdio.h" HAVE_FTELLO64)
check_symbol_exists(gmtime_s "time.h" HAVE_GMTIME_S)
check_symbol_exists(inet_aton "arpa/inet.h" HAVE_INET_ATON)
check_include_file("inet.h" HAVE_INET_H)
check_include_file("inttypes.h" HAVE_INTTYPES_H)
check_include_file("in.h" HAVE_IN_H)
check_symbol_exists(isblank "ctype.h" HAVE_ISBLANK)
check_symbol_exists(localtime_s "time.h" HAVE_LOCALTIME_S)
check_symbol_exists(malloc "stdlib.h" HAVE_MALLOC)
check_include_file("malloc.h" HAVE_MALLOC_H)
check_include_file("memory.h" HAVE_MEMORY_H)
check_symbol_exists(memset "string.h" HAVE_MEMSET)
check_include_file("netinet/in.h" HAVE_NETINET_IN_H)
check_symbol_exists(pow "math.h" HAVE_POW)
check_include_file("process.h" HAVE_PROCESS_H)
check_library_exists(pthread pthread_self "" HAVE_PTHREAD)
check_include_file("pthread.h" HAVE_PTHREAD_H)
check_symbol_exists(realloc "stdlib.h" HAVE_REALLOC)
check_symbol_exists(select "sys/select.h" HAVE_SELECT)
check_include_file("signal.h" HAVE_SIGNAL_H)
check_symbol_exists(socket "sys/socket.h" HAVE_SOCKET)
check_include_file("socket.h" HAVE_SOCKET_H)
check_symbol_exists(sqrt "math.h" HAVE_SQRT)
check_include_file("stdbool.h" HAVE_STDBOOL_H)
check_include_file("stdint.h" HAVE_STDINT_H)
check_include_file("stdlib.h" HAVE_STDLIB_H)
check_symbol_exists(strcasecmp "string.h" HAVE_STRCASECMP)
check_symbol_exists(strchr "string.h" HAVE_STRCHR)
check_symbol_exists(strdup "string.h" HAVE_STRDUP)
check_include_file("stddef.h" HAVE_STDDEF_H)
check_include_file("strings.h" HAVE_STRINGS_H)
check_include_file("string.h" HAVE_STRING_H)
check_symbol_exists(strncasecmp "string.h" HAVE_STRNCASECMP)
check_symbol_exists(strspn "string.h" HAVE_STRSPN)
check_include_file("sys/param.h" HAVE_SYS_PARAM_H)
check_include_file("sys/select.h" HAVE_SYS_SELECT_H)
check_include_file("sys/socket.h" HAVE_SYS_SOCKET_H)
check_include_file("sys/stat.h" HAVE_SYS_STAT_H)
check_include_file("sys/time.h" HAVE_SYS_TIME_H)
check_include_file("sys/types.h" HAVE_SYS_TYPES_H)
check_include_file("sys/wait.h" HAVE_SYS_WAIT_H)
check_include_file("unistd.h" HAVE_UNISTD_H)
check_symbol_exists(vfork "unistd.h" HAVE_VFORK)
check_include_file("vfork.h" HAVE_VFORK_H)
check_include_file("windows.h" HAVE_WINDOWS_H)
check_include_file("winsock2.h" HAVE_WINSOCK2_H)
set(HAVE_WORKING_FORK ${HAVE_FORK})
set(HAVE_WORKING_VFORK ${HAVE_VFORK})
check_include_file("ws2tcpip.h" HAVE_WS2TCPIP_H)
check_type_size(_Bool _BOOL)
check_symbol_exists(_fseeki64 "stdio.h" HAVE__FSEEKI64)
check_symbol_exists(_ftelli64 "stdio.h" HAVE__FTELLI64)
check_symbol_exists(_strdup "string.h" HAVE__STRDUP)
check_symbol_exists(_stricasecmp "string.h" HAVE__STRICASECMP)
check_symbol_exists(_stricmp "string.h" HAVE__STRICMP)

# Features
# Large file support (fopen64 / disk files > 2 GB)
ALPHABOX_TEST_LARGE_FILES(HAVE_LARGE_FILES)

if (DISABLE_PCAP STREQUAL "yes")
    set(WANT_PCAP 0)
else()
    set(WANT_PCAP 1)
endif()

if(WANT_PCAP)
    find_package(PCAP)
    if(PCAP_FOUND)
        message(STATUS "pcap found. Networking support enabled")
        set(HAVE_PCAP 1)
        include_directories(${PCAP_INCLUDE_DIR})
        if(WIN32)
            target_link_libraries(alphabox ${PCAP_LIBRARY})
        else()
            # Not linked: libpcap is loaded when a pcap back end is created
            # (src/devices/net/DynLib.cpp), so the binary starts without it.
            # Where it was found is the loader's last place to look.
            list(GET PCAP_LIBRARY 0 _pcap_first)
            get_filename_component(ALPHABOX_PCAP_LIBDIR "${_pcap_first}" DIRECTORY)
        endif()
    else()
        message(STATUS "pcap not found. Networking support disabled")
    endif()
else()
    message(STATUS "pcap disabled. Networking support disabled")
endif()

# libusb: USB host passthrough (a real device on an emulated root hub port).
# The host's libusb may be for another architecture than this lane (an x86-64
# lane on an arm64 Mac finds Homebrew's arm64 build), so it is used only if a
# program built for this lane links against it.
if (NOT DISABLE_LIBUSB STREQUAL "yes")
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(LIBUSB QUIET IMPORTED_TARGET libusb-1.0)
    endif()
    if(LIBUSB_FOUND)
        include(CheckCXXSourceCompiles)
        set(CMAKE_REQUIRED_INCLUDES ${LIBUSB_INCLUDE_DIRS})
        set(CMAKE_REQUIRED_LIBRARIES ${LIBUSB_LDFLAGS})
        check_cxx_source_compiles(
            "#include <libusb.h>\nint main() { return libusb_init(nullptr); }"
            HAVE_LIBUSB)
        unset(CMAKE_REQUIRED_INCLUDES)
        unset(CMAKE_REQUIRED_LIBRARIES)
    endif()
    if(HAVE_LIBUSB)
        target_link_libraries(alphabox PkgConfig::LIBUSB)
        message(STATUS "libusb found. USB host passthrough enabled")
    else()
        message(STATUS "libusb not found (or not for this architecture). USB host passthrough disabled")
    endif()
else()
    message(STATUS "libusb disabled. USB host passthrough disabled")
endif()

# libslirp: the user-mode NAT network back end (type = "nat"). Found like
# libusb, and used only if a program built for this lane links against it
# (an x86-64 lane on an arm64 Mac would find Homebrew's arm64 build). Not
# bundled: it brings glib, so it is not even linked, only loaded at first use.
if (NOT DISABLE_SLIRP STREQUAL "yes" AND NOT WIN32)
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(LIBSLIRP QUIET IMPORTED_TARGET slirp)
        pkg_get_variable(ALPHABOX_SLIRP_LIBDIR slirp libdir)
    endif()
    if(LIBSLIRP_FOUND)
        include(CheckCXXSourceCompiles)
        set(CMAKE_REQUIRED_INCLUDES ${LIBSLIRP_INCLUDE_DIRS})
        set(CMAKE_REQUIRED_LIBRARIES ${LIBSLIRP_LDFLAGS})
        check_cxx_source_compiles(
            "#include <libslirp.h>\nint main() { return slirp_version_string() == nullptr; }"
            HAVE_SLIRP)
        unset(CMAKE_REQUIRED_INCLUDES)
        unset(CMAKE_REQUIRED_LIBRARIES)
    endif()
    if(HAVE_SLIRP)
        # Headers only: libslirp (and glib behind it) is loaded when a NAT
        # back end is created, so the binary starts without them.
        target_include_directories(alphabox PRIVATE ${LIBSLIRP_INCLUDE_DIRS})
        message(STATUS "libslirp found. NAT networking (type = \"nat\") enabled")
    else()
        message(STATUS "libslirp not found (or not for this architecture). NAT networking disabled")
    endif()
else()
    message(STATUS "libslirp disabled. NAT networking disabled")
endif()

if (DISABLE_SDL STREQUAL "yes")
    set(HAVE_SDL 0)
else()
    find_package(SDL3 CONFIG QUIET)
    if (SDL3_FOUND)
        set(HAVE_SDL 1)
        set(HAVE_SDL3 1)
        message(STATUS "SDL3 found (system)")
    elseif (EXISTS ${CMAKE_SOURCE_DIR}/third_party/SDL/CMakeLists.txt)
        # Bundled SDL3 (git submodule third_party/SDL), built statically so
        # the executable does not depend on an SDL3 shared library.
        set(SDL_SHARED OFF CACHE BOOL "" FORCE)
        set(SDL_STATIC ON CACHE BOOL "" FORCE)
        set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
        add_subdirectory(third_party/SDL EXCLUDE_FROM_ALL)
        set(HAVE_SDL 1)
        set(HAVE_SDL3 1)
        message(STATUS "SDL3 built from bundled submodule (third_party/SDL, static)")
    else()
        set(HAVE_SDL 0)
    endif()
endif()

if(HAVE_SDL)
    target_link_libraries(alphabox SDL3::SDL3)
    message(STATUS "SDL3 found. SDL graphics support enabled")
else()
    message(WARNING "SDL3 not found (no system SDL3 and third_party/SDL "
                    "submodule not initialized; run: git submodule update "
                    "--init). Building without SDL graphics support")
endif()

if (STATIC_COMPILE STREQUAL "yes")
    message(STATUS "Static compilation")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -static -lpthread -lrt -Wl,--whole-archive -lpthread -Wl,--no-whole-archive")
    set(CMAKE_EXE_LINKER_FLAGS "-static")
    set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
    set(BUILD_SHARED_LIBS OFF)
endif()

if (NOT CMAKE_GENERATOR MATCHES "Visual Studio")
    if (CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Og -ggdb")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Og -ggdb")
    elseif (CMAKE_BUILD_TYPE STREQUAL "Release")
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -DNDEBUG")
    endif()
endif()

if(HAVE_LARGE_FILES)
    message(STATUS "Large file support enabled")
else()
    message(WARNING "Building without large file graphics support")
endif()

find_package(Git)
if (GIT_FOUND)
    execute_process(COMMAND
        "${GIT_EXECUTABLE}" describe --match=NeVeRmAtCh --always --abbrev=40 --dirty
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_VARIABLE GIT_SHA
        ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)

    set(PACKAGE_GITSHA "\"${GIT_SHA}\"")
endif()

# The floating-point test, alphabox_fptest (test/fp; docs/development.md):
# the interpreter's IEEE and VAX routines against the host FPU, an exact VAX
# model and hand tables from the manuals, and against the same routines as
# they were at ALPHABOX_FP_BASELINE. `ctest` runs it. It compiles the
# routines against test/fp/shim/AlphaCPU.hpp; a quoted #include looks in the
# including file's own directory first, so they are compiled from copies
# made at build time, away from the real AlphaCPU.hpp. Needs unsigned
# __int128 (GCC, Clang on 64-bit hosts; not MSVC, not 32-bit ARM).
option(ALPHABOX_FPTEST "Build the floating-point test alphabox_fptest" ON)
set(ALPHABOX_FP_BASELINE "682932c" CACHE STRING
    "Commit whose floating-point routines alphabox_fptest compares against")
include(CheckCXXSourceCompiles)
check_cxx_source_compiles(
    "unsigned __int128 x = 3; int main() { return (int)(x >> 1); }"
    ALPHABOX_HAVE_INT128)
if (ALPHABOX_FPTEST AND ALPHABOX_HAVE_INT128)
  set(fp_cur ${CMAKE_BINARY_DIR}/fp_current_src)
  set(fp_cur_srcs)
  foreach(f AlphaCPU_ieeefloat.cpp AlphaCPU_vaxfloat.cpp AlphaCPU_paired.cpp)
    add_custom_command(OUTPUT ${fp_cur}/${f}
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/src/cpu/${f}
                ${fp_cur}/${f}
        DEPENDS ${CMAKE_SOURCE_DIR}/src/cpu/${f})
    list(APPEND fp_cur_srcs ${fp_cur}/${f})
  endforeach()
  add_executable(alphabox_fptest test/fp/fp_test.cpp test/fp/fp_host.cpp
      test/fp/fp_exec_current.cpp ${fp_cur_srcs})
  target_include_directories(alphabox_fptest PRIVATE test/fp/shim test/fp
      src/cpu src/common ${CMAKE_BINARY_DIR}/src ${CMAKE_BINARY_DIR})
  # The baseline's routines, for the comparison with the frozen code.
  set(fp_frozen ${CMAKE_BINARY_DIR}/fp_frozen_src)
  set(fp_frozen_ok ${GIT_FOUND})
  if (fp_frozen_ok)
    file(MAKE_DIRECTORY ${fp_frozen})
    foreach(f AlphaCPU_ieeefloat.cpp AlphaCPU_vaxfloat.cpp cpu_fp_operate.hpp)
      execute_process(COMMAND ${GIT_EXECUTABLE} show
              ${ALPHABOX_FP_BASELINE}:src/cpu/${f}
          WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
          OUTPUT_FILE ${fp_frozen}/${f} RESULT_VARIABLE fp_rc ERROR_QUIET)
      if (NOT fp_rc EQUAL 0)
        set(fp_frozen_ok FALSE)
      endif()
    endforeach()
  endif()
  if (fp_frozen_ok)
    target_sources(alphabox_fptest PRIVATE test/fp/fp_frozen.cpp)
    target_compile_definitions(alphabox_fptest PRIVATE FP_HAVE_FROZEN)
  else()
    message(STATUS "alphabox_fptest: baseline ${ALPHABOX_FP_BASELINE} not "
                   "available from git; built without the frozen comparison")
  endif()
  # The host FPU is the IEEE oracle: no folding or reordering around
  # fesetround/fetestexcept.
  if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set_source_files_properties(test/fp/fp_host.cpp PROPERTIES
        COMPILE_OPTIONS "-ffp-model=strict")
  elseif (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set_source_files_properties(test/fp/fp_host.cpp PROPERTIES
        COMPILE_OPTIONS "-frounding-math;-fsignaling-nans")
  endif()
  enable_testing()
  add_test(NAME fp COMMAND alphabox_fptest)
endif()

set(VERSION "\"${PROJECT_VERSION}\"")
set(PACKAGE ${PROJECT_NAME})
set(PACKAGE_BUGREPORT "tglozar@gmail.com")
set(PACKAGE_NAME string(TOLOWER ${PROJECT_NAME}))
set(PACKAGE_VERSION ${PROJECT_VERSION})
set(PACKAGE_STRING ${PACKAGE_NAME}-${PACKAGE_VERSION})
set(PACKAGE_TARNAME ${PACKAGE_STRING}.tar.gz)
set(PACKAGE_URL "https://github.com/artur/alphabox")

set(PTHREAD_CREATE_JOINABLE "PTHREAD_CREATE_JOINABLE")
set(SELECT_TYPE_ARG1 "int")
set(SELECT_TYPE_ARG234 "fd_set *")
set(SELECT_TYPE_ARG5 "struct timeval *")
set(RETSIGTYPE "void")
set(STDC_HEADERS 1)
check_include_files("time.h;sys/time.h" TIME_WITH_SYS_TIME)

configure_file(src/config.hpp.in src/config.hpp)

# Unit test of the 16550 register model (test/unit/uart16550_test.cpp),
# built with every lane: run build/uart16550_test, or ctest.
add_executable(uart16550_test test/unit/uart16550_test.cpp
    src/devices/isa/Uart16550.cpp)
target_include_directories(uart16550_test PRIVATE src/common src/devices/isa)
enable_testing()
add_test(NAME uart16550 COMMAND uart16550_test)

install(TARGETS alphabox DESTINATION bin)

# Unit tests: one device's source built with the rest of the machine stubbed
# out, driven through its registers (test/unit/). `ctest --test-dir <lane>`
# runs them; each is also a plain executable in the lane.
enable_testing()
add_executable(kbc_test test/unit/kbc_test.cpp
    src/devices/isa/Keyboard.cpp src/gui/scancodes.cpp)
target_include_directories(kbc_test PRIVATE
    $<TARGET_PROPERTY:alphabox,INCLUDE_DIRECTORIES>)
target_link_libraries(kbc_test Threads::Threads)
add_test(NAME kbc COMMAND kbc_test)

# The host key chain without SDL: HID usage (the SDL3 scancode) to BX_KEY, and
# the scan code tables of sets 1, 2 and 3 with the 8042's translation.
add_executable(scancodes_test test/unit/scancodes_test.cpp
    src/gui/scancodes.cpp src/gui/hostkeys.cpp)
target_include_directories(scancodes_test PRIVATE
    $<TARGET_PROPERTY:alphabox,INCLUDE_DIRECTORIES>)
add_test(NAME scancodes COMMAND scancodes_test)

# The keymap file loader, alone.
add_executable(keymap_test test/unit/keymap_test.cpp src/gui/keymap.cpp)
target_include_directories(keymap_test PRIVATE
    $<TARGET_PROPERTY:alphabox,INCLUDE_DIRECTORIES>)
add_test(NAME keymap COMMAND keymap_test)

# The ALi M1543C's ISA core as separate models, driven through their ports
# (the clocked ones on a clock the test moves): the 8254 timer, the 8259
# pair, the MC146818 TOY clock, the Super I/O and ISA PnP ports.
add_executable(i8254_test test/unit/i8254_test.cpp src/devices/isa/I8254.cpp)
target_include_directories(i8254_test PRIVATE src/common src/devices/isa)
add_test(NAME i8254 COMMAND i8254_test)
add_executable(i8259_test test/unit/i8259_test.cpp src/devices/isa/I8259.cpp)
target_include_directories(i8259_test PRIVATE src/common src/devices/isa)
add_test(NAME i8259 COMMAND i8259_test)
add_executable(mc146818_test test/unit/mc146818_test.cpp
    src/devices/isa/Mc146818.cpp)
target_include_directories(mc146818_test PRIVATE src/common src/devices/isa)
add_test(NAME mc146818 COMMAND mc146818_test)
add_executable(superio_test test/unit/superio_test.cpp
    src/devices/isa/SuperIo.cpp)
target_include_directories(superio_test PRIVATE src/common src/devices/isa)
add_test(NAME superio COMMAND superio_test)

# The TIG-bus flash, with a replay of the ES40 console's own accesses.
add_executable(flash_test test/unit/flash_test.cpp
    src/platforms/es40/Flash.cpp)
target_include_directories(flash_test PRIVATE
    $<TARGET_PROPERTY:alphabox,INCLUDE_DIRECTORIES>)
target_compile_definitions(flash_test PRIVATE
    TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/test/unit/data")
add_test(NAME flash COMMAND flash_test)
add_executable(i2c_test test/unit/i2c_test.cpp src/devices/common/i2c_spd.cpp
    src/platforms/ds10/PCF8584.cpp)
target_include_directories(i2c_test PRIVATE
    $<TARGET_PROPERTY:alphabox,INCLUDE_DIRECTORIES>)
target_compile_definitions(i2c_test PRIVATE
    TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/test/unit/data")
add_test(NAME i2c COMMAND i2c_test)
add_executable(eeprom93_test test/unit/eeprom93_test.cpp
    src/devices/common/Eeprom93cx6.cpp)
target_include_directories(eeprom93_test PRIVATE
    $<TARGET_PROPERTY:alphabox,INCLUDE_DIRECTORIES>)
add_test(NAME eeprom93 COMMAND eeprom93_test)

# The wire-OR of the chipset's interrupt inputs (CSystem::interrupt).
add_executable(irqlines_test test/unit/irqlines_test.cpp)
target_include_directories(irqlines_test PRIVATE src/system)
target_link_libraries(irqlines_test Threads::Threads)
add_test(NAME irqlines COMMAND irqlines_test)

# The start hand-off through an EV7's RBOX_SCRATCH1 (StartHandoff.hpp): the
# console's start of a secondary against a secondary the host holds up.
add_executable(start_handoff_test test/unit/start_handoff_test.cpp)
target_include_directories(start_handoff_test PRIVATE src/chipsets/marvel)
target_link_libraries(start_handoff_test Threads::Threads)
add_test(NAME start_handoff COMMAND start_handoff_test)

# One processor's interval-timer schedule (TickSchedule.hpp) on a made-up
# clock: the count kept for a thread that runs and for one the host starves,
# the catch-up's spacing, a processor keeping processor 0's phase.
add_executable(tick_schedule_test test/unit/tick_schedule_test.cpp)
target_include_directories(tick_schedule_test PRIVATE src/cpu)
add_test(NAME tick_schedule COMMAND tick_schedule_test)

# The 21464's TPUs as processors on the Marvel boards: their numbers on each
# layout, memory and IO7 with TPU 0, a register block and interrupt target
# per TPU (Topology.cpp; docs/platforms/ev8.md, S4).
add_executable(tpu_topology_test test/unit/tpu_topology_test.cpp
    src/chipsets/marvel/Topology.cpp src/platforms/es80/Es80.cpp
    src/platforms/gs1280/Gs1280.cpp)
target_include_directories(tpu_topology_test PRIVATE
    $<TARGET_PROPERTY:alphabox,INCLUDE_DIRECTORIES>)
target_link_libraries(tpu_topology_test Threads::Threads)
add_test(NAME tpu_topology COMMAND tpu_topology_test)

# The Marvel model in 21464 mode where it has no image path: the 48-bit map
# and the dense PIDs, routes and R_WHOAMI (docs/platforms/ev8-native.md,
# section 2; test/cpu/marvel8_mp.py is the boot image).
add_executable(marvel21464_test test/unit/marvel21464_test.cpp
    src/chipsets/marvel/Topology.cpp src/platforms/es80/Es80.cpp
    src/platforms/gs1280/Gs1280.cpp)
target_include_directories(marvel21464_test PRIVATE
    $<TARGET_PROPERTY:alphabox,INCLUDE_DIRECTORIES>)
target_link_libraries(marvel21464_test Threads::Threads)
add_test(NAME marvel21464 COMMAND marvel21464_test)

# The processor rows (CpuModels.cpp) against their sources: IMPLVER is the
# Alpha Architecture Reference Manual's Table D-4.
add_executable(cpumodels_test test/unit/cpumodels_test.cpp
    src/cpu/CpuModels.cpp)
target_include_directories(cpumodels_test PRIVATE
    $<TARGET_PROPERTY:alphabox,INCLUDE_DIRECTORIES>)
target_link_libraries(cpumodels_test Threads::Threads)
add_test(NAME cpumodels COMMAND cpumodels_test)

# Tarantula's section of the state file (src/cpu/Tarantula.cpp): read back
# whole, absent from an older file, refused when damaged.
add_executable(tarantula_state_test test/unit/tarantula_state_test.cpp
    src/cpu/Tarantula.cpp)
target_include_directories(tarantula_state_test PRIVATE
    $<TARGET_PROPERTY:alphabox,INCLUDE_DIRECTORIES>)
target_link_libraries(tarantula_state_test Threads::Threads)
add_test(NAME tarantula_state COMMAND tarantula_state_test)

# The PCI configuration space every device inherits (CPCIDevice).
add_executable(pcicfg_test test/unit/pcicfg_test.cpp
    src/devices/pci/PCIDevice.cpp)
target_include_directories(pcicfg_test PRIVATE
    $<TARGET_PROPERTY:alphabox,INCLUDE_DIRECTORIES>)
target_link_libraries(pcicfg_test Threads::Threads)
add_test(NAME pcicfg COMMAND pcicfg_test)

# The PCI-PCI bridge: header, bus numbers, windows, swizzle, secondary reset.
add_executable(pcibridge_test test/unit/pcibridge_test.cpp
    src/devices/pci/PCIDevice.cpp src/devices/pci/bridge/PCIBridge.cpp
    src/devices/pci/bridge/PCIBridgeChips.cpp)
target_include_directories(pcibridge_test PRIVATE
    $<TARGET_PROPERTY:alphabox,INCLUDE_DIRECTORIES>)
target_link_libraries(pcibridge_test Threads::Threads)
add_test(NAME pcibridge COMMAND pcibridge_test)

# The host endpoints' addresses in either IP family (HostSocket.cpp: the
# serial ports, the gdb stub, the UDP back end): host:port with IPv6
# brackets, the printed form, and listening on loopback, dual stack on ::.
add_executable(hostsock_test test/unit/hostsock_test.cpp
    src/common/HostSocket.cpp)
target_include_directories(hostsock_test PRIVATE src/common)
add_test(NAME hostsock COMMAND hostsock_test)

# The NAT network back end (libslirp) driven with hand-built guest frames:
# DHCP, BOOTP, TFTP, TCP to the host, hostfwd, loss and retransmission.
if(HAVE_SLIRP)
    add_executable(nat_test test/unit/nat_test.cpp
        src/devices/net/NetworkNat.cpp src/devices/net/DynLib.cpp)
    target_include_directories(nat_test PRIVATE
        $<TARGET_PROPERTY:alphabox,INCLUDE_DIRECTORIES>)
    target_link_libraries(nat_test Threads::Threads ${CMAKE_DL_LIBS})
    add_test(NAME nat COMMAND nat_test)
endif()

message(STATUS "C++ compiler flags  : ${CMAKE_CXX_FLAGS}")
message(STATUS "C compiler flags    : ${CMAKE_C_FLAGS}")
message(STATUS "Linker flags        : ${CMAKE_EXE_LINKER_FLAGS} ${CMAKE_SHARED_LINKER_FLAGS} ${CMAKE_STATIC_LINKER_FLAGS}")
