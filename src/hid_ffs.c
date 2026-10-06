/*
 * rkkvm-hid : userspace USB HID gadget for RK3506 (FunctionFS)
 *
 * Exposes a boot-protocol keyboard (interface 0) and relative mouse
 * (interface 1) to a target host over the board's OTG port, without
 * needing the in-kernel usb_f_hid module.
 *
 * Control protocol (line based, default 127.0.0.1:5001):
 *   kd <mod_hex> <key_hex>      key down
 *   ku <mod_hex> <key_hex>      key up
 *   k  <mod_hex> <key_hex>      key tap (down+up)
 *   m  <dx> <dy> <btn> <wheel>  mouse move, btn bitmask (1=L 2=R 4=M)
 *   mb <btn>                    set mouse buttons
 *   r                           release everything
 * replies: "ok\n" / "err\n"
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <linux/usb/ch9.h>
#include <linux/usb/functionfs.h>

#ifndef USB_DT_HID
#define USB_DT_HID		0x21
#endif
#ifndef USB_DT_REPORT
#define USB_DT_REPORT		0x22
#endif

#define UDC_DEFAULT		"ff740000.usb"
#define GADGET_DIR		"/sys/kernel/config/usb_gadget/rkkvm"
#define FFS_MNT			"/dev/ffs-hid"
#define FFS_NAME		"hid"
#define LISTEN_PORT		5001

static const char *g_udc = UDC_DEFAULT;
static int ep0_fd = -1, kbd_fd = -1, mouse_fd = -1, abs_fd = -1, cons_fd = -1;
static volatile int g_enabled = 0;
static volatile sig_atomic_t g_stop = 0;

/* Per-endpoint writer mailbox: the command thread only posts the latest
 * report; a dedicated thread does the (possibly blocking) FFS write so a
 * stuck endpoint can never wedge the control server. */
struct eplink {
	int fd;
	pthread_mutex_t m;
	pthread_cond_t c;
	uint8_t buf[16];
	int len;
	int has;
};
static struct eplink g_ep_kbd, g_ep_mouse, g_ep_abs, g_ep_cons;

static void *ep_writer(void *arg)
{
	struct eplink *e = arg;
	for (;;) {
		pthread_mutex_lock(&e->m);
		while (!e->has)
			pthread_cond_wait(&e->c, &e->m);
		int l = e->len;
		uint8_t b[16];
		memcpy(b, e->buf, l);
		e->has = 0;
		pthread_mutex_unlock(&e->m);
		if (g_enabled && e->fd >= 0)
			(void)write(e->fd, b, l);
	}
	return NULL;
}

static void ep_post(struct eplink *e, const uint8_t *data, int len)
{
	if (e->fd < 0)
		return;
	if (len > (int)sizeof e->buf)
		len = sizeof e->buf;
	pthread_mutex_lock(&e->m);
	memcpy(e->buf, data, len);
	e->len = len;
	e->has = 1;
	pthread_cond_signal(&e->c);
	pthread_mutex_unlock(&e->m);
}

static void ep_init(struct eplink *e, int fd)
{
	e->fd = fd;
	e->len = 0;
	e->has = 0;
	pthread_mutex_init(&e->m, NULL);
	pthread_cond_init(&e->c, NULL);
	pthread_t th;
	if (pthread_create(&th, NULL, ep_writer, e) == 0)
		pthread_detach(th);
}

static void on_term(int sig)
{
	(void)sig;
	g_stop = 1;
}

/* ----------------------- HID report descriptors -------------------------- */

static const uint8_t kbd_report[] = {
	0x05, 0x01,        /* Usage Page (Generic Desktop) */
	0x09, 0x06,        /* Usage (Keyboard) */
	0xA1, 0x01,        /* Collection (Application) */
	0x05, 0x07,        /*   Usage Page (Key Codes) */
	0x19, 0xE0,        /*   Usage Minimum (224) */
	0x29, 0xE7,        /*   Usage Maximum (231) */
	0x15, 0x00,        /*   Logical Minimum (0) */
	0x25, 0x01,        /*   Logical Maximum (1) */
	0x75, 0x01,        /*   Report Size (1) */
	0x95, 0x08,        /*   Report Count (8) */
	0x81, 0x02,        /*   Input (Data,Var,Abs) modifiers */
	0x95, 0x01,        /*   Report Count (1) */
	0x75, 0x08,        /*   Report Size (8) */
	0x81, 0x01,        /*   Input (Const) reserved */
	0x95, 0x06,        /*   Report Count (6) */
	0x75, 0x08,        /*   Report Size (8) */
	0x15, 0x00,        /*   Logical Minimum (0) */
	0x25, 0x65,        /*   Logical Maximum (101) */
	0x05, 0x07,        /*   Usage Page (Key Codes) */
	0x19, 0x00,        /*   Usage Minimum (0) */
	0x29, 0x65,        /*   Usage Maximum (101) */
	0x81, 0x00,        /*   Input (Data,Array) keys */
	0xC0               /* End Collection */
};

