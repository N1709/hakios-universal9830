/* Temporary, root-only diagnostic for the native USB mux kernel socket path.
 * No host, credential or trust emulation: one read-only native QueryType.
 */
#include <netinet/in.h>
#include <sys/kpi_socket.h>
#include <sys/systm.h>
#include <sys/kauth.h>
#include <sys/sysctl.h>
#include <sys/uio.h>

static int usbmux_probe_io(socket_t so, void *data, size_t length, bool send) {
	struct iovec iov = {.iov_base = data, .iov_len = length};
	struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1};
	while (iov.iov_len != 0) {
		size_t done = 0;
		int error = send ? sock_send(so, &msg, 0, &done) : sock_receive(so, &msg, 0, &done);
		if (error != 0 || done == 0) {
			return error != 0 ? error : ECONNRESET;
		}
		iov.iov_base = (char *)iov.iov_base + done;
		iov.iov_len -= done;
	}
	return 0;
}

static int usbmux_probe_query SYSCTL_HANDLER_ARGS {
#pragma unused(oidp, arg1, arg2)
	const char query[] = "<?xml version=\"1.0\"?><plist version=\"1.0\"><dict>"
			     "<key>Label</key><string>Jesvi kernel transport probe</string>"
			     "<key>Request</key><string>QueryType</string></dict></plist>";
	struct sockaddr_in address = {.sin_len = sizeof(address),
				      .sin_family = AF_INET,
				      .sin_port = htons(62078),
				      .sin_addr.s_addr = htonl(0x7f000001)};
	struct timeval timeout = {.tv_sec = 2, .tv_usec = 0};
	socket_t so = NULL;
	uint32_t length = htonl(sizeof(query) - 1);
	char reply[2048];
	int error;
	if (!kauth_cred_issuser(kauth_cred_get())) {
		return EPERM;
	}
	if (req->oldptr == USER_ADDR_NULL) {
		return SYSCTL_OUT(req, NULL, sizeof(reply));
	}
	error = sock_socket(PF_INET, SOCK_STREAM, 0, NULL, NULL, &so);
	if (error != 0) {
		return error;
	}
	error = sock_setsockopt(so, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
	if (error == 0)
		error = sock_setsockopt(so, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
	if (error == 0)
		error = sock_connect(so, (struct sockaddr *)&address, MSG_DONTWAIT);
	if (error == EINPROGRESS)
		error = sock_connectwait(so, &timeout);
	if (error == 0)
		error = usbmux_probe_io(so, &length, sizeof(length), true);
	if (error == 0)
		error = usbmux_probe_io(so, (void *)(uintptr_t)query, sizeof(query) - 1, true);
	if (error == 0)
		error = usbmux_probe_io(so, &length, sizeof(length), false);
	length = ntohl(length);
	if (error == 0 && (length == 0 || length >= sizeof(reply)))
		error = EMSGSIZE;
	if (error == 0)
		error = usbmux_probe_io(so, reply, length, false);
	sock_close(so);
	if (error != 0)
		return error;
	reply[length] = 0;
	return SYSCTL_OUT(req, reply, length + 1);
}

SYSCTL_PROC(_kern, OID_AUTO, usb_lockdown_query, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, 0, 0, usbmux_probe_query,
	    "A", "Root-only native lockdown kernel transport probe");
