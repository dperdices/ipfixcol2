/**
 * \file src/core/parser_plugin.c
 * \author Lukas Hutak <lukas.hutak@cesnet.cz>
 * \brief Internal parser plugin (source file)
 * \date 2018
 */

/* Copyright (C) 2018 CESNET, z.s.p.o.
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

#include "fpipe.h"
#include "context.h"
#include "parser_plugin.h"
#include "parser.h"

int
parser_plugin_init(ipx_ctx_t *ctx, const char *params)
{
    (void) params; // Not used

    // Subscribe to receive IPFIX and Session messages
    const uint16_t mask = IPX_MSG_IPFIX | IPX_MSG_SESSION;
    if (ipx_ctx_subscribe(ctx, &mask, NULL) != IPX_OK) {
        IPX_CTX_ERROR(ctx, "Failed to subscribe to receive IPFIX and Transport Session Messages.",
            NULL);
        return IPX_ERR_ARG;
    }

    // Create a parser
    ipx_parser_t *parser = ipx_parser_create(ctx);
    if (!parser) {
        IPX_CTX_ERROR(ctx, "Failed to create a parser of IPFIX Messages!", NULL);
        return IPX_ERR_NOMEM;
    }

    ipx_ctx_private_set(ctx, parser);
    return IPX_OK;
}

void
parser_plugin_destroy(ipx_ctx_t *ctx, void *cfg)
{
    ipx_parser_t *parser = (ipx_parser_t *) cfg;

    // Create a garbage message
    ipx_msg_garbage_cb cb = (ipx_msg_garbage_cb) &ipx_parser_destroy;
    ipx_msg_garbage_t *garbage = ipx_msg_garbage_create(parser, cb);
    if (!garbage) {
        /* Failed to create a message
         * Unfortunately, we can't destroy the parser because its (Options) Template can be still
         * references by earlier IPFIX Messages -> memory leak
         */
        return;
    }

    if (ipx_ctx_msg_pass(ctx, ipx_msg_garbage2base(garbage)) != IPX_OK) {
        IPX_CTX_ERROR(ctx, "Failed to pass a garbage message with processor!", NULL);
    }
}

/**
 * \brief Process Transport Session event message
 *
 * If the event is of close type, information about the particular Transport Session will be
 * removed, i.e. all template managers and counters of sequence numbers.
 * \param[in] ctx    Plugin context
 * \param[in] parser IPFIX Message parser
 * \param[in] msg    Transport Session message
 * \return Always #IPX_OK
 */
static inline int
parser_plugin_process_session(ipx_ctx_t *ctx, ipx_parser_t *parser, ipx_msg_session_t *msg)
{
    if (ipx_msg_session_get_event(msg) != IPX_MSG_SESSION_CLOSE) {
        // Ignore non-close events
        return IPX_OK;
    }

    int rc;
    const struct ipx_session *session = ipx_msg_session_get_session(msg);

    ipx_msg_garbage_t *g_msg;
    if ((rc = ipx_parser_session_remove(parser, session, &g_msg)) == IPX_OK) {
        // Send garbage
        if (g_msg == NULL) {
            IPX_CTX_WARNING(ctx, "A memory allocation failed (%s:%d).", __FILE__, __LINE__);
            return IPX_OK;
        }

        ipx_ctx_msg_pass(ctx, ipx_msg_garbage2base(g_msg));
        return IPX_OK;
    }

    switch (rc) {
    case IPX_ERR_NOTFOUND:
        IPX_CTX_WARNING(ctx, "Received a request to close unknown Transport Session '%s'.",
            session->ident);
        break;
    default:
        IPX_CTX_ERROR(ctx, "ipx_parser_session_remove() returned an unexpected value (%s:%d, "
            "CODE: %d).", __FILE__, __LINE__, rc);
        break;
    }

    return IPX_OK;
}