static const uint8_t mouse_report[] = {
	0x05, 0x01,        /* Usage Page (Generic Desktop) */
	0x09, 0x02,        /* Usage (Mouse) */
	0xA1, 0x01,        /* Collection (Application) */
	0x09, 0x01,        /*   Usage (Pointer) */
	0xA1, 0x00,        /*   Collection (Physical) */
	0x05, 0x09,        /*     Usage Page (Buttons) */
	0x19, 0x01,        /*     Usage Minimum (1) */
	0x29, 0x03,        /*     Usage Maximum (3) */
	0x15, 0x00,        /*     Logical Minimum (0) */
	0x25, 0x01,        /*     Logical Maximum (1) */
	0x95, 0x03,        /*     Report Count (3) */
	0x75, 0x01,        /*     Report Size (1) */
	0x81, 0x02,        /*     Input (Data,Var,Abs) */
	0x95, 0x01,        /*     Report Count (1) */
	0x75, 0x05,        /*     Report Size (5) */
	0x81, 0x01,        /*     Input (Const) padding */
	0x05, 0x01,        /*     Usage Page (Generic Desktop) */
	0x09, 0x30,        /*     Usage (X) */
	0x09, 0x31,        /*     Usage (Y) */
	0x09, 0x38,        /*     Usage (Wheel) */
	0x15, 0x81,        /*     Logical Minimum (-127) */
	0x25, 0x7F,        /*     Logical Maximum (127) */
	0x75, 0x08,        /*     Report Size (8) */
	0x95, 0x03,        /*     Report Count (3) */
	0x81, 0x06,        /*     Input (Data,Var,Rel) */
	0xC0,              /*   End Collection */
	0xC0               /* End Collection */
};

/* Absolute pointer (mouse with absolute X/Y), 6-byte report:
 * [buttons, x_lo, x_hi, y_lo, y_hi, wheel] with x,y in 0..32767 */
static const uint8_t absmouse_report[] = {
	0x05, 0x01,        /* Usage Page (Generic Desktop) */
	0x09, 0x02,        /* Usage (Mouse) */
	0xA1, 0x01,        /* Collection (Application) */
	0x09, 0x01,        /*   Usage (Pointer) */
	0xA1, 0x00,        /*   Collection (Physical) */
	0x05, 0x09,        /*     Usage Page (Buttons) */
	0x19, 0x01,        /*     Usage Minimum (1) */
	0x29, 0x03,        /*     Usage Maximum (3) */
	0x15, 0x00,        /*     Logical Minimum (0) */
	0x25, 0x01,        /*     Logical Maximum (1) */
	0x95, 0x03,        /*     Report Count (3) */
	0x75, 0x01,        /*     Report Size (1) */
	0x81, 0x02,        /*     Input (Data,Var,Abs) */
	0x95, 0x01,        /*     Report Count (1) */
	0x75, 0x05,        /*     Report Size (5) */
	0x81, 0x01,        /*     Input (Const) padding */
	0x05, 0x01,        /*     Usage Page (Generic Desktop) */
	0x09, 0x30,        /*     Usage (X) */
	0x09, 0x31,        /*     Usage (Y) */
	0x15, 0x00,        /*     Logical Minimum (0) */
	0x26, 0xFF, 0x7F,  /*     Logical Maximum (32767) */
	0x75, 0x10,        /*     Report Size (16) */
	0x95, 0x02,        /*     Report Count (2) */
	0x81, 0x02,        /*     Input (Data,Var,Abs) */
	0x09, 0x38,        /*     Usage (Wheel) */
	0x15, 0x81,        /*     Logical Minimum (-127) */
	0x25, 0x7F,        /*     Logical Maximum (127) */
	0x75, 0x08,        /*     Report Size (8) */
	0x95, 0x01,        /*     Report Count (1) */
	0x81, 0x06,        /*     Input (Data,Var,Rel) */
	0xC0,              /*   End Collection */
	0xC0               /* End Collection */
};

/* Consumer Control (multimedia keys), 2-byte report: [usage_lo, usage_hi] */
static const uint8_t consumer_report[] = {
	0x05, 0x0C,        /* Usage Page (Consumer) */
	0x09, 0x01,        /* Usage (Consumer Control) */
	0xA1, 0x01,        /* Collection (Application) */
	0x15, 0x00,        /*   Logical Minimum (0) */
	0x26, 0xFF, 0x03,  /*   Logical Maximum (1023) */
	0x19, 0x00,        /*   Usage Minimum (0) */
	0x2A, 0xFF, 0x03,  /*   Usage Maximum (1023) */
	0x75, 0x10,        /*   Report Size (16) */
	0x95, 0x01,        /*   Report Count (1) */
	0x81, 0x00,        /*   Input (Data,Array) */
	0xC0               /* End Collection */
};

