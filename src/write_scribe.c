/**
 * Write scribe plugin 
 **/

#include "collectd.h"
#include "common.h"
#include "plugin.h"
#include "configfile.h"

#include "utils_format_json.h"
#include "utils_format_dse_insights.h"
#include "utils_tail.h"
#include "scribe_capi.h"
#include "unixsock.h"

#define SCRIBE_BUF_SIZE 8192

static char *scribe_config_file = NULL;

struct instance_definition_s {
    char                 *instance;
    char                 *path;
    cu_tail_t            *tail;
    cdtime_t              interval;
    ssize_t               time_from;
    struct instance_definition_s *next;
};

typedef struct instance_definition_s instance_definition_t;

static int scribe_write_messages (const data_set_t *ds, const value_list_t *vl)
{
    char buffer[SCRIBE_BUF_SIZE];
    size_t   bfree = sizeof(buffer);
    size_t   bfill = 0;

    if (!is_scribe_initialized())
        return -1;

    if (0 != strcmp (ds->type, vl->type))
    {
        ERROR ("scribe_write plugin: DS type does not match "
                "value list type");
        return -1;
    }

    //one metric at a time (in collectd they can be combined)
    for (int i = 0; i < ds->ds_num; i++) {
        memset (buffer, 0, sizeof (buffer));

        int r = format_insights_initialize(buffer, &bfill, &bfree);
        if (r == 0)
            r = format_insights_value_list(buffer, &bfill, &bfree, ds, vl, 1, NULL, 0, 0, NULL, i, i+1);
        if (r == 0)
           r = format_insights_finalize(buffer, &bfill, &bfree);

        if (strlen(buffer) > 0 && r == 0)
          scribe_log(buffer, "metrics");
    }

    return (0);
} /* int wl_write_messages */

static int scribe_write (const data_set_t *ds, const value_list_t *vl,
        user_data_t *user_data)
{
    int status = scribe_write_messages (ds, vl);
    return (status);
}

static int scribe_init(void)
{
    srand(time(NULL) ^ getpid());

    if (strcasecmp(scribe_config_file, "env") == 0) {
        scribe_config_file = getenv("SCRIBE_CONFIG_FILE");

        if (scribe_config_file == NULL) {
            ERROR("Missing SCRIBE_CONFIG_FILE Environment Variable");
            return -1;
        }
    }

    new_scribe2(scribe_config_file);

    us_init();

    return (0);
}

static int scribe_shutdown(void)
{
    us_shutdown_listener();

    if (is_scribe_initialized())
        delete_scribe();

    return (0);
}

static void scribe_instance_definition_destroy(void *arg){
    instance_definition_t *id;

    id = arg;
    if (id == NULL)
        return;

    if (id->tail != NULL)
        cu_tail_destroy (id->tail);
    id->tail = NULL;

    sfree(id->instance);
    sfree(id->path);
    sfree(id);
}

static int scribe_tail_read (user_data_t *ud) {
    instance_definition_t *id;
    id = ud->data;

    if (id->tail == NULL)
    {
        id->tail = cu_tail_create (id->path);
        if (id->tail == NULL)
        {
            ERROR ("write_scribe plugin: cu_tail_create (\"%s\") failed.",
                    id->path);
            return (-1);
        }
    }

    while (1)
    {
        char buffer[SCRIBE_BUF_SIZE + 512];
        char log_buffer[SCRIBE_BUF_SIZE];
        size_t   bfree = sizeof(buffer);
        size_t   bfill = 0;
        int status;

        status = cu_tail_readline (id->tail, log_buffer, (int) sizeof(log_buffer));
        if (status != 0)
        {
            ERROR ("scribe_write plugin: File \"%s\": cu_tail_readline failed "
                    "with status %i.", id->path, status);
            return (-1);
        }

        int buffer_len = strlen (log_buffer);
        if (buffer_len == 0)
            break;

        /* Remove newlines at the end of line. */
        while (buffer_len > 0) {
            if ((log_buffer[buffer_len - 1] == '\n') || (log_buffer[buffer_len - 1] == '\r')) {
                log_buffer[buffer_len - 1] = 0;
                buffer_len--;
            } else {
                break;
            }
        }


        if (!is_scribe_initialized())
            break;

        //one metric at a time (in collectd they can be combined)
        memset (buffer, 0, sizeof (buffer));

        int r = format_insights_initialize(buffer, &bfill, &bfree);
        if (r == 0)
            r = format_insights_log(buffer, &bfill, &bfree, log_buffer, id->instance);
        if (r == 0)
            r = format_insights_finalize(buffer, &bfill, &bfree);

        if (strlen(buffer) > 0 && r == 0)
            scribe_log(buffer, "logs");
    }

    return (0);
}

