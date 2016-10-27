//--------------------------------------------------------------------------
// Copyright (C) 2014-2016 Cisco and/or its affiliates. All rights reserved.
// Copyright (C) 2005-2013 Sourcefire, Inc.
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of the GNU General Public License Version 2 as published
// by the Free Software Foundation.  You may not use, modify or distribute
// this program under any other version of the GNU General Public License.
//
// This program is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
//--------------------------------------------------------------------------

// service_rtmp.cc author Sourcefire Inc.

#include "service_rtmp.h"

#include "application_ids.h"
#include "service_api.h"
#include "app_info_table.h"
#include "appid_module.h"

#include "log/messages.h"
#include "main/snort_debug.h"
#include "utils/util.h"

#define RTMP_PORT 1935

#define RTMP_VER_3 3

#define RTMP_HANDSHAKE1_SIZE 1536    /* C1/S1 */
#define RTMP_HANDSHAKE2_SIZE 1536    /* C2/S2 */

#define RTMP_CHUNK_SIZE 128

#define RTMP_AMF0_COMMAND_MESSAGE_ID 20

#define RTMP_COMMAND_TYPE_CONNECT     "connect"
#define RTMP_COMMAND_TYPE_CONNECT_LEN 7

#define RTMP_PROPERTY_KEY_SWFURL      "swfUrl"
#define RTMP_PROPERTY_KEY_SWFURL_LEN  6
#define RTMP_PROPERTY_KEY_PAGEURL     "pageUrl"
#define RTMP_PROPERTY_KEY_PAGEURL_LEN 7

#define AMF0_TYPE_NUMBER     0x00
#define AMF0_TYPE_BOOLEAN    0x01
#define AMF0_TYPE_STRING     0x02
#define AMF0_TYPE_OBJECT     0x03
#define AMF0_TYPE_OBJECT_END 0x09    /* Preceded by 0x00,0x00. */

#define CHECK_SIZE(n) do { if (size < (n)) goto parse_rtmp_message_fail; } while (0)
#define ADVANCE_DATA(n) do { data += (n); size -= (n); } while (0)

enum RTMPState
{
    RTMP_STATE_INIT = 0,              /* Haven't seen anything yet. */
    RTMP_STATE_SENT_HANDSHAKE0,       /* C0/S0 */
    RTMP_STATE_SENDING_HANDSHAKE1,    /* C1/S1 -- client/server_bytes_left */
    RTMP_STATE_SENT_HANDSHAKE1,       /* C1/S1 */
    RTMP_STATE_SENDING_HANDSHAKE2,    /* C2/S2 -- client/server_bytes_left */
    RTMP_STATE_SENT_HANDSHAKE2,       /* C2/S2 */
    RTMP_STATE_DONE                   /* As in "this detector is done watching the client or
                                         server". */
};

struct ServiceRTMPData
{
    RTMPState client_state;
    RTMPState server_state;
    uint16_t client_bytes_left;
    uint16_t server_bytes_left;
    char* swfUrl;
    char* pageUrl;
};

static int rtmp_init(const InitServiceAPI* const api);
static int rtmp_validate(ServiceValidationArgs* args);

static const RNAServiceElement svc_element =
{
    nullptr,
    &rtmp_validate,
    nullptr,
    DETECTOR_TYPE_DECODER,
    1,
    1,
    0,
    "rtmp"
};

static const RNAServiceValidationPort pp[] =
{
    { &rtmp_validate, 1935, IpProtocol::TCP, 0 },
    { &rtmp_validate, 1935, IpProtocol::UDP, 0 },
    { nullptr, 0, IpProtocol::PROTO_NOT_SET, 0 }
};

RNAServiceValidationModule rtmp_service_mod =
{
    "rtmp",
    &rtmp_init,
    pp,
    nullptr,
    nullptr,
    0,
    nullptr,
    0
};

static AppRegistryEntry appIdRegistry[] =
{
    { APP_ID_RTMP, APPINFO_FLAG_SERVICE_ADDITIONAL }
};

static int rtmp_init(const InitServiceAPI* const init_api)
{
    unsigned i;
    for (i = 0; i < (sizeof(appIdRegistry) / sizeof(*appIdRegistry)); i++)
    {
        DebugFormat(DEBUG_INSPECTOR, "registering appId: %d\n", appIdRegistry[i].appId);
        init_api->RegisterAppId(&rtmp_validate, appIdRegistry[i].appId,
            appIdRegistry[i].additionalInfo);
    }
    return 0;
}