/* HID class descriptor (9 bytes) */
struct usb_hid_desc {
	__u8  bLength;
	__u8  bDescriptorType;
	__le16 bcdHID;
	__u8  bCountryCode;
	__u8  bNumDescriptors;
	__u8  bDescriptorType2;
	__le16 wDescriptorLength;
} __attribute__((packed));

#define LE16(x) ((__le16)(uint16_t)(x))
#define LE32(x) ((__le32)(uint32_t)(x))

#define KBD_EP_ADDR	0x81
#define MOUSE_EP_ADDR	0x82
#define ABS_EP_ADDR	0x83
#define CONS_EP_ADDR	0x84

struct speed_descs {
	struct usb_interface_descriptor kbd_intf;
	struct usb_hid_desc kbd_hid;
	struct usb_endpoint_descriptor_no_audio kbd_ep;
	struct usb_interface_descriptor mouse_intf;
	struct usb_hid_desc mouse_hid;
	struct usb_endpoint_descriptor_no_audio mouse_ep;
	struct usb_interface_descriptor abs_intf;
	struct usb_hid_desc abs_hid;
	struct usb_endpoint_descriptor_no_audio abs_ep;
	struct usb_interface_descriptor cons_intf;
	struct usb_hid_desc cons_hid;
	struct usb_endpoint_descriptor_no_audio cons_ep;
} __attribute__((packed));

