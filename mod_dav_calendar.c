/* Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


/*
 * The Apache mod_dav_calendar module adds CalDav support.
 *
 *  Author: Graham Leggett
 *
 * Configure directives in the following order:
 *
 * - DavAccessPrincipalUrl: Tell the CalDAV client what the principal
 *   URL of the current user. This URL is an expression.
 *
 * - DavCalendarHome: Tell the CalDAV client the name of the calendar
 *   collection, or the ordinary collection containing calendar
 *   collections. This is an expression, and as a collection must have
 *   a trailing slash. A typical expression might be
 *   /dav/calendars/%{escape:%{REMOTE_USER}}/.
 *
 * - DavCalendarProvision: Tell the server what calendar collections
 *   should be auto provisioned on first access. A typical expression
 *   might be /dav/calendars/%{escape:%{REMOTE_USER}}/Home/. More than
 *   one calendar collection can be specified. All parent collections
 *   will be created automatically.
 *
 * Possible configuration:
 *
 * <IfModule dav_calendar_module>
 *   # Calendar backed by mod_dav_fs for storage.
 *   DavLockDB /tmp/lockdb
 *
 *   <Location /dav>
 *     Dav on
 *     DavCalendar on
 *
 * <IfModule dav_access_module>
 *     # Allow every resource to expose who the current principal is
 *     DavAccessPrincipalUrl /dav/principals/%{escape:%{REMOTE_USER}}
 * </IfModule>
 *
 *     # this calendar is visible to anyone, logged in or not
 *     #  DavCalendarHome /dav/calendars/public
 *     DavCalendarHome /dav/calendars/%{escape:%{REMOTE_USER}}/
 *
 *     AuthBasicProvider file
 *     AuthUserFile conf/passwd
 *     AuthType basic
 *     AuthName calendar
 *
 *     Require valid-user
 *
 *   </Location>
 *   <Location /dav/principals>
 *     # Every principal is told where the calendars are
 *     # this calendar only exists if the user has logged in
 *     #  DavCalendarHome /dav/calendars/%{escape:%{REMOTE_USER}}/
 *   </Location>
 *   <Location /dav/calendars>
 *     DavCalendar on
 *     DavCalendarProvision /dav/calendars/%{escape:%{REMOTE_USER}}/Home/
 *     DavCalendarTimezone UTC
 *   </Location>
 *   <Location /dav/calendars/public>
 *     Require all granted
 *   </Location>
 *
 *   Alias /dav /www/htdocs/dav
 *   <Directory /www/htdocs/dav>
 *
 *     Options FollowSymLinks
 *     AllowOverride None
 *
 *   </Directory>
 * </IfModule>
 *
 * TODO: We are still not in compliance with https://tools.ietf.org/html/rfc4791.
 *
 * - We do not yet enforce many of the preconditions defined in the RFC.
 * - We do not support https://tools.ietf.org/html/rfc7953 yet.
 *
 */
#include <apr_lib.h>
#include <apr_escape.h>
#include <apr_strings.h>
#include "apr_sha1.h"
#include "apr_encode.h"
#include "apr_tables.h"

#include "httpd.h"
#include "http_config.h"
#include "http_core.h"
#include "http_log.h"
#include "http_protocol.h"
#include "http_request.h"
#include "util_script.h"

#include <libical/ical.h>

#include "mod_dav.h"

#undef PACKAGE_BUGREPORT
#undef PACKAGE_NAME
#undef PACKAGE_STRING
#undef PACKAGE_TARNAME
#undef PACKAGE_VERSION
#include "config.h"

module AP_MODULE_DECLARE_DATA dav_calendar_module;

typedef struct
{
    int dav_calendar_set :1;
    int dav_calendar_timezone_set :1;
    int max_resource_size_set :1;
    apr_array_header_t *dav_calendar_homes;
    apr_array_header_t *dav_calendar_provisions;
    const char *dav_calendar_timezone;
    apr_off_t max_resource_size;
    int dav_calendar;

} dav_calendar_config_rec;

typedef struct {
    const char *real;
    const char *fake;
    ap_regex_t *regexp;
} dav_calendar_alias_entry;

typedef struct
{
    apr_array_header_t *aliases;
} dav_calendar_server_rec;

/* forward-declare the hook structures */
static const dav_hooks_liveprop dav_hooks_liveprop_calendar;

#define DAV_XML_NAMESPACE "DAV:"
#define DAV_CALENDAR_XML_NAMESPACE "urn:ietf:params:xml:ns:caldav"

#define DEFAULT_TIMEZONE "BEGIN:VCALENDAR\r\nVERSION:2.0\r\n" \
    "PRODID:-//Graham Leggett//" \
    PACKAGE_STRING \
    "//EN\r\nBEGIN:VTIMEZONE\r\nTZID:UTC\r\nEND:VTIMEZONE\r\nEND:VCALENDAR\r\n"

#define DEFAULT_MAX_RESOURCE_SIZE 10*1024*1024

#define DAV_CALENDAR_HANDLER "httpd/calendar-summary"

#define DAV_CALENDAR_COLLATION_ASCII_CASEMAP "i;ascii-casemap"
#define DAV_CALENDAR_COLLATION_OCTET "i;octet"

/* MKCALENDAR method */
static int iM_MKCALENDAR;

/*
 * dav_log_err()
 *
 * Write error information to the log.
 */
static void dav_log_err(request_rec *r, dav_error *err, int level)
{
    dav_error *errscan;

    /* Log the errors */
    /* ### should have a directive to log the first or all */
    for (errscan = err; errscan != NULL; errscan = errscan->prev) {
        if (errscan->desc == NULL)
            continue;

        /* Intentional no APLOGNO */
        ap_log_rerror(APLOG_MARK, level, errscan->aprerr, r, "%s  [%d, #%d]",
                      errscan->desc, errscan->status, errscan->error_id);
    }
}

static void dav_prop_log_errors(dav_prop_ctx *ctx)
{
    dav_log_err(ctx->r, ctx->err, APLOG_ERR);
}


/*
** The namespace URIs that we use. This list and the enumeration must
** stay in sync.
*/
static const char * const dav_calendar_namespace_uris[] =
{
    DAV_CALENDAR_XML_NAMESPACE,

    NULL        /* sentinel */
};
enum {
    DAV_CALENDAR_URI_DAV            /* the DAV: namespace URI */
};

enum {
    DAV_CALENDAR_PROPID_calendar_data = 1,
    DAV_CALENDAR_PROPID_calendar_description,
    DAV_CALENDAR_PROPID_calendar_home_set,
/*
    DAV_CALENDAR_PROPID_calendar_timezone,
*/
    DAV_CALENDAR_PROPID_getctag,
    DAV_CALENDAR_PROPID_max_attendees_per_instance,
    DAV_CALENDAR_PROPID_max_date_time,
    DAV_CALENDAR_PROPID_max_instances,
    DAV_CALENDAR_PROPID_max_resource_size,
    DAV_CALENDAR_PROPID_min_date_time,
/*
    DAV_CALENDAR_PROPID_supported_calendar_component_set,
*/
    DAV_CALENDAR_PROPID_supported_calendar_data,
    DAV_CALENDAR_PROPID_supported_collation_set
};

static const dav_liveprop_spec dav_calendar_props[] =
{
    /* standard calendar properties */
    {
        DAV_CALENDAR_URI_DAV,
        "calendar-data",
        DAV_CALENDAR_PROPID_calendar_data,
        0
    },
    {
        DAV_CALENDAR_URI_DAV,
        "calendar-description",
        DAV_CALENDAR_PROPID_calendar_description,
        0
    },
    {
        DAV_CALENDAR_URI_DAV,
        "calendar-home-set",
        DAV_CALENDAR_PROPID_calendar_home_set,
        0
    },
/*
    {
        DAV_CALENDAR_URI_DAV,
        "calendar-timezone",
        DAV_CALENDAR_PROPID_calendar_timezone,
        0
    },
*/
    {
        DAV_CALENDAR_URI_DAV,
        "getctag",
        DAV_CALENDAR_PROPID_getctag,
        0
    },
    {
        DAV_CALENDAR_URI_DAV,
        "max-attendees-per-instance",
        DAV_CALENDAR_PROPID_max_attendees_per_instance,
        0
    },
    {
        DAV_CALENDAR_URI_DAV,
        "max-date-time",
        DAV_CALENDAR_PROPID_max_date_time,
        0
    },
    {
        DAV_CALENDAR_URI_DAV,
        "max-instances",
        DAV_CALENDAR_PROPID_max_instances,
        0
    },
    {
        DAV_CALENDAR_URI_DAV,
        "max-resource-size",
        DAV_CALENDAR_PROPID_max_resource_size,
        0
    },
    {
        DAV_CALENDAR_URI_DAV,
        "min-date-time",
        DAV_CALENDAR_PROPID_min_date_time,
        0
    },
/*
    {
        DAV_CALENDAR_URI_DAV,
        "supported-calendar-component-set",
        DAV_CALENDAR_PROPID_supported_calendar_component_set,
        0
    },
*/
    {
        DAV_CALENDAR_URI_DAV,
        "supported-calendar-data",
        DAV_CALENDAR_PROPID_supported_calendar_data,
        0
    },
    {
        DAV_CALENDAR_URI_DAV,
        "supported-collation-set",
        DAV_CALENDAR_PROPID_supported_collation_set,
        0
    },

    { 0 }        /* sentinel */
};

static const dav_liveprop_group dav_calendar_liveprop_group =
{
    dav_calendar_props,
    dav_calendar_namespace_uris,
    &dav_hooks_liveprop_calendar
};

typedef struct dav_calendar_ctx {
    request_rec *r;
    apr_bucket_brigade *bb;
    dav_error *err;
    icalparser *parser;
    icalcomponent *comp;
    const apr_xml_doc *doc;
    const apr_xml_elem *elem;
    apr_sha1_ctx_t *sha1;
    int ns;
    int match;
} dav_calendar_ctx;

static apr_status_t icalparser_cleanup(void *data)
{
    icalparser *comp = data;
    icalparser_free(comp);
    return APR_SUCCESS;
}

static apr_status_t icalcomponent_cleanup(void *data)
{
    icalcomponent *comp = data;
    icalcomponent_free(comp);
    return APR_SUCCESS;
}

static char dav_calendar_ascii_toupper(char c)
{
    /* ascii only, ignore locale */
    return c < 0 ? c : c | ' ';
}

static int dav_calendar_text_match_ascii_casecmp(const char *match,
        const char *text)
{
    /* https://tools.ietf.org/html/rfc4790#section-9.2 */

    while (*text) {
        const char *smatch = match;
        const char *stext = text;

        while (*stext
                && dav_calendar_ascii_toupper(*smatch)
                        != dav_calendar_ascii_toupper(*stext)) {
            stext++;
        }

        while (*stext && *smatch
                && dav_calendar_ascii_toupper(*smatch)
                        == dav_calendar_ascii_toupper(*stext)) {
            stext++;
            smatch++;
        }

        if (*smatch == 0) {
            return 1;
        }

        text++;
    }

    return 0;
}

static int dav_calendar_text_match_octet(const char *match, const char *text)
{
    /* https://tools.ietf.org/html/rfc4790#section-9.3 */

    /*
     * The ordering algorithm is as follows:
     *
     * 1.  If both strings are the empty string, return the result "equal".
     *
     * 2.  If the first string is empty and the second is not, return the
     *     result "less".
     *
     * 3.  If the second string is empty and the first is not, return the
     *     result "greater".
     *
     * 4.  If both strings begin with the same octet value, remove the first
     *     octet from both strings and repeat this algorithm from step 1.
     *
     * 5.  If the unsigned value (0 to 255) of the first octet of the first
     *     string is less than the unsigned value of the first octet of the
     *     second string, then return "less".
     * 6.  If this step is reached, return "greater".
     *
     * The matching operation returns "match" if the sorting algorithm would
     * return "equal".  Otherwise, the matching operation returns "no-
     * match".
     *
     * The substring operation returns "match" if the first string is the
     * empty string, or if there exists a substring of the second string of
     * length equal to the length of the first string, which would result in
     * a "match" result from the equality function.  Otherwise, the
     * substring operation returns "no-match".
     */

    if (strstr(text, match)) {
        return 1;
    }

    return 0;
}

static dav_error *dav_calendar_text_match(dav_calendar_ctx *ctx,
        const apr_xml_elem *timezone, const apr_xml_elem *text_match,
        const char *text)
{
    dav_error *err;

    const apr_xml_attr *collation, *negate_condition;

    const char *match;
    int negate;

    /* we already matched? */
    if (ctx->match) {
        return NULL;
    }

    /*
     * <!ELEMENT text-match (#PCDATA)>
     *   PCDATA value: string
     *
     * <!ATTLIST text-match collation        CDATA "i;ascii-casemap"
     *                      negate-condition (yes | no) "no">
     */

    match = dav_xml_get_cdata(text_match, ctx->r->pool, 1 /* strip_white */);

    negate_condition = dav_find_attr_ns(text_match, APR_XML_NS_NONE,
            "negate-condition");
    if (!negate_condition || !negate_condition->value
            || !strcmp(negate_condition->value,
                    "no")) {
        negate = 0;
    }
    else if (!strcmp(negate_condition->value,
            "yes")) {
        negate = 1;
    }
    else {

        /* MUST violation */
        err = dav_new_error(ctx->r->pool, HTTP_FORBIDDEN, 0,
                APR_SUCCESS,
                "Negate-condition attribute must contain "
                "yes or no.");
        err->tagname = "CALDAV:valid-filter";

        return err;
    }

    collation = dav_find_attr_ns(text_match, APR_XML_NS_NONE, "collation");
    if (collation) {

        if (!collation || !collation->value
                || !strcmp(collation->value,
                DAV_CALENDAR_COLLATION_ASCII_CASEMAP)) {

            if (dav_calendar_text_match_ascii_casecmp(match, text)) {

                if (!negate) {

                    /* we have a match! */
                    ctx->match = 1;

                }

            }
            else {

                if (negate) {

                    /* we have a match! */
                    ctx->match = 1;

                }

            }

        }
        else if (!strcmp(collation->value,
                DAV_CALENDAR_COLLATION_OCTET)) {

            if (dav_calendar_text_match_octet(match, text)) {

                if (!negate) {

                    /* we have a match! */
                    ctx->match = 1;

                }

            }
            else {

                if (negate) {

                    /* we have a match! */
                    ctx->match = 1;

                }

            }

        }
        else {

            /* MUST violation */
            err = dav_new_error(ctx->r->pool, HTTP_FORBIDDEN, 0,
                    APR_SUCCESS,
                    "Collation attribute must contain "
                    DAV_CALENDAR_COLLATION_ASCII_CASEMAP " or "
                    DAV_CALENDAR_COLLATION_OCTET ".");
            err->tagname = "CALDAV:supported-collation";

            return err;
        }
    }

    return NULL;
}

static struct icaltimetype dav_calendar_get_datetime_with_component(
        icalproperty *prop, icalcomponent *comp)
{
    icalcomponent *cp;
    icalparameter *param;

    struct icaltimetype ret;

    ret = icalvalue_get_datetime(icalproperty_get_value(prop));

    if (icaltime_is_utc(ret)) {
        return ret;
    }

    if ((param = icalproperty_get_first_parameter(prop, ICAL_TZID_PARAMETER)) != NULL) {
        const char *tzid = icalparameter_get_tzid(param);
        icaltimezone *tz = NULL;

        if (!comp) {
            comp = icalproperty_get_parent(prop);
        }

        for (cp = comp; cp; cp = icalcomponent_get_parent(cp)) {

            tz = icalcomponent_get_timezone(cp, tzid);
            if (tz) {
                break;
            }
        }

        if (!tz) {
            tz = icaltimezone_get_builtin_timezone_from_tzid(tzid);
        }

        if (!tz) {
            tz = icaltimezone_get_builtin_timezone(tzid);
        }

        if (tz) {
            ret = icaltime_set_timezone(&ret, tz);
        }
    }

    return ret;
}

