#ifndef DBUS_SESSION_H
#define DBUS_SESSION_H

#include <stddef.h>
#include <stdint.h>

#include "auth.h"
#include "dbus.h"
#include "dbus_bus.h"

/*
 * D-Bus object/interface identity used for every signal this module
 * publishes. Kept here (rather than duplicated at each call site) so a
 * future well-known-name/object-manager implementation has one place to
 * look.
 */
#define DBUS_SESSION_OBJECT_PATH   "/org/forestos/Session"
#define DBUS_SESSION_INTERFACE     "org.forestos.Session1"

#define DBUS_SESSION_MEMBER_LOGIN  "SessionLogin"
#define DBUS_SESSION_MEMBER_LOGOUT "SessionLogout"
#define DBUS_SESSION_MEMBER_SWITCH "SessionSwitch"

/*
 * Wire body for SessionLogin / SessionLogout signals: a snapshot of the
 * tty_session_t fields observers care about, read via session_get()/
 * session_get_current() at publish time. This is a fixed-layout struct
 * rather than proper D-Bus type-signature marshaling because the codec
 * layer (dbus_codec.c) does not implement variant/struct body marshaling
 * yet (see its "Placeholder length model" TODO) - this mirrors that same
 * level of maturity for the body instead of inventing a parallel scheme.
 */
typedef struct {
    uint32_t session_id;
    uint32_t session_type;   /* session_type_t  */
    uint32_t state;          /* session_state_t */
    uint32_t logged_in;      /* 0 / 1            */
    uint32_t uid;
    uint32_t gid;
    char username[AUTH_NAME_LEN];
} dbus_session_state_body_t;

/* Wire body for the SessionSwitch signal: previous + newly active session. */
typedef struct {
    uint32_t from_session_id;
    uint32_t to_session_id;
    dbus_session_state_body_t to_session;
} dbus_session_switch_body_t;

int dbus_session_endpoint_init(dbus_bus_endpoint_t *endpoint);
int dbus_session_bus_address(char *out, size_t out_size);

/*
 * Fill out_body from the live tty_session_t for session_num (as returned
 * by session_get()). Pass 0 for session_num to snapshot whatever
 * session_get_current() currently reports instead. Returns 0 on success,
 * -1 on bad arguments, -2 if the session slot does not exist.
 */
int dbus_session_state_body_from_session(uint32_t session_num,
                                         dbus_session_state_body_t *out_body);

/*
 * Build and (attempt to) publish a SessionLogin/SessionLogout/SessionSwitch
 * signal on the session bus for the given endpoint. Session state is read
 * on-demand from session.c via session_get()/session_get_current(); these
 * calls never modify session state.
 *
 * Return values:
 *   0    - signal built, validated, and handed off to the transport
 *   -1   - invalid arguments (NULL endpoint, no such session slot, ...)
 *   -2   - message assembly failed (see dbus_message_* return codes)
 *   -100 - transport not yet implemented (endpoint could not be connected);
 *          propagated from dbus_bus_endpoint_connect(), which is still a
 *          TODO(worker6-phase4) stub as of this writing.
 */
int dbus_session_publish_login(dbus_bus_endpoint_t *endpoint, uint32_t session_num);
int dbus_session_publish_logout(dbus_bus_endpoint_t *endpoint, uint32_t session_num);
int dbus_session_publish_switch(dbus_bus_endpoint_t *endpoint,
                                uint32_t from_session_num,
                                uint32_t to_session_num);

/*
 * Convenience wrappers that operate on whatever session_get_current()
 * currently reports, for callers that just switched the active session
 * and want to announce it without tracking session numbers themselves.
 */
int dbus_session_publish_current_login(dbus_bus_endpoint_t *endpoint);
int dbus_session_publish_current_logout(dbus_bus_endpoint_t *endpoint);

#endif