static const struct {
	struct usb_functionfs_descs_head_v2 header;
	__le32 fs_count;
	__le32 hs_count;
	struct speed_descs fs;
	struct speed_descs hs;
} __attribute__((packed)) g_descs = {
	.header = {
		.magic = LE32(FUNCTIONFS_DESCRIPTORS_MAGIC_V2),
		.length = LE32(sizeof g_descs),
		.flags = LE32(FUNCTIONFS_HAS_FS_DESC | FUNCTIONFS_HAS_HS_DESC),
	},
	.fs_count = LE32(12),
	.hs_count = LE32(12),
	.fs = {
		.kbd_intf = {
			.bLength = sizeof(struct usb_interface_descriptor),
			.bDescriptorType = USB_DT_INTERFACE,
			.bInterfaceNumber = 0,
			.bAlternateSetting = 0,
			.bNumEndpoints = 1,
			.bInterfaceClass = USB_CLASS_HID,
			.bInterfaceSubClass = 1,   /* boot */
			.bInterfaceProtocol = 1,   /* keyboard */
			.iInterface = 0,
		},
		.kbd_hid = {
			.bLength = sizeof(struct usb_hid_desc),
			.bDescriptorType = USB_DT_HID,
			.bcdHID = LE16(0x0111),
			.bCountryCode = 0,
			.bNumDescriptors = 1,
			.bDescriptorType2 = USB_DT_REPORT,
			.wDescriptorLength = LE16(sizeof kbd_report),
		},
		.kbd_ep = {
			.bLength = sizeof(struct usb_endpoint_descriptor_no_audio),
			.bDescriptorType = USB_DT_ENDPOINT,
			.bEndpointAddress = KBD_EP_ADDR,
			.bmAttributes = USB_ENDPOINT_XFER_INT,
			.wMaxPacketSize = LE16(8),
			.bInterval = 10,
		},
		.mouse_intf = {
			.bLength = sizeof(struct usb_interface_descriptor),
			.bDescriptorType = USB_DT_INTERFACE,
			.bInterfaceNumber = 1,
			.bAlternateSetting = 0,
			.bNumEndpoints = 1,
			.bInterfaceClass = USB_CLASS_HID,
			.bInterfaceSubClass = 1,   /* boot */
			.bInterfaceProtocol = 2,   /* mouse */
			.iInterface = 0,
		},
		.mouse_hid = {
			.bLength = sizeof(struct usb_hid_desc),
			.bDescriptorType = USB_DT_HID,
			.bcdHID = LE16(0x0111),
			.bCountryCode = 0,
			.bNumDescriptors = 1,
			.bDescriptorType2 = USB_DT_REPORT,
			.wDescriptorLength = LE16(sizeof mouse_report),
		},
		.mouse_ep = {
			.bLength = sizeof(struct usb_endpoint_descriptor_no_audio),
			.bDescriptorType = USB_DT_ENDPOINT,
			.bEndpointAddress = MOUSE_EP_ADDR,
			.bmAttributes = USB_ENDPOINT_XFER_INT,
			.wMaxPacketSize = LE16(4),
			.bInterval = 10,
		},
		.abs_intf = {
			.bLength = sizeof(struct usb_interface_descriptor),
			.bDescriptorType = USB_DT_INTERFACE,
			.bInterfaceNumber = 2,
			.bAlternateSetting = 0,
			.bNumEndpoints = 1,
			.bInterfaceClass = USB_CLASS_HID,
			.bInterfaceSubClass = 0,
			.bInterfaceProtocol = 0,
			.iInterface = 0,
		},
		.abs_hid = {
			.bLength = sizeof(struct usb_hid_desc),
			.bDescriptorType = USB_DT_HID,
			.bcdHID = LE16(0x0111),
			.bCountryCode = 0,
			.bNumDescriptors = 1,
			.bDescriptorType2 = USB_DT_REPORT,
			.wDescriptorLength = LE16(sizeof absmouse_report),
		},
		.abs_ep = {
			.bLength = sizeof(struct usb_endpoint_descriptor_no_audio),
			.bDescriptorType = USB_DT_ENDPOINT,
			.bEndpointAddress = ABS_EP_ADDR,
			.bmAttributes = USB_ENDPOINT_XFER_INT,
			.wMaxPacketSize = LE16(6),
			.bInterval = 10,
		},
		.cons_intf = {
			.bLength = sizeof(struct usb_interface_descriptor),
			.bDescriptorType = USB_DT_INTERFACE,
			.bInterfaceNumber = 3,
			.bAlternateSetting = 0,
			.bNumEndpoints = 1,
			.bInterfaceClass = USB_CLASS_HID,
			.bInterfaceSubClass = 0,
			.bInterfaceProtocol = 0,
			.iInterface = 0,
		},
		.cons_hid = {
			.bLength = sizeof(struct usb_hid_desc),
			.bDescriptorType = USB_DT_HID,
			.bcdHID = LE16(0x0111),
			.bCountryCode = 0,
			.bNumDescriptors = 1,
			.bDescriptorType2 = USB_DT_REPORT,
			.wDescriptorLength = LE16(sizeof consumer_report),
		},
		.cons_ep = {
			.bLength = sizeof(struct usb_endpoint_descriptor_no_audio),
			.bDescriptorType = USB_DT_ENDPOINT,
			.bEndpointAddress = CONS_EP_ADDR,
			.bmAttributes = USB_ENDPOINT_XFER_INT,
			.wMaxPacketSize = LE16(2),
			.bInterval = 10,
		},
	},
	.hs = {
		.kbd_intf = {
			.bLength = sizeof(struct usb_interface_descriptor),
			.bDescriptorType = USB_DT_INTERFACE,
			.bInterfaceNumber = 0,
			.bAlternateSetting = 0,
			.bNumEndpoints = 1,
			.bInterfaceClass = USB_CLASS_HID,
			.bInterfaceSubClass = 1,
			.bInterfaceProtocol = 1,
			.iInterface = 0,
		},
		.kbd_hid = {
			.bLength = sizeof(struct usb_hid_desc),
			.bDescriptorType = USB_DT_HID,
			.bcdHID = LE16(0x0111),
			.bCountryCode = 0,
			.bNumDescriptors = 1,
			.bDescriptorType2 = USB_DT_REPORT,
			.wDescriptorLength = LE16(sizeof kbd_report),
		},
		.kbd_ep = {
			.bLength = sizeof(struct usb_endpoint_descriptor_no_audio),
			.bDescriptorType = USB_DT_ENDPOINT,
			.bEndpointAddress = KBD_EP_ADDR,
			.bmAttributes = USB_ENDPOINT_XFER_INT,
			.wMaxPacketSize = LE16(8),
			.bInterval = 8,
		},
		.mouse_intf = {
			.bLength = sizeof(struct usb_interface_descriptor),
			.bDescriptorType = USB_DT_INTERFACE,
			.bInterfaceNumber = 1,
			.bAlternateSetting = 0,
			.bNumEndpoints = 1,
			.bInterfaceClass = USB_CLASS_HID,
			.bInterfaceSubClass = 1,
			.bInterfaceProtocol = 2,
			.iInterface = 0,
		},
		.mouse_hid = {
			.bLength = sizeof(struct usb_hid_desc),
			.bDescriptorType = USB_DT_HID,
			.bcdHID = LE16(0x0111),
			.bCountryCode = 0,
			.bNumDescriptors = 1,
			.bDescriptorType2 = USB_DT_REPORT,
			.wDescriptorLength = LE16(sizeof mouse_report),
		},
		.mouse_ep = {
			.bLength = sizeof(struct usb_endpoint_descriptor_no_audio),
			.bDescriptorType = USB_DT_ENDPOINT,
			.bEndpointAddress = MOUSE_EP_ADDR,
			.bmAttributes = USB_ENDPOINT_XFER_INT,
			.wMaxPacketSize = LE16(4),
			.bInterval = 8,
		},
		.abs_intf = {
			.bLength = sizeof(struct usb_interface_descriptor),
			.bDescriptorType = USB_DT_INTERFACE,
			.bInterfaceNumber = 2,
			.bAlternateSetting = 0,
			.bNumEndpoints = 1,
			.bInterfaceClass = USB_CLASS_HID,
			.bInterfaceSubClass = 0,
			.bInterfaceProtocol = 0,
			.iInterface = 0,
		},
		.abs_hid = {
			.bLength = sizeof(struct usb_hid_desc),
			.bDescriptorType = USB_DT_HID,
			.bcdHID = LE16(0x0111),
			.bCountryCode = 0,
			.bNumDescriptors = 1,
			.bDescriptorType2 = USB_DT_REPORT,
			.wDescriptorLength = LE16(sizeof absmouse_report),
		},
		.abs_ep = {
			.bLength = sizeof(struct usb_endpoint_descriptor_no_audio),
			.bDescriptorType = USB_DT_ENDPOINT,
			.bEndpointAddress = ABS_EP_ADDR,
			.bmAttributes = USB_ENDPOINT_XFER_INT,
			.wMaxPacketSize = LE16(6),
			.bInterval = 8,
		},
		.cons_intf = {
			.bLength = sizeof(struct usb_interface_descriptor),
			.bDescriptorType = USB_DT_INTERFACE,
			.bInterfaceNumber = 3,
			.bAlternateSetting = 0,
			.bNumEndpoints = 1,
			.bInterfaceClass = USB_CLASS_HID,
			.bInterfaceSubClass = 0,
			.bInterfaceProtocol = 0,
			.iInterface = 0,
		},
		.cons_hid = {
			.bLength = sizeof(struct usb_hid_desc),
			.bDescriptorType = USB_DT_HID,
			.bcdHID = LE16(0x0111),
			.bCountryCode = 0,
			.bNumDescriptors = 1,
			.bDescriptorType2 = USB_DT_REPORT,
			.wDescriptorLength = LE16(sizeof consumer_report),
		},
		.cons_ep = {
			.bLength = sizeof(struct usb_endpoint_descriptor_no_audio),
			.bDescriptorType = USB_DT_ENDPOINT,
			.bEndpointAddress = CONS_EP_ADDR,
			.bmAttributes = USB_ENDPOINT_XFER_INT,
			.wMaxPacketSize = LE16(2),
			.bInterval = 8,
		},
	},
};

