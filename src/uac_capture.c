/*
 * uac_capture - userspace USB Audio Class capture for the HDMI capture stick
 * (VID:PID 345f:2130).  The kernel has no snd-usb-audio, so we drive the
 * AudioStreaming interface (3, alt 1, isochronous IN ep 0x82, PCM 48k/2ch/16)
 * via libusb and write raw S16LE PCM to stdout.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <libusb-1.0/libusb.h>

#define EP_IN	0x82
#define MAXPKT	192
#define NPKT	16

static int outfd = 1;
static struct libusb_transfer *xfer;

static void xfer_cb(struct libusb_transfer *t)
{
	if (t->status == LIBUSB_TRANSFER_COMPLETED) {
		for (int i = 0; i < t->num_iso_packets; i++) {
			struct libusb_iso_packet_descriptor *p = &t->iso_packet_desc[i];
			if (p->actual_length) {
				unsigned char *b = libusb_get_iso_packet_buffer_simple(t, i);
				if (b)
					if (write(outfd, b, p->actual_length) < 0) {}
			}
		}
	}
	if (libusb_submit_transfer(t) < 0) {
		/* fatal; leave to timeout */
	}
}

int main(int argc, char **argv)
{
	libusb_context *ctx;
	if (argc > 1) {
		int fd = open(argv[1], O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fd >= 0)
			outfd = fd;
	}
	if (libusb_init(&ctx) < 0) {
		fprintf(stderr, "libusb_init failed\n");
		return 1;
	}
	libusb_device_handle *h =
		libusb_open_device_with_vid_pid(ctx, 0x345f, 0x2130);
	if (!h) {
		fprintf(stderr, "open 345f:2130 failed\n");
		return 1;
	}
	if (libusb_kernel_driver_active(h, 2) == 1)
		libusb_detach_kernel_driver(h, 2);
	if (libusb_claim_interface(h, 2) < 0)
		fprintf(stderr, "claim iface 2 failed (continue)\n");
	if (libusb_kernel_driver_active(h, 3) == 1)
		libusb_detach_kernel_driver(h, 3);
	if (libusb_claim_interface(h, 3) < 0) {
		fprintf(stderr, "claim iface 3 failed\n");
		return 1;
	}
	/* Feature Unit (unit 2 on AudioControl iface 2): unmute master */
	{
		unsigned char m = 0;
		int r = libusb_control_transfer(h, 0x21, 0x01, (0x01 << 8),
					       (2 << 8) | 2, &m, 1, 1000);
		fprintf(stderr, "unmute FEATURE unit2 -> %d\n", r);
		/* also try volume-ish selectors away; and output terminal? */
	}
	if (libusb_set_interface_alt_setting(h, 3, 1) < 0) {
		fprintf(stderr, "set alt 1 failed\n");
		return 1;
	}
	int buflen = NPKT * MAXPKT;
	unsigned char *buf = malloc(buflen);
	xfer = libusb_alloc_transfer(NPKT);
	libusb_fill_iso_transfer(xfer, h, EP_IN, buf, buflen, NPKT,
				 xfer_cb, NULL, 0);
	libusb_set_iso_packet_lengths(xfer, MAXPKT);
	if (libusb_submit_transfer(xfer) < 0) {
		fprintf(stderr, "submit failed\n");
		return 1;
	}
	for (;;) {
		struct timeval tv = { 1, 0 };
		if (libusb_handle_events_timeout(ctx, &tv) < 0)
			break;
	}
	return 0;
}