static void rtmp_free(void* ss)    /* AppIdFreeFCN */
{
    ServiceRTMPData* ss_tmp = (ServiceRTMPData*)ss;
    snort_free(ss_tmp->swfUrl);
    snort_free(ss_tmp->pageUrl);
    snort_free(ss_tmp);
}

static int parse_rtmp_chunk_basic_header(const uint8_t** data_inout, uint16_t* size_inout,
    uint8_t* format, uint32_t* chunk_stream_id)
{
    const uint8_t* data = *data_inout;
    uint16_t size = *size_inout;

    if (size < 1)
        return 0;
    *format = (data[0] & 0xC0) >> 6;

    *chunk_stream_id = (data[0] & 0x3F);
    if (*chunk_stream_id == 0)
    {
        if (size < 2)
            return 0;
        *chunk_stream_id = data[1] + 64;
        data += 2;
        size -= 2;
    }
    else if (*chunk_stream_id == 1)
    {
        *chunk_stream_id = data[2] * 256 + data[1] + 64;
        if (size < 3)
            return 0;
        data += 3;
        size -= 3;
    }
    else
    {
        data += 1;
        size -= 1;
    }

    *data_inout = data;
    *size_inout = size;
    return 1;
}

static int parse_rtmp_message_header(const uint8_t** data_inout, uint16_t* size_inout,
    uint32_t* chunk_stream_id, uint32_t* message_length, uint8_t* message_type_id)
{
    const uint8_t* data = *data_inout;
    uint16_t size = *size_inout;

    uint8_t fmt;
    unsigned hdr_len;

    if (!parse_rtmp_chunk_basic_header(&data, &size, &fmt, chunk_stream_id))
        return 0;
    switch (fmt)
    {
    case 0:
        hdr_len = 11;
        break;
    case 1:
        hdr_len = 7;
        break;
    default:
        return 0;
    }
    if (size < hdr_len)
        return 0;

    *message_length  = (data[3] << 16) + (data[4] << 8) + data[5];
    *message_type_id = data[6];

    data += hdr_len;
    size -= hdr_len;

    *data_inout = data;
    *size_inout = size;
    return 1;
}

static int unchunk_rtmp_message_body(const uint8_t** data_inout, uint16_t* size_inout,
    uint32_t chunk_stream_id, uint32_t message_length, uint8_t* message_body)
{
    const uint8_t* data = *data_inout;
    uint16_t size = *size_inout;

    while (message_length > 0)
    {
        uint32_t chunk_len;

        chunk_len = message_length;
        if (message_length > RTMP_CHUNK_SIZE)
            chunk_len = RTMP_CHUNK_SIZE;
        if (size < chunk_len)
            return 0;

        memcpy(message_body, data, chunk_len);
        data += chunk_len;
        size -= chunk_len;
        message_body   += chunk_len;
        message_length -= chunk_len;

        if (message_length > 0)
        {
            uint8_t fmt;
            uint32_t id;

            if (!parse_rtmp_chunk_basic_header(&data, &size, &fmt, &id))
                return 0;
            if (fmt != 3)
                return 0;
            if (id != chunk_stream_id)
                return 0;
        }
    }

    *data_inout = data;
    *size_inout = size;
    return 1;
}

static char* duplicate_string(const uint8_t** data_inout, uint16_t* size_inout)
{
    const uint8_t* data = *data_inout;
    uint16_t size = *size_inout;

    uint16_t field_len;
    char* str;

    if (size < (1 + 2))
        return nullptr;
    if (data[0] != AMF0_TYPE_STRING)
        return nullptr;
    field_len = (data[1] << 8) + data[2];
    if (field_len == 0)
        return nullptr;
    data += 1 + 2;
    size -= 1 + 2;

    if (size < field_len)
        return nullptr;
    str = (char*)snort_alloc(field_len + 1);
    memcpy(str, data, field_len);
    str[field_len] = '\0';
    data += field_len;
    size -= field_len;

    *data_inout = data;
    *size_inout = size;
    return str;
}