#define STR_PRODUCT "RK3506 IP-KVM"
static const struct {
	struct usb_functionfs_strings_head header;
	struct {
		__le16 code;
		char str1[sizeof(STR_PRODUCT)];
	} __attribute__((packed)) lang0;
} __attribute__((packed)) g_strings = {
	.header = {
		.magic = LE32(FUNCTIONFS_STRINGS_MAGIC),
		.length = LE32(sizeof g_strings),
		.str_count = LE32(1),
		.lang_count = LE32(1),
	},
	.lang0 = {
		.code = LE16(0x0409),
		.str1 = STR_PRODUCT,
	},
};

/* ----------------------------- helpers ----------------------------------- */

static int write_file(const char *path, const char *val)
{
	int fd = open(path, O_WRONLY);
	if (fd < 0)
		return -1;
	int r = write(fd, val, strlen(val));
	close(fd);
	return r < 0 ? -1 : 0;
}

static void unbind_other_udc(void)
{
	DIR *d = opendir("/sys/kernel/config/usb_gadget");
	if (!d)
		return;
	struct dirent *e;
	char path[256], buf[128];
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.')
			continue;
		snprintf(path, sizeof path,
			 "/sys/kernel/config/usb_gadget/%s/UDC", e->d_name);
		int fd = open(path, O_RDWR);
		if (fd < 0)
			continue;
		int n = read(fd, buf, sizeof buf - 1);
		if (n > 0) {
			buf[n] = 0;
			char *nl = strchr(buf, '\n');
			if (nl)
				*nl = 0;
			if (!strcmp(buf, g_udc)) {
				lseek(fd, 0, SEEK_SET);
				write(fd, "", 1);
			}
		}
		close(fd);
	}
	closedir(d);
}

static void reset_function(void)
{
	char p[256];
	/* unbind our gadget, drop the ffs function and any stale mount */
	write_file(GADGET_DIR "/UDC", "");
	snprintf(p, sizeof p, "%s/configs/b.1/ffs." FFS_NAME, GADGET_DIR);
	unlink(p);
	snprintf(p, sizeof p, "%s/functions/ffs." FFS_NAME, GADGET_DIR);
	rmdir(p);
	umount(FFS_MNT);
}

static int setup_gadget(void)
{
	mkdir("/sys/kernel/config/usb_gadget", 0755);

	reset_function();

	if (mkdir(GADGET_DIR, 0755) < 0 && errno != EEXIST) {
		perror("mkdir gadget");
		return -1;
	}
	write_file(GADGET_DIR "/idVendor", "0x1d6b");
	write_file(GADGET_DIR "/idProduct", "0x0104");
	write_file(GADGET_DIR "/bcdDevice", "0x0100");
	write_file(GADGET_DIR "/bcdUSB", "0x0200");

	mkdir(GADGET_DIR "/strings/0x409", 0777);
	write_file(GADGET_DIR "/strings/0x409/serialnumber", "rk3506-0001");
	write_file(GADGET_DIR "/strings/0x409/manufacturer", "Tronlong");
	write_file(GADGET_DIR "/strings/0x409/product", "RK3506 IP-KVM");

	mkdir(GADGET_DIR "/configs/b.1", 0777);
	mkdir(GADGET_DIR "/configs/b.1/strings/0x409", 0777);
	write_file(GADGET_DIR "/configs/b.1/strings/0x409/configuration", "HID");
	write_file(GADGET_DIR "/configs/b.1/MaxPower", "250");

	if (mkdir(GADGET_DIR "/functions/ffs." FFS_NAME, 0777) < 0 &&
	    errno != EEXIST) {
		perror("mkdir ffs");
		return -1;
	}
	if (symlink(GADGET_DIR "/functions/ffs." FFS_NAME,
		    GADGET_DIR "/configs/b.1/ffs." FFS_NAME) < 0 &&
	    errno != EEXIST) {
		perror("symlink ffs");
		return -1;
	}

	unbind_other_udc();

	mkdir(FFS_MNT, 0755);
	if (mount(FFS_NAME, FFS_MNT, "functionfs", 0, NULL) < 0) {
		if (errno != EBUSY) {
			perror("mount functionfs");
			return -1;
		}
	}
	return 0;
}