static dav_error *dav_calendar_time_range(dav_calendar_ctx *ctx,
        const apr_xml_elem *time_range, icaltimetype **stt, icaltimetype **ett)
{
    dav_error *err;

    const apr_xml_attr *start, *end;

    /* we already matched? */
    if (ctx->match) {
        return NULL;
    }

    /*
     * <!ELEMENT time-range EMPTY>
     *
     * <!ATTLIST time-range start CDATA #IMPLIED
     *                      end   CDATA #IMPLIED>
     * start value: an iCalendar "date with UTC time"
     * end value: an iCalendar "date with UTC time"
     */

    *stt = apr_palloc(ctx->r->pool, sizeof(icaltimetype));

    start = dav_find_attr_ns(time_range, APR_XML_NS_NONE, "start");
    if (!start) {
        **stt = icaltime_from_string("00000101000000Z");
    }
    else {
        **stt = icaltime_from_string(start->value);
        if (icalerrno != ICAL_NO_ERROR) {
            err = dav_new_error(ctx->r->pool, HTTP_FORBIDDEN, 0,
                    APR_EGENERAL, icalerror_perror());
            err->tagname = "CALDAV:valid-filter";
            return err;
        }
    }

    *ett = apr_palloc(ctx->r->pool, sizeof(icaltimetype));

    end = dav_find_attr_ns(time_range, APR_XML_NS_NONE, "end");
    if (!end) {
        **ett = icaltime_from_string("99991231235959Z");
    }
    else {
        **ett = icaltime_from_string(end->value);
        if (icalerrno != ICAL_NO_ERROR) {
            err = dav_new_error(ctx->r->pool, HTTP_FORBIDDEN, 0,
                    APR_EGENERAL, icalerror_perror());
            err->tagname = "CALDAV:valid-filter";
            return err;
        }
    }

    if (!start && !end) {
        /* MUST violation */
        err = dav_new_error(ctx->r->pool, HTTP_FORBIDDEN, 0,
                APR_SUCCESS,
                "Start and/or end attribute must exist in time-range");
        err->tagname = "CALDAV:valid-filter";
        return err;
    }

    return NULL;
}

static dav_error *dav_calendar_prop_time_range(dav_calendar_ctx *ctx,
        const apr_xml_elem *timezone, icalcomponent *comp, icalproperty *prop,
        icaltimetype *stt, icaltimetype *ett)
{

    icaltimetype time;
    icaltime_span test;
    icaltime_span span;

    /* we already matched? */
    if (ctx->match) {
        return NULL;
    }

    switch (icalproperty_isa(prop)) {
    case ICAL_DTEND_PROPERTY:

        time = icalcomponent_get_dtend(comp);

        break;
    case ICAL_DUE_PROPERTY:

        time = icalcomponent_get_due(comp);

        break;
    case ICAL_DTSTART_PROPERTY:

        time = icalcomponent_get_dtstart(comp);

        break;

    case ICAL_DTSTAMP_PROPERTY:
        time = icalcomponent_get_dtstamp(comp);

        break;
    case ICAL_COMPLETED_PROPERTY:
    case ICAL_CREATED_PROPERTY:
    case ICAL_LASTMODIFIED_PROPERTY:

        time = dav_calendar_get_datetime_with_component(prop, comp);

        break;
    default:
        time = icaltime_null_time();
    }

    test = icaltime_span_new(time, time, 0);
    span = icaltime_span_new(*stt, *ett, 0);

    if (icalproperty_recurrence_is_excluded(comp, &time, &time) ||
            icaltime_span_overlaps(&test, &span)) {

        /* we have a match! */
        ctx->match = 1;

    }

    return NULL;
}

static void dav_calendar_alarm_callback(icalcomponent *comp,
        struct icaltime_span *span, void *data)
{
    dav_calendar_ctx *ctx = data;

    /* we have a match! */
    ctx->match = 1;

}

static void dav_calendar_event_callback(icalcomponent *comp,
        struct icaltime_span *span, void *data)
{
    dav_calendar_ctx *ctx = data;

    /* we have a match! */
    ctx->match = 1;

}

static dav_error *dav_calendar_comp_time_range(dav_calendar_ctx *ctx,
        const apr_xml_elem *timezone,
        icalcomponent *comp, icaltimetype *stt, icaltimetype *ett)
{

    /* we already matched? */
    if (ctx->match) {
        return NULL;
    }

    switch (icalcomponent_isa(comp)) {

    case ICAL_VEVENT_COMPONENT: {

        /*
         * A VEVENT component overlaps a given time range if the condition
         * for the corresponding component state specified in the table below
         * is satisfied.  Note that, as specified in [RFC2445], the DTSTART
         * property is REQUIRED in the VEVENT component.  The conditions
         * depend on the presence of the DTEND and DURATION properties in the
         * VEVENT component.  Furthermore, the value of the DTEND property
         *
         * MUST be later in time than the value of the DTSTART property.  The
         * duration of a VEVENT component with no DTEND and DURATION
         * properties is 1 day (+P1D) when the DTSTART is a DATE value, and 0
         * seconds when the DTSTART is a DATE-TIME value.
         *
         * +---------------------------------------------------------------+
         * | VEVENT has the DTEND property?                                |
         * |   +-----------------------------------------------------------+
         * |   | VEVENT has the DURATION property?                         |
         * |   |   +-------------------------------------------------------+
         * |   |   | DURATION property value is greater than 0 seconds?    |
         * |   |   |   +---------------------------------------------------+
         * |   |   |   | DTSTART property is a DATE-TIME value?            |
         * |   |   |   |   +-----------------------------------------------+
         * |   |   |   |   | Condition to evaluate                         |
         * +---+---+---+---+-----------------------------------------------+
         * | Y | N | N | * | (start <  DTEND AND end > DTSTART)            |
         * +---+---+---+---+-----------------------------------------------+
         * | N | Y | Y | * | (start <  DTSTART+DURATION AND end > DTSTART) |
         * |   |   +---+---+-----------------------------------------------+
         * |   |   | N | * | (start <= DTSTART AND end > DTSTART)          |
         * +---+---+---+---+-----------------------------------------------+
         * | N | N | N | Y | (start <= DTSTART AND end > DTSTART)          |
         * +---+---+---+---+-----------------------------------------------+
         * | N | N | N | N | (start <  DTSTART+P1D AND end > DTSTART)      |
         * +---+---+---+---+-----------------------------------------------+
         */

        icalcomponent_foreach_recurrence(comp, *stt, *ett,
                dav_calendar_event_callback, ctx);

        break;
    }
    case ICAL_VTODO_COMPONENT: {

        /*
         * A VTODO component is said to overlap a given time range if the
         * condition for the corresponding component state specified in the
         * table below is satisfied.  The conditions depend on the presence
         * of the DTSTART, DURATION, DUE, COMPLETED, and CREATED properties
         * in the VTODO component.  Note that, as specified in [RFC2445], the
         * DUE value MUST be a DATE-TIME value equal to or after the DTSTART
         * value if specified.
         *
         * +-------------------------------------------------------------------+
         * | VTODO has the DTSTART property?                                   |
         * |   +---------------------------------------------------------------+
         * |   |   VTODO has the DURATION property?                            |
         * |   |   +-----------------------------------------------------------+
         * |   |   | VTODO has the DUE property?                               |
         * |   |   |   +-------------------------------------------------------+
         * |   |   |   | VTODO has the COMPLETED property?                     |
         * |   |   |   |   +---------------------------------------------------+
         * |   |   |   |   | VTODO has the CREATED property?                   |
         * |   |   |   |   |   +-----------------------------------------------+
         * |   |   |   |   |   | Condition to evaluate                         |
         * +---+---+---+---+---+-----------------------------------------------+
         * | Y | Y | N | * | * | (start  <= DTSTART+DURATION)  AND             |
         * |   |   |   |   |   | ((end   >  DTSTART)  OR                       |
         * |   |   |   |   |   |  (end   >= DTSTART+DURATION))                 |
         * +---+---+---+---+---+-----------------------------------------------+
         * | Y | N | Y | * | * | ((start <  DUE)      OR  (start <= DTSTART))  |
         * |   |   |   |   |   | AND                                           |
         * |   |   |   |   |   | ((end   >  DTSTART)  OR  (end   >= DUE))      |
         * +---+---+---+---+---+-----------------------------------------------+
         * | Y | N | N | * | * | (start  <= DTSTART)  AND (end >  DTSTART)     |
         * +---+---+---+---+---+-----------------------------------------------+
         * | N | N | Y | * | * | (start  <  DUE)      AND (end >= DUE)         |
         * +---+---+---+---+---+-----------------------------------------------+
         * | N | N | N | Y | Y | ((start <= CREATED)  OR  (start <= COMPLETED))|
         * |   |   |   |   |   | AND                                           |
         * |   |   |   |   |   | ((end   >= CREATED)  OR  (end   >= COMPLETED))|
         * +---+---+---+---+---+-----------------------------------------------+
         * | N | N | N | Y | N | (start  <= COMPLETED) AND (end  >= COMPLETED) |
         * +---+---+---+---+---+-----------------------------------------------+
         * | N | N | N | N | Y | (end    >  CREATED)                           |
         * +---+---+---+---+---+-----------------------------------------------+
         * | N | N | N | N | N | TRUE                                          |
         * +---+---+---+---+---+-----------------------------------------------+
         */

        icalcomponent_foreach_recurrence(comp, *stt, *ett,
                dav_calendar_event_callback, ctx);

        break;
    }
    case ICAL_VJOURNAL_COMPONENT: {

        /*
         * A VJOURNAL component overlaps a given time range if the condition
         * for the corresponding component state specified in the table below
         * is satisfied.  The conditions depend on the presence of the
         * DTSTART property in the VJOURNAL component and on whether the
         * DTSTART is a DATE-TIME or DATE value.  The effective "duration" of
         * a VJOURNAL component is 1 day (+P1D) when the DTSTART is a DATE
         * value, and 0 seconds when the DTSTART is a DATE-TIME value.
         *
         * +----------------------------------------------------+
         * | VJOURNAL has the DTSTART property?                 |
         * |   +------------------------------------------------+
         * |   | DTSTART property is a DATE-TIME value?         |
         * |   |   +--------------------------------------------+
         * |   |   | Condition to evaluate                      |
         * +---+---+--------------------------------------------+
         * | Y | Y | (start <= DTSTART)     AND (end > DTSTART) |
         * +---+---+--------------------------------------------+
         * | Y | N | (start <  DTSTART+P1D) AND (end > DTSTART) |
         * +---+---+--------------------------------------------+
         * | N | * | FALSE                                      |
         * +---+---+--------------------------------------------+
         */
        icaltime_span span = icalcomponent_get_span(comp);
        icaltime_span limit = icaltime_span_new(*stt, *ett, 1);

        if (icaltime_span_overlaps(&span, &limit)) {

            /* we have a match! */
            ctx->match = 1;

        }

        break;
    }
    case ICAL_VFREEBUSY_COMPONENT: {

        /*
         * A VFREEBUSY component overlaps a given time range if the condition
         * for the corresponding component state specified in the table below
         * is satisfied.  The conditions depend on the presence in the
         * VFREEBUSY component of the DTSTART and DTEND properties, and any
         * FREEBUSY properties in the absence of DTSTART and DTEND.  Any
         * DURATION property is ignored, as it has a special meaning when
         * used in a VFREEBUSY component.
         *
         * When only FREEBUSY properties are used, each period in each
         * FREEBUSY property is compared against the time range, irrespective
         * of the type of free busy information (free, busy, busy-tentative,
         * busy-unavailable) represented by the property.
         *
         *
         * +------------------------------------------------------+
         * | VFREEBUSY has both the DTSTART and DTEND properties? |
         * |   +--------------------------------------------------+
         * |   | VFREEBUSY has the FREEBUSY property?             |
         * |   |   +----------------------------------------------+
         * |   |   | Condition to evaluate                        |
         * +---+---+----------------------------------------------+
         * | Y | * | (start <= DTEND) AND (end > DTSTART)         |
         * +---+---+----------------------------------------------+
         * | N | Y | (start <  freebusy-period-end) AND           |
         * |   |   | (end   >  freebusy-period-start)             |
         * +---+---+----------------------------------------------+
         * | N | N | FALSE                                        |
         * +---+---+----------------------------------------------+
         */
        icaltime_span span = icalcomponent_get_span(comp);
        icaltime_span limit = icaltime_span_new(*stt, *ett, 1);

        if (icaltime_span_overlaps(&span, &limit)) {

            /* we have a match! */
            ctx->match = 1;

        }

        break;
    }
    case ICAL_VALARM_COMPONENT: {

        /*
         * A VALARM component is said to overlap a given time range if the
         * following condition holds:
         *
         *    (start <= trigger-time) AND (end > trigger-time)
         *
         * A VALARM component can be defined such that it triggers repeatedly.
         * Such a VALARM component is said to overlap a given time range if at
         * least one of its triggers overlaps the time range.
         */
        icalproperty *prop;
        struct icaltriggertype tr;
        struct icaldurationtype duration = icaldurationtype_null_duration();
        int repeat = 1;

        if ((prop = icalcomponent_get_first_property(comp,
                ICAL_TRIGGER_PROPERTY))) {

            tr = icalproperty_get_trigger(prop);;

            if (!icaltime_is_null_time(tr.time)) {

                /* simple time value - direct comparison */

                icaltime_span span = icaltime_span_new(tr.time, tr.time, 1);
                icaltime_span limit = icaltime_span_new(*stt, *ett, 1);

                if (icaltime_span_overlaps(&span, &limit)) {

                    /* we have a match! */
                    ctx->match = 1;

                }


            }
            else {

                /* this is fun - relative to the parent then */

                icaltimetype st = *stt;
                icaltimetype et = *ett;

                if ((prop = icalcomponent_get_first_property(comp,
                        ICAL_DURATION_PROPERTY))) {
                    duration = icalproperty_get_duration(prop);
                }

                if ((prop = icalcomponent_get_first_property(comp,
                        ICAL_REPEAT_PROPERTY))) {
                    repeat = icalproperty_get_repeat(prop) + 1;
                }

                icaltime_adjust(&st, 0, 0, 0,
                        icaldurationtype_as_int(duration) * repeat);
                icaltime_adjust(&et, 0, 0, 0,
                        icaldurationtype_as_int(duration) * repeat);

                icalcomponent_foreach_recurrence(icalcomponent_get_parent(comp),
                        st, et, dav_calendar_alarm_callback, ctx);

            }

        }

        break;
    }
    default:
        break;
    }

    /*
     * The calendar properties COMPLETED, CREATED, DTEND, DTSTAMP,
     * DTSTART, DUE, and LAST-MODIFIED overlap a given time range if the
     * following condition holds:
     *
     *     (start <= date-time) AND (end > date-time)
     *
     * Note that if DTEND is not present in a VEVENT, but DURATION is, then
     * the test should instead operate on the 'effective' DTEND, i.e.,
     * DTSTART+DURATION.  Similarly, if DUE is not present in a VTODO, but
     * DTSTART and DURATION are, then the test should instead operate on the
     * 'effective' DUE, i.e., DTSTART+DURATION.
     *
     * The semantic of CALDAV:time-range is not defined for any other
     * calendar components and properties.
     */

    return NULL;
}

static void dav_calendar_freebusy_callback(icalcomponent *comp,
        struct icaltime_span *span, void *data)
{
    icalcomponent *freebusy = data;
    icalproperty *prop;
    icalparameter *param;
    icaltimezone *utc_zone;

    enum icalproperty_status status;
    struct icalperiodtype period;

    status = icalcomponent_get_status(comp);

    utc_zone = icaltimezone_get_utc_timezone();

    if (span->is_busy) {

        period.start = icaltime_from_timet_with_zone(span->start, 0, utc_zone);
        period.end = icaltime_from_timet_with_zone(span->end, 0, utc_zone);
        period.duration = icaldurationtype_null_duration();

        prop = icalproperty_new_freebusy(period);
        param = icalparameter_new_fbtype(ICAL_FBTYPE_BUSY);
        icalproperty_add_parameter(prop, param);

        icalcomponent_add_property(freebusy, prop);
    }

    else if (status == ICAL_STATUS_TENTATIVE) {

        period.start = icaltime_from_timet_with_zone(span->start, 0, utc_zone);
        period.end = icaltime_from_timet_with_zone(span->end, 0, utc_zone);
        period.duration = icaldurationtype_null_duration();

        prop = icalproperty_new_freebusy(period);
        param = icalparameter_new_fbtype(ICAL_FBTYPE_BUSYTENTATIVE);
        icalproperty_add_parameter(prop, param);

        icalcomponent_add_property(freebusy, prop);
    }

}