static int skip_property_value(const uint8_t** data_inout, uint16_t* size_inout)
{
    const uint8_t* data = *data_inout;
    uint16_t size = *size_inout;

    uint8_t type;
    uint16_t field_len;

    if (size < 1)
        return 0;
    type = data[0];
    data += 1;
    size -= 1;

    switch (type)
    {
    case AMF0_TYPE_NUMBER:
        if (size < 8)
            return 0;
        data += 8;
        size -= 8;
        break;

    case AMF0_TYPE_BOOLEAN:
        if (size < 1)
            return 0;
        data += 1;
        size -= 1;
        break;

    case AMF0_TYPE_STRING:
        if (size < 2)
            return 0;
        field_len = (data[0] << 8) + data[1];
        data += 2;
        size -= 2;
        if (size < field_len)
            return 0;
        data += field_len;
        size -= field_len;
        break;

    default:
        return 0;
    }

    *data_inout = data;
    *size_inout = size;
    return 1;
}

static int parse_rtmp_message(const uint8_t** data_inout, uint16_t* size_inout, ServiceRTMPData* ss)
{
    const uint8_t* data = *data_inout;
    uint16_t size = *size_inout;
    int ret  = 1;

    uint32_t id;
    uint32_t msg_len;
    uint8_t msg_type;
    uint16_t field_len;
    uint8_t* body = nullptr;

    if (!parse_rtmp_message_header(&data, &size, &id, &msg_len, &msg_type))
        goto parse_rtmp_message_fail;
    if (msg_type != RTMP_AMF0_COMMAND_MESSAGE_ID)
        goto parse_rtmp_message_fail;

    body = (uint8_t*)snort_alloc(msg_len);
    if (!unchunk_rtmp_message_body(&data, &size, id, msg_len, body))
        goto parse_rtmp_message_fail;
    *data_inout = data;
    *size_inout = size;

    /* Now we have a message body of a command (hopefully a connect). */
    data = body;
    size = msg_len;

    /* Make sure it's a connect command. */
    CHECK_SIZE(1 + 2);
    if (data[0] != AMF0_TYPE_STRING)
        goto parse_rtmp_message_fail;
    field_len = (data[1] << 8) + data[2];
    if (field_len == 0)
        goto parse_rtmp_message_fail;
    ADVANCE_DATA(1 + 2);
    CHECK_SIZE(field_len);
    if (strncmp((const char*)data, RTMP_COMMAND_TYPE_CONNECT, field_len) != 0)
        goto parse_rtmp_message_fail;
    ADVANCE_DATA(field_len);

    /* Make sure transaction ID is next. */
    CHECK_SIZE(1 + 8);
    if (data[0] != AMF0_TYPE_NUMBER)
        goto parse_rtmp_message_fail;
    ADVANCE_DATA(1 + 8);

    /* Make sure we have the command object next. */
    CHECK_SIZE(1);
    if (data[0] != AMF0_TYPE_OBJECT)
        goto parse_rtmp_message_fail;
    ADVANCE_DATA(1);

    /* Search command object for desired metadata. */
    do
    {
        /* Check for end of object. */
        CHECK_SIZE(3);    /* Need at least this much for full end of object. */
        field_len = (data[0] << 8) + data[1];
        if (field_len == 0)
        {
            if (data[2] == AMF0_TYPE_OBJECT_END)
                break;
            else
                goto parse_rtmp_message_fail;
        }
        ADVANCE_DATA(2);    /* Not at end, so just get to start of key string for continued
                               processing below. */

        /* See if we're interested in this property key (or just skip it). */
        CHECK_SIZE(field_len);
        if (    (ss->swfUrl == nullptr)
            && (field_len == RTMP_PROPERTY_KEY_SWFURL_LEN)
            && (strncmp((const char*)data, RTMP_PROPERTY_KEY_SWFURL,
            RTMP_PROPERTY_KEY_SWFURL_LEN) == 0) )
        {
            /* swfUrl */
            ADVANCE_DATA(field_len);
            ss->swfUrl = duplicate_string(&data, &size);
            if (ss->swfUrl == nullptr)
                goto parse_rtmp_message_fail;
        }
        else if (    (ss->pageUrl == nullptr)
            && (field_len == RTMP_PROPERTY_KEY_PAGEURL_LEN)
            && (strncmp((const char*)data, RTMP_PROPERTY_KEY_PAGEURL,
            RTMP_PROPERTY_KEY_PAGEURL_LEN) == 0) )
        {
            /* pageUrl */
            ADVANCE_DATA(field_len);
            ss->pageUrl = duplicate_string(&data, &size);
            if (ss->pageUrl == nullptr)
                goto parse_rtmp_message_fail;
        }
        else
        {
            /* Something we dont care about... */
            ADVANCE_DATA(field_len);
            if (!skip_property_value(&data, &size))
                goto parse_rtmp_message_fail;
        }
    }
    while (size > 0);

parse_rtmp_message_done:
    snort_free(body);
    return ret;

parse_rtmp_message_fail:
    ret = 0;
    goto parse_rtmp_message_done;
}

