/*
 * Copyright (c) 2016 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl.
 *
 * Change Date: 2019-07-01
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */

#include <gw_authenticator.h>
#include <modutil.h>

/**
 * @file authenticator.c - Authenticator modules
 */

/**
 * @brief Initialize an authenticator module
 *
 * Process the options into an array and pass them to the authenticator
 * initialization function
 *
 * The authenticator must implement the @c initialize entry point if this
 * function is called. If the authenticator does not implement this, behavior is
 * undefined.
 *
 * @param func Authenticator entry point
 * @param options Authenticator options
 * @return Authenticator instance or NULL on error
 */
void* authenticator_init(GWAUTHENTICATOR *func, const char *options)
{
        char *optarray[AUTHENTICATOR_MAX_OPTIONS];
        size_t optlen = options ? strlen(options) : 0;
        char optcopy[optlen + 1];
        int optcount = 0;

        if (options)
        {
            strcpy(optcopy, options);
            char *opt = optcopy;

            while (opt)
            {
                char *end = strnchr_esc(opt, ',', sizeof(optcopy) - (opt - optcopy));

                if (end)
                {
                    *end++ = '\0';
                }

                optarray[optcount++] = opt;
                opt = end;
            }
        }

        optarray[optcount] = NULL;

        return func->initialize(optarray);
}