static dav_error *dav_calendar_freebusy_time_range(dav_calendar_ctx *ctx,
        icalcomponent *comp, icaltimetype *stt, icaltimetype *ett)
{

    icalcomponent *freebusy, *cp, *next;

    /*
     * Only VEVENT components without a TRANSP property or with the TRANSP
     * property set to OPAQUE, and VFREEBUSY components SHOULD be considered
     * in generating the free busy time information.
     *
     * In the case of VEVENT components, the free or busy time type (FBTYPE)
     * of the FREEBUSY properties in the returned VFREEBUSY component SHOULD
     * be derived from the value of the TRANSP and STATUS properties, as
     * outlined in the table below:
     *
     *   +---------------------------++------------------+
     *   |          VEVENT           ||    VFREEBUSY     |
     *   +-------------+-------------++------------------+
     *   | TRANSP      | STATUS      || FBTYPE           |
     *   +=============+=============++==================+
     *   |             | CONFIRMED   || BUSY             |
     *   |             | (default)   ||                  |
     *   | OPAQUE      +-------------++------------------+
     *   | (default)   | CANCELLED   || FREE             |
     *   |             +-------------++------------------+
     *   |             | TENTATIVE   || BUSY-TENTATIVE   |
     *   |             +-------------++------------------+
     *   |             | x-name      || BUSY or          |
     *   |             |             || x-name           |
     *   +-------------+-------------++------------------+
     *   |             | CONFIRMED   ||                  |
     *   | TRANSPARENT | CANCELLED   || FREE             |
     *   |             | TENTATIVE   ||                  |
     *   |             | x-name      ||                  |
     *   +-------------+-------------++------------------+
     *
     */
    freebusy = icalcomponent_get_first_component(comp,
            ICAL_VFREEBUSY_COMPONENT);

    if (freebusy) {
        icalcomponent_remove_component(comp, freebusy);
    }
    else {
        freebusy = icalcomponent_new(ICAL_VFREEBUSY_COMPONENT);

        icalcomponent_add_property(freebusy, icalproperty_new_dtstart(*stt));
        icalcomponent_add_property(freebusy, icalproperty_new_dtend(*ett));
    }

    for (cp = icalcomponent_get_first_component(comp, ICAL_ANY_COMPONENT);
           cp; cp = next) {

        if (icalcomponent_isa(cp) == ICAL_VEVENT_COMPONENT) {

            icalcomponent_foreach_recurrence(comp,
                    *stt, *ett, dav_calendar_freebusy_callback, freebusy);

        }
        else if (icalcomponent_isa(cp) == ICAL_VTIMEZONE_COMPONENT) {
            continue;
        }

        next = icalcomponent_get_next_component(comp, ICAL_ANY_COMPONENT);

        icalcomponent_remove_component(comp, cp);
    }

    /*
     * If no calendar object resources are found to satisfy these
     * conditions, a VFREEBUSY component with no FREEBUSY property MUST be
     * returned.
     */
    if (icalcomponent_count_properties(freebusy, ICAL_FREEBUSY_PROPERTY)) {
        icalcomponent_add_component(comp, freebusy);
    }
    else {
        icalcomponent_free(freebusy);
    }

    return NULL;
}

static dav_error *dav_calendar_param_filter(dav_calendar_ctx *ctx,
        const apr_xml_elem *timezone, const apr_xml_elem *param_filter,
        icalproperty *prop, icalparameter *param, icaltimetype *stt,
        icaltimetype *ett)
{
    dav_error *err;

    const apr_xml_elem *is_not_defined = NULL;
    const apr_xml_elem *text_match = NULL;
    const apr_xml_elem *elem = NULL;
    const apr_xml_attr *name;

    /* we already matched? */
    if (ctx->match) {
        return NULL;
    }

    /*
     * <!ELEMENT param-filter (is-not-defined | text-match?)>
     *
     * <!ATTLIST param-filter name CDATA #REQUIRED>
     * name value: a property parameter name (e.g., PARTSTAT)
     */

    /* do children of param match param_filter? */

    int found = 0;
    while (param) {

        const char *prname;

        icalparameter_kind kind = icalparameter_isa(param);

        if (kind == ICAL_X_PARAMETER) {
            prname = icalparameter_get_xname(param);
        }
        else if (kind == ICAL_IANA_PARAMETER) {
            prname = icalparameter_get_iana_name(param);
        }

        elem = param_filter;

        while (elem) {

            name = dav_find_attr_ns(elem, APR_XML_NS_NONE, "name");
            if (!name) {
                /* MUST violation */
                err = dav_new_error(ctx->r->pool, HTTP_FORBIDDEN, 0,
                        APR_SUCCESS,
                        "Name attribute must exist in param-filter");
                err->tagname = "CALDAV:valid-filter";
                return err;
            }

            /* matched our name? found it */
            if (prname && name->value && !strcmp(prname, name->value)) {
                found = 1;
                break;
            }

            elem = dav_find_next_ns(elem, ctx->ns, "param-filter");
        }

        if (!found) {
            /* not found, no match yet, unless... */

            elem = param_filter;

            while (elem) {
                if (dav_find_child_ns(elem, ctx->ns, "is-not-defined")) {

                    /* we have a match! */
                    ctx->match = 1;

                    break;
                }

                elem = dav_find_next_ns(elem, ctx->ns, "param-filter");
            }

        } else {

            /* found, look at the next level */

            /* explicit is-not-defined? */
            if ((is_not_defined = dav_find_child_ns(elem, ctx->ns, "is-not-defined"))) {
                /* found, but we didn't want to find, so no match */

            }

            else {

                text_match = dav_find_child_ns(elem, ctx->ns, "text-match");
                if (text_match) {

                    const char *text =
                            icalparameter_enum_to_string(icalparameter_get_value(param));

                    if (!text) {
                        text = icalparameter_get_xvalue(param);
                    }

                    err = dav_calendar_text_match(ctx, timezone, text_match, text);
                    if (err) {
                        return err;
                    }

                }

                /* none of the above? we have a match */
                if (!stt && !ett && !text_match) {

                    /* we have a match! */
                    ctx->match = 1;

                }

            }
        }

        param = icalproperty_get_next_parameter(prop,
                ICAL_ANY_PARAMETER);
    }

    return NULL;
}

static dav_error *dav_calendar_prop_filter(dav_calendar_ctx *ctx,
        const apr_xml_elem *timezone, const apr_xml_elem *prop_filter,
        icalcomponent *comp, icalproperty *prop, icaltimetype *stt,
        icaltimetype *ett)
{
    dav_error *err;

    const apr_xml_elem *param_filter = NULL;
    const apr_xml_elem *is_not_defined = NULL;
    const apr_xml_elem *time_range = NULL;
    const apr_xml_elem *text_match = NULL;
    const apr_xml_elem *elem = NULL;
    const apr_xml_attr *name;

    const char *ppname;

    /* we already matched? */
    if (ctx->match) {
        return NULL;
    }

    /*
     * <!ELEMENT prop-filter (is-not-defined |
     *                        ((time-range | text-match)?,
     *                         param-filter*))>
     *
     * <!ATTLIST prop-filter name CDATA #REQUIRED>
     * name value: a calendar property name (e.g., ATTENDEE)
     */

    /* do children of prop match prop_filter? */

    int found = 0;
    while (prop) {

        ppname = icalproperty_get_property_name(prop);

        elem = prop_filter;

        while (elem) {

            name = dav_find_attr_ns(elem, APR_XML_NS_NONE, "name");
            if (!name) {
                /* MUST violation */
                err = dav_new_error(ctx->r->pool, HTTP_FORBIDDEN, 0,
                        APR_SUCCESS,
                        "Name attribute must exist in prop-filter");
                err->tagname = "CALDAV:valid-filter";
                return err;
            }

            /* matched our name? found it */
            if (ppname && name->value && !strcmp(ppname, name->value)) {
                found = 1;
                break;
            }

            elem = dav_find_next_ns(elem, ctx->ns, "prop-filter");
        }

        if (!found) {
            /* not found, no match yet, unless... */

            elem = prop_filter;

            while (elem) {
                if (dav_find_child_ns(elem, ctx->ns, "is-not-defined")) {

                    /* we have a match! */
                    ctx->match = 1;

                    break;
                }

                elem = dav_find_next_ns(elem, ctx->ns, "prop-filter");
            }

        } else {

            /* found, look at the next level */

            /* explicit is-not-defined? */
            if ((is_not_defined = dav_find_child_ns(elem, ctx->ns, "is-not-defined"))) {
                /* found, but we didn't want to find, so no match */

            }

            else {

                if ((time_range = dav_find_child_ns(elem, ctx->ns, "time-range"))) {

                    err = dav_calendar_time_range(ctx, time_range, &stt, &ett);
                    if (err) {
                        return err;
                    }

                }

                if ((text_match = dav_find_child_ns(elem, ctx->ns, "text-match"))) {

                    const char *text = icalproperty_get_value_as_string(prop);

                    err = dav_calendar_text_match(ctx, timezone, text_match, text);
                    if (err) {
                        return err;
                    }

                }

                if ((param_filter = dav_find_child_ns(elem, ctx->ns,
                        "param-filter"))) {

                    err = dav_calendar_param_filter(ctx, timezone, param_filter,
                            prop, icalproperty_get_first_parameter(prop,
                                    ICAL_ANY_PARAMETER), stt, ett);
                    if (err) {
                        return err;
                    }
                }

                if (stt && ett) {

                    err = dav_calendar_prop_time_range(ctx, timezone, comp, prop,
                            stt, ett);
                    if (err) {
                        return err;
                    }

                }

                /* none of the above? we have a match */
                if (!stt && !ett && !time_range && !text_match && !param_filter) {

                    /* we have a match! */
                    ctx->match = 1;

                }

            }
        }

        prop = icalcomponent_get_next_property(comp,
                ICAL_ANY_PROPERTY);
    }

    return NULL;
}

static dav_error *dav_calendar_comp_filter(dav_calendar_ctx *ctx,
        const apr_xml_elem *timezone, const apr_xml_elem *comp_filter,
        icalcomponent *comp, icaltimetype *stt, icaltimetype *ett)
{
    dav_error *err;

    const apr_xml_elem *prop_filter = NULL;
    const apr_xml_elem *is_not_defined = NULL;
    const apr_xml_elem *time_range = NULL;
    const apr_xml_elem *elem = NULL;
    const apr_xml_attr *name;

    /* we already matched? */
    if (ctx->match) {
        return NULL;
    }

    /*
     * <!ELEMENT comp-filter (is-not-defined | (time-range?,
     *                        prop-filter*, comp-filter*))>
     *
     * <!ATTLIST comp-filter name CDATA #REQUIRED>
     * name value: a calendar object or calendar component
     *             type (e.g., VEVENT)
     */

    /* do children of comp match comp_filter? */
    int found = 0;
    while (comp) {

        icalcomponent_kind ev = icalcomponent_isa(comp);

        elem = comp_filter;

        while (elem) {

            name = dav_find_attr_ns(elem, APR_XML_NS_NONE, "name");
            if (!name) {
                /* MUST violation */
                err = dav_new_error(ctx->r->pool, HTTP_FORBIDDEN, 0,
                        APR_SUCCESS,
                        "Name attribute must exist in comp-filter");
                err->tagname = "CALDAV:valid-filter";
                return err;
            }

            /*
             * Bug: https://github.com/libical/libical/issues/433
             *
             * There is no way to get the component name, and so we cannot
             * support filtering of experimental components.
             */
            /* no kind match? leave with no match */
            if (ev == icalcomponent_string_to_kind((char *) name->value)) {
                found = 1;
                break;
            }

            elem = dav_find_next_ns(elem, ctx->ns, "comp-filter");
        }

        if (!found) {
            /* not found, no match yet, unless... */

            elem = comp_filter;

            while (elem) {
                if (dav_find_child_ns(elem, ctx->ns, "is-not-defined")) {

                    /* we have a match! */
                    ctx->match = 1;

                    break;
                }

                elem = dav_find_next_ns(elem, ctx->ns, "comp-filter");
            }

        } else {

            /* found, look at the next level */

            /* explicit is-not-defined? */
            if ((is_not_defined = dav_find_child_ns(elem, ctx->ns, "is-not-defined"))) {
                /* found, but we didn't want to find, so no match */

            }

            else {

                if ((time_range = dav_find_child_ns(elem, ctx->ns, "time-range"))) {

                    err = dav_calendar_time_range(ctx, time_range, &stt, &ett);
                    if (err) {
                        return err;
                    }

                }

                if ((prop_filter = dav_find_child_ns(elem, ctx->ns,
                        "prop-filter"))) {

                    err = dav_calendar_prop_filter(ctx, timezone, prop_filter,
                            comp, icalcomponent_get_first_property(comp,
                                    ICAL_ANY_PROPERTY), stt, ett);
                    if (err) {
                        return err;
                    }
                }

                if ((comp_filter = dav_find_child_ns(elem, ctx->ns,
                        "comp-filter"))) {

                    err = dav_calendar_comp_filter(ctx, timezone, comp_filter,
                            icalcomponent_get_first_component(comp,
                                    ICAL_ANY_COMPONENT), stt, ett);
                    if (err) {
                        return err;
                    }
                }

                if (stt && ett && !comp_filter && !prop_filter) {

                    if (icalcomponent_isa(comp) == ICAL_VCALENDAR_COMPONENT) {

                        comp = icalcomponent_get_first_component(comp,
                                                        ICAL_ANY_COMPONENT);
                        while (comp) {

                            err = dav_calendar_comp_time_range(ctx, timezone, comp,
                                    stt, ett);
                            if (err) {
                                return err;
                            }

                            comp = icalcomponent_get_next_component(comp,
                                    ICAL_ANY_COMPONENT);
                        }
                    }
                    else {

                        err = dav_calendar_comp_time_range(ctx, timezone, comp, stt,
                                ett);
                        if (err) {
                            return err;
                        }

                    }

                }

                /* none of the above? we have a match */
                if (!stt && !ett && !time_range && !prop_filter && !comp_filter) {

                    /* we have a match! */
                    ctx->match = 1;

                }
            }
        }

        comp = icalcomponent_get_next_component(comp,
                ICAL_ANY_COMPONENT);
    }

    return NULL;
}

static dav_error *dav_calendar_filter(dav_calendar_ctx *ctx, icalcomponent *comp)
{
    dav_error *err;

    const apr_xml_doc *doc = NULL;
    const apr_xml_elem *filter = NULL;
    const apr_xml_elem *comp_filter = NULL;
    const apr_xml_elem *timezone = NULL;

    if (!ctx->doc) {
        return NULL;
    }

    doc = ctx->doc;

    /*
     * <!ELEMENT calendar-query ((DAV:allprop |
     *                            DAV:propname |
     *                            DAV:prop)?, filter, timezone?)>
     */
    if (dav_validate_root_ns(doc, ctx->ns, "calendar-query")) {

        /*
         * <!ELEMENT filter (comp-filter)>
         */
        if ((filter = dav_find_child_ns(doc->root, ctx->ns, "filter")) == NULL) {
            /* MUST violation */
            err = dav_new_error(ctx->r->pool, HTTP_FORBIDDEN, 0, APR_SUCCESS,
                    "Filter element must exist beneath calendar-query");
            err->tagname = "CALDAV:valid-filter";
            return err;
        }

        timezone = dav_find_child_ns(doc->root, ctx->ns, "timezone");
        if (timezone) {

            icalcomponent *tz = icalparser_parse_string(
                    dav_xml_get_cdata(timezone, ctx->r->pool,
                            1 /* strip_white */));
            if(icalerrno != ICAL_NO_ERROR) {
                if (tz) {
                    icalcomponent_free(tz);
                }
                err = dav_new_error(ctx->r->pool, HTTP_FORBIDDEN, 0, APR_SUCCESS,
                        icalerror_perror());
                err->tagname = "CALDAV:valid-filter";
                return err;
            }

            icalcomponent_merge_component(comp, tz);
        }

        if ((comp_filter = dav_find_child_ns(filter, ctx->ns, "comp-filter")) == NULL) {
            /* MUST violation */
            err = dav_new_error(ctx->r->pool, HTTP_FORBIDDEN, 0, APR_SUCCESS,
                    "Comp-filter element must exist beneath filter element");
            err->tagname = "CALDAV:valid-filter";
            return err;
        }

        if ((err = dav_calendar_comp_filter(ctx, timezone, comp_filter, comp,
                NULL, NULL))) {
            return err;
        }

        return NULL;
    }

    else if (dav_validate_root_ns(doc, ctx->ns, "calendar-multiget")) {

        /* no filters on multiget */
        ctx->match = 1;

        return NULL;
    }

    else if (dav_validate_root_ns(doc, ctx->ns, "free-busy-query")) {

        icaltimetype *stt;
        icaltimetype *ett;

        const apr_xml_elem *time_range = NULL;

        if ((time_range = dav_find_child_ns(doc->root, ctx->ns, "time-range"))) {

            err = dav_calendar_time_range(ctx, time_range, &stt, &ett);
            if (err) {
                return err;
            }

        }
        else {
            /* MUST violation */
            err = dav_new_error(ctx->r->pool, HTTP_FORBIDDEN, 0, APR_SUCCESS,
                    "Time-range element must exist beneath free-busy-query element");
            err->tagname = "CALDAV:valid-filter";
            return err;
        }

        if ((err = dav_calendar_freebusy_time_range(ctx, comp, stt, ett))) {
            return err;
        }

        return NULL;
    }

    /* MUST violation */
    err = dav_new_error(ctx->r->pool, HTTP_FORBIDDEN, 0, APR_SUCCESS,
            "Root element not validated");
    err->tagname = "CALDAV:valid-filter";
    return err;
}

