/**
 * \file src/core/message_garbage.c
 * \author Lukas Hutak <lukas.hutak@cesnet.cz>
 * \brief Garbage message (source file)
 * \date 2016-2018
 */

/* Copyright (C) 2016-2018 CESNET, z.s.p.o.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name of the Company nor the names of its contributors
 *    may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
 *
 * ALTERNATIVELY, provided that this notice is retained in full, this
 * product may be distributed under the terms of the GNU General Public
 * License (GPL) version 2 or later, in which case the provisions
 * of the GPL apply INSTEAD OF those given above.
 *
 * This software is provided ``as is'', and any express or implied
 * warranties, including, but not limited to, the implied warranties of
 * merchantability and fitness for a particular purpose are disclaimed.
 * In no event shall the company or contributors be liable for any
 * direct, indirect, incidental, special, exemplary, or consequential
 * damages (including, but not limited to, procurement of substitute
 * goods or services; loss of use, data, or profits; or business
 * interruption) however caused and on any theory of liability, whether
 * in contract, strict liability, or tort (including negligence or
 * otherwise) arising in any way out of the use of this software, even
 * if advised of the possibility of such damage.
 *
 */

#include <stddef.h>
#include <stdlib.h>
#include <assert.h>

#include <ipfixcol2.h>
#include "message_base.h"

/**
 * \internal
 * \addtogroup ipxGarbageMessage
 * @{
 */

/**
 * \brief Structure of a garbage message
 */
struct ipx_msg_garbage {
    /**
     * Identification of this message.
     * \warning This MUST be always first in this structure and the "type" MUST be #IPX_MSG_GARBAGE.
     */
    struct ipx_msg msg_header;

    /** Object to be destroyed     */
    void *object_ptr;
    /** Object destruction function */
    ipx_msg_garbage_cb object_destructor;
};

static_assert(offsetof(struct ipx_msg_garbage, msg_header.type) == 0,
    "Message header must be the first element of each IPFIXcol message.");

// Create a garbage message
ipx_msg_garbage_t *
ipx_msg_garbage_create(void *object, ipx_msg_garbage_cb callback)
{
    assert(callback != NULL);
    struct ipx_msg_garbage *msg;

    msg = malloc(sizeof(*msg));
    if (!msg) {
        return NULL;
    }

    ipx_msg_header_init(&msg->msg_header, IPX_MSG_GARBAGE);
    msg->object_ptr = object;
    msg->object_destructor = callback;
    return msg;
}

// Destroy a garbage message
void
ipx_msg_garbage_destroy(ipx_msg_garbage_t *msg)
{
    // Destroy garbage
    msg->object_destructor(msg->object_ptr);

    // Destroy message itself
    ipx_msg_header_destroy((ipx_msg_t *) msg);
    free(msg);
}

/**@}*/