static int rtmp_validate(ServiceValidationArgs* args)
{
    ServiceRTMPData* ss;
    AppIdSession* asd = args->asd;
    const uint8_t* data = args->data;
    const int dir = args->dir;
    uint16_t size = args->size;

    if (!size)
        goto inprocess;

    ss = (ServiceRTMPData*)rtmp_service_mod.api->data_get(asd, rtmp_service_mod.flow_data_index);
    if (!ss)
    {
        ss = (ServiceRTMPData*)snort_calloc(sizeof(ServiceRTMPData));
        rtmp_service_mod.api->data_add(asd, ss, rtmp_service_mod.flow_data_index, &rtmp_free);
    }

    /* Client -> Server */
    if (dir == APP_ID_FROM_INITIATOR)
    {
        /* Consume this packet. */
        while (size > 0)
        {
            switch (ss->client_state)
            {
            case RTMP_STATE_INIT:
                /* C0 is just a version number.  Must be valid. */
                if (*data != RTMP_VER_3)
                {
                    goto fail;
                }
                ss->client_state = RTMP_STATE_SENT_HANDSHAKE0;
                data += 1;
                size -= 1;
                break;

            case RTMP_STATE_SENT_HANDSHAKE0:
                /* Just skip RTMP_HANDSHAKE1_SIZE bytes for C1. */
                ss->client_state      = RTMP_STATE_SENDING_HANDSHAKE1;
                ss->client_bytes_left = RTMP_HANDSHAKE1_SIZE;
            /* fall through */

            case RTMP_STATE_SENDING_HANDSHAKE1:
                if (size < ss->client_bytes_left)
                {
                    /* We've still got more to get next time around. */
                    ss->client_bytes_left -= size;
                    size = 0;
                }
                else
                {
                    /* We've gotten all of the bytes that we wanted. */
                    ss->client_state = RTMP_STATE_SENT_HANDSHAKE1;
                    data += ss->client_bytes_left;
                    size -= ss->client_bytes_left;
                }
                break;

            case RTMP_STATE_SENT_HANDSHAKE1:
                /* Client can't start sending C2 until it has received S1. */
                if (ss->server_state < RTMP_STATE_SENT_HANDSHAKE1)
                {
                    goto fail;
                }
                /* Just skip RTMP_HANDSHAKE2_SIZE bytes for C2. */
                ss->client_state      = RTMP_STATE_SENDING_HANDSHAKE2;
                ss->client_bytes_left = RTMP_HANDSHAKE2_SIZE;
            /* fall through */

            case RTMP_STATE_SENDING_HANDSHAKE2:
                if (size < ss->client_bytes_left)
                {
                    /* We've still got more to get next time around. */
                    ss->client_bytes_left -= size;
                    size = 0;
                }
                else
                {
                    /* We've gotten all of the bytes that we wanted. */
                    ss->client_state = RTMP_STATE_SENT_HANDSHAKE2;
                    data += ss->client_bytes_left;
                    size -= ss->client_bytes_left;
                }
                break;

            case RTMP_STATE_SENT_HANDSHAKE2:
                if (parse_rtmp_message(&data, &size, ss))
                {
                    /* Got our connect command.  We're done. */
                    ss->client_state = RTMP_STATE_DONE;
                }
                else
                {
                    /* No connect command found.  Bail out. */
                    goto fail;
                }
            /* fall through */

            case RTMP_STATE_DONE:
                /* We're done with client, so just blindly consume all data. */
                size = 0;
                break;

            default:
                goto fail;        /* No reason to ever get here. */
            }
        }
    }
    /* Server -> Client */
    else if (dir == APP_ID_FROM_RESPONDER)
    {
        /* Consume this packet. */
        while (size > 0)
        {
            switch (ss->server_state)
            {
            case RTMP_STATE_INIT:
                /* Client must initiate. */
                if (ss->client_state < RTMP_STATE_SENT_HANDSHAKE0)
                {
                    goto fail;
                }
                /* S0 is just a version number.  Must be valid. */
                if (*data != RTMP_VER_3)
                {
                    goto fail;
                }
                ss->server_state = RTMP_STATE_SENT_HANDSHAKE0;
                data += 1;
                size -= 1;
                break;

            case RTMP_STATE_SENT_HANDSHAKE0:
                /* Just skip RTMP_HANDSHAKE1_SIZE bytes for S1. */
                ss->server_state      = RTMP_STATE_SENDING_HANDSHAKE1;
                ss->server_bytes_left = RTMP_HANDSHAKE1_SIZE;
            /* fall through */

            case RTMP_STATE_SENDING_HANDSHAKE1:
                if (size < ss->server_bytes_left)
                {
                    /* We've still got more to get next time around. */
                    ss->server_bytes_left -= size;
                    size = 0;
                }
                else
                {
                    /* We've gotten all of the bytes that we wanted. */
                    ss->server_state = RTMP_STATE_SENT_HANDSHAKE1;
                    data += ss->server_bytes_left;
                    size -= ss->server_bytes_left;
                }
                break;

            case RTMP_STATE_SENT_HANDSHAKE1:
                /* Server can't start sending S2 until it has received C1. */
                if (ss->client_state < RTMP_STATE_SENT_HANDSHAKE1)
                {
                    goto fail;
                }
                /* Just skip RTMP_HANDSHAKE2_SIZE bytes for S2. */
                ss->server_state      = RTMP_STATE_SENDING_HANDSHAKE2;
                ss->server_bytes_left = RTMP_HANDSHAKE2_SIZE;
            /* fall through */

            case RTMP_STATE_SENDING_HANDSHAKE2:
                if (size < ss->server_bytes_left)
                {
                    /* We've still got more to get next time around. */
                    ss->server_bytes_left -= size;
                    size = 0;
                    break;        /* Not done yet. */
                }
                else
                {
                    /* We've gotten all of the bytes that we wanted. */
                    ss->server_state = RTMP_STATE_SENT_HANDSHAKE2;
                    data += ss->server_bytes_left;
                    size -= ss->server_bytes_left;
                }
            /* fall through */

            case RTMP_STATE_SENT_HANDSHAKE2:
                /* No more interest in watching server. */
                ss->server_state = RTMP_STATE_DONE;
            /* fall through */

            case RTMP_STATE_DONE:
                /* We're done with server, so just blindly consume all data. */
                size = 0;
                break;

            default:
                goto fail;        /* No reason to ever get here. */
            }
        }
    }

    /* Are we there yet? */
    if (    (ss->client_state == RTMP_STATE_DONE)
        && (ss->server_state == RTMP_STATE_DONE) )
    {
        goto success;
    }

    /* Give up if it's taking us too long to figure out this thing. */
    if (asd->session_packet_count >= AppIdConfig::get_appid_config()->mod_config->rtmp_max_packets)
    {
        goto fail;
    }

inprocess:
    rtmp_service_mod.api->service_inprocess(asd, args->pkt, dir, &svc_element);
    return SERVICE_INPROCESS;

fail:
    snort_free(ss->swfUrl);
    snort_free(ss->pageUrl);
    ss->swfUrl = ss->pageUrl = nullptr;
    rtmp_service_mod.api->fail_service(asd, args->pkt, dir, &svc_element,
        rtmp_service_mod.flow_data_index);
    return SERVICE_NOMATCH;

success:
    if (ss->swfUrl != nullptr)
    {
        if (!asd->hsession)
            asd->hsession = (httpSession*)snort_calloc(sizeof(httpSession));

        if (asd->hsession->url == nullptr)
        {
            asd->hsession->url = ss->swfUrl;
            asd->scan_flags |= SCAN_HTTP_HOST_URL_FLAG;
        }
        else
        {
            snort_free(ss->swfUrl);
        }
        ss->swfUrl = nullptr;
    }
    if (ss->pageUrl != nullptr)
    {
        if (!asd->hsession)
            asd->hsession = (httpSession*)snort_calloc(sizeof(httpSession));

        if (!AppIdConfig::get_appid_config()->mod_config->referred_appId_disabled &&
            (asd->hsession->referer == nullptr))
            asd->hsession->referer = ss->pageUrl;
        else
            snort_free(ss->pageUrl);
        ss->pageUrl = nullptr;
    }
    rtmp_service_mod.api->add_service(asd, args->pkt, dir, &svc_element,
        APP_ID_RTMP, nullptr, nullptr, nullptr);
    appid_stats.rtmp_flows++;
    return SERVICE_SUCCESS;
}

