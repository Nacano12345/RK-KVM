/* usbdump - print USB config descriptors (interfaces, endpoints, class extras) */
#include <stdio.h>
#include <string.h>
#include <libusb-1.0/libusb.h>

int main(int argc, char **argv)
{
	libusb_context *ctx;
	libusb_init(&ctx);
	libusb_device **list;
	ssize_t n = libusb_get_device_list(ctx, &list);
	int vid = argc > 1 ? (int)strtol(argv[1], NULL, 16) : 0x345f;
	int pid = argc > 2 ? (int)strtol(argv[2], NULL, 16) : 0x2130;
	for (ssize_t i = 0; i < n; i++) {
		struct libusb_device_descriptor dd;
		libusb_get_device_descriptor(list[i], &dd);
		if (dd.idVendor != vid || dd.idProduct != pid)
			continue;
		printf("Device %04x:%04x at bus %d addr %d\n", dd.idVendor,
		       dd.idProduct, libusb_get_bus_number(list[i]),
		       libusb_get_device_address(list[i]));
		struct libusb_config_descriptor *cfg;
		if (libusb_get_active_config_descriptor(list[i], &cfg) != 0)
			continue;
		printf(" bNumInterfaces=%d\n", cfg->bNumInterfaces);
		for (int j = 0; j < cfg->bNumInterfaces; j++) {
			const struct libusb_interface *ifp = &cfg->interface[j];
			for (int k = 0; k < ifp->num_altsetting; k++) {
				const struct libusb_interface_descriptor *id =
					&ifp->altsetting[k];
				printf("  intf %d alt %d: class=%02x sub=%02x proto=%02x eps=%d",
				       id->bInterfaceNumber, id->bAlternateSetting,
				       id->bInterfaceClass, id->bInterfaceSubClass,
				       id->bInterfaceProtocol, id->bNumEndpoints);
				if (id->extra_length) {
					printf("  extra[%d]:", id->extra_length);
					for (int x = 0; x < id->extra_length; x++)
						printf(" %02x", id->extra[x]);
				}
				printf("\n");
				for (int e = 0; e < id->bNumEndpoints; e++) {
					const struct libusb_endpoint_descriptor *ep =
						&id->endpoint[e];
					printf("    ep addr=%02x attr=%02x maxpkt=%d interval=%d\n",
					       ep->bEndpointAddress, ep->bmAttributes,
					       ep->wMaxPacketSize, ep->bInterval);
				}
			}
		}
		libusb_free_config_descriptor(cfg);
	}
	libusb_free_device_list(list, 1);
	libusb_exit(ctx);
	return 0;
}