/**
 * \brief Hard remove of a Transport Session (TS)
 *
 * This function should be called when the TS sends malformed messages or when an internal error
 * has occurred and a parser is not able to process IPFIX Messages of the TS anymore.
 * After calling this function, the Session is removed from the parser (if an Input plugin doesn't
 * support feedback) or blocked until connection is closed (if an Input plugin supports feedback).
 *
 * \warning Plugin context MUST be able to pass messages!
 * \param[in] ctx    Plugin context
 * \param[in] parser Parser
 * \param[in] ts     Transport Session to remove
 * \return #IPX_OK on success
 * \return #IPX_ERR_ARG in case of fatal internal error
 */
static inline int
parser_plugin_remove_session(ipx_ctx_t *ctx, ipx_parser_t *parser, const struct ipx_session *ts)
{
    int rc;
    ipx_msg_garbage_t *garbage;

    // Try to send request to close the Transport Session
    ipx_fpipe_t *feedback = ipx_ctx_fpipe_get(ctx);
    if (!feedback) {
        // Feedback not available -> hard remove!
        IPX_CTX_WARNING(ctx, "Unable to send a request to close a Transport Session '%s' "
                "(not supported by the input plugin). Removing all internal info about the session!",
            ts->ident);

        rc = ipx_parser_session_remove(parser, ts, &garbage);
        if (rc == IPX_OK && garbage != NULL) {
            ipx_ctx_msg_pass(ctx, ipx_msg_garbage2base(garbage));
        }

        return IPX_OK;
    }

    // Block the Transport Session and send request
    ipx_parser_session_block(parser, ts);
    if (ipx_fpipe_write(feedback, ts) != IPX_OK) {
        IPX_CTX_ERROR(ctx, "Due to a fatal internal error the parser cannot continue!", NULL);
        return IPX_ERR_ARG;
    }

    return IPX_OK;
}

/**
 * \brief Process IPFIX Message
 *
 * Iterate over all IPFIX Sets in the Message and process templates and add references to
 * Data records. The function takes care of passing messages to the next plugin. However, only
 * successfully parsed messages are passed to the next plugin. Other messages are dropped.
 *
 * In case of any error (malformed message, memory allocation error, etc), tries to send a request
 * to close the Transport Session. If this feature is not available, information about the session
 * is will be removed. Because the UDP Transport Session by its nature doesn't support any
 * feedback, formatting errors are ignored by, for example, removing (Options) Templates that
 * caused parsing errors, etc.
 *
 * \param[in] ctx    Plugin context
 * \param[in] parser IPFIX Message parser
 * \param[in] ipfix  IPFIX Message
 * \return #IPX_OK on success or on non-fatal failure
 * \return #IPX_ERR_ARG on a fatal failure
 */
static inline int
parser_plugin_process_ipfix(ipx_ctx_t *ctx, ipx_parser_t *parser, ipx_msg_ipfix_t *ipfix)
{
    int rc;
    ipx_msg_garbage_t *garbage;

    if ((rc = ipx_parser_process(parser, &ipfix, &garbage)) == IPX_OK) {
        // Everything is fine, pass the message(s)
        ipx_ctx_msg_pass(ctx, ipx_msg_ipfix2base(ipfix));

        if (garbage != NULL) {
            /* Garbage MUST be send after the IPFIX Message because the message can have
             * references to templates in this garbage message!
             */
            ipx_ctx_msg_pass(ctx, ipx_msg_garbage2base(garbage));
        }
        return IPX_OK;
    }

    if (rc == IPX_ERR_DENIED) {
        // Due to previous failures, connection to the session is blocked
        ipx_msg_ipfix_destroy(ipfix);
        return IPX_OK;
    }

    // Something bad happened -> try to close the Transport Session
    const struct ipx_msg_ctx *msg_ctx = ipx_msg_ipfix_get_ctx(ipfix);
    if (rc == IPX_ERR_FORMAT && msg_ctx->session->type == FDS_SESSION_UDP) {
        // In case of UDP and malformed message, just drop the message
        ipx_msg_ipfix_destroy(ipfix);
        return IPX_OK;
    }

    // Try to send request to close the Transport Session or remove it
    rc = parser_plugin_remove_session(ctx, parser, msg_ctx->session);
    ipx_msg_ipfix_destroy(ipfix); // Note: msg_ctx is not available anymore!
    return rc;
}

