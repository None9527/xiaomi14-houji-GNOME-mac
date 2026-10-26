/*
 * xs-files: file transfers over the client connection (XS_MSG_FILE_* /
 * XS_MSG_CLIENT_FILE_*). The sender walks files and folders on its own
 * thread; the receiver writes them below a destination folder.
 */
#pragma once

#include <glib.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../../protocol/xs-protocol.h"

/* Writes one message; called from the sender thread. */
typedef void (*XsFilesSendFunc) (uint16_t type, const void *head, size_t head_size,
                                 const void *body, size_t body_size, void *user_data);

typedef struct _XsFileSender XsFileSender;

XsFileSender *xs_file_sender_new (XsFilesSendFunc send, void *user_data);
/* Stops a transfer in progress (the receiver gets an abandoned END). */
void xs_file_sender_free (XsFileSender *sender);

/* Queues local `paths` (files or folders). Fails when they add up to more
 * than `max_bytes` (0: no limit). */
bool xs_file_sender_send (XsFileSender *sender, const char *const *paths,
                          uint32_t purpose, uint64_t max_bytes, GError **error);

typedef struct _XsFileReceiver XsFileReceiver;

/* Called on the main context when a transfer completes: the top-level
 * entries received, as absolute paths. */
typedef void (*XsFilesReceivedFunc) (uint32_t purpose, const char *const *paths,
                                     void *user_data);

XsFileReceiver *xs_file_receiver_new (XsFilesReceivedFunc received, void *user_data);
/* Abandons a transfer in progress, removing what it wrote. */
void xs_file_receiver_free (XsFileReceiver *receiver);

/* The payload of an XS_MSG_CLIENT_FILE_* message; false on a protocol error. */
bool xs_file_receiver_handle (XsFileReceiver *receiver, uint16_t type,
                              const uint8_t *payload, size_t size);
