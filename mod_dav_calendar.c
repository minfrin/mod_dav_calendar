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
#include <apr_strings.h>

#include "httpd.h"
#include "http_config.h"
#include "http_core.h"
#include "http_log.h"
#include "http_protocol.h"
#include "http_request.h"
#include "util_script.h"

#include "mod_dav.h"

module AP_MODULE_DECLARE_DATA dav_calendar_module;

typedef struct
{
    int dav_calendar_set :1;
    apr_array_header_t *dav_calendar_homes;
    apr_array_header_t *dav_calendar_provisions;
    int dav_calendar;
} dav_calendar_config_rec;

/* forward-declare the hook structures */
static const dav_hooks_liveprop dav_hooks_liveprop_calendar;

#define DAV_XML_NAMESPACE "DAV:"
#define DAV_CALENDAR_XML_NAMESPACE "urn:ietf:params:xml:ns:caldav"

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
    case DAV_CALENDAR_PROPID_calendar_home_set:
    	/* property allowed, handled below */

        break;
    default:
        /* ### what the heck was this property? */
        return DAV_PROP_INSERT_NOTDEF;
    }

    /* assert: value != NULL */

    /* get the information and global NS index for the property */
    global_ns = dav_get_liveprop_info(propid, &dav_calendar_liveprop_group, &info);

    /* assert: info != NULL && info->name != NULL */

    if (what == DAV_PROP_INSERT_VALUE) {

    	apr_text_append(p, phdr, apr_psprintf(p, "<lp%d:%s>",
                global_ns, info->name));

        switch (propid) {
        case DAV_CALENDAR_PROPID_calendar_home_set: {
            int i;
            ap_expr_info_t **homes = (ap_expr_info_t **)conf->dav_calendar_homes->elts;

            for (i = 0; i < conf->dav_calendar_homes->nelts; ++i) {
                const char *err = NULL, *url;

            	url = ap_expr_str_exec(r, homes[i], &err);
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

            break;
        }
        default:
        	break;
        }

        apr_text_append(p, phdr, apr_psprintf(p, "</lp%d:%s>" DEBUG_CR,
                global_ns, info->name));

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
    request_rec *r = resource->hooks->get_request_rec(resource);

	const dav_provider *provider;
	dav_error *err;
    dav_lockdb *lockdb;
    dav_propdb *propdb;
    int result = DECLINED;

    *type = *uri = NULL;

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

static int dav_calendar_query_report(const dav_resource *resource,
    const apr_xml_doc *doc, ap_filter_t *output, dav_error **err)
{

    return HTTP_NOT_IMPLEMENTED;
}

static int dav_calendar_multiget_report(const dav_resource *resource,
    const apr_xml_doc *doc, ap_filter_t *output, dav_error **err)
{

    return HTTP_NOT_IMPLEMENTED;
}

static int dav_calendar_free_busy_query_report(const dav_resource *resource,
    const apr_xml_doc *doc, ap_filter_t *output, dav_error **err)
{

    return HTTP_NOT_IMPLEMENTED;
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
            return dav_calendar_query_report(resource, doc, output, err);
        }
        else if (strcmp(doc->root->name, "calendar-multiget") == 0) {
        	return dav_calendar_multiget_report(resource, doc, output, err);
        }
        else if (strcmp(doc->root->name, "free-busy-query") == 0) {
        	return dav_calendar_free_busy_query_report(resource, doc, output, err);
        }
        else {
            /* NOTE: if you add a report, don't forget to add it to the
             *       dav_svn__reports_list[] array.
             */
            *err = dav_new_error(resource->pool, HTTP_NOT_IMPLEMENTED, 0, 0,
                                 "The requested report is unknown");
            return HTTP_NOT_IMPLEMENTED;
        }
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

	report = apr_array_push(reports);
	report->nmspace = DAV_CALENDAR_XML_NAMESPACE;
	report->name = "free-busy-query";
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
    	    apr_xml_elem elem[1] = { { 0 } };
    	    apr_text text = { 0 };
            dav_prop_name restype[1] = { { DAV_XML_NAMESPACE, "resourcetype" } };
            dav_namespace_map *map = NULL;

    	    ns = apr_array_make(resource->pool, 3, sizeof(const char *));

    	    *(const char **)apr_array_push(ns) = DAV_XML_NAMESPACE;
    	    *(const char **)apr_array_push(ns) = DAV_CALENDAR_XML_NAMESPACE;

    	    elem->name = restype->name;
    	    elem->ns = 1;
    	    elem->first_cdata.first = &text;
    	    text.text = "calendar";

    	    if ((err = provider->propdb->map_namespaces(db, ns, &map)) != NULL) {
    	        err = dav_push_error(r->pool, err->status, 0,
    	                              "Namespace could not be mapped, "
    	                              "preventing the creation of a "
    	                              "calendar collection.",
    	                              err);
            }
    	    else if ((err =  provider->propdb->store(db, restype, elem, map)) != NULL) {
    	        err = dav_push_error(r->pool, err->status, 0,
    	                              "Property could not be stored, "
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

static dav_error *dav_calendar_provision_calendar(request_rec *r)
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

    	if ((err = parent->hooks->create_collection(parent))) {
        	return dav_push_error(r->pool, err->status, 0,
                                  apr_psprintf(r->pool,
                                  "Could not create calendar provision "
                                  "parent directory: %s",
								  ap_escape_html(r->pool, parent->uri)),
                                  err);
    	}
    }

    /* create calendar */
    if ((err = dav_calendar_make_calendar(r, resource))) {
        return err;
    }

    return NULL;
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
            *err = dav_calendar_provision_calendar(lookup.rnew);
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

static void *create_dav_calendar_dir_config(apr_pool_t *p, char *d)
{
	dav_calendar_config_rec *conf = apr_pcalloc(p, sizeof(dav_calendar_config_rec));

    conf->dav_calendar_homes = apr_array_make(p, 2, sizeof(const char *));
    conf->dav_calendar_provisions = apr_array_make(p, 2, sizeof(const char *));

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

static const command_rec dav_calendar_cmds[] =
{
    AP_INIT_FLAG("DavCalendar",
        set_dav_calendar, NULL, RSRC_CONF | ACCESS_CONF,
        "When enabled, the URL space will support calendars."),
    AP_INIT_TAKE1("DavCalendarHome", add_dav_calendar_home, NULL, RSRC_CONF | ACCESS_CONF,
        "Set the URL template to use for the calendar home. "
    	"Recommended value is \"/calendars/%{escape:%{REMOTE_USER}}\"."),
    AP_INIT_TAKE1("DavCalendarProvision", add_dav_calendar_provision, NULL, RSRC_CONF | ACCESS_CONF,
        "Set the URL template to use for calendar auto provision. "
    	"Recommended value is \"/calendars/%{escape:%{REMOTE_USER}}/Home\"."),
    { NULL }
};

static int dav_calendar_handler(request_rec *r)
{

	dav_calendar_config_rec *conf = ap_get_module_config(r->per_dir_config,
            &dav_calendar_module);

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

static void register_hooks(apr_pool_t *p)
{
    ap_hook_fixups(dav_calendar_fixups, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_handler(dav_calendar_handler, NULL, NULL, APR_HOOK_MIDDLE);

    dav_register_liveprop_group(p, &dav_calendar_liveprop_group);
    dav_hook_find_liveprop(dav_calendar_find_liveprop, NULL, NULL, APR_HOOK_MIDDLE);

    dav_options_provider_register(p, "dav_calendar", &options);
    dav_resource_type_provider_register(p, "dav_calendar", &resource_types);

    dav_hook_deliver_report(dav_calendar_deliver_report, NULL, NULL, APR_HOOK_MIDDLE);
    dav_hook_gather_reports(dav_calendar_gather_reports,
                                  NULL, NULL, APR_HOOK_MIDDLE);

    dav_hook_auto_provision(dav_calendar_auto_provision,
                            NULL, NULL, APR_HOOK_MIDDLE);
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
