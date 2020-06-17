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
 */
#include <apr_lib.h>

#include "httpd.h"
#include "http_config.h"
#include "http_core.h"
#include "http_log.h"
#include "http_protocol.h"
#include "http_request.h"
#include "util_script.h"

#include "mod_dav.h"

module AP_MODULE_DECLARE_DATA dav_calendar_module;

/* forward-declare the hook structures */
static const dav_hooks_liveprop dav_hooks_liveprop_calendar;

/*
** The namespace URIs that we use. This list and the enumeration must
** stay in sync.
*/
static const char * const dav_calendar_namespace_uris[] =
{
    "urn:ietf:params:xml:ns:caldav",

    NULL        /* sentinel */
};
enum {
    DAV_CALENDAR_URI_DAV            /* the DAV: namespace URI */
};

enum {
    DAV_CALENDAR_PROPID_calendar_description = 1,
    DAV_CALENDAR_PROPID_calendar_home_set,
    DAV_CALENDAR_PROPID_calendar_timezone,
    DAV_CALENDAR_PROPID_getctag,
    DAV_CALENDAR_PROPID_max_attendees_per_instance,
    DAV_CALENDAR_PROPID_max_date_time,
    DAV_CALENDAR_PROPID_max_instances,
    DAV_CALENDAR_PROPID_max_resource_size,
    DAV_CALENDAR_PROPID_min_date_time,
    DAV_CALENDAR_PROPID_supported_calendar_component_set,
    DAV_CALENDAR_PROPID_supported_calendar_data,
    DAV_CALENDAR_PROPID_supported_collation_set
};

static const dav_liveprop_spec dav_calendar_props[] =
{
    /* standard calendar properties */
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
    {
        DAV_CALENDAR_URI_DAV,
        "calendar-timezone",
		DAV_CALENDAR_PROPID_calendar_timezone,
        0
    },
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
    {
        DAV_CALENDAR_URI_DAV,
        "supported-calendar-component-set",
		DAV_CALENDAR_PROPID_supported_calendar_component_set,
        0
    },
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


typedef struct
{
    int dav_calendar_set :1;
    int dav_calendar;
} dav_calendar_config_rec;

static dav_error *dav_calendar_options_header(request_rec *r,
		const dav_resource *resource, apr_text_header *phdr)
{
    apr_text_append(r->pool, phdr, "calendar-access");

    return NULL;
}

static dav_error *dav_calendar_options_method(request_rec *r,
		const dav_resource *resource, apr_text_header *phdr)
{
//    apr_text_append(r->pool, phdr, "MKCALENDAR");
//    apr_text_append(r->pool, phdr, "REPORT");

    return NULL;
}

static dav_options_provider options =
{
    dav_calendar_options_header,
    dav_calendar_options_method,
    NULL
};

static void *create_dav_calendar_dir_config(apr_pool_t *p, char *d)
{
	dav_calendar_config_rec *conf = apr_pcalloc(p, sizeof(dav_calendar_config_rec));

    return conf;
}

static void *merge_dav_calendar_dir_config(apr_pool_t *p, void *basev, void *addv)
{
	dav_calendar_config_rec *new = (dav_calendar_config_rec *) apr_pcalloc(p,
            sizeof(dav_calendar_config_rec));
	dav_calendar_config_rec *add = (dav_calendar_config_rec *) addv;
	dav_calendar_config_rec *base = (dav_calendar_config_rec *) basev;

    new->dav_calendar = (add->dav_calendar_set == 0) ? base->dav_calendar : add->dav_calendar;
    new->dav_calendar_set = add->dav_calendar_set || base->dav_calendar_set;

    return new;
}

static const char *set_dav_calendar(cmd_parms *cmd, void *dconf, int flag)
{
    dav_calendar_config_rec *conf = dconf;

    conf->dav_calendar = flag;
    conf->dav_calendar_set = 1;

    return NULL;
}

static const command_rec dav_calendar_cmds[] =
{
    AP_INIT_FLAG("DavCalendar",
        set_dav_calendar, NULL, RSRC_CONF | ACCESS_CONF,
        "When enabled, the URL space will support calendars."),
    { NULL }
};

static int dav_calendar_handler(request_rec *r)
{

	dav_calendar_config_rec *conf = ap_get_module_config(r->per_dir_config,
            &dav_calendar_module);

    return DECLINED;

}

static void register_hooks(apr_pool_t *p)
{
    ap_hook_handler(dav_calendar_handler, NULL, NULL, APR_HOOK_MIDDLE);

    dav_register_liveprop_group(p, &dav_calendar_liveprop_group);

    dav_options_provider_register(p, "dav_calendar", &options);
}

AP_DECLARE_MODULE(dav_calendar) =
{
    STANDARD20_MODULE_STUFF,
    create_dav_calendar_dir_config, /* dir config creater */
    merge_dav_calendar_dir_config,  /* dir merger --- default is to override */
    NULL,                           /* server config */
    NULL,                           /* merge server config */
    dav_calendar_cmds,              /* command apr_table_t */
    register_hooks                  /* register hooks */
};
