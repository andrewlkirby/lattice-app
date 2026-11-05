#include "control/push/PushDisplay.h"
#include "control/push/PushProtocol.h"

#if LATTICE_LIBUSB
    #if defined(_MSC_VER)
        #pragma warning(push)
        #pragma warning(disable : 4200) // libusb.h: zero-sized arrays
    #endif
    #include <libusb.h>
    #if defined(_MSC_VER)
        #pragma warning(pop)
    #endif
#endif

namespace lattice::control::push
{

#if LATTICE_LIBUSB

namespace
{
/** One Push's display interface, claimed, with a libusb context of its own (so nothing else in
    the process shares its state). */
class UsbLink final : public DisplayLink
{
public:
    UsbLink(libusb_context* c, libusb_device_handle* h) : context(c), handle(h) {}
    ~UsbLink() override
    {
        libusb_release_interface(handle, displayInterface);
        libusb_close(handle);
        libusb_exit(context);
    }
    bool write(const std::uint8_t* data, int size) override
    {
        int done = 0;
        // The manual's own timeout for a transfer (Allocating Libusb Transfers).
        const int r = libusb_bulk_transfer(handle, displayEndpoint, const_cast<unsigned char*>(data), size, &done, 1000);
        return r == 0 && done == size;
    }

private:
    libusb_context* context;
    libusb_device_handle* handle;
};

DisplayStatus statusOf(int error)
{
    switch (error)
    {
    case LIBUSB_ERROR_NOT_SUPPORTED: return DisplayStatus::noDriver; // Windows: no WinUSB on the interface
    case LIBUSB_ERROR_ACCESS:
    #if defined(_WIN32)
        return DisplayStatus::busy; // Windows: another program has it open
    #else
        return DisplayStatus::noPermission; // no udev rule
    #endif
    case LIBUSB_ERROR_BUSY: return DisplayStatus::busy;
    case LIBUSB_ERROR_NO_DEVICE:
    case LIBUSB_ERROR_NOT_FOUND: return DisplayStatus::notFound;
    default: return DisplayStatus::failed;
    }
}
} // namespace

OpenedLink openUsbDisplay(int productId)
{
    libusb_context* context = nullptr;
    if (libusb_init_context(&context, nullptr, 0) < 0)
        return {nullptr, DisplayStatus::failed};
    libusb_device** devices = nullptr;
    const auto count = libusb_get_device_list(context, &devices);
    DisplayStatus status = DisplayStatus::notFound;
    libusb_device_handle* handle = nullptr;
    for (ssize_t i = 0; i < count && handle == nullptr; ++i)
    {
        libusb_device_descriptor descriptor{};
        if (libusb_get_device_descriptor(devices[i], &descriptor) < 0)
            continue;
        if (descriptor.idVendor != vendorId || descriptor.idProduct != productId)
            continue;
        if (const int r = libusb_open(devices[i], &handle); r < 0)
        {
            status = statusOf(r);
            handle = nullptr;
            continue;
        }
        if (const int r = libusb_claim_interface(handle, displayInterface); r < 0)
        {
            status = statusOf(r);
            libusb_close(handle);
            handle = nullptr;
        }
    }
    if (devices != nullptr)
        libusb_free_device_list(devices, 1);
    if (handle == nullptr)
    {
        libusb_exit(context);
        return {nullptr, status};
    }
    return {std::make_unique<UsbLink>(context, handle), DisplayStatus::connected};
}

#else

OpenedLink openUsbDisplay(int)
{
    return {nullptr, DisplayStatus::unsupported};
}

#endif

} // namespace lattice::control::push