/* filter by <C:prop/> beneath calendar-data */
static dav_error *dav_calendar_prop(dav_calendar_ctx *ctx,
        const apr_xml_elem *parent, icalcomponent *icomp)
{
    dav_error *err;

    icalproperty *cp, *next;
    const char *pname;

    const apr_xml_elem *elem = NULL;
    const apr_xml_attr *name, *novalue;

    /* anything to filter? */
    elem = dav_find_child_ns(parent, ctx->ns, "allprop");
    if (elem) {
        return NULL;
    }

    elem = dav_find_child_ns(parent, ctx->ns, "prop");
    if (!elem) {
        return NULL;
    }

    for (cp = icalcomponent_get_first_property(icomp, ICAL_ANY_PROPERTY);
            cp != NULL; cp = next) {

        next = icalcomponent_get_next_property(icomp, ICAL_ANY_PROPERTY);

        pname = icalproperty_get_property_name(cp);

        elem = dav_find_child_ns(parent, ctx->ns, "prop");
        if (elem) {
            int found = 0;
            while (elem) {

                name = dav_find_attr_ns(elem, APR_XML_NS_NONE, "name");
                if (!name) {
                    /* MUST violation */
                    err = dav_new_error(ctx->r->pool, HTTP_FORBIDDEN, 0, APR_SUCCESS,
                            "Name attribute must exist in prop");
                    err->tagname = "CALDAV:valid-filter";
                    return err;
                }

                novalue = dav_find_attr_ns(elem, APR_XML_NS_NONE, "novalue");

                if (pname && name->value && !strcmp(pname, name->value)) {
                    found = 1;
                    break;
                }

                elem = dav_find_next_ns(elem, ctx->ns, "prop");
            }

            if (!found) {
                /* not found, strip the property */

                icalcomponent_remove_property(icomp, cp);
                icalproperty_free(cp);

            }
            else {
                /* found, strip the value? */

                if (novalue && !strcasecmp(novalue->value, "yes")) {
                    icalvalue *v = icalproperty_get_value(cp);

                    if (v) {
                        v = icalvalue_new_from_string(icalvalue_isa(v), "");

                        icalproperty_set_value(cp, v);
                    }
                }

            }
        }
    }

    return NULL;
}

/* filter by <C:comp/> beneath calendar-data */
static dav_error *dav_calendar_comp(dav_calendar_ctx *ctx,
        const apr_xml_elem *parent, icalcomponent **icomp)
{
    /*
     * We walk the ical component, and the <C:comp/> elements, and if
     * any <C:comp/> elements are found, we strip out everything outside
     * of the set of those elements.
     */

    dav_error *err;

    const apr_xml_elem *elem = NULL;
    const apr_xml_attr *name;

    icalcomponent *cm, *next;
    icalcomponent_kind ev = icalcomponent_isa(*icomp);

    int found = 0;

    /* anything to filter? */
    elem = dav_find_child_ns(parent, ctx->ns, "allcomp");
    if (elem) {
        return NULL;
    }

    elem = dav_find_child_ns(parent, ctx->ns, "comp");
    if (!elem) {
        return NULL;
    }

    while (elem) {

        name = dav_find_attr_ns(elem, APR_XML_NS_NONE, "name");
        if (!name) {
            /* MUST violation */
            err = dav_new_error(ctx->r->pool, HTTP_FORBIDDEN, 0, APR_SUCCESS,
                    "Name attribute must exist in comp");
            err->tagname = "CALDAV:valid-filter";
            return err;
        }

        /*
         * Bug: https://github.com/libical/libical/issues/433
         *
         * There is no way to get the component name, and so we cannot
         * support filtering of experimental components.
         */
        if (ev == icalcomponent_string_to_kind((char *) name->value)) {
            found = 1;
            break;
        }

        elem = dav_find_next_ns(elem, ctx->ns, "comp");
    }

    if (!found) {
        /* not found, strip it */

        icalcomponent *iparent = icalcomponent_get_parent(*icomp);
        if (iparent) {
            icalcomponent_remove_component(iparent, *icomp);
        } else {
            icalcomponent_free(*icomp);
            *icomp = NULL;
        }

    } else {
        /* found, look at the next level */

        err = dav_calendar_prop(ctx, elem, *icomp);
        if (err) {
            return err;
        }

        for (cm = icalcomponent_get_first_component(*icomp, ICAL_ANY_COMPONENT);
                cm != NULL; cm = next) {

            next = icalcomponent_get_next_component(*icomp, ICAL_ANY_COMPONENT);

            err = dav_calendar_comp(ctx, elem, &cm);
            if (err) {
                return err;
            }
        }
    }

    return NULL;
}

static apr_status_t dav_calendar_brigade_split_folded_line(apr_bucket_brigade *bbOut,
                                                           apr_bucket_brigade *bbIn,
                                                           apr_read_type_e block,
                                                           apr_off_t maxbytes)
{
    apr_off_t readbytes = 0;
    int state = 0;

    while (!APR_BRIGADE_EMPTY(bbIn)) {
        const char *pos = NULL;
        const char *str;
        apr_size_t len;
        apr_status_t rv;
        apr_bucket *e;

        e = APR_BRIGADE_FIRST(bbIn);
        rv = apr_bucket_read(e, &str, &len, block);

        if (rv != APR_SUCCESS) {
            return rv;
        }

        if (state == 0) {
            pos = memchr(str, APR_ASCII_CR, len);
            if (pos) {
                len = pos - str;
                apr_bucket_split(e, len);
                state = APR_ASCII_CR;
            }
            else {
                pos = memchr(str, APR_ASCII_LF, len);
                if (pos) {
                    len = pos - str;
                    apr_bucket_split(e, len);
                    state = APR_ASCII_LF;
                }
            }
        }

        else if (state == APR_ASCII_CR) {
            if (len) {
                if (*str == APR_ASCII_CR) {
                    apr_bucket_split(e, 1);
                    apr_bucket_delete(e);
                    state = APR_ASCII_LF;
                    continue;
                }
            }
        }

        else if (state == APR_ASCII_LF) {
            if (len) {
                if (*str == APR_ASCII_LF) {
                    apr_bucket_split(e, 1);
                    apr_bucket_delete(e);
                    state = APR_ASCII_BLANK;
                    continue;
                }
            }
        }

        else if (state == APR_ASCII_BLANK) {
            if (len) {
                if (*str == APR_ASCII_BLANK || *str == APR_ASCII_TAB) {
                    apr_bucket_split(e, 1);
                    apr_bucket_delete(e);
                    state = 0;
                    continue;
                }
                else {
                    return APR_SUCCESS;
                }
            }
        }

        readbytes += len;

        APR_BUCKET_REMOVE(e);
        if (APR_BUCKET_IS_METADATA(e) || len > APR_BUCKET_BUFF_SIZE/4) {
            APR_BRIGADE_INSERT_TAIL(bbOut, e);
        }
        else {
            if (len > 0) {
                rv = apr_brigade_write(bbOut, NULL, NULL, str, len);
                if (rv != APR_SUCCESS) {
                    return rv;
                }
            }
            apr_bucket_destroy(e);
        }
        /* We didn't find a CRLF within the maximum line length. */
        if (readbytes >= maxbytes) {
            break;
        }
    }

    return APR_SUCCESS;
}

static int dav_calendar_parse_icalendar_filter(ap_filter_t *f,
        apr_bucket_brigade *bb)
{
    dav_calendar_config_rec *conf = ap_get_module_config(f->r->per_dir_config,
            &dav_calendar_module);

    dav_calendar_ctx *ctx = f->ctx;

    icalcomponent *comp;

    apr_bucket *e;
    char *buffer;
    apr_size_t len = 0;
    apr_status_t rv = APR_SUCCESS;


    while (!APR_BRIGADE_EMPTY(bb)) {

        e = APR_BRIGADE_FIRST(bb);

        /* EOS means we are done. */
        if (APR_BUCKET_IS_EOS(e)) {
            break;
        }

        /* grab a line of max HUGE_STRING_LEN - RFC5545 says SHOULD be 75 chars, not MUST */
        if (APR_SUCCESS
                == (rv = dav_calendar_brigade_split_folded_line(ctx->bb, bb, 1, HUGE_STRING_LEN))) {
            apr_off_t offset = 0;
            apr_size_t size = 0;

            apr_brigade_length(ctx->bb, 1, &offset);

            if (offset >= HUGE_STRING_LEN) {
                ctx->err = dav_new_error(f->r->pool, HTTP_INTERNAL_SERVER_ERROR, 0, APR_EGENERAL,
                        "iCalendar line was too long - not a calendar?");
            }

            len += offset;

            if (len > conf->max_resource_size) {
                return APR_ENOSPC;
            }

            buffer = icalmemory_new_buffer(offset + 1);

            size = offset;
            if ((rv = apr_brigade_flatten(ctx->bb, buffer, &size)) != APR_SUCCESS) {
                icalmemory_free_buffer(buffer);
                return rv;
            }
            buffer[size] = 0;

            comp = icalparser_add_line(ctx->parser, buffer);
            if(icalerrno != ICAL_NO_ERROR) {
                ctx->err = dav_new_error(f->r->pool, HTTP_INTERNAL_SERVER_ERROR, 0, APR_EGENERAL,
                        icalerror_perror());
                return APR_EGENERAL;
            }

            /* found a calendar? */
            if (comp) {

                /* apply search <C:filter/>, ctx->match will contain the result */
                ctx->err = dav_calendar_filter(ctx, comp);
                if (ctx->err) {
                    icalcomponent_free(comp);
                    return APR_EGENERAL;
                }

                if (ctx->elem) {

                    /* strip away everything not listed beneath <C:comp/> */
                    ctx->err = dav_calendar_comp(ctx, ctx->elem,
                            &comp);
                    if (ctx->err) {
                        icalcomponent_free(comp);
                        return APR_EGENERAL;
                    }
                }

                if (!ctx->comp) {
                    ctx->comp = comp;
                    apr_pool_cleanup_register(f->r->pool, comp, icalcomponent_cleanup,
                            apr_pool_cleanup_null);
                }
                else {
                    icalcomponent_merge_component(ctx->comp, comp);
                }

            }

            apr_brigade_cleanup(ctx->bb);

        }
        else {
            return rv;
        }

    }

    return APR_SUCCESS;
}

static ap_filter_t *dav_calendar_create_parse_icalendar_filter(request_rec *r,
        dav_calendar_ctx *ctx)
{
    ap_filter_rec_t *rec = apr_pcalloc(r->pool, sizeof(ap_filter_rec_t));
    ap_filter_t *f = apr_pcalloc(r->pool, sizeof(ap_filter_t));
    ap_filter_func ff;

    /* just enough to bootstrap our filter */
    ff.out_func = dav_calendar_parse_icalendar_filter;
    rec->filter_func = ff;
    f->frec = rec;
    f->r = r;
    f->ctx = ctx;

    ctx->match = 0;

    if (ctx->doc && ctx->doc->namespaces) {
        ctx->ns = apr_xml_insert_uri(ctx->doc->namespaces,
                DAV_CALENDAR_XML_NAMESPACE);
    }
    ctx->bb = apr_brigade_create(r->pool, r->connection->bucket_alloc);

    ctx->parser = icalparser_new();

    apr_pool_cleanup_register(f->r->pool, ctx->parser, icalparser_cleanup,
            apr_pool_cleanup_null);

    return f;
}

static dav_prop_insert dav_calendar_insert_prop(const dav_resource *resource,
        int propid, dav_prop_insert what, apr_text_header *phdr)
{
    request_rec *r = resource->hooks->get_request_rec(resource);

    dav_calendar_config_rec *conf = ap_get_module_config(r->per_dir_config,
            &dav_calendar_module);

    apr_pool_t *p = resource->pool;
    const dav_liveprop_spec *info;
    int global_ns;

    switch (propid) {
    case DAV_CALENDAR_PROPID_calendar_data:
        /* property allowed only in a calendar-multiget */
        if (!r || r->method_number != M_REPORT) {
            return DAV_PROP_INSERT_NOTDEF;
        }

        break;
    case DAV_CALENDAR_PROPID_calendar_home_set:
        /* property allowed, handled below */

        break;
    case DAV_CALENDAR_PROPID_max_resource_size:
        /* property allowed, handled below */

        break;
    case DAV_CALENDAR_PROPID_supported_collation_set:
        /* property allowed, handled below */

    default:
        /* ### what the heck was this property? */
        return DAV_PROP_INSERT_NOTDEF;
    }

    /* assert: value != NULL */

    /* get the information and global NS index for the property */
    global_ns = dav_get_liveprop_info(propid, &dav_calendar_liveprop_group, &info);

    /* assert: info != NULL && info->name != NULL */

    if (what == DAV_PROP_INSERT_VALUE) {

        switch (propid) {
        case DAV_CALENDAR_PROPID_calendar_data: {
            dav_error *err;
            dav_liveprop_elem *element;
            dav_calendar_ctx ctx = { 0 };
            ctx.r = r;

            apr_pool_userdata_get((void **)&element, DAV_PROP_ELEMENT, resource->pool);

            if (element) {
                ctx.doc = element->doc;
                ctx.elem = element->elem;
            }

            /* we have to "deliver" the stream into an output filter */
            if (!resource->hooks->handle_get) {
                int status;

                request_rec *rr = ap_sub_req_method_uri("GET", resource->uri, r,
                        dav_calendar_create_parse_icalendar_filter(r, &ctx));

                ctx.r = rr;

                status = ap_run_sub_req(rr);
                if (status != OK) {

                    err = dav_push_error(r->pool, status, 0,
                                         "Unable to read calendar.",
                                         ctx.err);
                    dav_log_err(r, err, APLOG_ERR);

                    return DAV_PROP_INSERT_NOTDEF;
                }

            }

            /* mod_dav delivers the body */
            else if ((err = (*resource->hooks->deliver)(resource,
                    dav_calendar_create_parse_icalendar_filter(r, &ctx))) != NULL) {

                err = dav_push_error(r->pool, err->status, 0,
                                     "Unable to read calendar.",
                                     ctx.err);
                dav_log_err(r, err, APLOG_ERR);

                return DAV_PROP_INSERT_NOTDEF;
            }

            /* how did the parsing go? */
            if (ctx.err || !ctx.comp) {
                err = dav_push_error(r->pool, err->status, 0,
                                     "Unable to parse calendar.",
                                     ctx.err);
                dav_log_err(r, err, APLOG_ERR);

                return DAV_PROP_INSERT_NOTDEF;
            }

            // FIXME: if there is no match, we want the entire resource to vanish from results
            if (ctx.match && ctx.comp) {

                apr_text_append(p, phdr, apr_psprintf(p, "<lp%d:%s>",
                        global_ns, info->name));

                apr_text_append(p, phdr,
                        apr_pescape_entity(p,
                                icalcomponent_as_ical_string(ctx.comp), 0));

                apr_text_append(p, phdr, apr_psprintf(p, "</lp%d:%s>" DEBUG_CR,
                        global_ns, info->name));

            }

            break;
        }
        case DAV_CALENDAR_PROPID_calendar_home_set: {
            int i;

            apr_text_append(p, phdr, apr_psprintf(p, "<lp%d:%s>",
                    global_ns, info->name));

            for (i = 0; i < conf->dav_calendar_homes->nelts; ++i) {
                const char *err = NULL, *url;
                ap_expr_info_t *home = APR_ARRAY_IDX(conf->dav_calendar_homes, i, ap_expr_info_t *);

                url = ap_expr_str_exec(r, home, &err);
                if (err) {
                    ap_log_rerror(APLOG_MARK, APLOG_WARNING, 0, r,
                                    "Failure while evaluating the calendar-home-set URL expression for '%s', "
                                    "calendar home value ignored: %s", r->uri, err);
                }
                else {
                    apr_text_append(p, phdr,
                            apr_psprintf(p, "<D:href>%s</D:href>", url));
                }

            }

            apr_text_append(p, phdr, apr_psprintf(p, "</lp%d:%s>" DEBUG_CR,
                    global_ns, info->name));

            break;
        }
        case DAV_CALENDAR_PROPID_max_resource_size: {

            apr_text_append(p, phdr, apr_psprintf(p, "<lp%d:%s>",
                    global_ns, info->name));

            apr_text_append(p, phdr,
                    apr_psprintf(p, "<D:href>%" APR_OFF_T_FMT "</D:href>",
                            conf->max_resource_size));

            apr_text_append(p, phdr, apr_psprintf(p, "</lp%d:%s>" DEBUG_CR,
                    global_ns, info->name));

            break;
        }
        case DAV_CALENDAR_PROPID_supported_collation_set: {

            apr_text_append(p, phdr, apr_psprintf(p, "<lp%d:%s>",
                    global_ns, info->name));

            apr_text_append(p, phdr,
                    apr_psprintf(p,
                        "<lp%d:supported-collation>"
                        DAV_CALENDAR_COLLATION_ASCII_CASEMAP
                        "</lp%d:supported-collation>",
                        global_ns, global_ns));
            apr_text_append(p, phdr,
                    apr_psprintf(p,
                        "<lp%d:supported-collation>"
                        DAV_CALENDAR_COLLATION_OCTET
                        "</lp%d:supported-collation>",
                        global_ns, global_ns));

            apr_text_append(p, phdr, apr_psprintf(p, "</lp%d:%s>" DEBUG_CR,
                    global_ns, info->name));

            break;
        }
        default:
            break;
        }

    }
    else if (what == DAV_PROP_INSERT_NAME) {
        apr_text_append(p, phdr, apr_psprintf(p, "<lp%d:%s/>" DEBUG_CR, global_ns, info->name));
    }
    else {
        /* assert: what == DAV_PROP_INSERT_SUPPORTED */
        apr_text_append(p, phdr, "<D:supported-live-property D:name=\"");
        apr_text_append(p, phdr, info->name);
        apr_text_append(p, phdr, "\" D:namespace=\"");
        apr_text_append(p, phdr, dav_calendar_namespace_uris[info->ns]);
        apr_text_append(p, phdr, "\"/>" DEBUG_CR);
    }

    /* we inserted what was asked for */
    return what;
}