static int open_ffs(void)
{
	char path[256];

	snprintf(path, sizeof path, "%s/ep0", FFS_MNT);
	ep0_fd = open(path, O_RDWR);
	if (ep0_fd < 0) {
		perror("open ep0");
		return -1;
	}
	if (write(ep0_fd, &g_descs, sizeof g_descs) < 0) {
		perror("write descriptors");
		return -1;
	}
	if (write(ep0_fd, &g_strings, sizeof g_strings) < 0) {
		perror("write strings");
		return -1;
	}
	snprintf(path, sizeof path, "%s/ep1", FFS_MNT);
	kbd_fd = open(path, O_RDWR);
	if (kbd_fd < 0) {
		perror("open ep1 (kbd)");
		return -1;
	}
	snprintf(path, sizeof path, "%s/ep2", FFS_MNT);
	mouse_fd = open(path, O_RDWR);
	if (mouse_fd < 0) {
		perror("open ep2 (mouse)");
		return -1;
	}
	snprintf(path, sizeof path, "%s/ep3", FFS_MNT);
	abs_fd = open(path, O_RDWR);
	if (abs_fd < 0) {
		perror("open ep3 (abs mouse)");
		return -1;
	}
	snprintf(path, sizeof path, "%s/ep4", FFS_MNT);
	cons_fd = open(path, O_RDWR);
	if (cons_fd < 0) {
		perror("open ep4 (consumer)");
		return -1;
	}
	ep_init(&g_ep_kbd, kbd_fd);
	ep_init(&g_ep_mouse, mouse_fd);
	ep_init(&g_ep_abs, abs_fd);
	ep_init(&g_ep_cons, cons_fd);
	return 0;
}

static int bind_udc(void)
{
	return write_file(GADGET_DIR "/UDC", g_udc);
}

/* --------------------------- HID report state ---------------------------- */

static uint8_t g_kbd_mod = 0;
static uint8_t g_kbd_keys[6] = {0, 0, 0, 0, 0, 0};
static uint8_t g_mouse_btn = 0;

static void kbd_send(void)
{
	uint8_t rep[8];
	rep[0] = g_kbd_mod;
	rep[1] = 0;
	memcpy(&rep[2], g_kbd_keys, 6);
	ep_post(&g_ep_kbd, rep, sizeof rep);
}

static void mouse_send(int dx, int dy, int wheel)
{
	uint8_t rep[4];
	if (dx > 127) dx = 127;
	if (dx < -127) dx = -127;
	if (dy > 127) dy = 127;
	if (dy < -127) dy = -127;
	if (wheel > 127) wheel = 127;
	if (wheel < -127) wheel = -127;
	rep[0] = g_mouse_btn;
	rep[1] = (uint8_t)(int8_t)dx;
	rep[2] = (uint8_t)(int8_t)dy;
	rep[3] = (uint8_t)(int8_t)wheel;
	ep_post(&g_ep_mouse, rep, sizeof rep);
}

static void abs_send(int x, int y, int wheel)
{
	uint8_t rep[6];
	if (x < 0) x = 0;
	if (x > 32767) x = 32767;
	if (y < 0) y = 0;
	if (y > 32767) y = 32767;
	if (wheel > 127) wheel = 127;
	if (wheel < -127) wheel = -127;
	rep[0] = g_mouse_btn;
	rep[1] = x & 0xff;
	rep[2] = (x >> 8) & 0x7f;
	rep[3] = y & 0xff;
	rep[4] = (y >> 8) & 0x7f;
	rep[5] = (uint8_t)(int8_t)wheel;
	ep_post(&g_ep_abs, rep, sizeof rep);
}

static void consumer_send(uint16_t usage)
{
	uint8_t rep[2] = { usage & 0xff, (usage >> 8) & 0xff };
	ep_post(&g_ep_cons, rep, sizeof rep);
}

