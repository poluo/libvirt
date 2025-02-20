/*
 * virnetmessage.c: basic RPC message encoding/decoding
 *
 * Copyright (C) 2010-2012 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

#include <config.h>

#include <unistd.h>

#include "virnetmessage.h"
#include "viralloc.h"
#include "virerror.h"
#include "virlog.h"
#include "virfile.h"
#include "virutil.h"
#include "virstring.h"

#define VIR_FROM_THIS VIR_FROM_RPC

VIR_LOG_INIT("rpc.netmessage");

virNetMessage *virNetMessageNew(bool tracked)
{
    virNetMessage *msg;

    msg = g_new0(virNetMessage, 1);

    msg->tracked = tracked;
    VIR_DEBUG("msg=%p tracked=%d", msg, tracked);

    return msg;
}


void
virNetMessageClearFDs(virNetMessage *msg)
{
    size_t i;

    for (i = 0; i < msg->nfds; i++)
        VIR_FORCE_CLOSE(msg->fds[i]);

    msg->donefds = 0;
    msg->nfds = 0;
    VIR_FREE(msg->fds);
}


void
virNetMessageClearPayload(virNetMessage *msg)
{
    virNetMessageClearFDs(msg);

    msg->bufferOffset = 0;
    msg->bufferLength = 0;
    VIR_FREE(msg->buffer);
}


void virNetMessageClear(virNetMessage *msg)
{
    bool tracked = msg->tracked;

    VIR_DEBUG("msg=%p nfds=%zu", msg, msg->nfds);

    virNetMessageClearPayload(msg);
    memset(msg, 0, sizeof(*msg));
    msg->tracked = tracked;
}


void virNetMessageFree(virNetMessage *msg)
{
    if (!msg)
        return;

    VIR_DEBUG("msg=%p nfds=%zu cb=%p", msg, msg->nfds, msg->cb);

    if (msg->cb)
        msg->cb(msg, msg->opaque);

    virNetMessageClearPayload(msg);
    g_free(msg);
}

void virNetMessageQueuePush(virNetMessage **queue, virNetMessage *msg)
{
    virNetMessage *tmp = *queue;

    if (tmp) {
        while (tmp->next)
            tmp = tmp->next;
        tmp->next = msg;
    } else {
        *queue = msg;
    }
}


virNetMessage *virNetMessageQueueServe(virNetMessage **queue)
{
    virNetMessage *tmp = *queue;

    if (tmp) {
        *queue = g_steal_pointer(&tmp->next);
    }

    return tmp;
}


int virNetMessageDecodeLength(virNetMessage *msg)
{
    XDR xdr;
    unsigned int len;
    int ret = -1;

    xdrmem_create(&xdr, msg->buffer,
                  msg->bufferLength, XDR_DECODE);
    if (!xdr_u_int(&xdr, &len)) {
        virReportError(VIR_ERR_RPC, "%s", _("Unable to decode message length"));
        goto cleanup;
    }
    msg->bufferOffset = xdr_getpos(&xdr);

    if (len < VIR_NET_MESSAGE_LEN_MAX) {
        virReportError(VIR_ERR_RPC,
                       _("packet %d bytes received from server too small, want %d"),
                       len, VIR_NET_MESSAGE_LEN_MAX);
        goto cleanup;
    }

    /* Length includes length word - adjust to real length to read. */
    len -= VIR_NET_MESSAGE_LEN_MAX;

    if (len > VIR_NET_MESSAGE_MAX) {
        virReportError(VIR_ERR_RPC,
                       _("packet %d bytes received from server too large, want %d"),
                       len, VIR_NET_MESSAGE_MAX);
        goto cleanup;
    }

    /* Extend our declared buffer length and carry
       on reading the header + payload */
    msg->bufferLength += len;
    VIR_REALLOC_N(msg->buffer, msg->bufferLength);

    VIR_DEBUG("Got length, now need %zu total (%u more)",
              msg->bufferLength, len);

    ret = 0;

 cleanup:
    xdr_destroy(&xdr);
    return ret;
}


/*
 * @msg: the complete incoming message, whose header to decode
 *
 * Decodes the header part of the message, but does not
 * validate the decoded fields in the header. It expects
 * bufferLength to refer to length of the data packet. Upon
 * return bufferOffset will refer to the amount of the packet
 * consumed by decoding of the header.
 *
 * returns 0 if successfully decoded, -1 upon fatal error
 */