static int dav_calendar_is_writable(const dav_resource *resource, int propid)
{
    const dav_liveprop_spec *info;

    (void) dav_get_liveprop_info(propid, &dav_calendar_liveprop_group, &info);
    return info->is_writable;
}

static dav_error *dav_calendar_patch_validate(const dav_resource *resource,
    const apr_xml_elem *elem, int operation, void **context, int *defer_to_dead)
{
    /* We have no writable properties */
    return NULL;
}

static dav_error *dav_calendar_patch_exec(const dav_resource *resource,
    const apr_xml_elem *elem, int operation, void *context,
    dav_liveprop_rollback **rollback_ctx)
{
    /* We have no writable properties */
    return NULL;
}

static void dav_calendar_patch_commit(const dav_resource *resource, int operation,
    void *context, dav_liveprop_rollback *rollback_ctx)
{
    /* We have no writable properties */
}

static dav_error *dav_calendar_patch_rollback(const dav_resource *resource,
    int operation, void *context, dav_liveprop_rollback *rollback_ctx)
{
    /* We have no writable properties */
    return NULL;
}

static const dav_hooks_liveprop dav_hooks_liveprop_calendar =
{
    dav_calendar_insert_prop,
    dav_calendar_is_writable,
    dav_calendar_namespace_uris,
    dav_calendar_patch_validate,
    dav_calendar_patch_exec,
    dav_calendar_patch_commit,
    dav_calendar_patch_rollback
};

static int dav_calendar_find_liveprop(const dav_resource *resource,
    const char *ns_uri, const char *name, const dav_hooks_liveprop **hooks)
{
    return dav_do_find_liveprop(ns_uri, name, &dav_calendar_liveprop_group, hooks);
}

static dav_error *dav_calendar_options_header(request_rec *r,
        const dav_resource *resource, apr_text_header *phdr)
{
    apr_text_append(r->pool, phdr, "calendar-access");

    return NULL;
}

static dav_error *dav_calendar_options_method(request_rec *r,
        const dav_resource *resource, apr_text_header *phdr)
{
    apr_text_append(r->pool, phdr, "MKCALENDAR");
    apr_text_append(r->pool, phdr, "REPORT");

    return NULL;
}

static dav_options_provider options =
{
    dav_calendar_options_header,
    dav_calendar_options_method,
    NULL
};

static int dav_calendar_get_resource_type(const dav_resource *resource,
                                    const char **type, const char **uri)
{
    request_rec *r;

    const dav_provider *provider;
    dav_error *err;
    dav_lockdb *lockdb;
    dav_propdb *propdb;
    int result = DECLINED;

    *type = *uri = NULL;

    if (resource && resource->hooks && resource->hooks->get_request_rec) {
        r = resource->hooks->get_request_rec(resource);
    }
    else {
        return result;
    }

    /* find the dav provider */
    provider = dav_get_provider(r);
    if (provider == NULL) {
        return dav_handle_err(r, dav_new_error(r->pool, HTTP_METHOD_NOT_ALLOWED, 0, 0,
                apr_psprintf(r->pool,
                        "DAV not enabled for %s",
                        ap_escape_html(r->pool, r->uri))), NULL);
    }

    /* open lock database, to report on supported lock properties */
    /* ### should open read-only */
    if ((err = dav_open_lockdb(r, 0, &lockdb)) != NULL) {
        return dav_handle_err(r, dav_push_error(r->pool, err->status, 0,
                "The lock database could not be opened, "
                "cannot retrieve the resource type.",
                err), NULL);
    }

    /* open the property database (readonly) for the resource */
    if ((err = dav_open_propdb(r, lockdb, resource, 1, NULL,
                               &propdb)) != NULL) {
        if (lockdb != NULL) {
            dav_close_lockdb(lockdb);
        }

        return dav_handle_err(r, dav_push_error(r->pool, err->status, 0,
                "The property database could not be opened, "
                "cannot retrieve the resource type.",
                err), NULL);
    }

    if (propdb) {
        dav_db *db = NULL;
        const dav_prop_name prop = { "DAV:", "resourcetype" };
        dav_prop_name name[1] = { { NULL, NULL } };

        if ((err = provider->propdb->open(resource->pool, resource, 1, &db)) != NULL) {
            result = dav_handle_err(r, dav_push_error(r->pool, err->status, 0,
                    "Property database could not be opened, "
                    "cannot retrieve the resource type.",
                    err), NULL);
        }

        if (db) {
            if ((err = provider->propdb->first_name(db, name)) != NULL) {
                result = dav_handle_err(r, dav_push_error(r->pool, err->status, 0,
                        "Property could not be retrieved, "
                        "cannot retrieve the resource type.",
                        err), NULL);
            }
            else {

                while (name->ns != NULL) {
                    apr_text_header hdr[1] = { { 0 } };
                    int f;

                    if (name->name && prop.name && strcmp(name->name, prop.name) == 0
                            && ((name->ns && prop.ns && strcmp(name->ns, prop.ns) == 0)
                                    || (!name->ns && !prop.ns))) {

                        if ((err = provider->propdb->output_value(db, name, NULL, hdr, &f)) != NULL) {

                            result = dav_handle_err(r, dav_push_error(r->pool, err->status, 0,
                                    "Property value could not be retrieved, "
                                    "cannot retrieve the resource type.",
                                    err), NULL);

                            break;
                        }

                        if (strstr(hdr->first->text, ">calendar<")) {

                            *type = "calendar";
                            *uri = DAV_CALENDAR_XML_NAMESPACE;

                            result = OK;

                        }

                        break;
                    }
                    if ((err = provider->propdb->next_name(db, name)) != NULL) {

                        result = dav_handle_err(r, dav_push_error(r->pool, err->status, 0,
                                "Property could not be retrieved, "
                                "cannot retrieve the resource type.",
                                err), NULL);

                        break;
                    }

                }
                provider->propdb->close(db);
            }
        }

        dav_close_propdb(propdb);
    }

    if (lockdb != NULL) {
        dav_close_lockdb(lockdb);
    }

    return result;
}

static dav_resource_type_provider resource_types =
{
    dav_calendar_get_resource_type
};

static dav_error * dav_calendar_etag_walker(dav_walk_resource *wres, int calltype)
{
    dav_calendar_ctx *cctx = wres->walk_ctx;
    const char *etag;

    /* avoid loops */
    if (calltype != DAV_CALLTYPE_MEMBER) {
        return NULL;
    }

    etag = (*wres->resource->hooks->getetag)(wres->resource);

    if (etag) {
        if (cctx->sha1) {
            apr_sha1_update(cctx->sha1, etag, strlen(etag));
        }
    }
    else {
        cctx->sha1 = NULL;
    }

    return NULL;
}

static dav_error * dav_calendar_get_walker(dav_walk_resource *wres, int calltype)
{
    request_rec *r = wres->resource->hooks->get_request_rec(wres->resource);

    dav_calendar_ctx *cctx = wres->walk_ctx;
    dav_error *err;

    /* avoid loops */
    if (calltype != DAV_CALLTYPE_MEMBER) {
        return NULL;
    }

    err = cctx->err = NULL;

    /* check for any method preconditions */
    if (dav_run_method_precondition(cctx->r, NULL, wres->resource, NULL, &err) != DECLINED
            && err) {
        dav_log_err(r, err, APLOG_DEBUG);
        return NULL;
    }

    /* we have to "deliver" the stream into an output filter */
    if (!wres->resource->hooks->handle_get) {
        int status;

        request_rec *rr = ap_sub_req_method_uri("GET", wres->resource->uri, r,
                dav_calendar_create_parse_icalendar_filter(r, cctx));

        status = ap_run_sub_req(rr);
        if (status != OK) {

            err = dav_push_error(rr->pool, status, 0,
                    "Unable to read calendar.",
                    cctx->err);

        }
        ap_destroy_sub_req(rr);

    }

    /* mod_dav delivers the body */
    else if ((err = (*wres->resource->hooks->deliver)(wres->resource,
            dav_calendar_create_parse_icalendar_filter(r, cctx))) != NULL) {

        err = dav_push_error(r->pool, 0, 0,
                "Unable to read calendar.", err);

    }

    /* how did the parsing go? */
    if (!cctx->comp) {

        err = dav_push_error(r->pool, 0, 0,
                "Unable to parse calendar.",
                cctx->err);

    }

    if (err) {
        dav_log_err(r, err, APLOG_DEBUG);
    }

    return NULL;
}

/* Use POOL to temporarily construct a dav_response object (from WRES
   STATUS, and PROPSTATS) and stream it via WRES's ctx->brigade. */
static void dav_stream_response(dav_walk_resource *wres,
                                int status,
                                dav_get_props_result *propstats,
                                apr_pool_t *pool)
{
    dav_response resp = { 0 };
    dav_walker_ctx *ctx = wres->walk_ctx;

    resp.href = wres->resource->uri;
    resp.status = status;
    if (propstats) {
        resp.propresult = *propstats;
    }

    dav_send_one_response(&resp, ctx->bb, ctx->r, pool);
}

static void dav_calendar_cache_badprops(dav_walker_ctx *ctx)
{
    const apr_xml_elem *elem;
    apr_text_header hdr = { 0 };

    /* just return if we built the thing already */
    if (ctx->propstat_404 != NULL) {
        return;
    }

    apr_text_append(ctx->w.pool, &hdr,
                    "<D:propstat>" DEBUG_CR
                    "<D:prop>" DEBUG_CR);

    elem = dav_find_child(ctx->doc->root, "prop");
    for (elem = elem->first_child; elem; elem = elem->next) {
        apr_text_append(ctx->w.pool, &hdr,
                        apr_xml_empty_elem(ctx->w.pool, elem));
    }

    apr_text_append(ctx->w.pool, &hdr,
                    "</D:prop>" DEBUG_CR
                    "<D:status>HTTP/1.1 404 Not Found</D:status>" DEBUG_CR
                    "</D:propstat>" DEBUG_CR);

    ctx->propstat_404 = hdr.first;
}

static dav_error * dav_calendar_report_walker(dav_walk_resource *wres, int calltype)
{
    dav_walker_ctx *ctx = wres->walk_ctx;
    dav_error *err = NULL;
    dav_propdb *propdb;
    dav_get_props_result propstats = { 0 };

    /* ignore collections */
    if (wres->resource->collection) {
        return NULL;
    }

    /* check for any method preconditions */
    if (dav_run_method_precondition(ctx->r, NULL, wres->resource, ctx->doc, &err) != DECLINED
            && err) {
        dav_log_err(ctx->r, err, APLOG_DEBUG);
        return NULL;
    }

    /*
    ** Note: ctx->doc can only be NULL for DAV_PROPFIND_IS_ALLPROP. Since
    ** dav_get_allprops() does not need to do namespace translation,
    ** we're okay.
    **
    ** Note: we cast to lose the "const". The propdb won't try to change
    ** the resource, however, since we are opening readonly.
    */
    err = dav_popen_propdb(ctx->scratchpool,
                           ctx->r, ctx->w.lockdb, wres->resource, 1,
                           ctx->doc ? ctx->doc->namespaces : NULL, &propdb);
    if (err != NULL) {
        /* ### do something with err! */

        if (ctx->propfind_type == DAV_PROPFIND_IS_PROP) {
            dav_get_props_result badprops = { 0 };

            /* some props were expected on this collection/resource */
            dav_calendar_cache_badprops(ctx);
            badprops.propstats = ctx->propstat_404;
            dav_stream_response(wres, 0, &badprops, ctx->scratchpool);
        }
        else {
            /* no props on this collection/resource */
            dav_stream_response(wres, HTTP_OK, NULL, ctx->scratchpool);
        }

        apr_pool_clear(ctx->scratchpool);
        return NULL;
    }
    /* ### what to do about closing the propdb on server failure? */

    if (ctx->propfind_type == DAV_PROPFIND_IS_PROP) {
        propstats = dav_get_props(propdb, ctx->doc);
    }
    else {
        dav_prop_insert what = ctx->propfind_type == DAV_PROPFIND_IS_ALLPROP
                                 ? DAV_PROP_INSERT_VALUE
                                 : DAV_PROP_INSERT_NAME;
        propstats = dav_get_allprops(propdb, what);
    }
    dav_stream_response(wres, 0, &propstats, ctx->scratchpool);

    dav_close_propdb(propdb);

    /* at this point, ctx->scratchpool has been used to stream a
       single response.  this function fully controls the pool, and
       thus has the right to clear it for the next iteration of this
       callback. */
    apr_pool_clear(ctx->scratchpool);

    return NULL;
}


static dav_error *dav_calendar_query_report(request_rec *r,
    const dav_resource *resource,
    const apr_xml_doc *doc, ap_filter_t *output)
{
    dav_error *err;
    dav_walker_ctx ctx = { { 0 } };
    dav_response *multi_status;
    int depth;
    int ns = 0;

    /* ### validate that only one of these three elements is present */

    /* default is allprop */
    ctx.propfind_type = DAV_PROPFIND_IS_ALLPROP;
    if (dav_find_child(doc->root, "propname") != NULL) {
        ctx.propfind_type = DAV_PROPFIND_IS_PROPNAME;
    }
    else if (dav_find_child(doc->root, "prop") != NULL) {
        ctx.propfind_type = DAV_PROPFIND_IS_PROP;
    }

    ns = apr_xml_insert_uri(doc->namespaces, DAV_CALENDAR_XML_NAMESPACE);

    if (dav_find_child_ns(doc->root, ns, "filter") != NULL) {
        ctx.propfind_type = DAV_PROPFIND_IS_PROP;
    }
    else {
        /* "calendar-query" element must have filter */
        return dav_new_error(resource->pool, HTTP_BAD_REQUEST, 0, 0,
                "The \"calendar-query\" element does not contain a "
                "filter element.");
    }

    if ((depth = dav_get_depth(r, 0)) < 0) {
        return dav_new_error(resource->pool, HTTP_BAD_REQUEST, 0, 0,
                "The \"depth\" header was not valid.");
    }

    ctx.w.walk_type = DAV_WALKTYPE_NORMAL | DAV_WALKTYPE_AUTH;
    ctx.w.func = dav_calendar_report_walker;
    ctx.w.walk_ctx = &ctx;
    ctx.w.pool = r->pool;
    ctx.w.root = resource;

    ctx.doc = (apr_xml_doc *)doc;
    ctx.r = r;
    ctx.bb = apr_brigade_create(r->pool, r->connection->bucket_alloc);
    apr_pool_create(&ctx.scratchpool, r->pool);
    apr_pool_tag(ctx.scratchpool, "mod_dav-scratch");

    /* ### should open read-only */
    if ((err = dav_open_lockdb(r, 0, &ctx.w.lockdb)) != NULL) {
        return dav_push_error(r->pool, err->status, 0,
                             "The lock database could not be opened, "
                             "preventing access to the various lock "
                             "properties for the PROPFIND.",
                             err);
    }
    if (ctx.w.lockdb != NULL) {
        /* if we have a lock database, then we can walk locknull resources */
        ctx.w.walk_type |= DAV_WALKTYPE_LOCKNULL;
    }

    /* send <multistatus> tag, with all doc->namespaces attached.  */

    /* NOTE: we *cannot* leave out the doc's namespaces from the
       initial <multistatus> tag.  if a 404 was generated for an HREF,
       then we need to spit out the doc's namespaces for use by the
       404. Note that <response> elements will override these ns0,
       ns1, etc, but NOT within the <response> scope for the
       badprops. */
    dav_begin_multistatus(ctx.bb, r, HTTP_MULTI_STATUS,
                          doc ? doc->namespaces : NULL);

    /* Have the provider walk the resource. */
    err = (*resource->hooks->walk)(&ctx.w, depth, &multi_status);

    if (ctx.w.lockdb != NULL) {
        (*ctx.w.lockdb->hooks->close_lockdb)(ctx.w.lockdb);
    }

    if (err != NULL) {
        /* If an error occurred during the resource walk, there's
           basically nothing we can do but abort the connection and
           log an error.  This is one of the limitations of HTTP; it
           needs to "know" the entire status of the response before
           generating it, which is just impossible in these streamy
           response situations. */
        err = dav_push_error(r->pool, err->status, 0,
                             "Provider encountered an error while streaming"
                             " a multistatus PROPFIND response.", err);
        dav_log_err(r, err, APLOG_ERR);
        r->connection->aborted = 1;
        return NULL;
    }

    dav_finish_multistatus(r, ctx.bb);

    /* the response has been sent. */
    return NULL;
}