static void kbd_key_down(uint8_t key)
{
	if (!key)
		return;
	for (int i = 0; i < 6; i++)
		if (g_kbd_keys[i] == key)
			return;
	for (int i = 0; i < 6; i++) {
		if (g_kbd_keys[i] == 0) {
			g_kbd_keys[i] = key;
			kbd_send();
			return;
		}
	}
	/* no free slot: shift array */
	memmove(g_kbd_keys, g_kbd_keys + 1, 5);
	g_kbd_keys[5] = key;
	kbd_send();
}

static void kbd_key_up(uint8_t key)
{
	for (int i = 0; i < 6; i++) {
		if (g_kbd_keys[i] == key) {
			g_kbd_keys[i] = 0;
			kbd_send();
			return;
		}
	}
}

static void kbd_reset(void)
{
	g_kbd_mod = 0;
	memset(g_kbd_keys, 0, sizeof g_kbd_keys);
	kbd_send();
}

/* ----------------------------- ep0 events -------------------------------- */

static void handle_setup(struct usb_ctrlrequest *setup)
{
	uint8_t type = setup->bRequestType;
	uint8_t req = setup->bRequest;
	uint16_t wValue = setup->wValue;
	uint16_t wIndex = setup->wIndex;
	uint16_t wLength = setup->wLength;
	char tmp[8];

	if (type & USB_DIR_IN) {
		if ((type & USB_TYPE_MASK) == USB_TYPE_STANDARD &&
		    req == USB_REQ_GET_DESCRIPTOR &&
		    (wValue >> 8) == USB_DT_REPORT) {
			int iface = wIndex & 0xff;
			const uint8_t *rd;
			size_t rl;
			if (iface == 0) {
				rd = kbd_report;
				rl = sizeof kbd_report;
			} else if (iface == 2) {
				rd = absmouse_report;
				rl = sizeof absmouse_report;
			} else if (iface == 3) {
				rd = consumer_report;
				rl = sizeof consumer_report;
			} else {
				rd = mouse_report;
				rl = sizeof mouse_report;
			}
			if (rl > wLength)
				rl = wLength;
			(void)write(ep0_fd, rd, rl);
			return;
		}
		/* unsupported IN request -> stall via ep0 read */
		(void)read(ep0_fd, tmp, 1);
	} else {
		/* OUT or no-data: read to consume/complete status */
		if (wLength)
			(void)read(ep0_fd, tmp, wLength > sizeof tmp ?
				   sizeof tmp : wLength);
		else
			(void)read(ep0_fd, tmp, 1);
	}
}

static void *ep0_thread(void *arg)
{
	(void)arg;
	struct usb_functionfs_event ev[4];
	for (;;) {
		ssize_t n = read(ep0_fd, ev, sizeof ev);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			/* transient / cancel errors during (re)enumeration must not
			 * kill this thread, or the next enumeration can't be served */
			if (errno == EAGAIN || errno == EWOULDBLOCK ||
			    errno == EIDRM || errno == ENODEV ||
			    errno == ESHUTDOWN) {
				usleep(2000);
				continue;
			}
			perror("ep0 read");
			break;
		}
		int cnt = n / (int)sizeof(struct usb_functionfs_event);
		for (int i = 0; i < cnt; i++) {
			switch (ev[i].type) {
			case FUNCTIONFS_ENABLE:
				g_enabled = 1;
				fprintf(stderr, "hid: enabled\n");
				break;
			case FUNCTIONFS_DISABLE:
				g_enabled = 0;
				kbd_reset();
				fprintf(stderr, "hid: disabled\n");
				break;
			case FUNCTIONFS_SETUP:
				handle_setup(&ev[i].u.setup);
				break;
			default:
				break;
			}
		}
	}
	return NULL;
}

/* ---------------------------- control server ----------------------------- */

