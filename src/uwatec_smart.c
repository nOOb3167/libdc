#include <stdlib.h> // malloc, free
#include <string.h>	// strncmp, strstr
#include <time.h>	// time, strftime
#include <assert.h>	// assert

#include "device-private.h"
#include "uwatec_smart.h"
#include "irda.h"
#include "utils.h"

#define WARNING(expr) \
{ \
	message ("%s:%d: %s\n", __FILE__, __LINE__, expr); \
}

#define EXITCODE(rc) \
( \
	rc == -1 ? DEVICE_STATUS_IO : DEVICE_STATUS_TIMEOUT \
)

typedef struct uwatec_smart_device_t uwatec_smart_device_t;

struct uwatec_smart_device_t {
	device_t base;
	struct irda *socket;
	unsigned int address;
	unsigned int timestamp;
};

static const device_backend_t uwatec_smart_device_backend;

static int
device_is_uwatec_smart (device_t *abstract)
{
	if (abstract == NULL)
		return 0;

    return abstract->backend == &uwatec_smart_device_backend;
}


static void
uwatec_smart_discovery (unsigned int address, const char *name, unsigned int charset, unsigned int hints, void *userdata)
{
	message ("device: address=%08x, name=%s, charset=%02x, hints=%04x\n", address, name, charset, hints);

	uwatec_smart_device_t *device = (uwatec_smart_device_t*) userdata;
	if (device == NULL)
		return;

	if (strncmp (name, "UWATEC Galileo Sol", 18) == 0 ||
		strncmp (name, "Uwatec Smart", 12) == 0 ||
		strstr (name, "Uwatec") != NULL ||
		strstr (name, "UWATEC") != NULL ||
		strstr (name, "Aladin") != NULL ||
		strstr (name, "ALADIN") != NULL ||
		strstr (name, "Smart") != NULL ||
		strstr (name, "SMART") != NULL ||
		strstr (name, "Galileo") != NULL ||
		strstr (name, "GALILEO") != NULL) {
		message ("Found an Uwatec dive computer.\n");
		device->address = address;
	}
}