static dav_error *dav_calendar_multiget_report(request_rec *r,
    const dav_resource *resource,
    const apr_xml_doc *doc, ap_filter_t *output)
{
    dav_error *err;
    apr_xml_elem *href_elem;
    dav_resource *child_resource;
    dav_walker_ctx ctx = { { 0 } };
    dav_response *multi_status = NULL;

    /* ### validate that only one of these three elements is present */

    if (doc == NULL || dav_find_child(doc->root, "allprop") != NULL) {
        /* note: no request body implies allprop */
        ctx.propfind_type = DAV_PROPFIND_IS_ALLPROP;
    }
    else if (dav_find_child(doc->root, "propname") != NULL) {
        ctx.propfind_type = DAV_PROPFIND_IS_PROPNAME;
    }
    else if (dav_find_child(doc->root, "prop") != NULL) {
        ctx.propfind_type = DAV_PROPFIND_IS_PROP;
    }
    else {
        /* "calendar-multiget" element must have one of the above three children */
        return dav_new_error(resource->pool, HTTP_BAD_REQUEST, 0, 0,
                "The \"calendar-multiget\" element does not contain one of "
                "the required child elements (the specific command).");
    }

    href_elem = dav_find_child(doc->root, "href");
    if (!href_elem) {
        return dav_new_error(resource->pool, HTTP_BAD_REQUEST, 0, 0,
                "The \"calendar-multiget\" element does not contain one or "
                "more href elements.");
    }

    ctx.w.walk_type = DAV_WALKTYPE_NORMAL | DAV_WALKTYPE_AUTH;
    ctx.w.func = dav_calendar_report_walker;
    ctx.w.walk_ctx = &ctx;
    ctx.w.pool = r->pool;
    ctx.w.root = NULL;

    ctx.doc = (apr_xml_doc *)doc;
    ctx.r = r;
    ctx.bb = apr_brigade_create(r->pool, r->connection->bucket_alloc);
    apr_pool_create(&ctx.scratchpool, r->pool);
    apr_pool_tag(ctx.scratchpool, "mod_dav-scratch");

    /* ### should open read-only */
    if ((err = dav_open_lockdb(r, 0, &ctx.w.lockdb)) != NULL) {
        return dav_push_error(r->pool, err->status, 0,
                             "The lock database could not be opened, "
                             "preventing access to the various lock "
                             "properties for the PROPFIND.",
                             err);
    }
    if (ctx.w.lockdb != NULL) {
        /* if we have a lock database, then we can walk locknull resources */
        ctx.w.walk_type |= DAV_WALKTYPE_LOCKNULL;
    }

    /* send <multistatus> tag, with all doc->namespaces attached.  */

    /* NOTE: we *cannot* leave out the doc's namespaces from the
       initial <multistatus> tag.  if a 404 was generated for an HREF,
       then we need to spit out the doc's namespaces for use by the
       404. Note that <response> elements will override these ns0,
       ns1, etc, but NOT within the <response> scope for the
       badprops. */
    dav_begin_multistatus(ctx.bb, r, HTTP_MULTI_STATUS,
                          doc ? doc->namespaces : NULL);

    /* walk each href eleement */
    for (; href_elem; href_elem = href_elem->next) {
        dav_lookup_result lookup;

        const char *href = dav_xml_get_cdata(href_elem, ctx.scratchpool, 1 /* strip_white */);

        err = NULL;

        /* get a subrequest for the source, so that we can get a dav_resource
           for that source. */
        lookup = dav_lookup_uri(href, r, 0 /* must_be_absolute */);
        if (lookup.rnew == NULL) {
            err = &lookup.err;
        }
        else if (lookup.rnew->status != HTTP_OK) {
            err = dav_push_error(r->pool, lookup.rnew->status, 0,
                    "Could not access the resource.",
                    NULL);
        }

        /* get the resource from each subrequest */
        else if ((err = dav_get_resource(lookup.rnew, 0 /* label_allowed */,
                0 /* use_checked_in */, &child_resource)) == NULL) {
            /* success */
            ctx.w.root = child_resource;
        }

        /* send a response for any errors */
        if (err != NULL) {
            dav_response *new_response;

            new_response = apr_pcalloc(ctx.scratchpool, sizeof(*new_response));

            new_response->href = href;
            new_response->status = err->status;
            if (err->desc != NULL) {
                new_response->desc = apr_pstrcat(r->pool,
                                                new_response->desc,
                                                " The error was: ",
                                                err->desc, NULL);
            }

            dav_send_one_response(new_response, ctx.bb, r, ctx.scratchpool);
        }

        /* Have the provider walk each resource. */
        if ((err = (*resource->hooks->walk)(&ctx.w, 0, &multi_status)) != NULL) {
            break;
        }

        if (lookup.rnew) {
            ap_destroy_sub_req(lookup.rnew);
        }

    }

    if (ctx.w.lockdb != NULL) {
        (*ctx.w.lockdb->hooks->close_lockdb)(ctx.w.lockdb);
    }

    if (err != NULL) {
        /* If an error occurred during the resource walk, there's
           basically nothing we can do but abort the connection and
           log an error.  This is one of the limitations of HTTP; it
           needs to "know" the entire status of the response before
           generating it, which is just impossible in these streamy
           response situations. */
        err = dav_push_error(r->pool, err->status, 0,
                             "Provider encountered an error while streaming"
                             " a multistatus PROPFIND response.", err);
        dav_log_err(r, err, APLOG_ERR);
        r->connection->aborted = 1;
        return NULL;
    }

    dav_finish_multistatus(r, ctx.bb);

    /* the response has been sent. */
    return NULL;
}

static dav_error *dav_calendar_free_busy_query_report(request_rec *r,
    const dav_resource *resource,
    const apr_xml_doc *doc, ap_filter_t *output)
{
    dav_error *err;
    dav_walk_params w = { 0 };
    dav_calendar_ctx cctx = { 0 };
    dav_response *multi_status;
    apr_bucket *e;
    icalcomponent *timezone;
    const char *ical;
    apr_size_t ical_len;
    int depth;
    int ns = 0;
    int status;

    /* ### validate that only time-range is present */

    ns = apr_xml_insert_uri(doc->namespaces, DAV_CALENDAR_XML_NAMESPACE);

    if (!dav_find_child_ns(doc->root, ns, "time-range")) {
        /* "calendar-query" element must have filter */
        return dav_new_error(resource->pool, HTTP_BAD_REQUEST, 0, 0,
                "The \"calendar-query\" element does not contain a "
                "time-range element.");
    }

    if ((depth = dav_get_depth(r, 0)) < 0) {
        return dav_new_error(resource->pool, HTTP_BAD_REQUEST, 0, 0,
                "The \"depth\" header was not valid.");
    }

    w.walk_type = DAV_WALKTYPE_NORMAL | DAV_WALKTYPE_AUTH;
    w.func = dav_calendar_get_walker;
    w.walk_ctx = &cctx;
    w.pool = r->pool;
    w.root = resource;

    cctx.doc = (apr_xml_doc *)doc;
    cctx.r = r;
    cctx.bb = apr_brigade_create(r->pool, r->connection->bucket_alloc);

    /* ### should open read-only */
    if ((err = dav_open_lockdb(r, 0, &w.lockdb)) != NULL) {
        return dav_push_error(r->pool, err->status, 0,
                             "The lock database could not be opened, "
                             "preventing access to the various lock "
                             "properties for the PROPFIND.",
                             err);
    }
    if (w.lockdb != NULL) {
        /* if we have a lock database, then we can walk locknull resources */
        w.walk_type |= DAV_WALKTYPE_LOCKNULL;
    }

    /* Have the provider walk the resource. */
    err = (*resource->hooks->walk)(&w, depth, &multi_status);

    if (w.lockdb != NULL) {
        (*w.lockdb->hooks->close_lockdb)(w.lockdb);
    }

    if (err != NULL) {
        return err;
    }

    /* remove timezone component, not wanted for this report */
    while ((timezone = icalcomponent_get_first_component(cctx.comp,
            ICAL_VTIMEZONE_COMPONENT))) {
        if (timezone) {
            icalcomponent_remove_component(cctx.comp, timezone);
            icalcomponent_free(timezone);
        }
    }

    ical = icalcomponent_as_ical_string(cctx.comp);
    ical_len = strlen(ical);

    apr_brigade_cleanup(cctx.bb);

    ap_set_content_length(r, ical_len);
    ap_set_content_type(r, "text/calendar");

    e = apr_bucket_pool_create(ical, ical_len, r->pool,
            r->connection->bucket_alloc);
    APR_BRIGADE_INSERT_TAIL(cctx.bb, e);

    e = apr_bucket_eos_create(r->connection->bucket_alloc);
    APR_BRIGADE_INSERT_TAIL(cctx.bb, e);

    status = ap_pass_brigade(r->output_filters, cctx.bb);

    if (status == APR_SUCCESS
        || r->status != HTTP_OK
        || r->connection->aborted) {
        /* all ok */
    }
    else {
        /* no way to know what type of error occurred */
        ap_log_rerror(APLOG_MARK, APLOG_DEBUG, status, r,
                      "dav_calendar_handler: ap_pass_brigade returned %i",
                      status);
    }

    /* the response has been sent. */
    return NULL;
}

static int dav_calendar_find_ns(const apr_array_header_t *namespaces, const char *uri)
{
    int i;

    for (i = 0; i < namespaces->nelts; ++i) {
        if (strcmp(APR_XML_GET_URI_ITEM(namespaces, i), uri) == 0) {
            return i;
        }
    }
    return -1;
}

static int dav_calendar_deliver_report(request_rec *r,
        const dav_resource *resource,
        const apr_xml_doc *doc,
        ap_filter_t *output, dav_error **err)
{
    int ns = dav_calendar_find_ns(doc->namespaces, DAV_CALENDAR_XML_NAMESPACE);

    if (doc->root->ns == ns) {

        if (strcmp(doc->root->name, "calendar-query") == 0) {
            *err = dav_calendar_query_report(r, resource, doc, output);
        }
        else if (strcmp(doc->root->name, "calendar-multiget") == 0) {
            *err = dav_calendar_multiget_report(r, resource, doc, output);
        }
        else if (strcmp(doc->root->name, "free-busy-query") == 0) {
            *err = dav_calendar_free_busy_query_report(r, resource, doc, output);
        }
        else {
            /* NOTE: if you add a report, don't forget to add it to the
             *       dav_svn__reports_list[] array.
             */
            *err = dav_new_error(resource->pool, HTTP_NOT_IMPLEMENTED, 0, 0,
                                 "The requested report is unknown");
            return HTTP_NOT_IMPLEMENTED;
        }
        if (*err) {
            return (*err)->status;
        }
        return DONE;
    }

    return DECLINED;
}

void dav_calendar_gather_reports(request_rec *r, const dav_resource *resource,
    apr_array_header_t *reports, dav_error **err)
{
    dav_report_elem *report;

    report = apr_array_push(reports);
    report->nmspace = DAV_CALENDAR_XML_NAMESPACE;
    report->name = "calendar-query";

    report = apr_array_push(reports);
    report->nmspace = DAV_CALENDAR_XML_NAMESPACE;
    report->name = "calendar-multiget";

    /*
    report = apr_array_push(reports);
    report->nmspace = DAV_CALENDAR_XML_NAMESPACE;
    report->name = "free-busy-query";
    */
}

static dav_error *dav_calendar_check_calender(request_rec *r, dav_resource *resource,
        const dav_provider *provider, apr_array_header_t *mkcols)
{
    dav_error *err;
    dav_resource *parent;

    /* a calendar resource must not already exist */
    if (resource->exists) {
        return dav_new_error(r->pool, HTTP_CONFLICT, 0, 0,
                             apr_psprintf(r->pool,
                             "Calendar collection already exists: %s",
                             ap_escape_html(r->pool, resource->uri)));
    }

    /* walk backwards through the parents, until NULL. Parents must
     * either exist and be collections, or not exist. If the parent is a
     * non-collection, or is a calendar collection, we fail.
     *
     * Keep track of non existing parents - they will be created.
     */

    if ((err = resource->hooks->get_parent_resource(resource, &parent)) != NULL) {
        return err;
    }

    while ((parent)) {

        if (!parent->collection) {
            return dav_new_error(r->pool, HTTP_CONFLICT, 0, 0,
                               apr_psprintf(r->pool,
                                            "The parent resource of %s "
                                            "is not a collection.",
                                            ap_escape_html(r->pool, r->uri)));
        }

        if (mkcols && !parent->exists) {
            dav_resource **mkcol = apr_array_push(mkcols);
            *mkcol = parent;
        }

        if (parent->exists) {
            dav_lockdb *lockdb;
            dav_propdb *propdb;

            /* open lock database, to report on supported lock properties */
            /* ### should open read-only */
            if ((err = dav_open_lockdb(r, 0, &lockdb)) != NULL) {
                return dav_push_error(r->pool, err->status, 0,
                                      "The lock database could not be opened, "
                                      "preventing the checking of a parent "
                                      "calendar collection.",
                                      err);
            }

            /* open the property database (readonly) for the resource */
            if ((err = dav_open_propdb(r, lockdb, resource, 1, NULL,
                                       &propdb)) != NULL) {
                if (lockdb != NULL) {
                    dav_close_lockdb(lockdb);
                }

                return dav_push_error(r->pool, err->status, 0,
                                      "The property database could not be opened, "
                                      "preventing the checking of a parent "
                                      "calendar collection.",
                                      err);
            }

            if (propdb) {
                dav_db *db = NULL;
                const dav_prop_name prop = { "DAV:", "resourcetype" };
                dav_prop_name name[1] = { { NULL, NULL } };

                if ((err = provider->propdb->open(resource->pool, parent, 1, &db)) != NULL) {
                    return err;
                }

                if (db) {
                    if ((err = provider->propdb->first_name(db, name)) != NULL) {
                        return err;
                    }

                    while (name->ns != NULL) {
                        apr_text_header hdr[1] = { { 0 } };
                        int f;

                        if (name->name && prop.name && strcmp(name->name, prop.name) == 0
                                && ((name->ns && prop.ns && strcmp(name->ns, prop.ns) == 0)
                                        || (!name->ns && !prop.ns))) {
                            if ((err = provider->propdb->output_value(db, name, NULL, hdr, &f)) != NULL) {
                                return err;
                            }

                            if (strstr(hdr->first->text, ">calendar<")) {

                                err = dav_new_error(r->pool, HTTP_CONFLICT, 0, 0,
                                        apr_psprintf(r->pool,
                                                "A calendar collection cannot be created "
                                                "under another calendar collection: %s",
                                                ap_escape_html(r->pool, r->uri)));

                            }

                            break;
                        }
                        if ((err = provider->propdb->next_name(db, name)) != NULL) {
                            break;
                        }

                    }
                    provider->propdb->close(db);
                }

            }

            dav_close_propdb(propdb);

            if (lockdb != NULL) {
                dav_close_lockdb(lockdb);
            }

        }

        if ((err = parent->hooks->get_parent_resource(parent, &parent)) != NULL) {
            return err;
        }
    }

    return NULL;
}