int virNetMessageDecodeHeader(virNetMessage *msg)
{
    XDR xdr;
    int ret = -1;

    if (msg->bufferLength < VIR_NET_MESSAGE_LEN_MAX) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Unable to decode header until len is received"));
        return -1;
    }

    msg->bufferOffset = VIR_NET_MESSAGE_LEN_MAX;

    /* Parse the header. */
    xdrmem_create(&xdr,
                  msg->buffer + msg->bufferOffset,
                  msg->bufferLength - msg->bufferOffset,
                  XDR_DECODE);

    if (!xdr_virNetMessageHeader(&xdr, &msg->header)) {
        virReportError(VIR_ERR_RPC, "%s", _("Unable to decode message header"));
        goto cleanup;
    }

    msg->bufferOffset += xdr_getpos(&xdr);

    ret = 0;

 cleanup:
    xdr_destroy(&xdr);
    return ret;
}


/*
 * @msg: the outgoing message, whose header to encode
 *
 * Encodes the length word and header of the message, setting the
 * message offset ready to encode the payload. Leaves space
 * for the length field later. Upon return bufferLength will
 * refer to the total available space for message, while
 * bufferOffset will refer to current space used by header
 *
 * returns 0 if successfully encoded, -1 upon fatal error
 */
int virNetMessageEncodeHeader(virNetMessage *msg)
{
    XDR xdr;
    int ret = -1;
    unsigned int len = 0;

    msg->bufferLength = VIR_NET_MESSAGE_INITIAL + VIR_NET_MESSAGE_LEN_MAX;
    VIR_REALLOC_N(msg->buffer, msg->bufferLength);
    msg->bufferOffset = 0;

    /* Format the header. */
    xdrmem_create(&xdr,
                  msg->buffer,
                  msg->bufferLength,
                  XDR_ENCODE);

    /* The real value is filled in shortly */
    if (!xdr_u_int(&xdr, &len)) {
        virReportError(VIR_ERR_RPC, "%s", _("Unable to encode message length"));
        goto cleanup;
    }

    if (!xdr_virNetMessageHeader(&xdr, &msg->header)) {
        virReportError(VIR_ERR_RPC, "%s", _("Unable to encode message header"));
        goto cleanup;
    }

    len = xdr_getpos(&xdr);
    xdr_setpos(&xdr, 0);

    /* Fill in current length - may be re-written later
     * if a payload is added
     */
    if (!xdr_u_int(&xdr, &len)) {
        virReportError(VIR_ERR_RPC, "%s", _("Unable to re-encode message length"));
        goto cleanup;
    }

    msg->bufferOffset += len;

    ret = 0;

 cleanup:
    xdr_destroy(&xdr);
    return ret;
}


int virNetMessageEncodeNumFDs(virNetMessage *msg)
{
    XDR xdr;
    unsigned int numFDs = msg->nfds;
    int ret = -1;

    xdrmem_create(&xdr, msg->buffer + msg->bufferOffset,
                  msg->bufferLength - msg->bufferOffset, XDR_ENCODE);

    if (numFDs > VIR_NET_MESSAGE_NUM_FDS_MAX) {
        virReportError(VIR_ERR_RPC,
                       _("Too many FDs to send %d, expected %d maximum"),
                       numFDs, VIR_NET_MESSAGE_NUM_FDS_MAX);
        goto cleanup;
    }

    if (!xdr_u_int(&xdr, &numFDs)) {
        virReportError(VIR_ERR_RPC, "%s", _("Unable to encode number of FDs"));
        goto cleanup;
    }
    msg->bufferOffset += xdr_getpos(&xdr);

    VIR_DEBUG("Send %zu FDs to peer", msg->nfds);

    ret = 0;

 cleanup:
    xdr_destroy(&xdr);
    return ret;
}


