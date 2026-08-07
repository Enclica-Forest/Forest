#include <stddef.h>

#include "include/dbus.h"
#include "include/dbus_bus.h"
#include "include/dbus_codec.h"
#include "include/dbus_session.h"
#include "include/session.h"
#include "include/string.h"
#include "include/xdg.h"

int dbus_session_endpoint_init(dbus_bus_endpoint_t *endpoint) {
    return dbus_bus_endpoint_init(endpoint, DBUS_BUS_KIND_SESSION);
}

int dbus_session_bus_address(char *out, size_t out_size) {
    return xdg_dbus_session_bus_address_resolve(NULL, NULL, out, out_size);
}

/*
 * Serial numbers for outgoing signals. Session-state signals are
 * low-frequency (login/logout/switch), so a simple monotonic counter
 * is enough; it deliberately never resets to 0 so an observer can treat
 * serial 0 as "no message sent yet".
 */
static uint32_t g_dbus_session_serial = 1U;

int dbus_session_state_body_from_session(uint32_t session_num,
                                         dbus_session_state_body_t *out_body) {
    tty_session_t *session;

    if (out_body == NULL) {
        return -1;
    }

    /* session_num == 0 means "whatever session_get_current() reports". */
    session = (session_num == 0U) ? session_get_current() : session_get(session_num);
    if (session == NULL) {
        return -2;
    }

    memset(out_body, 0, sizeof(*out_body));
    out_body->session_id = session->session_id;
    out_body->session_type = (uint32_t)session->type;
    out_body->state = (uint32_t)session->state;
    out_body->logged_in = session->logged_in ? 1U : 0U;
    out_body->uid = session->user_info.uid;
    out_body->gid = session->user_info.gid;
    strncpy(out_body->username, session->user_info.name, sizeof(out_body->username) - 1U);
    out_body->username[sizeof(out_body->username) - 1U] = '\0';

    return 0;
}

/* Stamp the path/interface/member header fields shared by every signal. */
static int dbus_session_add_common_fields(dbus_message_t *msg, const char *member) {
    int rc;

    rc = dbus_message_add_field(msg, DBUS_HEADER_PATH,
                                (const uint8_t *)DBUS_SESSION_OBJECT_PATH,
                                (uint32_t)strlen(DBUS_SESSION_OBJECT_PATH));
    if (rc != 0) {
        return rc;
    }

    rc = dbus_message_add_field(msg, DBUS_HEADER_INTERFACE,
                                (const uint8_t *)DBUS_SESSION_INTERFACE,
                                (uint32_t)strlen(DBUS_SESSION_INTERFACE));
    if (rc != 0) {
        return rc;
    }

    return dbus_message_add_field(msg, DBUS_HEADER_MEMBER,
                                  (const uint8_t *)member,
                                  (uint32_t)strlen(member));
}

/*
 * Assemble a SIGNAL message for `member` carrying `body`, validate it, and
 * hand it to the bus endpoint. The wire header is encoded up front so a
 * future transport implementation has a ready-to-write frame; the field
 * payloads and body already live in `msg`/`body` for it to marshal.
 */
static int dbus_session_publish_message(dbus_bus_endpoint_t *endpoint,
                                        const char *member,
                                        const uint8_t *body,
                                        uint32_t body_len) {
    dbus_message_t msg;
    uint8_t header_bytes[DBUS_WIRE_HEADER_SIZE];
    int rc;

    if ((endpoint == NULL) || (member == NULL)) {
        return -1;
    }

    rc = dbus_message_init(&msg, DBUS_MESSAGE_SIGNAL, 0U, g_dbus_session_serial, body, body_len);
    if (rc != 0) {
        return -2;
    }

    rc = dbus_session_add_common_fields(&msg, member);
    if (rc != 0) {
        return -2;
    }

    if (dbus_message_validate(&msg) != 0) {
        return -2;
    }

    if (dbus_encode_wire_header(&msg.header, header_bytes, sizeof(header_bytes)) < 0) {
        return -2;
    }

    if (!endpoint->connected) {
        rc = dbus_bus_endpoint_connect(endpoint);
        if (rc != 0) {
            /*
             * TODO(worker6-phase4): dbus_bus_endpoint_connect() is still a
             * stub (no socket/transport yet). Once it lands, this is the
             * spot to write header_bytes + marshaled fields + body to the
             * connected endpoint before returning success.
             */
            return rc;
        }
    }

    g_dbus_session_serial++;
    return 0;
}

int dbus_session_publish_login(dbus_bus_endpoint_t *endpoint, uint32_t session_num) {
    dbus_session_state_body_t body;

    if (dbus_session_state_body_from_session(session_num, &body) != 0) {
        return -1;
    }

    return dbus_session_publish_message(endpoint, DBUS_SESSION_MEMBER_LOGIN,
                                        (const uint8_t *)&body, (uint32_t)sizeof(body));
}

int dbus_session_publish_logout(dbus_bus_endpoint_t *endpoint, uint32_t session_num) {
    dbus_session_state_body_t body;

    if (dbus_session_state_body_from_session(session_num, &body) != 0) {
        return -1;
    }

    return dbus_session_publish_message(endpoint, DBUS_SESSION_MEMBER_LOGOUT,
                                        (const uint8_t *)&body, (uint32_t)sizeof(body));
}

int dbus_session_publish_switch(dbus_bus_endpoint_t *endpoint,
                                uint32_t from_session_num,
                                uint32_t to_session_num) {
    dbus_session_switch_body_t body;
    tty_session_t *from_session;

    memset(&body, 0, sizeof(body));

    if (from_session_num != 0U) {
        from_session = session_get(from_session_num);
        body.from_session_id = (from_session != NULL) ? from_session->session_id : from_session_num;
    }

    if (dbus_session_state_body_from_session(to_session_num, &body.to_session) != 0) {
        return -1;
    }
    body.to_session_id = body.to_session.session_id;

    return dbus_session_publish_message(endpoint, DBUS_SESSION_MEMBER_SWITCH,
                                        (const uint8_t *)&body, (uint32_t)sizeof(body));
}

int dbus_session_publish_current_login(dbus_bus_endpoint_t *endpoint) {
    return dbus_session_publish_login(endpoint, 0U);
}

int dbus_session_publish_current_logout(dbus_bus_endpoint_t *endpoint) {
    return dbus_session_publish_logout(endpoint, 0U);
}