static dav_error *dav_calendar_make_calendar(request_rec *r, dav_resource *resource)
{
    dav_error *err;
    const dav_provider *provider;
    dav_lockdb *lockdb;
    dav_propdb *propdb;

    dav_calendar_config_rec *conf = ap_get_module_config(r->per_dir_config,
            &dav_calendar_module);

    /* find the dav provider */
    provider = dav_get_provider(r);
    if (provider == NULL) {
        return dav_new_error(r->pool, HTTP_METHOD_NOT_ALLOWED, 0, 0,
                             apr_psprintf(r->pool,
                             "DAV not enabled for %s",
                             ap_escape_html(r->pool, r->uri)));
    }

    /* resource->collection = 1; */
    if ((err = resource->hooks->create_collection(resource))) {
        return dav_push_error(r->pool, err->status, 0,
                apr_psprintf(r->pool,
                        "Could not create calendar collection: %s",
                        ap_escape_html(r->pool, resource->uri)),
                        err);
    }

    /* set the resource type to calendar */

    /* open lock database, to report on supported lock properties */
    /* ### should open read-only */
    if ((err = dav_open_lockdb(r, 0, &lockdb)) != NULL) {
        return dav_push_error(r->pool, err->status, 0,
                              "The lock database could not be opened, "
                              "preventing the creation of a "
                              "calendar collection.",
                              err);
    }

    /* open the property database (readonly) for the resource */
    if ((err = dav_open_propdb(r, lockdb, resource, 1, NULL,
                               &propdb)) != NULL) {
        if (lockdb != NULL) {
            dav_close_lockdb(lockdb);
        }

        return dav_push_error(r->pool, err->status, 0,
                              "The property database could not be opened, "
                              "preventing the creation of a "
                              "calendar collection.",
                              err);
    }

    if (propdb) {
        dav_db *db = NULL;

        if ((err = provider->propdb->open(resource->pool, resource, 0, &db)) != NULL) {
            err = dav_push_error(r->pool, err->status, 0,
                                  "Property database could not be opened, "
                                  "preventing the creation of a "
                                  "calendar collection.",
                                  err);
        }
        else {

            apr_array_header_t *ns;
            apr_xml_elem elem[2] = { { 0 } };
            apr_text text[2] = { { 0 } };
            dav_prop_name restype[2] = { { DAV_XML_NAMESPACE, "resourcetype" },
                    { DAV_CALENDAR_XML_NAMESPACE, "calendar-timezone" } };
            dav_namespace_map *map = NULL;

            ns = apr_array_make(resource->pool, 3, sizeof(const char *));

            *(const char **)apr_array_push(ns) = DAV_XML_NAMESPACE;
            *(const char **)apr_array_push(ns) = DAV_CALENDAR_XML_NAMESPACE;

            elem[0].name = restype[0].name;
            elem[0].ns = 1;
            elem[0].first_cdata.first = &text[0];
            text[0].text = "calendar";

            elem[1].name = restype[1].name;
            elem[1].ns = 1;
            elem[1].first_cdata.first = &text[1];
            text[1].text = conf->dav_calendar_timezone;

            if ((err = provider->propdb->map_namespaces(db, ns, &map)) != NULL) {
                err = dav_push_error(r->pool, err->status, 0,
                                      "Namespace could not be mapped, "
                                      "preventing the creation of a "
                                      "calendar collection.",
                                      err);
            }
            else if ((err =  provider->propdb->store(db, &restype[0], &elem[0], map)) != NULL) {
                err = dav_push_error(r->pool, err->status, 0,
                                      "Property 'calendar' could not be stored, "
                                      "preventing the creation of a "
                                      "calendar collection.",
                                      err);
            }
            else if ((err =  provider->propdb->store(db, &restype[1], &elem[1], map)) != NULL) {
                err = dav_push_error(r->pool, err->status, 0,
                                      "Property 'calendar-timezone' could not be stored, "
                                      "preventing the creation of a "
                                      "calendar collection.",
                                      err);
            }

            provider->propdb->close(db);

        }

        dav_close_propdb(propdb);
    }

    if (lockdb != NULL) {
        dav_close_lockdb(lockdb);
    }

    return err;
}

static dav_error *dav_calendar_provision_calendar(request_rec *r, dav_resource *trigger)
{
    dav_error *err;
    const dav_provider *provider;
    dav_resource *resource = NULL;
    apr_array_header_t *mkcols;
    int i;

    /* find the dav provider */
    provider = dav_get_provider(r);
    if (provider == NULL) {
        return dav_new_error(r->pool, HTTP_METHOD_NOT_ALLOWED, 0, 0,
                             apr_psprintf(r->pool,
                             "DAV not enabled for %s",
                             ap_escape_html(r->pool, resource->uri)));
    }

    /* resolve calendar resource */
    if ((err = provider->repos->get_resource(r, NULL, NULL, 0, &resource))) {
        return dav_push_error(r->pool, err->status, 0,
                              apr_psprintf(r->pool,
                                           "Could not get calendar provision URL: %s",
                                           ap_escape_html(r->pool, resource->uri)),
                              err);
    }

    /* already exists and is a collection? we're done */
    if (resource->exists && resource->collection) {
        return NULL;
    }

    /* sanity check parents */
    mkcols = apr_array_make(r->pool, 2, sizeof(dav_resource *));
    if ((err = dav_calendar_check_calender(r, resource, provider, mkcols))) {
        return err;
    }

    /* create parent collections */
    for (i = mkcols->nelts - 1; i >= 0; i--) {
        dav_resource *parent = APR_ARRAY_IDX(mkcols, i, dav_resource *);

        if (trigger->hooks->is_same_resource(trigger, parent)) {
            err = trigger->hooks->create_collection(trigger);
        }
        else {
            err = parent->hooks->create_collection(parent);
        }

        if (err) {
            return dav_push_error(r->pool, err->status, 0,
                                  apr_psprintf(r->pool,
                                  "Could not create calendar provision "
                                  "parent directory: %s",
                                  ap_escape_html(r->pool, parent->uri)),
                                  err);
        }
    }

    /* create calendar */
    if (trigger->hooks->is_same_resource(trigger, resource)) {
        err = dav_calendar_make_calendar(r, trigger);
    }
    else {
        err = dav_calendar_make_calendar(r, resource);
    }

    return err;
}

static int dav_calendar_auto_provision(request_rec *r, dav_resource *resource,
        dav_error **err)
{
    dav_calendar_config_rec *conf = ap_get_module_config(r->per_dir_config,
            &dav_calendar_module);

    ap_expr_info_t **provs = (ap_expr_info_t **)conf->dav_calendar_provisions->elts;
    int i;

    if (!conf->dav_calendar_provisions->nelts) {
        return DECLINED;
    }

    for (i = 0; i < conf->dav_calendar_provisions->nelts; ++i) {
        const char *error = NULL, *path;

        path = ap_expr_str_exec(r, provs[i], &error);
        if (error) {
            *err = dav_new_error(r->pool, HTTP_FORBIDDEN, 0, APR_SUCCESS,
                    apr_psprintf(r->pool, "Could not evaluate calendar provision URL: %s",
                            error));
            return DONE;
        }
        else {
            dav_lookup_result lookup = { 0 };

            /* sanity - if no path prefix, skip */
            if (strncmp(r->uri, path, strlen(r->uri))) {
                continue;
            }

            lookup = dav_lookup_uri(path, r, 0 /* must_be_absolute */);

            if (lookup.rnew == NULL) {
                *err = dav_new_error(r->pool, lookup.err.status, 0, APR_SUCCESS,
                        lookup.err.desc);
            }
            if (lookup.rnew->status != HTTP_OK) {
                *err = dav_new_error(r->pool, lookup.rnew->status, 0, APR_SUCCESS,
                        apr_psprintf(r->pool, "Could not lookup calendar provision URL: %s", path));
            }

            /* make the calendar */
            *err = dav_calendar_provision_calendar(lookup.rnew, resource);
            if (*err != NULL) {
                return DONE;
            }

            ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r,
                            "mod_dav_calendar: Auto provisioned %s", lookup.rnew->uri);

            /* clean up */
            if (lookup.rnew) {
                ap_destroy_sub_req(lookup.rnew);
            }

        }

    }

    return DONE;
}

static void *create_dav_calendar_config(apr_pool_t *p, server_rec *s)
{
    dav_calendar_server_rec *a =
    (dav_calendar_server_rec *) apr_pcalloc(p, sizeof(dav_calendar_server_rec));

    a->aliases = apr_array_make(p, 5, sizeof(dav_calendar_alias_entry));

    return a;
}

static void *create_dav_calendar_dir_config(apr_pool_t *p, char *d)
{
    dav_calendar_config_rec *conf = apr_pcalloc(p, sizeof(dav_calendar_config_rec));

    conf->dav_calendar_timezone = DEFAULT_TIMEZONE;
    conf->max_resource_size = DEFAULT_MAX_RESOURCE_SIZE;

    conf->dav_calendar_homes = apr_array_make(p, 2, sizeof(const char *));
    conf->dav_calendar_provisions = apr_array_make(p, 2, sizeof(const char *));

    return conf;
}

static void *merge_dav_calendar_config(apr_pool_t *p, void *basev, void *overridesv)
{
    dav_calendar_server_rec *a =
    (dav_calendar_server_rec *) apr_pcalloc(p, sizeof(dav_calendar_server_rec));
    dav_calendar_server_rec *base = (dav_calendar_server_rec *) basev;
    dav_calendar_server_rec *overrides = (dav_calendar_server_rec *) overridesv;

    a->aliases = apr_array_append(p, overrides->aliases, base->aliases);

    return a;
}

static void *merge_dav_calendar_dir_config(apr_pool_t *p, void *basev, void *addv)
{
    dav_calendar_config_rec *new = (dav_calendar_config_rec *) apr_pcalloc(p,
            sizeof(dav_calendar_config_rec));
    dav_calendar_config_rec *add = (dav_calendar_config_rec *) addv;
    dav_calendar_config_rec *base = (dav_calendar_config_rec *) basev;

    new->dav_calendar = (add->dav_calendar_set == 0) ? base->dav_calendar : add->dav_calendar;
    new->dav_calendar_set = add->dav_calendar_set || base->dav_calendar_set;

    new->dav_calendar_timezone = (add->dav_calendar_timezone_set == 0) ?
            base->dav_calendar_timezone : add->dav_calendar_timezone;
    new->dav_calendar_timezone_set = add->dav_calendar_timezone_set || base->dav_calendar_timezone_set;

    new->max_resource_size = (add->max_resource_size_set == 0) ? base->max_resource_size : add->max_resource_size;
    new->max_resource_size_set = add->max_resource_size_set || base->max_resource_size_set;

    new->dav_calendar_homes = apr_array_append(p, add->dav_calendar_homes, base->dav_calendar_homes);
    new->dav_calendar_provisions = apr_array_append(p, add->dav_calendar_provisions, base->dav_calendar_provisions);

    return new;
}

static const char *set_dav_calendar(cmd_parms *cmd, void *dconf, int flag)
{
    dav_calendar_config_rec *conf = dconf;

    conf->dav_calendar = flag;
    conf->dav_calendar_set = 1;

    return NULL;
}

static const char *set_dav_calendar_timezone(cmd_parms *cmd, void *dconf, const char *tz)
{
    dav_calendar_config_rec *conf = dconf;

    icalcomponent *calendar, *timezone;

    calendar = icalcomponent_new(ICAL_VCALENDAR_COMPONENT);
    icalcomponent_add_property(calendar, icalproperty_new_version("2.0"));
    icalcomponent_add_property(calendar,
            icalproperty_new_prodid("-//Graham Leggett//" PACKAGE_STRING "//EN"));

    timezone = icalcomponent_new(ICAL_VTIMEZONE_COMPONENT);
    icalcomponent_add_property(timezone, icalproperty_new_tzid(tz));

    icalcomponent_add_component(calendar,timezone);

    conf->dav_calendar_timezone = icalcomponent_as_ical_string(calendar);
    conf->dav_calendar_timezone_set = 1;

    icalcomponent_free(calendar);

    return NULL;
}

static const char *set_dav_calendar_max_resource_size(cmd_parms *cmd,
        void *dconf, const char *arg)
{
    dav_calendar_config_rec *conf = dconf;

    if (apr_strtoff(&conf->max_resource_size, arg, NULL, 10) != APR_SUCCESS
            || conf->max_resource_size < 4096) {
        return "DavCalendarMaxResourceSize needs to be a positive integer larger than 4096.";
    }

    conf->max_resource_size_set = 1;

    return NULL;
}


static const char *add_dav_calendar_home(cmd_parms *cmd, void *dconf, const char *home)
{
    dav_calendar_config_rec *conf = dconf;
    const char *expr_err = NULL;

    ap_expr_info_t **homes = apr_array_push(conf->dav_calendar_homes);

    (*homes) = ap_expr_parse_cmd(cmd, home, AP_EXPR_FLAG_STRING_RESULT,
            &expr_err, NULL);

    if (expr_err) {
        return apr_pstrcat(cmd->temp_pool,
                "Cannot parse expression '", home, "': ",
                expr_err, NULL);
    }

    return NULL;
}

static const char *add_dav_calendar_provision(cmd_parms *cmd, void *dconf, const char *prov)
{
    dav_calendar_config_rec *conf = dconf;
    const char *expr_err = NULL;

    ap_expr_info_t **provs = apr_array_push(conf->dav_calendar_provisions);

    (*provs) = ap_expr_parse_cmd(cmd, prov, AP_EXPR_FLAG_STRING_RESULT,
            &expr_err, NULL);

    if (expr_err) {
        return apr_pstrcat(cmd->temp_pool,
                "Cannot parse expression '", prov, "': ",
                expr_err, NULL);
    }

    return NULL;
}

static const char *add_alias_internal(cmd_parms *cmd, void *dummy,
                                      const char *fake, const char *real,
                                      int use_regex)
{
    server_rec *s = cmd->server;
    dav_calendar_server_rec *conf = ap_get_module_config(s->module_config,
                                                   &dav_calendar_module);
    dav_calendar_alias_entry *new = apr_array_push(conf->aliases);

    const char *err = ap_check_cmd_context(cmd, NOT_IN_DIR_CONTEXT);

    if (err != NULL) {
        return err;
    }

    if (use_regex) {
        new->regexp = ap_pregcomp(cmd->pool, fake, AP_REG_EXTENDED);
        if (new->regexp == NULL)
            return "Regular expression could not be compiled.";
        new->real = real;
    }
    else {
        new->real = real;
    }
    new->fake = fake;

    return NULL;
}

static const char *add_dav_calendar_alias(cmd_parms *cmd, void *dummy, const char *fake,
        const char *real)
{
    return add_alias_internal(cmd, dummy, fake, real, 0);
}

static const char *add_dav_calendar_alias_regex(cmd_parms *cmd, void *dummy,
                                   const char *fake, const char *real)
{
    return add_alias_internal(cmd, dummy, fake, real, 1);
}

static const command_rec dav_calendar_cmds[] =
{
    AP_INIT_FLAG("DavCalendar",
        set_dav_calendar, NULL, RSRC_CONF | ACCESS_CONF,
        "When enabled, the URL space will support calendars."),
    AP_INIT_TAKE1("DavCalendarTimezone", set_dav_calendar_timezone, NULL, RSRC_CONF | ACCESS_CONF,
        "Set the default timezone for auto provisioned calendars. Defaults to UTC."),
    AP_INIT_TAKE1("DavCalendarMaxResourceSize", set_dav_calendar_max_resource_size, NULL, RSRC_CONF | ACCESS_CONF,
        "Set the maximum resource size of an individual calendar. Defaults to 10MB."),
    AP_INIT_TAKE1("DavCalendarHome", add_dav_calendar_home, NULL, RSRC_CONF | ACCESS_CONF,
        "Set the URL template to use for the calendar home. "
        "Recommended value is \"/calendars/%{escape:%{REMOTE_USER}}\"."),
    AP_INIT_TAKE1("DavCalendarProvision", add_dav_calendar_provision, NULL, RSRC_CONF | ACCESS_CONF,
        "Set the URL template to use for calendar auto provision. "
        "Recommended value is \"/calendars/%{escape:%{REMOTE_USER}}/Home\"."),
    AP_INIT_TAKE2("DavCalendarAlias", add_dav_calendar_alias, NULL, RSRC_CONF | ACCESS_CONF,
        "Calendar alias and the real calendar collection."),
    AP_INIT_TAKE2("DavCalendarAliasMatch", add_dav_calendar_alias_regex, NULL, RSRC_CONF,
        "A calendar alias regular expression and a calendar collecion URL to alias to"),
    { NULL }
};

static int dav_calendar_post_config(apr_pool_t *p, apr_pool_t *plog,
                                    apr_pool_t *ptemp, server_rec *s)
{
    /* Register CalDAV methods */
    iM_MKCALENDAR = ap_method_register(p, "MKCALENDAR");

    return OK;
}