static int scribe_config_add_file_tail(oconfig_item_t *ci)
{
  instance_definition_t* id;
  int status = 0;
  int i;

  /* Registration variables */
  char *cb_name;
  user_data_t cb_data;

  id = malloc(sizeof(*id));
  if (id == NULL)
      return (-1);
  memset(id, 0, sizeof(*id));
  id->instance = NULL;
  id->path = NULL;
  id->time_from = -1;
  id->next = NULL;

  status = cf_util_get_string (ci, &id->path);
  if (status != 0) {
      sfree (id);
      return (status);
  }

  /* Use default interval. */
  id->interval = plugin_get_interval();

  for (i = 0; i < ci->children_num; ++i){
      oconfig_item_t *option = ci->children + i;
      status = 0;

      if (strcasecmp("Instance", option->key) == 0)
          status = cf_util_get_string(option, &id->instance);
      else if (strcasecmp("Interval", option->key) == 0)
          cf_util_get_cdtime(option, &id->interval);
      else {
          WARNING("scribe_write plugin: Option `%s' not allowed here.", option->key);
          status = -1;
      }

      if (status != 0)
          break;
  }

  if (status != 0) {
      scribe_instance_definition_destroy(id);
      return (-1);
  }

  /* Verify all necessary options have been set. */
  if (id->path == NULL){
      WARNING("scribe_write plugin: Option `Path' must be set.");
      status = -1;
  }

  if (status != 0){
      scribe_instance_definition_destroy(id);
      return (-1);
  }

  cb_name = ssnprintf_alloc("write_scribe/%s", id->path);
  memset(&cb_data, 0, sizeof(cb_data));
  cb_data.data = id;
  cb_data.free_func = scribe_instance_definition_destroy;
  status = plugin_register_complex_read(NULL, cb_name, scribe_tail_read, id->interval, &cb_data);

  if (status != 0){
      ERROR("scribe_write plugin: Registering complex read function failed.");
      scribe_instance_definition_destroy(id);
      return (-1);
  }

  return (0);
}

static int scribe_config(oconfig_item_t *ci)
{
    int i;
    for (i = 0; i < ci->children_num; ++i) {
        int status = 0;
        oconfig_item_t *child = ci->children + i;
        if (strcasecmp("ConfigFile", child->key) == 0)
            status = cf_util_get_string (child, &scribe_config_file);
        else if (strcasecmp("File", child->key) == 0)
            scribe_config_add_file_tail(child);
        else
            WARNING("write_scribe plugin: Ignoring unknown config option `%s'.", child->key);

         if (status != 0) {
            ERROR("Error reading %s", child->key);
            return (status);
         }
    }

    if (scribe_config_file == NULL)
    {
        ERROR("write_scribe requires the ConfigFile to be defined");
        return -10;
    }

    //config unixsock
    return us_config_complex(ci);
}

static void scribe_plugin_log (int severity, const char *msg,
                user_data_t __attribute__((unused)) *user_data)
{
      char buffer[SCRIBE_BUF_SIZE + 512];
      size_t   bfree = sizeof(buffer);
      size_t   bfill = 0;

      if (!is_scribe_initialized() || strlen(msg) == 0)
         return;

      memset (buffer, 0, sizeof (buffer));

      int r = format_insights_initialize(buffer, &bfill, &bfree);
      if (r == 0)
          r = format_insights_log(buffer, &bfill, &bfree, (char *) msg, "collectd.log");
      if (r == 0)
          r = format_insights_finalize(buffer, &bfill, &bfree);

      if (strlen(buffer) > 0 && r == 0)
          scribe_log(buffer, "collectd");

} /* void logfile_log (int, const char *) */


static int scribe_notification (const notification_t *n,
                user_data_t __attribute__((unused)) *user_data)
{
        /*char *buf = ssnprintf_alloc("{\"severity\":%u, \"host\":\"%s\", \"timestamp\":%.3f, \"plugin\":\"%s\", \"plugin_instance\":\"%s\", \"type\":\"%s\", \"type_instance\":\"%s\", \"message\":\"%s\"}", \
                   n->severity, replace_json_reserved(n->host), CDTIME_T_TO_DOUBLE ((n->time != 0) ? n->time : cdtime ()), replace_json_reserved(n->plugin), replace_json_reserved(n->plugin_instance), replace_json_reserved(n->type), \
                   replace_json_reserved(n->type_instance), replace_json_reserved(n->message));

        scribe_log(buf, "notificatons"); */
        return (0);
} /* int logfile_notification */



void module_register (void)
{
    plugin_register_init("write_scribe", scribe_init);
    plugin_register_complex_config("write_scribe", scribe_config);
    plugin_register_shutdown("write_scribe", scribe_shutdown);
    plugin_register_write ("write_scribe", scribe_write, NULL);
    plugin_register_log("write_scribe", scribe_plugin_log, NULL);
    plugin_register_notification("write_scribe", scribe_notification, NULL);
}

/* vim: set sw=4 ts=4 sts=4 tw=78 et : */