device_status_t
uwatec_smart_device_open (device_t **out)
{
	if (out == NULL)
		return DEVICE_STATUS_ERROR;

	// Allocate memory.
	uwatec_smart_device_t *device = malloc (sizeof (uwatec_smart_device_t));
	if (device == NULL) {
		WARNING ("Failed to allocate memory.");
		return DEVICE_STATUS_MEMORY;
	}

	// Initialize the base class.
	device_init (&device->base, &uwatec_smart_device_backend);

	// Set the default values.
	device->socket = NULL;
	device->address = 0;
	device->timestamp = 0;

	irda_init ();

	// Open the irda socket.
	int rc = irda_socket_open (&device->socket);
	if (rc == -1) {
		WARNING ("Failed to open the irda socket.");
		irda_cleanup ();
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Discover the device.
	rc = irda_socket_discover (device->socket, uwatec_smart_discovery, device);
	if (rc == -1) {
		WARNING ("Failed to discover the device.");
		irda_socket_close (device->socket);
		irda_cleanup ();
		free (device);
		return DEVICE_STATUS_IO;
	}

	if (device->address == 0) {
		WARNING ("No dive computer found.");
		irda_socket_close (device->socket);
		irda_cleanup ();
		free (device);
		return DEVICE_STATUS_ERROR;
	}

	// Connect the device.
	rc = irda_socket_connect_lsap (device->socket, device->address, 1);
	if (rc == -1) {
		WARNING ("Failed to connect the device.");
		irda_socket_close (device->socket);
		irda_cleanup ();
		free (device);
		return DEVICE_STATUS_IO;
	}

	*out = (device_t*) device;

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
uwatec_smart_device_close (device_t *abstract)
{
	uwatec_smart_device_t *device = (uwatec_smart_device_t*) abstract;

	if (! device_is_uwatec_smart (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	// Close the device.
	if (irda_socket_close (device->socket) == -1) {
		irda_cleanup ();
		free (device);
		return DEVICE_STATUS_IO;
	}

	irda_cleanup ();

	// Free memory.	
	free (device);

	return DEVICE_STATUS_SUCCESS;
}


device_status_t
uwatec_smart_device_set_timestamp (device_t *abstract, unsigned int timestamp)
{
	uwatec_smart_device_t *device = (uwatec_smart_device_t*) abstract;

	if (! device_is_uwatec_smart (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	device->timestamp = timestamp;

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
uwatec_smart_transfer (uwatec_smart_device_t *device, const unsigned char command[], unsigned int csize, unsigned char answer[], unsigned int asize)
{
	int rc = irda_socket_write (device->socket, command, csize);
	if (rc != csize) {
		WARNING ("Failed to send the command.");
		return EXITCODE (rc);
	}

	rc = irda_socket_read (device->socket, answer, asize);
	if (rc != asize) {
		WARNING ("Failed to receive the answer.");
		return EXITCODE (rc);
	}

	return DEVICE_STATUS_SUCCESS;
}


device_status_t
uwatec_smart_device_handshake (device_t *abstract)
{
	uwatec_smart_device_t *device = (uwatec_smart_device_t*) abstract;

	if (! device_is_uwatec_smart (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	unsigned char command[5] = {0};
	unsigned char answer[1] = {0};

	// Handshake (stage 1).

	command[0] = 0x1B;

	int rc = uwatec_smart_transfer (device, command, 1, answer, 1);
	if (rc != DEVICE_STATUS_SUCCESS)
		return rc;

	message ("handshake: header=%02x\n", answer[0]);

	if (answer[0] != 0x01) {
		WARNING ("Unexpected answer byte(s).");
		return DEVICE_STATUS_PROTOCOL;
	}

	// Handshake (stage 2).

	command[0] = 0x1C;
	command[1] = 0x10;
	command[2] = 0x27;
	command[3] = 0;
	command[4] = 0;

	rc = uwatec_smart_transfer (device, command, 5, answer, 1);
	if (rc != DEVICE_STATUS_SUCCESS)
		return rc;

	message ("handshake: header=%02x\n", answer[0]);

	if (answer[0] != 0x01) {
		WARNING ("Unexpected answer byte(s).");
		return DEVICE_STATUS_PROTOCOL;
	}
	
	return DEVICE_STATUS_SUCCESS;
}


device_status_t
uwatec_smart_device_version (device_t *abstract, unsigned char data[], unsigned int size)
{
	uwatec_smart_device_t *device = (uwatec_smart_device_t*) abstract;

	if (! device_is_uwatec_smart (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	unsigned char command[1] = {0};
	unsigned char answer[UWATEC_SMART_VERSION_SIZE] = {0};

	// Dive Computer Time.

	command[0] = 0x1A;

	int rc = uwatec_smart_transfer (device, command, 1, answer + 0, 4);
	if (rc != DEVICE_STATUS_SUCCESS)
		return rc;

	time_t device_time = answer[0] + (answer[1] << 8) + 
						(answer[2] << 16) + (answer[3] << 24);
	message ("handshake: timestamp=0x%08x\n", device_time);

	// PC Time and Time Correction.

	time_t now = time (NULL);
	char datetime[21] = {0};
	strftime (datetime, sizeof (datetime), "%Y-%m-%dT%H:%M:%SZ", gmtime (&now));
	message ("handshake: now=%lu (%s)\n", (unsigned long)now, datetime);

	// Serial Number

	command[0] = 0x14;

	rc = uwatec_smart_transfer (device, command, 1, answer + 4, 4);
	if (rc != DEVICE_STATUS_SUCCESS)
		return rc;

	unsigned int serial = answer[4] + (answer[5] << 8) + 
							(answer[6] << 16) + (answer[7] << 24);
	message ("handshake: serial=0x%08x\n", serial);

	// Dive Computer Model.

	command[0] = 0x10;

	rc = uwatec_smart_transfer (device, command, 1, answer + 8, 1);
	if (rc != DEVICE_STATUS_SUCCESS)
		return rc;

	message ("handshake: model=0x%02x\n", answer[8]);

	if (size >= UWATEC_SMART_VERSION_SIZE) {
		memcpy (data, answer, UWATEC_SMART_VERSION_SIZE);
	} else {
		WARNING ("Insufficient buffer space available.");
		return DEVICE_STATUS_MEMORY;
	}

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
uwatec_smart_dump (uwatec_smart_device_t *device, unsigned char *data[], unsigned int *size)
{
	unsigned char command[9] = {0};
	unsigned char answer[4] = {0};

	// Data Length.

	command[0] = 0xC6;
	command[1] = (device->timestamp      ) & 0xFF;
	command[2] = (device->timestamp >> 8 ) & 0xFF;
	command[3] = (device->timestamp >> 16) & 0xFF;
	command[4] = (device->timestamp >> 24) & 0xFF;
	command[5] = 0x10;
	command[6] = 0x27;
	command[7] = 0;
	command[8] = 0;

	int rc = uwatec_smart_transfer (device, command, 9, answer, 4);
	if (rc != DEVICE_STATUS_SUCCESS)
		return rc;

	unsigned int length = answer[0] + (answer[1] << 8) + 
						(answer[2] << 16) + (answer[3] << 24);
	message ("handshake: length=%u\n", length);

  	if (length == 0)
  		return DEVICE_STATUS_SUCCESS;

	unsigned char *package = malloc (length * sizeof (unsigned char));
	if (package == NULL) {
		WARNING ("Memory allocation error.");
		return DEVICE_STATUS_MEMORY;
	}

	// Data.

	command[0] = 0xC4;
	command[1] = (device->timestamp      ) & 0xFF;
	command[2] = (device->timestamp >> 8 ) & 0xFF;
	command[3] = (device->timestamp >> 16) & 0xFF;
	command[4] = (device->timestamp >> 24) & 0xFF;
	command[5] = 0x10;
	command[6] = 0x27;
	command[7] = 0;
	command[8] = 0;

	rc = uwatec_smart_transfer (device, command, 9, answer, 4);
	if (rc != DEVICE_STATUS_SUCCESS) {
		free (package);
		return rc;
	}

	unsigned int total = answer[0] + (answer[1] << 8) + 
						(answer[2] << 16) + (answer[3] << 24);
	message ("handshake: total=%u\n", total);

	assert (total == length + 4);

	unsigned int nbytes = 0;
	while (nbytes < length) {
		unsigned int len = length - nbytes;
		if (len > 32)
			len = 32;
		rc = irda_socket_read (device->socket, package + nbytes, len);
		if (rc < 0) {
			WARNING ("Failed to receive the answer.");
			free (package);
			return EXITCODE (rc);
		}
		nbytes += rc;
		message ("len=%u, rc=%i, nbytes=%u\n", len, rc, nbytes);
	}

	*data = package;
	*size = length;

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
uwatec_smart_device_dump (device_t *abstract, unsigned char data[], unsigned int size)
{
	uwatec_smart_device_t *device = (uwatec_smart_device_t*) abstract;

	if (! device_is_uwatec_smart (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	unsigned int length = 0;
	unsigned char *buffer = NULL;
	int rc = uwatec_smart_dump (device, &buffer, &length);
	if (rc != DEVICE_STATUS_SUCCESS)
		return rc;

	if (length <= size) {
		memcpy (data, buffer, length);
	} else {
		WARNING ("Insufficient buffer space available.");
		free (buffer); 
		return DEVICE_STATUS_MEMORY;
	}

	free (buffer);

	return length;
}


static device_status_t
uwatec_smart_device_foreach (device_t *abstract, dive_callback_t callback, void *userdata)
{
	uwatec_smart_device_t *device = (uwatec_smart_device_t*) abstract;

	if (! device_is_uwatec_smart (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	unsigned int length = 0;
	unsigned char *buffer = NULL;
	int rc = uwatec_smart_dump (device, &buffer, &length);
	if (rc != DEVICE_STATUS_SUCCESS)
		return rc;

	rc = uwatec_smart_extract_dives (buffer, length, callback, userdata);
	if (rc != DEVICE_STATUS_SUCCESS) {
		free (buffer);
		return rc;
	}

	free (buffer);

	return DEVICE_STATUS_SUCCESS;
}


device_status_t
uwatec_smart_extract_dives (const unsigned char data[], unsigned int size, dive_callback_t callback, void *userdata)
{
	const unsigned char header[4] = {0xa5, 0xa5, 0x5a, 0x5a};

	// Search the data stream for start markers.
	unsigned int previous = size;
	unsigned int current = (size >= 4 ? size - 4 : 0);
	while (current > 0) {
		current--;
		if (memcmp (data + current, header, sizeof (header)) == 0) {
			// Get the length of the profile data.
			unsigned int len = data[current + 4] + (data[current + 5] << 8) + 
							(data[current + 6] << 16) + (data[current + 7] << 24);

			// Check for a buffer overflow.
			if (current + len > previous)
				return DEVICE_STATUS_ERROR;

			if (callback && !callback (data + current, len, userdata))
				return DEVICE_STATUS_SUCCESS;

			// Prepare for the next dive.
			previous = current;
			current = (current >= 4 ? current - 4 : 0);
		}
	}

	return DEVICE_STATUS_SUCCESS;
}


static const device_backend_t uwatec_smart_device_backend = {
	DEVICE_TYPE_UWATEC_SMART,
	NULL, /* handshake */
	NULL, /* version */
	NULL, /* read */
	NULL, /* write */
	uwatec_smart_device_dump, /* dump */
	uwatec_smart_device_foreach, /* foreach */
	uwatec_smart_device_close /* close */
};