int virNetMessageDecodeNumFDs(virNetMessage *msg)
{
    XDR xdr;
    unsigned int numFDs;
    int ret = -1;
    size_t i;

    xdrmem_create(&xdr, msg->buffer + msg->bufferOffset,
                  msg->bufferLength - msg->bufferOffset, XDR_DECODE);
    if (!xdr_u_int(&xdr, &numFDs)) {
        virReportError(VIR_ERR_RPC, "%s", _("Unable to decode number of FDs"));
        goto cleanup;
    }
    msg->bufferOffset += xdr_getpos(&xdr);

    if (numFDs > VIR_NET_MESSAGE_NUM_FDS_MAX) {
        virReportError(VIR_ERR_RPC,
                       _("Received too many FDs %d, expected %d maximum"),
                       numFDs, VIR_NET_MESSAGE_NUM_FDS_MAX);
        goto cleanup;
    }

    if (msg->nfds == 0) {
        msg->nfds = numFDs;
        msg->fds = g_new0(int, msg->nfds);

        for (i = 0; i < msg->nfds; i++)
            msg->fds[i] = -1;
    }

    VIR_DEBUG("Got %zu FDs from peer", msg->nfds);

    ret = 0;

 cleanup:
    xdr_destroy(&xdr);
    return ret;
}


int virNetMessageEncodePayload(virNetMessage *msg,
                               xdrproc_t filter,
                               void *data)
{
    XDR xdr;
    unsigned int msglen;

    /* Serialise payload of the message. This assumes that
     * virNetMessageEncodeHeader has already been run, so
     * just appends to that data */
    xdrmem_create(&xdr, msg->buffer + msg->bufferOffset,
                  msg->bufferLength - msg->bufferOffset, XDR_ENCODE);

    /* Try to encode the payload. If the buffer is too small increase it. */
    while (!(*filter)(&xdr, data, 0)) {
        unsigned int newlen = msg->bufferLength - VIR_NET_MESSAGE_LEN_MAX;
        newlen *= 2;

        if (newlen > VIR_NET_MESSAGE_MAX) {
            virReportError(VIR_ERR_RPC, "%s", _("Unable to encode message payload"));
            goto error;
        }

        xdr_destroy(&xdr);

        msg->bufferLength = newlen + VIR_NET_MESSAGE_LEN_MAX;

        VIR_REALLOC_N(msg->buffer, msg->bufferLength);

        xdrmem_create(&xdr, msg->buffer + msg->bufferOffset,
                      msg->bufferLength - msg->bufferOffset, XDR_ENCODE);

        VIR_DEBUG("Increased message buffer length = %zu", msg->bufferLength);
    }

    /* Get the length stored in buffer. */
    msg->bufferOffset += xdr_getpos(&xdr);
    xdr_destroy(&xdr);

    /* Re-encode the length word. */
    VIR_DEBUG("Encode length as %zu", msg->bufferOffset);
    xdrmem_create(&xdr, msg->buffer, VIR_NET_MESSAGE_HEADER_XDR_LEN, XDR_ENCODE);
    msglen = msg->bufferOffset;
    if (!xdr_u_int(&xdr, &msglen)) {
        virReportError(VIR_ERR_RPC, "%s", _("Unable to encode message length"));
        goto error;
    }
    xdr_destroy(&xdr);

    msg->bufferLength = msg->bufferOffset;
    msg->bufferOffset = 0;
    return 0;

 error:
    xdr_destroy(&xdr);
    return -1;
}


int virNetMessageDecodePayload(virNetMessage *msg,
                               xdrproc_t filter,
                               void *data)
{
    XDR xdr;

    /* Deserialise payload of the message. This assumes that
     * virNetMessageDecodeHeader has already been run, so
     * just start from after that data */
    xdrmem_create(&xdr, msg->buffer + msg->bufferOffset,
                  msg->bufferLength - msg->bufferOffset, XDR_DECODE);

    if (!(*filter)(&xdr, data, 0)) {
        virReportError(VIR_ERR_RPC, "%s", _("Unable to decode message payload"));
        goto error;
    }

    /* Get the length stored in buffer. */
    msg->bufferLength += xdr_getpos(&xdr);
    xdr_destroy(&xdr);
    return 0;

 error:
    xdr_destroy(&xdr);
    return -1;
}


/**
 * virNetMessageEncodePayloadRaw:
 * @msg: message to encode payload into
 * @data: data to encode into @msg
 * @len: lenght of @data
 *
 * Encodes message payload. If @data is NULL or @len is 0 an empty message is
 * encoded.
 */