int
parser_plugin_process(ipx_ctx_t *ctx, void *cfg, ipx_msg_t *msg)
{
    int rc;
    ipx_parser_t *parser = (ipx_parser_t *) cfg;

    switch (ipx_msg_get_type(msg)) {
    case IPX_MSG_IPFIX:
        // Process IPFIX Message
        rc = parser_plugin_process_ipfix(ctx, parser, ipx_msg_base2ipfix(msg));
        break;
    case IPX_MSG_SESSION:
        // Process Transport Session
        rc = parser_plugin_process_session(ctx, parser, ipx_msg_base2session(msg));
        ipx_ctx_msg_pass(ctx, msg);
        break;
    default:
        // Unexpected type of the message
        IPX_CTX_WARNING(ctx, "Received unexpected type of internal message. Skipping...", NULL);
        ipx_ctx_msg_pass(ctx, msg);
        rc = IPX_OK;
        break;
    }

    if (rc != IPX_OK) {
        // Unrecoverable error
        return IPX_ERR_NOMEM;
    }

    return IPX_OK;
}

int
parser_plugin_update_prepare(ipx_ctx_t *ctx, void *cfg, uint16_t what, const char *params)
{
    (void) ctx;
    (void) cfg;
    (void) params;

    if ((what & IPX_PU_IEMGR) == 0) {
        // Nothing to update
        return IPX_OK;
    }

    /* The elements will be replaced during commit because the processor can still receive
     * new (Options) Template Definitions, etc.
     */
    return IPX_READY;
}

/** Auxiliary structure for callback function */
struct parser_plugin_update_fail {
    /** Plugin context necessary for passing garbage messages */
    ipx_ctx_t *ctx;
    /** Status code of operators */
    int status;
};

/**
 * \brief Try to remove or block a Transport Session (auxiliary callback)
 * \param[in] parser Parser
 * \param[in] ts     Transport Session to remove
 * \param[in] data   Auxiliary data structure
 */
static void
parser_plugin_update_fail_cb(ipx_parser_t *parser, const struct ipx_session *ts, void *data)
{
    struct parser_plugin_update_fail *fail_data = data;
    if (fail_data->status != IPX_OK) {
        // Fatal failure
        return;
    }

    int rc = parser_plugin_remove_session(fail_data->ctx, parser, ts);
    if (rc != IPX_OK) {
        fail_data->status = rc;
    }
}

int
parser_plugin_update_commit(ipx_ctx_t *ctx, void *cfg, void *update)
{
    (void) update;
    ipx_parser_t *parser = (ipx_parser_t *) cfg;
    const fds_iemgr_t *iemgr = ipx_ctx_iemgr_get(ctx);

    // Redefine all IE managers
    ipx_msg_garbage_t *garbage;
    if (ipx_parser_ie_source(parser, iemgr, &garbage) == IPX_OK) {
        // Success
        if (garbage != NULL) {
            ipx_ctx_msg_pass(ctx, ipx_msg_garbage2base(garbage));
        }

        return IPX_OK;
    }

    // Memory allocation error has occurred -> close all sessions
    struct parser_plugin_update_fail data = {
        .status = IPX_OK,
        .ctx = ctx
    };

    ipx_parser_session_for(parser, &parser_plugin_update_fail_cb, &data);
    if (data.status != IPX_OK) {
        // Something is horribly wrong
        return IPX_ERR_DENIED;
    }

    return IPX_OK;
}

void
parser_plugin_update_abort(ipx_ctx_t *ctx, void *cfg, void *update)
{
    // Nothing to do
    (void) ctx;
    (void) cfg;
    (void) update;
}