static void handle_command(char *line, char *reply, size_t rlen)
{
	char cmd[8] = {0};
	unsigned a, b;
	int n;

	if (sscanf(line, "%7s", cmd) != 1) {
		snprintf(reply, rlen, "err\n");
		return;
	}
	if (!strcmp(cmd, "kd")) {
		if (sscanf(line, "%*s %x %x", &a, &b) == 2) {
			g_kbd_mod = (uint8_t)a;
			kbd_key_down((uint8_t)b);
			kbd_send();
			snprintf(reply, rlen, "ok\n");
			return;
		}
	} else if (!strcmp(cmd, "ku")) {
		if (sscanf(line, "%*s %x %x", &a, &b) == 2) {
			g_kbd_mod = (uint8_t)a;
			kbd_key_up((uint8_t)b);
			kbd_send();
			snprintf(reply, rlen, "ok\n");
			return;
		}
	} else if (!strcmp(cmd, "km")) {
		if (sscanf(line, "%*s %x", &a) == 1) {
			g_kbd_mod = (uint8_t)a;
			kbd_send();
			snprintf(reply, rlen, "ok\n");
			return;
		}
	} else if (!strcmp(cmd, "k")) {
		if (sscanf(line, "%*s %x %x", &a, &b) == 2) {
			g_kbd_mod = (uint8_t)a;
			kbd_key_down((uint8_t)b);
			usleep(15000);
			kbd_key_up((uint8_t)b);
			snprintf(reply, rlen, "ok\n");
			return;
		}
	} else if (!strcmp(cmd, "m")) {
		int dx, dy, btn, wheel;
		if (sscanf(line, "%*s %d %d %d %d", &dx, &dy, &btn, &wheel) == 4) {
			g_mouse_btn = (uint8_t)btn;
			mouse_send(dx, dy, wheel);
			snprintf(reply, rlen, "ok\n");
			return;
		}
	} else if (!strcmp(cmd, "ma")) {
		int x, y, btn, wheel;
		if (sscanf(line, "%*s %d %d %d %d", &x, &y, &btn, &wheel) == 4) {
			g_mouse_btn = (uint8_t)btn;
			abs_send(x, y, wheel);
			snprintf(reply, rlen, "ok\n");
			return;
		}
	} else if (!strcmp(cmd, "c")) {
		unsigned v;
		if (sscanf(line, "%*s %x", &v) == 1) {
			consumer_send((uint16_t)v);
			usleep(15000);
			consumer_send(0);
			snprintf(reply, rlen, "ok\n");
			return;
		}
	} else if (!strcmp(cmd, "cd")) {
		unsigned v;
		if (sscanf(line, "%*s %x", &v) == 1) {
			consumer_send((uint16_t)v);
			snprintf(reply, rlen, "ok\n");
			return;
		}
	} else if (!strcmp(cmd, "cu")) {
		consumer_send(0);
		snprintf(reply, rlen, "ok\n");
		return;
	} else if (!strcmp(cmd, "mb")) {
		if (sscanf(line, "%*s %d", &n) == 1) {
			g_mouse_btn = (uint8_t)n;
			mouse_send(0, 0, 0);
			snprintf(reply, rlen, "ok\n");
			return;
		}
	} else if (!strcmp(cmd, "r")) {
		kbd_reset();
		g_mouse_btn = 0;
		mouse_send(0, 0, 0);
		snprintf(reply, rlen, "ok\n");
		return;
	}
	snprintf(reply, rlen, "err\n");
}

static int run_server(void)
{
	int srv = socket(AF_INET, SOCK_STREAM, 0);
	if (srv < 0) {
		perror("socket");
		return -1;
	}
	int one = 1;
	setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	struct sockaddr_in sa;
	memset(&sa, 0, sizeof sa);
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_ANY);
	sa.sin_port = htons(LISTEN_PORT);
	if (bind(srv, (struct sockaddr *)&sa, sizeof sa) < 0) {
		perror("bind");
		return -1;
	}
	if (listen(srv, 8) < 0) {
		perror("listen");
		return -1;
	}
	fprintf(stderr, "hid: control server on :%d\n", LISTEN_PORT);

	for (;;) {
		int c = accept(srv, NULL, NULL);
		if (c < 0) {
			if (errno == EINTR) {
				if (g_stop)
					break;
				continue;
			}
			perror("accept");
			break;
		}
		setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
		char buf[256];
		ssize_t n;
		while ((n = read(c, buf, sizeof buf - 1)) > 0) {
			buf[n] = 0;
			char *save = NULL;
			for (char *line = strtok_r(buf, "\n", &save); line;
			     line = strtok_r(NULL, "\n", &save)) {
				char reply[16];
				handle_command(line, reply, sizeof reply);
				(void)write(c, reply, strlen(reply));
			}
		}
		close(c);
	}
	return 0;
}

int main(int argc, char **argv)
{
	if (argc > 1)
		g_udc = argv[1];

	signal(SIGPIPE, SIG_IGN);
	signal(SIGTERM, on_term);
	signal(SIGINT, on_term);

	if (setup_gadget() < 0)
		return 1;
	if (open_ffs() < 0)
		return 1;
	if (bind_udc() < 0) {
		perror("bind UDC");
		return 1;
	}
	fprintf(stderr, "hid: bound to %s\n", g_udc);

	pthread_t th;
	if (pthread_create(&th, NULL, ep0_thread, NULL) != 0) {
		perror("pthread_create");
		return 1;
	}
	run_server();

	fprintf(stderr, "hid: shutting down\n");
	g_enabled = 0;
	/* Close FunctionFS fds first so the kernel can tear the function
	 * down; unbinding the UDC while ep0 is open wedges configfs. */
	if (kbd_fd >= 0)
		close(kbd_fd);
	if (mouse_fd >= 0)
		close(mouse_fd);
	if (abs_fd >= 0)
		close(abs_fd);
	if (cons_fd >= 0)
		close(cons_fd);
	if (ep0_fd >= 0)
		close(ep0_fd);
	kbd_fd = mouse_fd = abs_fd = cons_fd = ep0_fd = -1;
	/* Do NOT unbind the UDC here: on this kernel, unbinding the ffs
	 * gadget after use can wedge configfs. configfs is reset on reboot. */
	return 0;
}