int virNetMessageEncodePayloadRaw(virNetMessage *msg,
                                  const char *data,
                                  size_t len)
{
    XDR xdr;
    unsigned int msglen;

    if (data && len > 0) {
        /* If the message buffer is too small for the payload increase it accordingly. */
        if ((msg->bufferLength - msg->bufferOffset) < len) {
            if ((msg->bufferOffset + len) >
                (VIR_NET_MESSAGE_MAX + VIR_NET_MESSAGE_LEN_MAX)) {
                virReportError(VIR_ERR_RPC,
                               _("Stream data too long to send "
                                 "(%zu bytes needed, %zu bytes available)"),
                               len,
                               VIR_NET_MESSAGE_MAX +
                               VIR_NET_MESSAGE_LEN_MAX -
                               msg->bufferOffset);
                return -1;
            }

            msg->bufferLength = msg->bufferOffset + len;

            VIR_REALLOC_N(msg->buffer, msg->bufferLength);

            VIR_DEBUG("Increased message buffer length = %zu", msg->bufferLength);
        }

        memcpy(msg->buffer + msg->bufferOffset, data, len);
        msg->bufferOffset += len;
    }

    /* Re-encode the length word. */
    VIR_DEBUG("Encode length as %zu", msg->bufferOffset);
    xdrmem_create(&xdr, msg->buffer, VIR_NET_MESSAGE_HEADER_XDR_LEN, XDR_ENCODE);
    msglen = msg->bufferOffset;
    if (!xdr_u_int(&xdr, &msglen)) {
        virReportError(VIR_ERR_RPC, "%s", _("Unable to encode message length"));
        goto error;
    }
    xdr_destroy(&xdr);

    msg->bufferLength = msg->bufferOffset;
    msg->bufferOffset = 0;
    return 0;

 error:
    xdr_destroy(&xdr);
    return -1;
}


void virNetMessageSaveError(struct virNetMessageError *rerr)
{
    virErrorPtr verr;

    /* This func may be called several times & the first
     * error is the one we want because we don't want
     * cleanup code overwriting the first one.
     */
    if (rerr->code != VIR_ERR_OK)
        return;

    memset(rerr, 0, sizeof(*rerr));
    verr = virGetLastError();
    if (verr) {
        rerr->code = verr->code;
        rerr->domain = verr->domain;
        if (verr->message) {
            rerr->message = g_new0(char *, 1);
            *rerr->message = g_strdup(verr->message);
        }
        rerr->level = verr->level;
        if (verr->str1) {
            rerr->str1 = g_new0(char *, 1);
            *rerr->str1 = g_strdup(verr->str1);
        }
        if (verr->str2) {
            rerr->str2 = g_new0(char *, 1);
            *rerr->str2 = g_strdup(verr->str2);
        }
        if (verr->str3) {
            rerr->str3 = g_new0(char *, 1);
            *rerr->str3 = g_strdup(verr->str3);
        }
        rerr->int1 = verr->int1;
        rerr->int2 = verr->int2;
    } else {
        rerr->code = VIR_ERR_INTERNAL_ERROR;
        rerr->domain = VIR_FROM_RPC;
        rerr->message = g_new0(virNetMessageNonnullString, 1);
        *rerr->message = g_strdup(_("Library function returned error but did not set virError"));
        rerr->level = VIR_ERR_ERROR;
    }
}


int virNetMessageDupFD(virNetMessage *msg,
                       size_t slot)
{
    int fd;

    if (slot >= msg->nfds) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("No FD available at slot %zu"), slot);
        return -1;
    }

    if ((fd = dup(msg->fds[slot])) < 0) {
        virReportSystemError(errno,
                             _("Unable to duplicate FD %d"),
                             msg->fds[slot]);
        return -1;
    }
    if (virSetInherit(fd, false) < 0) {
        VIR_FORCE_CLOSE(fd);
        virReportSystemError(errno,
                             _("Cannot set close-on-exec %d"),
                             fd);
        return -1;
    }
    return fd;
}

int virNetMessageAddFD(virNetMessage *msg,
                       int fd)
{
    int newfd = -1;

    if ((newfd = dup(fd)) < 0) {
        virReportSystemError(errno,
                             _("Unable to duplicate FD %d"),
                             fd);
        goto error;
    }

    if (virSetInherit(newfd, false) < 0) {
        virReportSystemError(errno,
                             _("Cannot set close-on-exec %d"),
                             newfd);
        goto error;
    }
    VIR_APPEND_ELEMENT(msg->fds, msg->nfds, newfd);
    return 0;
 error:
    VIR_FORCE_CLOSE(newfd);
    return -1;
}