static int dav_calendar_handle_get(request_rec *r)
{
    dav_error *err;
    const dav_provider *provider;
    dav_resource *resource = NULL;
    apr_bucket_brigade *bb;
    apr_bucket *e;
    dav_calendar_ctx cctx = { 0 };
    dav_walk_params w = { 0 };
    dav_response *multi_status;
    const char *type, *ns, *ical;
    apr_sha1_ctx_t sha1 = { { 0 } };
    unsigned char digest[APR_SHA1_DIGESTSIZE];
    apr_size_t ical_len;
    int depth = 1;
    int status;

    /* for us? */
    if (!r->handler || strcmp(r->handler, DIR_MAGIC_TYPE)) {
        return DECLINED;
    }

    /* find the dav provider */
    provider = dav_get_provider(r);
    if (provider == NULL) {
        ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r,
                 "DAV not enabled for %s, ignoring GET request",
                 ap_escape_html(r->pool, r->uri));
        return DECLINED;
    }

    /* resolve calendar resource */
    if ((err = provider->repos->get_resource(r, NULL, NULL, 0, &resource))) {
        return dav_handle_err(r, err, NULL);
    }

    /* not existing or not a collection? not for us */
    if (!resource->exists || !resource->collection) {
        return DECLINED;
    }

    status = dav_calendar_get_resource_type(resource,  &type, &ns);
    switch (status) {
    case OK:
        if (!type || !ns || strcmp(type, "calendar") ||
                strcmp(ns, DAV_CALENDAR_XML_NAMESPACE)) {
            /* Not for us */
            ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r,
                    "Collection %s not a calendar collection, ignoring GET request",
                    ap_escape_html(r->pool, r->uri));
            return DECLINED;
        }
        break;
    case DECLINED:
        /* Not for us */
        return DECLINED;

    default:
        return status;
    }

    w.walk_type = DAV_WALKTYPE_NORMAL | DAV_WALKTYPE_AUTH;
    w.walk_ctx = &cctx;
    w.pool = r->pool;
    w.root = resource;
    cctx.r = r;

    /* ### should open read-only */
    if ((err = dav_open_lockdb(r, 0, &w.lockdb)) != NULL) {
        err = dav_push_error(r->pool, err->status, 0,
                             "The lock database could not be opened, "
                             "preventing access to the various lock "
                             "properties for the calendar GET.",
                             err);
        return dav_handle_err(r, err, NULL);
    }
    if (w.lockdb != NULL) {
        /* if we have a lock database, then we can walk locknull resources */
        w.walk_type |= DAV_WALKTYPE_LOCKNULL;
    }

    /* Have the provider walk the etags. */
    w.func = dav_calendar_etag_walker;
    cctx.sha1 = &sha1;
    apr_sha1_init(&sha1);
    err = (*resource->hooks->walk)(&w, depth, &multi_status);
    apr_sha1_final(digest, &sha1);

    /* Have the provider walk the resource. */
    if (!err) {

        if (cctx.sha1) {
            apr_table_set(r->headers_out, "ETag", apr_pstrcat(r->pool, "\"",
                    apr_pencode_base64_binary(r->pool, digest, APR_SHA1_DIGESTSIZE,
                            APR_ENCODE_NOPADDING, NULL), "\"", NULL));
        }

        /* handle conditional requests */
        status = ap_meets_conditions(r);
        if (status) {
            return status;
        }

        cctx.comp = icalcomponent_new(ICAL_VCALENDAR_COMPONENT);

        apr_pool_cleanup_register(r->pool, cctx.comp, icalcomponent_cleanup,
                apr_pool_cleanup_null);

        w.func = dav_calendar_get_walker;

        err = (*resource->hooks->walk)(&w, depth, &multi_status);
    }

    if (w.lockdb != NULL) {
        (*w.lockdb->hooks->close_lockdb)(w.lockdb);
    }

    if (err != NULL) {
        return dav_handle_err(r, err, NULL);
    }

    ical = icalcomponent_as_ical_string(cctx.comp);
    ical_len = strlen(ical);

    bb = apr_brigade_create(r->pool, r->connection->bucket_alloc);

    ap_set_content_length(r, ical_len);
    ap_set_content_type(r, "text/calendar");

    e = apr_bucket_pool_create(ical, ical_len, r->pool,
            r->connection->bucket_alloc);
    APR_BRIGADE_INSERT_TAIL(bb, e);

    e = apr_bucket_eos_create(r->connection->bucket_alloc);
    APR_BRIGADE_INSERT_TAIL(bb, e);

    status = ap_pass_brigade(r->output_filters, bb);
    apr_brigade_cleanup(bb);

    if (status == APR_SUCCESS
        || r->status != HTTP_OK
        || r->connection->aborted) {
        return OK;
    }
    else {
        /* no way to know what type of error occurred */
        ap_log_rerror(APLOG_MARK, APLOG_DEBUG, status, r,
                      "dav_calendar_handler: ap_pass_brigade returned %i",
                      status);
        return AP_FILTER_ERROR;
    }
}

/*
 * Call <func> for each context. This can stop when an error occurs, or
 * simply iterate through the whole list.
 *
 * Returns 1 if an error occurs (and the iteration is aborted). Returns 0
 * if all elements are processed.
 *
 * If <reverse> is true (non-zero), then the list is traversed in
 * reverse order.
 */
static int dav_process_ctx_list(void (*func)(dav_prop_ctx *ctx),
                                apr_array_header_t *ctx_list, int stop_on_error,
                                int reverse)
{
    int i = ctx_list->nelts;
    dav_prop_ctx *ctx = (dav_prop_ctx *)ctx_list->elts;

    if (reverse)
        ctx += i;

    while (i--) {
        if (reverse)
            --ctx;

        (*func)(ctx);
        if (stop_on_error && DAV_PROP_CTX_HAS_ERR(*ctx)) {
            return 1;
        }

        if (!reverse)
            ++ctx;
    }

    return 0;
}

static int dav_calendar_handle_mkcalendar(request_rec *r)
{

    dav_error *err;
    const dav_provider *provider;
    dav_resource *resource = NULL;
    apr_xml_doc *doc;
    apr_xml_elem *child;
    dav_response *multi_status;
    dav_propdb *propdb;
    apr_text *propstat_text;
    apr_array_header_t *ctx_list;
    dav_prop_ctx *ctx;
    dav_auto_version_info av_info;
    int resource_state;
    int result;
    int failure = 0;
    int ns;


    /* find the dav provider */
    provider = dav_get_provider(r);
    if (provider == NULL) {
        dav_handle_err(r, dav_new_error(r->pool, HTTP_METHOD_NOT_ALLOWED, 0, 0,
                             apr_psprintf(r->pool,
                             "DAV not enabled for %s",
                             ap_escape_html(r->pool, resource->uri))), NULL);
    }

    /* resolve calendar resource */
    if ((err = provider->repos->get_resource(r, NULL, NULL, 0, &resource))) {
        return dav_handle_err(r, err, NULL);
    }

    /* already exists and is a collection? we're done */
    if (resource->exists) {
        err = dav_new_error(r->pool, HTTP_METHOD_NOT_ALLOWED, 0, APR_SUCCESS,
                "Collection already exists");
        err->tagname = "resource-must-be-null";
        return dav_handle_err(r, err, NULL);
    }

    /* sanity check parents */
    if ((err = dav_calendar_check_calender(r, resource, provider, NULL))) {
        return dav_handle_err(r, err, NULL);
    }

    resource_state = dav_get_resource_state(r, resource);

    if ((err = dav_validate_request(r, resource, 0, NULL, &multi_status,
            resource_state == DAV_RESOURCE_NULL ?
                    DAV_VALIDATE_PARENT : DAV_VALIDATE_RESOURCE,
                    NULL))) {
        return dav_handle_err(r, err, multi_status);
    }

    /* if versioned resource, make sure parent is checked out */
    if ((err = dav_auto_checkout(r, resource, 1 /* parent_only */,
                                 &av_info)) != NULL) {
        return dav_handle_err(r, err, NULL);
    }

    /* create calendar */
    if ((err = dav_calendar_make_calendar(r, resource)) != NULL) {
        dav_auto_checkin(r, NULL, err != NULL /* undo if error */,
                0 /*unlock*/, &av_info);
        return dav_handle_err(r, err, NULL);
    }

    if ((result = ap_xml_parse_input(r, &doc)) != OK) {
        return result;
    }

    /* note: doc == NULL if no request body */
    if (doc == NULL) {
        dav_auto_checkin(r, NULL, err != NULL /* undo if error */,
                0 /*unlock*/, &av_info);

        return OK;
    }

    ns = apr_xml_insert_uri(doc->namespaces, DAV_CALENDAR_XML_NAMESPACE);

    if (!dav_validate_root_ns(doc, ns, "mkcalendar")) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
                      "The request body does not contain "
                      "a \"mkcalendar\" element.");
        return HTTP_BAD_REQUEST;
    }

    if ((err = dav_open_propdb(r, NULL, resource, 0, doc->namespaces,
                               &propdb)) != NULL) {
        /* undo any auto-checkout */
        dav_auto_checkin(r, resource, 1 /*undo*/, 0 /*unlock*/, &av_info);

        err = dav_push_error(r->pool, HTTP_INTERNAL_SERVER_ERROR, 0,
                             apr_psprintf(r->pool,
                                          "Could not open the property "
                                          "database for %s.",
                                          ap_escape_html(r->pool, r->uri)),
                             err);
        return dav_handle_err(r, err, NULL);
    }

    /* ### what to do about closing the propdb on server failure? */

    /* ### validate "live" properties */

    /* set up an array to hold property operation contexts */
    ctx_list = apr_array_make(r->pool, 10, sizeof(dav_prop_ctx));

    /* do a first pass to ensure that all "remove" properties exist */
    for (child = doc->root->first_child; child; child = child->next) {
        int is_remove;
        apr_xml_elem *prop_group;
        apr_xml_elem *one_prop;

        /* Ignore children that are not set/remove */
        if (child->ns != APR_XML_NS_DAV_ID
            || (!(is_remove = (strcmp(child->name, "remove") == 0))
                && strcmp(child->name, "set") != 0)) {
            continue;
        }

        /* make sure that a "prop" child exists for set/remove */
        if ((prop_group = dav_find_child(child, "prop")) == NULL) {
            dav_close_propdb(propdb);

            /* undo any auto-checkout */
            dav_auto_checkin(r, resource, 1 /*undo*/, 0 /*unlock*/, &av_info);

            /* This supplies additional information for the default message. */
            ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, APLOGNO(00588)
                          "A \"prop\" element is missing inside "
                          "the propertyupdate command.");
            return HTTP_BAD_REQUEST;
        }

        for (one_prop = prop_group->first_child; one_prop;
             one_prop = one_prop->next) {

            ctx = (dav_prop_ctx *)apr_array_push(ctx_list);
            ctx->propdb = propdb;
            ctx->operation = is_remove ? DAV_PROP_OP_DELETE : DAV_PROP_OP_SET;
            ctx->prop = one_prop;

            ctx->r = r;         /* for later use by dav_prop_log_errors() */

            dav_prop_validate(ctx);

            if ( DAV_PROP_CTX_HAS_ERR(*ctx) ) {
                failure = 1;
            }
        }
    }

    /* ### should test that we found at least one set/remove */

    /* execute all of the operations */
    if (!failure && dav_process_ctx_list(dav_prop_exec, ctx_list, 1, 0)) {
        failure = 1;
    }

    /* generate a failure/success response */
    if (failure) {
        (void)dav_process_ctx_list(dav_prop_rollback, ctx_list, 0, 1);
        propstat_text = dav_failed_proppatch(r->pool, ctx_list);
    }
    else {
        (void)dav_process_ctx_list(dav_prop_commit, ctx_list, 0, 0);
        propstat_text = dav_success_proppatch(r->pool, ctx_list);
    }

    /* make sure this gets closed! */
    dav_close_propdb(propdb);

    /* complete any auto-versioning */
    dav_auto_checkin(r, resource, failure, 0 /*unlock*/, &av_info);

    /* log any errors that occurred */
    (void)dav_process_ctx_list(dav_prop_log_errors, ctx_list, 0, 0);

    if (failure) {
        dav_response resp = { 0 };

        resp.href = resource->uri;

        /* ### should probably use something new to pass along this text... */
        resp.propresult.propstats = propstat_text;

        dav_send_multistatus(r, HTTP_MULTI_STATUS, &resp, doc->namespaces);

        return DONE;
    }
    else {

        r->status_line = ap_get_status_line(r->status = 201);

        return DONE;
    }
}

static int dav_calendar_try_alias_list(request_rec *r, apr_array_header_t *aliases)
{
    dav_calendar_alias_entry *entries = (dav_calendar_alias_entry *) aliases->elts;
    ap_regmatch_t regm[AP_MAX_REG_MATCH];
    const char *found = NULL;
    int i;

    for (i = 0; i < aliases->nelts; ++i) {
        dav_calendar_alias_entry *alias = &entries[i];

        if (alias->regexp) {
            if (!ap_regexec(alias->regexp, r->uri, AP_MAX_REG_MATCH, regm, 0)) {
                if (alias->real) {
                    found = ap_pregsub(r->pool, alias->real, r->uri,
                                       AP_MAX_REG_MATCH, regm);
                    if (!found) {
                        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
                                      "Regex substitution in '%s' failed. "
                                      "Replacement too long?", alias->real);
                        return HTTP_INTERNAL_SERVER_ERROR;
                    }
                }
                else {
                    return HTTP_INTERNAL_SERVER_ERROR;
                }
            }
        }
        else {
            if (!strcmp(r->uri, alias->fake)) {
                ap_set_context_info(r, alias->fake, alias->real);
                found = alias->real;
            }
        }

        if (found) {

            found = ap_escape_uri(r->pool, found);

            if (r->args) {
                found = apr_pstrcat(r->pool, found,
                                      "?", r->args, NULL);
            }

            ap_internal_redirect(found, r);

            return OK;
        }

    }

    return DECLINED;
}

static int dav_calendar_handler(request_rec *r)
{
    dav_calendar_server_rec *serverconf = ap_get_module_config(r->server->module_config,
            &dav_calendar_module);

    dav_calendar_config_rec *conf = ap_get_module_config(r->per_dir_config,
            &dav_calendar_module);

    int status;

    status = dav_calendar_try_alias_list(r, serverconf->aliases);
    if (status != DECLINED) {
        return status;
    }

    if (!conf || !conf->dav_calendar) {
        return DECLINED;
    }

    if (r->method_number == M_GET) {
        return dav_calendar_handle_get(r);
    }

    if (r->method_number == iM_MKCALENDAR) {
        return dav_calendar_handle_mkcalendar(r);
    }

    return DECLINED;
}

static int dav_calendar_method_precondition(request_rec *r,
        dav_resource *src, const dav_resource *dst,
        const apr_xml_doc *doc, dav_error **err)
{
    /* handle auto provisioning */
    if (src && !src->exists) {

        /*
        ** The hook implementer must ensure behaviour of the hook is both safe and
        ** idempotent as defined by RFC7231 section 4.2. For example, creating a
        ** collection resource on first OPTIONS is safe, as no representation would
        ** have been served prior to this call. Care must be taken to ensure that
        ** clients cannot create arbitrary resources using this hook resulting in
        ** capacity exhaustion. If the hook is not relevant, return DECLINED,
        ** otherwise DONE with any error in err.
        */

        return dav_calendar_auto_provision(r, src, err);
    }

    return DECLINED;
}

static int dav_calendar_fixups(request_rec *r)
{
    dav_calendar_config_rec *conf = ap_get_module_config(r->per_dir_config,
            &dav_calendar_module);

    if (conf->dav_calendar) {
        AP_REQUEST_SET_BNOTE(r, AP_REQUEST_STRONG_ETAG, AP_REQUEST_STRONG_ETAG);
    }

    return OK;
}

static int dav_calendar_type_checker(request_rec *r)
{
    /*
     * Short circuit other modules that want to overwrite the content type
     * as soon as they detect a directory.
     */
    if (r->content_type && !strcmp(r->content_type, DAV_CALENDAR_HANDLER)) {
        return OK;
    }

    return DECLINED;
}

static void register_hooks(apr_pool_t *p)
{
    static const char * const aszSucc[]={ "mod_autoindex.c",
                                          "mod_userdir.c",
                                          "mod_vhost_alias.c", NULL };

    ap_hook_post_config(dav_calendar_post_config, NULL, NULL, APR_HOOK_MIDDLE);

    dav_register_liveprop_group(p, &dav_calendar_liveprop_group);
    dav_hook_find_liveprop(dav_calendar_find_liveprop, NULL, NULL, APR_HOOK_MIDDLE);

    dav_options_provider_register(p, "dav_calendar", &options);
    dav_resource_type_provider_register(p, "dav_calendar", &resource_types);

    ap_hook_type_checker(dav_calendar_type_checker, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_fixups(dav_calendar_fixups, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_handler(dav_calendar_handler, NULL, aszSucc, APR_HOOK_MIDDLE);

    dav_hook_deliver_report(dav_calendar_deliver_report, NULL, NULL, APR_HOOK_MIDDLE);
    dav_hook_gather_reports(dav_calendar_gather_reports,
                                  NULL, NULL, APR_HOOK_MIDDLE);

    dav_hook_method_precondition(dav_calendar_method_precondition,
                                 NULL, NULL, APR_HOOK_MIDDLE);
}

AP_DECLARE_MODULE(dav_calendar) =
{
    STANDARD20_MODULE_STUFF,
    create_dav_calendar_dir_config, /* dir config creater */
    merge_dav_calendar_dir_config,  /* dir merger --- default is to override */
    create_dav_calendar_config,     /* server config */
    merge_dav_calendar_config,      /* merge server config */
    dav_calendar_cmds,              /* command apr_table_t */
    register_hooks                  /* register hooks */
};
