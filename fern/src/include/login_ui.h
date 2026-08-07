#ifndef LOGIN_UI_H
#define LOGIN_UI_H

#include "auth.h"

typedef enum {
    AUTH_USER_SUCCESS = 0,
    AUTH_USER_CANCELLED = 1,
    AUTH_USER_FAILED = 2
} auth_user_result_t;

// Runs the graphical login UI and returns when a user has successfully logged in.
auth_user_result_t login_ui_run(auth_user_info_t* out_user);

#endif // LOGIN_UI_H
