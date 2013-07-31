/* OpenVAS Manager
 * $Id$
 * Description: Module for OpenVAS Manager: the OMP library.
 *
 * Authors:
 * Matthew Mundell <matthew.mundell@greenbone.net>
 * Timo Pollmeier <timo.pollmeier@greenbone.net>
 *
 * Copyright:
 * Copyright (C) 2009-2013 Greenbone Networks GmbH
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2,
 * or, at your option, any later version as published by the Free
 * Software Foundation
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/**
 * @file  omp.c
 * @brief The OpenVAS Manager OMP library.
 *
 * This file defines an OpenVAS Management Protocol (OMP) library, for
 * implementing OpenVAS managers such as the OpenVAS Manager daemon.
 *
 * The library provides \ref process_omp_client_input.
 * This function parses a given string of OMP XML and tracks and manipulates
 * tasks in reaction to the OMP commands in the string.
 */

/**
 * @internal
 * The OMP-"Processor" is always in a state (\ref client_state_t
 * \ref client_state ) and currently looking at the opening of an OMP element
 * (\ref omp_xml_handle_start_element ), at the text of an OMP element
 * (\ref omp_xml_handle_text ) or at the closing of an OMP element
 * (\ref omp_xml_handle_end_element ).
 *
 * The state usually represents the current location of the parser within the
 * XML (OMP) tree.  There has to be one state for every OMP element.
 *
 * State transitions occur in the start and end element handler callbacks.
 *
 * Generally, the strategy is to wait until the closing of an element before
 * doing any action or sending a response.  Also, error cases are to be detected
 * in the end element handler.
 *
 * If data has to be stored, it goes to \ref command_data (_t) , which is a
 * union.
 * More specific incarnations of this union are e.g. \ref create_user_data (_t)
 * , where the data to create a new user is stored (until the end element of
 * that command is reached).
 *
 * For implementing new commands that have to store data (e.g. not
 * "\<help_extended/\>"), \ref command_data has to be freed and NULL'ed in case
 * of errors and the \ref current_state has to be reset.
 * It can then be assumed that it is NULL'ed at the start of every new
 * command element.  To implement a new start element handler, be sure to just
 * copy an existing case and keep its structure.
 *
 * Attributes are easier to implement than elements.
 * E.g.
 * @code
 * <key_value_pair key="k" value="v"/>
 * @endcode
 * is obviously easier to handle than
 * @code
 * <key><attribute name="k"/><value>v</value></key>
 * @endcode
 * .
 * For this reason the GET commands like GET_TASKS all use attributes only.
 *
 * However, for the other commands it is preferred to avoid attributes and use
 * the text of elements
 * instead, like in
 * @code
 * <key_value_pair><key>k</key><value>v</value></key_value_pair>
 * @endcode
 * .
 *
 * If new elements are built of multiple words, separate the words with an
 * underscore.
 */

#include "omp.h"
#include "manage.h"
#include "manage_sql.h"
/** @todo For access to scanner_t scanner. */
#include "otp.h"
#include "tracef.h"

#include <arpa/inet.h>
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <netinet/in.h>
#include <string.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <openvas/base/certificate.h>
#include <openvas/base/nvti.h>
#include <openvas/base/openvas_string.h>
#include <openvas/base/openvas_file.h>
#include <openvas/misc/openvas_auth.h>
#include <openvas/misc/openvas_logging.h>
#include <openvas/misc/resource_request.h>
#include <openvas/omp/xml.h>

#ifdef S_SPLINT_S
#include "splint.h"
#endif

#undef G_LOG_DOMAIN
/**
 * @brief GLib log domain.
 */
#define G_LOG_DOMAIN "md    omp"


/* Static headers. */

/** @todo Exported for manage_sql.c. */
void
buffer_results_xml (GString *, iterator_t *, task_t, int, int, int, int,
                    const char *, iterator_t *, int);


/* Helper functions. */

/**
 * @brief Check whether a string is a UUID.
 *
 * @param[in]  uuid  Potential UUID.
 *
 * @return 1 yes, 0 no.
 */
static int
is_uuid (const char *uuid)
{
  while (*uuid) if (isxdigit (*uuid) || (*uuid == '-')) uuid++; else return 0;
  return 1;
}

/**
 * @brief Return time defined by broken down time strings.
 *
 * If any argument is NULL, use the value from the current time.
 *
 * @param[in]   hour          Hour (0 to 23).
 * @param[in]   minute        Minute (0 to 59).
 * @param[in]   day_of_month  Day of month (1 to 31).
 * @param[in]   month         Month (1 to 12).
 * @param[in]   year          Year.
 * @param[in]   timezone      Timezone.
 *
 * @return Time described by arguments on success, -2 if failed to switch to
 *         timezone, -1 on error.
 */
static time_t
time_from_strings (const char *hour, const char *minute,
                   const char *day_of_month, const char *month,
                   const char *year, const char *timezone)
{
  struct tm given_broken, *now_broken;
  time_t now, ret;
  gchar *tz;

  tz = NULL;
  if (timezone)
    {
      /* Store current TZ. */
      tz = getenv ("TZ") ? g_strdup (getenv ("TZ")) : NULL;

      if (setenv ("TZ", timezone, 1) == -1)
        {
          g_free (tz);
          return -2;
        }
      tzset ();
    }

  time (&now);
  now_broken = localtime (&now);

  given_broken.tm_sec = 0;
  given_broken.tm_min = (minute ? atoi (minute) : now_broken->tm_min);
  given_broken.tm_hour = (hour ? atoi (hour) : now_broken->tm_hour);
  given_broken.tm_mday = (day_of_month
                           ? atoi (day_of_month)
                           : now_broken->tm_mday);
  given_broken.tm_mon = (month ? (atoi (month) - 1) : now_broken->tm_mon);
  given_broken.tm_year = (year ? (atoi (year) - 1900) : now_broken->tm_year);
  given_broken.tm_isdst = now_broken->tm_isdst;

  ret = mktime (&given_broken);

  if (timezone)
    {
      /* Revert to stored TZ. */
      if (tz)
        setenv ("TZ", tz, 1);
      else
        unsetenv ("TZ");
      g_free (tz);
      tzset ();
    }

  return ret;
}

/**
 * @brief Return interval defined by time and unit strings.
 *
 * @param[in]   value   Value.
 * @param[in]   unit    Calendar unit: second, minute, hour, day, week,
 *                      month, year or decade.  "second" if NULL.
 * @param[out]  months  Months return.
 *
 * @return Interval described by arguments on success, -2 if value was NULL,
 *         -1 if value was NULL.
 */
static time_t
interval_from_strings (const char *value, const char *unit, time_t *months)
{
  if (value == NULL)
    return -1;

  if ((unit == NULL) || (strcasecmp (unit, "second") == 0))
    {
      long int val;
      val = strtol (value, NULL, 10);
      if ((val >= INT_MAX) || (val < 0))
        return -3;
      return val;
    }

  if (strcasecmp (unit, "minute") == 0)
    {
      long int val;
      val = strtol (value, NULL, 10);
      if ((val >= (INT_MAX / 60)) || (val < 0))
        return -3;
      return val * 60;
    }

  if (strcasecmp (unit, "hour") == 0)
    {
      long int val;
      val = strtol (value, NULL, 10);
      if ((val >= (INT_MAX / (60 * 60))) || (val < 0))
        return -3;
      return val * 60 * 60;
    }

  if (strcasecmp (unit, "day") == 0)
    {
      long int val;
      val = strtol (value, NULL, 10);
      if ((val >= (INT_MAX / (60 * 60 * 24))) || (val < 0))
        return -3;
      return val * 60 * 60 * 24;
    }

  if (strcasecmp (unit, "week") == 0)
    {
      long int val;
      val = strtol (value, NULL, 10);
      if ((val >= (INT_MAX / (60 * 60 * 24 * 7))) || (val < 0))
        return -3;
      return val * 60 * 60 * 24 * 7;
    }

  if (months)
    {
      if (strcasecmp (unit, "month") == 0)
        {
          *months = atoi (value);
          if ((*months >= INT_MAX) || (*months < 0))
            return -3;
          return 0;
        }

      if (strcasecmp (unit, "year") == 0)
        {
          *months = atoi (value);
          if ((*months >= (INT_MAX / 12)) || (*months < 0))
            return -3;
          *months = *months * 12;
          return 0;
        }

      if (strcasecmp (unit, "decade") == 0)
        {
          *months = atoi (value);
          if ((*months >= (INT_MAX / (12 * 10))) || (*months < 0))
            return -3;
          *months = *months * 12 * 10;
          return 0;
        }
    }

  return -2;
}

/**
 * @brief Find an attribute in a parser callback list of attributes.
 *
 * @param[in]   attribute_names   List of names.
 * @param[in]   attribute_values  List of values.
 * @param[in]   attribute_name    Name of sought attribute.
 * @param[out]  attribute_value   Attribute value return.
 *
 * @return 1 if found, else 0.
 */
int
find_attribute (const gchar **attribute_names,
                const gchar **attribute_values,
                const char *attribute_name,
                const gchar **attribute_value)
{
  while (*attribute_names && *attribute_values)
    if (strcmp (*attribute_names, attribute_name))
      attribute_names++, attribute_values++;
    else
      {
        *attribute_value = *attribute_values;
        return 1;
      }
  return 0;
}

/**
 * @brief Find an attribute in a parser callback list of attributes and append
 * @brief it to a string using openvas_append_string.
 *
 * @param[in]   attribute_names   List of names.
 * @param[in]   attribute_values  List of values.
 * @param[in]   attribute_name    Name of sought attribute.
 * @param[out]  string            String to append attribute value to, if
 *                                found.
 *
 * @return 1 if found and appended, else 0.
 */
int
append_attribute (const gchar **attribute_names,
                  const gchar **attribute_values,
                  const char *attribute_name,
                  gchar **string)
{
  const gchar* attribute;
  if (find_attribute (attribute_names, attribute_values, attribute_name,
                      &attribute))
    {
      openvas_append_string (string, attribute);
      return 1;
    }
  return 0;
}


/* Help message. */

/**
 * @brief A command.
 */
typedef struct
{
  gchar *name;     ///< Command name.
  gchar *summary;  ///< Summary of command.
} command_t;

/**
 * @brief Response to the help command.
 */
static command_t omp_commands[]
 = {{"AUTHENTICATE", "Authenticate with the manager." },
    {"COMMANDS",     "Run a list of commands."},
    {"CREATE_AGENT", "Create an agent."},
    {"CREATE_CONFIG", "Create a config."},
    {"CREATE_ALERT", "Create an alert."},
    {"CREATE_FILTER", "Create a filter."},
    {"CREATE_LSC_CREDENTIAL", "Create a local security check credential."},
    {"CREATE_NOTE", "Create a note."},
    {"CREATE_OVERRIDE", "Create an override."},
    {"CREATE_PORT_LIST", "Create a port list."},
    {"CREATE_PORT_RANGE", "Create a port range in a port list."},
    {"CREATE_REPORT_FORMAT", "Create a report format."},
    {"CREATE_REPORT", "Create a report."},
    {"CREATE_SCHEDULE", "Create a schedule."},
    {"CREATE_SLAVE", "Create a slave."},
    {"CREATE_TARGET", "Create a target."},
    {"CREATE_TASK", "Create a task."},
    {"DELETE_AGENT", "Delete an agent."},
    {"DELETE_CONFIG", "Delete a config."},
    {"DELETE_ALERT", "Delete an alert."},
    {"DELETE_FILTER", "Delete a filter."},
    {"DELETE_LSC_CREDENTIAL", "Delete a local security check credential."},
    {"DELETE_NOTE", "Delete a note."},
    {"DELETE_OVERRIDE", "Delete an override."},
    {"DELETE_PORT_LIST", "Delete a port list."},
    {"DELETE_PORT_RANGE", "Delete a port range."},
    {"DELETE_REPORT", "Delete a report."},
    {"DELETE_REPORT_FORMAT", "Delete a report format."},
    {"DELETE_SCHEDULE", "Delete a schedule."},
    {"DELETE_SLAVE", "Delete a slave."},
    {"DELETE_TARGET", "Delete a target."},
    {"DELETE_TASK", "Delete a task."},
    {"EMPTY_TRASHCAN", "Empty the trashcan."},
    {"GET_AGENTS", "Get all agents."},
    {"GET_CONFIGS", "Get all configs."},
    {"GET_DEPENDENCIES", "Get dependencies for all available NVTs."},
    {"GET_ALERTS", "Get all alerts."},
    {"GET_FILTERS", "Get all filters."},
    {"GET_LSC_CREDENTIALS", "Get all local security check credentials."},
    {"GET_NOTES", "Get all notes."},
    {"GET_NVTS", "Get one or all available NVTs."},
    {"GET_NVT_FAMILIES", "Get a list of all NVT families."},
    {"GET_NVT_FEED_CHECKSUM", "Get checksum for entire NVT collection."},
    {"GET_OVERRIDES", "Get all overrides."},
    {"GET_PORT_LISTS", "Get all port lists."},
    {"GET_PREFERENCES", "Get preferences for all available NVTs."},
    {"GET_REPORTS", "Get all reports."},
    {"GET_REPORT_FORMATS", "Get all report formats."},
    {"GET_RESULTS", "Get results."},
    {"GET_SCHEDULES", "Get all schedules."},
    {"GET_SETTINGS", "Get all settings."},
    {"GET_SLAVES", "Get all slaves."},
    {"GET_SYSTEM_REPORTS", "Get all system reports."},
    {"GET_TARGET_LOCATORS", "Get configured target locators."},
    {"GET_TARGETS", "Get all targets."},
    {"GET_TASKS", "Get all tasks."},
    {"GET_VERSION", "Get the OpenVAS Manager Protocol version."},
    {"GET_INFO", "Get raw information for a given item."},
    {"HELP", "Get this help text."},
    {"MODIFY_AGENT", "Modify an existing agent."},
    {"MODIFY_ALERT", "Modify an existing alert."},
    {"MODIFY_CONFIG", "Update an existing config."},
    {"MODIFY_LSC_CREDENTIAL", "Modify an existing LSC credential."},
    {"MODIFY_FILTER", "Modify an existing filter."},
    {"MODIFY_NOTE", "Modify an existing note."},
    {"MODIFY_OVERRIDE", "Modify an existing override."},
    {"MODIFY_PORT_LIST", "Modify an existing port list."},
    {"MODIFY_REPORT", "Modify an existing report."},
    {"MODIFY_REPORT_FORMAT", "Modify an existing report format."},
    {"MODIFY_SCHEDULE", "Modify an existing schedule."},
    {"MODIFY_SETTING", "Modify an existing setting."},
    {"MODIFY_SLAVE", "Modify an existing slave."},
    {"MODIFY_TARGET", "Modify an existing target."},
    {"MODIFY_TASK", "Update an existing task."},
    {"PAUSE_TASK", "Pause a running task."},
    {"RESTORE", "Restore a resource."},
    {"RESUME_OR_START_TASK", "Resume task if stopped, else start task."},
    {"RESUME_PAUSED_TASK", "Resume a paused task."},
    {"RESUME_STOPPED_TASK", "Resume a stopped task."},
    {"RUN_WIZARD", "Run a wizard."},
    {"START_TASK", "Manually start an existing task."},
    {"STOP_TASK", "Stop a running task."},
    {"TEST_ALERT", "Run an alert."},
    {"VERIFY_AGENT", "Verify an agent."},
    {"VERIFY_REPORT_FORMAT", "Verify a report format."},
    {NULL, NULL}};


/* Status codes. */

/* HTTP status codes used:
 *
 *     200 OK
 *     201 Created
 *     202 Accepted
 *     400 Bad request
 *     401 Must auth
 *     404 Missing
 */

/**
 * @brief Response code for a syntax error.
 */
#define STATUS_ERROR_SYNTAX            "400"

/**
 * @brief Response code when authorisation is required.
 */
#define STATUS_ERROR_MUST_AUTH         "401"

/**
 * @brief Response code when authorisation is required.
 */
#define STATUS_ERROR_MUST_AUTH_TEXT    "Authenticate first"

/**
 * @brief Response code for forbidden access.
 */
#define STATUS_ERROR_ACCESS            "403"

/**
 * @brief Response code text for forbidden access.
 */
#define STATUS_ERROR_ACCESS_TEXT       "Access to resource forbidden"

/**
 * @brief Response code for a missing resource.
 */
#define STATUS_ERROR_MISSING           "404"

/**
 * @brief Response code text for a missing resource.
 */
#define STATUS_ERROR_MISSING_TEXT      "Resource missing"

/**
 * @brief Response code for a busy resource.
 */
#define STATUS_ERROR_BUSY              "409"

/**
 * @brief Response code text for a busy resource.
 */
#define STATUS_ERROR_BUSY_TEXT         "Resource busy"

/**
 * @brief Response code when authorisation failed.
 */
#define STATUS_ERROR_AUTH_FAILED       "400"

/**
 * @brief Response code text when authorisation failed.
 */
#define STATUS_ERROR_AUTH_FAILED_TEXT  "Authentication failed"

/**
 * @brief Response code on success.
 */
#define STATUS_OK                      "200"

/**
 * @brief Response code text on success.
 */
#define STATUS_OK_TEXT                 "OK"

/**
 * @brief Response code on success, when a resource is created.
 */
#define STATUS_OK_CREATED              "201"

/**
 * @brief Response code on success, when a resource is created.
 */
#define STATUS_OK_CREATED_TEXT         "OK, resource created"

/**
 * @brief Response code on success, when the operation will finish later.
 */
#define STATUS_OK_REQUESTED            "202"

/**
 * @brief Response code text on success, when the operation will finish later.
 */
#define STATUS_OK_REQUESTED_TEXT       "OK, request submitted"

/**
 * @brief Response code for an internal error.
 */
#define STATUS_INTERNAL_ERROR          "500"

/**
 * @brief Response code text for an internal error.
 */
#define STATUS_INTERNAL_ERROR_TEXT     "Internal error"

/**
 * @brief Response code when a service is unavailable.
 */
#define STATUS_SERVICE_UNAVAILABLE     "503"

/**
 * @brief Response code text when a service is unavailable.
 */
#define STATUS_SERVICE_UNAVAILABLE_TEXT "Service unavailable"

/**
 * @brief Response code when a service is down.
 */
#define STATUS_SERVICE_DOWN            "503"

/**
 * @brief Response code text when a service is down.
 */
#define STATUS_SERVICE_DOWN_TEXT       "Service temporarily down"


/* OMP parser. */

/**
 * @brief A handle on an OMP parser.
 */
typedef struct
{
  int (*client_writer) (const char*, void*);  ///< Writes to the client.
  void* client_writer_data;       ///< Argument to client_writer.
  int importing;                  ///< Whether the current op is importing.
  int read_over;                  ///< Read over any child elements.
  int parent_state;               ///< Parent state when reading over.
  gchar **disabled_commands;      ///< Disabled commands.
} omp_parser_t;

static int
process_omp (omp_parser_t *, const gchar *, gchar **);

/**
 * @brief Create an OMP parser.
 *
 * @param[in]  write_to_client       Function to write to client.
 * @param[in]  write_to_client_data  Argument to \p write_to_client.
 * @param[in]  disable               Commands to disable.  Freed by
 *                                   omp_parser_free.
 *
 * @return An OMP parser.
 */
omp_parser_t *
omp_parser_new (int (*write_to_client) (const char*, void*), void* write_to_client_data,
                gchar **disable)
{
  omp_parser_t *omp_parser = (omp_parser_t*) g_malloc0 (sizeof (omp_parser_t));
  omp_parser->client_writer = write_to_client;
  omp_parser->client_writer_data = write_to_client_data;
  omp_parser->read_over = 0;
  omp_parser->disabled_commands = disable;
  return omp_parser;
}

/**
 * @brief Free an OMP parser.
 *
 * @param[in]  omp_parser  OMP parser.
 *
 * @return An OMP parser.
 */
void
omp_parser_free (omp_parser_t *omp_parser)
{
  g_strfreev (omp_parser->disabled_commands);
  g_free (omp_parser);
}

/**
 * @brief Check if command has been disabled.
 *
 * @param[in]  omp_parser  Parser.
 * @param[in]  name        Command name.
 *
 * @return 1 disabled, 0 enabled.
 */
static int
command_disabled (omp_parser_t *omp_parser, const gchar *name)
{
  gchar **disabled;
  disabled = omp_parser->disabled_commands;
  if (disabled)
    while (*disabled)
      {
        if (strcasecmp (*disabled, name) == 0)
          return 1;
        disabled++;
      }
  return 0;
}


/* Command data passed between parser callbacks. */

/**
 * @brief Create a new preference.
 *
 * @param[in]  name      Name of preference.
 * @param[in]  type      Type of preference.
 * @param[in]  value     Value of preference.
 * @param[in]  nvt_name  Name of NVT of preference.
 * @param[in]  nvt_oid   OID of NVT of preference.
 * @param[in]  alts      Array of gchar's.  Alternative values for type radio.
 *
 * @return Newly allocated preference.
 */
static gpointer
preference_new (char *name, char *type, char *value, char *nvt_name,
                char *nvt_oid, array_t *alts)
{
  preference_t *preference;

  preference = (preference_t*) g_malloc0 (sizeof (preference_t));
  preference->name = name;
  preference->type = type;
  preference->value = value;
  preference->nvt_name = nvt_name;
  preference->nvt_oid = nvt_oid;
  preference->alts = alts;

  return preference;
}

/**
 * @brief Create a new NVT selector.
 *
 * @param[in]  name           Name of NVT selector.
 * @param[in]  type           Type of NVT selector.
 * @param[in]  include        Include/exclude flag.
 * @param[in]  family_or_nvt  Family or NVT.
 *
 * @return Newly allocated NVT selector.
 */
static gpointer
nvt_selector_new (char *name, char *type, int include, char *family_or_nvt)
{
  nvt_selector_t *selector;

  selector = (nvt_selector_t*) g_malloc0 (sizeof (nvt_selector_t));
  selector->name = name;
  selector->type = type;
  selector->include = include;
  selector->family_or_nvt = family_or_nvt;

  return selector;
}

/**
 * @brief Command data for the create_agent command.
 */
typedef struct
{
  char *comment;                  ///< Comment.
  char *copy;                     ///< UUID of resource to copy.
  char *howto_install;            ///< Install HOWTO.
  char *howto_use;                ///< Usage HOWTO.
  char *installer;                ///< Installer content.
  char *installer_filename;       ///< Installer filename.
  char *installer_signature;      ///< Installer signature.
  char *name;                     ///< Agent name.
} create_agent_data_t;

/**
 * @brief Free members of a create_agent_data_t and set them to NULL.
 */
static void
create_agent_data_reset (create_agent_data_t *data)
{
  free (data->comment);
  free (data->copy);
  free (data->howto_install);
  free (data->howto_use);
  free (data->installer);
  free (data->installer_filename);
  free (data->installer_signature);
  free (data->name);

  memset (data, 0, sizeof (create_agent_data_t));
}

/**
 * @brief Command data for the import part of the create_config command.
 */
typedef struct
{
  int import;                        ///< The import element was present.
  char *comment;                     ///< Comment.
  char *name;                        ///< Config name.
  array_t *nvt_selectors;            ///< Array of nvt_selector_t's.
  char *nvt_selector_name;           ///< In NVT_SELECTORS name of selector.
  char *nvt_selector_type;           ///< In NVT_SELECTORS type of selector.
  char *nvt_selector_include;        ///< In NVT_SELECTORS include/exclude flag.
  char *nvt_selector_family_or_nvt;  ///< In NVT_SELECTORS family/NVT flag.
  array_t *preferences;              ///< Array of preference_t's.
  array_t *preference_alts;          ///< Array of gchar's in PREFERENCES.
  char *preference_alt;              ///< Single radio alternative in PREFERENCE.
  char *preference_name;             ///< Name in PREFERENCE.
  char *preference_nvt_name;         ///< NVT name in PREFERENCE.
  char *preference_nvt_oid;          ///< NVT OID in PREFERENCE.
  char *preference_type;             ///< Type in PREFERENCE.
  char *preference_value;            ///< Value in PREFERENCE.
} import_config_data_t;

/**
 * @brief Command data for the create_config command.
 */
typedef struct
{
  char *comment;                     ///< Comment.
  char *copy;                        ///< Config to copy.
  import_config_data_t import;       ///< Config to import.
  char *name;                        ///< Name.
  char *rcfile;                      ///< RC file from which to create config.
} create_config_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
create_config_data_reset (create_config_data_t *data)
{
  import_config_data_t *import = (import_config_data_t*) &data->import;

  free (data->comment);
  free (data->copy);

  free (import->comment);
  free (import->name);
  array_free (import->nvt_selectors);
  free (import->nvt_selector_name);
  free (import->nvt_selector_type);
  free (import->nvt_selector_family_or_nvt);

  if (import->preferences)
    {
      guint index = import->preferences->len;
      while (index--)
        {
          const preference_t *preference;
          preference = (preference_t*) g_ptr_array_index (import->preferences,
                                                          index);
          if (preference)
            array_free (preference->alts);
        }
      array_free (import->preferences);
    }

  free (import->preference_alt);
  free (import->preference_name);
  free (import->preference_nvt_name);
  free (import->preference_nvt_oid);
  free (import->preference_type);
  free (import->preference_value);

  free (data->name);
  free (data->rcfile);

  memset (data, 0, sizeof (create_config_data_t));
}

/**
 * @brief Command data for the create_alert command.
 *
 * The pointers in the *_data arrays point to memory that contains two
 * strings concatentated, with a single \\0 between them.  The first string
 * is the name of the extra data (for example "To Address"), the second is
 * the value the the data (for example "alice@example.org").
 */
typedef struct
{
  char *comment;             ///< Comment.
  char *copy;                ///< UUID of alert to copy.
  char *condition;           ///< Condition for alert, e.g. "Always".
  array_t *condition_data;   ///< Array of pointers.  Extra data for condition.
  char *event;               ///< Event that will cause alert.
  array_t *event_data;       ///< Array of pointers.  Extra data for event.
  char *filter_id;           ///< UUID of filter.
  char *method;              ///< Method of alert, e.g. "Email".
  array_t *method_data;      ///< Array of pointer.  Extra data for method.
  char *name;                ///< Name of alert.
  char *part_data;           ///< Second part of data during *_data: value.
  char *part_name;           ///< First part of data during *_data: name.
} create_alert_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
create_alert_data_reset (create_alert_data_t *data)
{
  free (data->comment);
  free (data->copy);
  free (data->condition);
  array_free (data->condition_data);
  free (data->event);
  array_free (data->event_data);
  free (data->filter_id);
  free (data->method);
  array_free (data->method_data);
  free (data->name);
  free (data->part_data);
  free (data->part_name);

  memset (data, 0, sizeof (create_alert_data_t));
}

/**
 * @brief Command data for the create_filter command.
 */
typedef struct
{
  char *comment;                 ///< Comment.
  char *copy;                    ///< UUID of resource to copy.
  char *make_name_unique;        ///< Boolean.  Whether to make name unique.
  char *name;                    ///< Name of new filter.
  char *term;                    ///< Filter term.
  char *type;                    ///< Type of new filter.
} create_filter_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
create_filter_data_reset (create_filter_data_t *data)
{
  free (data->comment);
  free (data->copy);
  free (data->make_name_unique);
  free (data->name);
  free (data->term);
  free (data->type);

  memset (data, 0, sizeof (create_filter_data_t));
}

/**
 * @brief Command data for the create_lsc_credential command.
 */
typedef struct
{
  char *comment;           ///< Comment.
  char *copy;              ///< UUID of resource to copy.
  int key;                 ///< Whether the command included a key element.
  char *key_phrase;        ///< Passphrase for key.
  char *key_private;       ///< Private key from key.
  char *key_public;        ///< Public key from key.
  char *login;             ///< Login name.
  char *name;              ///< LSC credential name.
  char *password;          ///< Password associated with login name.
} create_lsc_credential_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
create_lsc_credential_data_reset (create_lsc_credential_data_t *data)
{
  free (data->comment);
  free (data->copy);
  free (data->key_phrase);
  free (data->key_private);
  free (data->key_public);
  free (data->login);
  free (data->name);
  free (data->password);

  memset (data, 0, sizeof (create_lsc_credential_data_t));
}

/**
 * @brief Command data for the create_note command.
 */
typedef struct
{
  char *active;       ///< Whether the note is active.
  char *copy;         ///< UUID of resource to copy.
  char *hosts;        ///< Hosts to which to limit override.
  char *nvt_oid;      ///< NVT to which to limit override.
  char *port;         ///< Port to which to limit override.
  char *result_id;    ///< ID of result to which to limit override.
  char *task_id;      ///< ID of task to which to limit override.
  char *text;         ///< Text of override.
  char *threat;       ///< Threat to which to limit override.
} create_note_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
create_note_data_reset (create_note_data_t *data)
{
  free (data->active);
  free (data->copy);
  free (data->hosts);
  free (data->nvt_oid);
  free (data->port);
  free (data->result_id);
  free (data->task_id);
  free (data->text);
  free (data->threat);

  memset (data, 0, sizeof (create_note_data_t));
}

/**
 * @brief Command data for the create_override command.
 */
typedef struct
{
  char *active;       ///< Whether the override is active.
  char *copy;         ///< UUID of resource to copy.
  char *hosts;        ///< Hosts to which to limit override.
  char *new_threat;   ///< New threat value of overridden results.
  char *nvt_oid;      ///< NVT to which to limit override.
  char *port;         ///< Port to which to limit override.
  char *result_id;    ///< ID of result to which to limit override.
  char *task_id;      ///< ID of task to which to limit override.
  char *text;         ///< Text of override.
  char *threat;       ///< Threat to which to limit override.
} create_override_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
create_override_data_reset (create_override_data_t *data)
{
  free (data->active);
  free (data->copy);
  free (data->hosts);
  free (data->new_threat);
  free (data->nvt_oid);
  free (data->port);
  free (data->result_id);
  free (data->task_id);
  free (data->text);
  free (data->threat);

  memset (data, 0, sizeof (create_override_data_t));
}

/**
 * @brief A port range.
 */
struct create_port_list_range
{
  char *comment;            ///< Comment.
  char *end;                ///< End.
  char *id;                 ///< UUID.
  char *start;              ///< Start.
  char *type;               ///< Type.
};

/**
 * @brief Port range type.
 */
typedef struct create_port_list_range create_port_list_range_t;

/**
 * @brief Command data for the create_port_list command.
 */
typedef struct
{
  char *comment;                 ///< Comment.
  char *id;                      ///< UUID.
  char *copy;                    ///< UUID of Port List to copy.
  int import;                    ///< Import flag.
  char *name;                    ///< Name of new port list.
  char *port_range;              ///< Port range for new port list.
  create_port_list_range_t *range;  ///< Current port range for import.
  array_t *ranges;               ///< Port ranges for import.
} create_port_list_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
create_port_list_data_reset (create_port_list_data_t *data)
{
  free (data->comment);
  free (data->copy);
  free (data->name);
  free (data->port_range);

  if (data->ranges)
    {
      guint index;

      index = data->ranges->len;
      while (index--)
        {
          create_port_list_range_t *range;
          range = (create_port_list_range_t*) g_ptr_array_index (data->ranges,
                                                                 index);
          if (range)
            {
              free (range->comment);
              free (range->end);
              free (range->id);
              free (range->start);
              free (range->type);
            }
        }
      array_free (data->ranges);
    }

  memset (data, 0, sizeof (create_port_list_data_t));
}

/**
 * @brief Command data for the create_port_range command.
 */
typedef struct
{
  char *comment;                 ///< Comment.
  char *end;                     ///< Last port.
  char *port_list_id;            ///< Port list for new port range.
  char *start;                   ///< First port.
  char *type;                    ///< Type of new port range.
} create_port_range_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
create_port_range_data_reset (create_port_range_data_t *data)
{
  free (data->comment);
  free (data->end);
  free (data->port_list_id);
  free (data->start);
  free (data->type);

  memset (data, 0, sizeof (create_port_range_data_t));
}

/**
 * @brief Command data for the create_report command.
 */
typedef struct
{
  char *detail_name;              ///< Name of current host detail.
  char *detail_value;             ///< Value of current host detail.
  char *detail_source_name;       ///< Name of source of current host detail.
  char *detail_source_type;       ///< Type of source of current host detail.
  char *detail_source_desc;       ///< Description of source of current detail.
  array_t *details;               ///< Host details.
  char *host_end;                 ///< End time for a host.
  char *host_end_host;            ///< Host name for end time.
  array_t *host_ends;             ///< All host ends.
  char *host_start;               ///< Start time for a host.
  char *host_start_host;          ///< Host name for start time.
  array_t *host_starts;           ///< All host starts.
  char *ip;                       ///< Current host for host details.
  char *result_description;       ///< Description of NVT for current result.
  char *result_host;              ///< Host for current result.
  char *result_nvt_oid;           ///< OID of NVT for current result.
  char *result_port;              ///< Port for current result.
  char *result_subnet;            ///< Subnet for current result.
  char *result_threat;            ///< Message type for current result.
  array_t *results;               ///< All results.
  char *scan_end;                 ///< End time for a scan.
  char *scan_start;               ///< Start time for a scan.
  char *task_comment;             ///< Comment for container task.
  char *task_id;                  ///< ID of container task.
  char *task_name;                ///< Name for container task.
  char *type;                     ///< Type of report.
  int wrapper;                    ///< Whether there was a wrapper REPORT.
} create_report_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
create_report_data_reset (create_report_data_t *data)
{
  if (data->details)
    {
      guint index = data->details->len;
      while (index--)
        {
          host_detail_t *detail;
          detail = (host_detail_t*) g_ptr_array_index (data->details, index);
          if (detail)
            host_detail_free (detail);
        }
      array_free (data->details);
    }
  free (data->host_end);
  free (data->host_start);
  free (data->ip);
  free (data->result_description);
  free (data->result_host);
  free (data->result_nvt_oid);
  free (data->result_port);
  free (data->result_subnet);
  free (data->result_threat);
  if (data->results)
    {
      guint index = data->results->len;
      while (index--)
        {
          create_report_result_t *result;
          result = (create_report_result_t*) g_ptr_array_index (data->results,
                                                                index);
          if (result)
            {
              free (result->host);
              free (result->description);
              free (result->nvt_oid);
              free (result->port);
              free (result->subnet);
            }
        }
      array_free (data->results);
    }
  free (data->scan_end);
  free (data->scan_start);
  free (data->task_comment);
  free (data->task_id);
  free (data->task_name);
  free (data->type);

  memset (data, 0, sizeof (create_report_data_t));
}

/**
 * @brief Command data for the create_report_format command.
 */
typedef struct
{
  char *content_type;     ///< Content type.
  char *description;      ///< Description.
  char *extension;        ///< File extension.
  char *file;             ///< Current file during ...GRFR_REPORT_FORMAT_FILE.
  char *file_name;        ///< Name of current file.
  array_t *files;         ///< All files.
  char *global;           ///< Global flag.
  char *id;               ///< ID.
  int import;             ///< Boolean.  Whether to import a format.
  char *name;             ///< Name.
  char *param_value;      ///< Param value during ...GRFR_REPORT_FORMAT_PARAM.
  char *param_default;    ///< Default value for above param.
  char *param_name;       ///< Name of above param.
  char *param_option;     ///< Current option of above param.
  array_t *param_options; ///< Options for above param.
  array_t *params_options; ///< Options for all params.
  char *param_type;       ///< Type of above param.
  char *param_type_min;   ///< Min qualifier of above type.
  char *param_type_max;   ///< Max qualifier of above type.
  array_t *params;        ///< All params.
  char *signature;        ///< Signature.
  char *summary;          ///< Summary.
  char *copy;             ///< UUID of Report Format to copy.
} create_report_format_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
create_report_format_data_reset (create_report_format_data_t *data)
{
  free (data->content_type);
  free (data->description);
  free (data->extension);
  free (data->file);
  free (data->file_name);
  array_free (data->files);
  free (data->global);
  free (data->id);
  free (data->name);
  free (data->copy);
  free (data->param_default);
  free (data->param_name);

  if (data->params_options)
    {
      guint index = data->params_options->len;
      while (index--)
        {
          array_t *options;
          options = (array_t*) g_ptr_array_index (data->params_options, index);
          if (options)
            array_free (options);
        }
      g_ptr_array_free (data->params_options, TRUE);
    }

  free (data->param_type);
  free (data->param_type_min);
  free (data->param_type_max);
  free (data->param_value);
  array_free (data->params);
  free (data->summary);

  memset (data, 0, sizeof (create_report_format_data_t));
}

/**
 * @brief Command data for the create_schedule command.
 */
typedef struct
{
  char *name;                    ///< Name for new schedule.
  char *comment;                 ///< Comment.
  char *copy;                    ///< UUID of resource to copy.
  char *first_time_day_of_month; ///< Day of month schedule must first run.
  char *first_time_hour;         ///< Hour schedule must first run.
  char *first_time_minute;       ///< Minute schedule must first run.
  char *first_time_month;        ///< Month schedule must first run.
  char *first_time_year;         ///< Year schedule must first run.
  char *period;                  ///< Period of schedule (how often it runs).
  char *period_unit;             ///< Unit of period: "hour", "day", "week", ....
  char *duration;                ///< Duration of schedule (how long it runs for).
  char *duration_unit;           ///< Unit of duration: "hour", "day", "week", ....
} create_schedule_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
create_schedule_data_reset (create_schedule_data_t *data)
{
  free (data->name);
  free (data->copy);
  free (data->comment);
  free (data->first_time_day_of_month);
  free (data->first_time_hour);
  free (data->first_time_minute);
  free (data->first_time_month);
  free (data->first_time_year);
  free (data->period);
  free (data->period_unit);
  free (data->duration);
  free (data->duration_unit);

  memset (data, 0, sizeof (create_schedule_data_t));
}

/**
 * @brief Command data for the create_slave command.
 */
typedef struct
{
  char *comment;                 ///< Comment.
  char *host;                    ///< Host for new slave.
  char *copy;                    ///< UUID of slave to copy.
  char *login;                   ///< Login on slave.
  char *name;                    ///< Name of new slave.
  char *password;                ///< Password for login.
  char *port;                    ///< Port on host.
} create_slave_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
create_slave_data_reset (create_slave_data_t *data)
{
  free (data->comment);
  free (data->copy);
  free (data->host);
  free (data->login);
  free (data->name);
  free (data->password);
  free (data->port);

  memset (data, 0, sizeof (create_slave_data_t));
}

/**
 * @brief Command data for the create_target command.
 */
typedef struct
{
  char *comment;                 ///< Comment.
  char *copy;                    ///< UUID of resource to copy.
  char *hosts;                   ///< Hosts for new target.
  char *port_list_id;            ///< Port list for new target.
  char *port_range;              ///< Port range for new target.
  char *ssh_lsc_credential_id;   ///< SSH LSC credential for new target.
  char *ssh_port;                ///< Port for SSH LSC.
  char *smb_lsc_credential_id;   ///< SMB LSC credential for new target.
  char *make_name_unique;        ///< Boolean.  Whether to make name unique.
  char *name;                    ///< Name of new target.
  char *target_locator;          ///< Target locator (source name).
  char *target_locator_password; ///< Target locator credentials: password.
  char *target_locator_username; ///< Target locator credentials: username.
} create_target_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
create_target_data_reset (create_target_data_t *data)
{
  free (data->comment);
  free (data->copy);
  free (data->hosts);
  free (data->port_list_id);
  free (data->port_range);
  free (data->ssh_lsc_credential_id);
  free (data->ssh_port);
  free (data->smb_lsc_credential_id);
  free (data->make_name_unique);
  free (data->name);
  free (data->target_locator);
  free (data->target_locator_password);
  free (data->target_locator_username);

  memset (data, 0, sizeof (create_target_data_t));
}

/**
 * @brief Command data for the create_task command.
 */
typedef struct
{
  char *config_id;      ///< ID of task config.
  array_t *alerts;      ///< IDs of alerts.
  char *copy;           ///< UUID of resource to copy.
  char *observers;      ///< Space separated names of observer users.
  name_value_t *preference;  ///< Current preference.
  array_t *preferences; ///< Preferences.
  char *schedule_id;    ///< ID of task schedule.
  char *slave_id;       ///< ID of task slave.
  char *target_id;      ///< ID of task target.
  task_t task;          ///< ID of new task.
} create_task_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
create_task_data_reset (create_task_data_t *data)
{
  free (data->config_id);
  free (data->copy);
  array_free (data->alerts);
  free (data->observers);
  if (data->preferences)
    {
      guint index = data->preferences->len;
      while (index--)
        {
          name_value_t *pair;
          pair = (name_value_t*) g_ptr_array_index (data->preferences, index);
          if (pair)
            {
              g_free (pair->name);
              g_free (pair->value);
            }
        }
    }
  array_free (data->preferences);
  free (data->schedule_id);
  free (data->slave_id);
  free (data->target_id);

  memset (data, 0, sizeof (create_task_data_t));
}

/**
 * @brief Command data for the delete_agent command.
 */
typedef struct
{
  char *agent_id;   ///< ID of agent to delete.
  int ultimate;     ///< Boolean.  Whether to remove entirely or to trashcan.
} delete_agent_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
delete_agent_data_reset (delete_agent_data_t *data)
{
  free (data->agent_id);

  memset (data, 0, sizeof (delete_agent_data_t));
}

/**
 * @brief Command data for the delete_config command.
 */
typedef struct
{
  char *config_id;   ///< ID of config to delete.
  int ultimate;      ///< Boolean.  Whether to remove entirely or to trashcan.
} delete_config_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
delete_config_data_reset (delete_config_data_t *data)
{
  free (data->config_id);

  memset (data, 0, sizeof (delete_config_data_t));
}

/**
 * @brief Command data for the delete_alert command.
 */
typedef struct
{
  char *alert_id;   ///< ID of alert to delete.
  int ultimate;     ///< Boolean.  Whether to remove entirely or to trashcan.
} delete_alert_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
delete_alert_data_reset (delete_alert_data_t *data)
{
  free (data->alert_id);

  memset (data, 0, sizeof (delete_alert_data_t));
}

/**
 * @brief Command data for the delete_filter command.
 */
typedef struct
{
  char *filter_id;   ///< ID of filter to delete.
  int ultimate;      ///< Boolean.  Whether to remove entirely or to trashcan.
} delete_filter_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
delete_filter_data_reset (delete_filter_data_t *data)
{
  free (data->filter_id);

  memset (data, 0, sizeof (delete_filter_data_t));
}

/**
 * @brief Command data for the delete_lsc_credential command.
 */
typedef struct
{
  char *lsc_credential_id;   ///< ID of LSC credential to delete.
  int ultimate;      ///< Boolean.  Whether to remove entirely or to trashcan.
} delete_lsc_credential_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
delete_lsc_credential_data_reset (delete_lsc_credential_data_t *data)
{
  free (data->lsc_credential_id);

  memset (data, 0, sizeof (delete_lsc_credential_data_t));
}

/**
 * @brief Command data for the delete_note command.
 */
typedef struct
{
  char *note_id;   ///< ID of note to delete.
  int ultimate;    ///< Boolean.  Whether to remove entirely or to trashcan.
} delete_note_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
delete_note_data_reset (delete_note_data_t *data)
{
  free (data->note_id);

  memset (data, 0, sizeof (delete_note_data_t));
}

/**
 * @brief Command data for the delete_override command.
 */
typedef struct
{
  char *override_id;   ///< ID of override to delete.
  int ultimate;        ///< Boolean.  Whether to remove entirely or to trashcan.
} delete_override_data_t;

/**
 * @brief Command data for the delete_port_list command.
 */
typedef struct
{
  char *port_list_id;  ///< ID of port list to delete.
  int ultimate;        ///< Boolean.  Whether to remove entirely or to trashcan.
} delete_port_list_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
delete_port_list_data_reset (delete_port_list_data_t *data)
{
  free (data->port_list_id);

  memset (data, 0, sizeof (delete_port_list_data_t));
}

/**
 * @brief Command data for the delete_port_range command.
 */
typedef struct
{
  char *port_range_id;  ///< ID of port range to delete.
} delete_port_range_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
delete_port_range_data_reset (delete_port_range_data_t *data)
{
  free (data->port_range_id);

  memset (data, 0, sizeof (delete_port_range_data_t));
}

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
delete_override_data_reset (delete_override_data_t *data)
{
  free (data->override_id);

  memset (data, 0, sizeof (delete_override_data_t));
}

/**
 * @brief Command data for the delete_report command.
 */
typedef struct
{
  char *report_id;   ///< ID of report to delete.
} delete_report_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
delete_report_data_reset (delete_report_data_t *data)
{
  free (data->report_id);

  memset (data, 0, sizeof (delete_report_data_t));
}

/**
 * @brief Command data for the delete_report_format command.
 */
typedef struct
{
  char *report_format_id;   ///< ID of report format to delete.
  int ultimate;     ///< Boolean.  Whether to remove entirely or to trashcan.
} delete_report_format_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
delete_report_format_data_reset (delete_report_format_data_t *data)
{
  free (data->report_format_id);

  memset (data, 0, sizeof (delete_report_format_data_t));
}

/**
 * @brief Command data for the delete_schedule command.
 */
typedef struct
{
  char *schedule_id;   ///< ID of schedule to delete.
  int ultimate;        ///< Boolean.  Whether to remove entirely or to trashcan.
} delete_schedule_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
delete_schedule_data_reset (delete_schedule_data_t *data)
{
  free (data->schedule_id);

  memset (data, 0, sizeof (delete_schedule_data_t));
}

/**
 * @brief Command data for the delete_slave command.
 */
typedef struct
{
  char *slave_id;   ///< ID of slave to delete.
  int ultimate;     ///< Boolean.  Whether to remove entirely or to trashcan.
} delete_slave_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
delete_slave_data_reset (delete_slave_data_t *data)
{
  free (data->slave_id);

  memset (data, 0, sizeof (delete_slave_data_t));
}

/**
 * @brief Command data for the delete_target command.
 */
typedef struct
{
  char *target_id;   ///< ID of target to delete.
  int ultimate;      ///< Boolean.  Whether to remove entirely or to trashcan.
} delete_target_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
delete_target_data_reset (delete_target_data_t *data)
{
  free (data->target_id);

  memset (data, 0, sizeof (delete_target_data_t));
}

/**
 * @brief Command data for the delete_task command.
 */
typedef struct
{
  char *task_id;   ///< ID of task to delete.
  int ultimate;    ///< Boolean.  Whether to remove entirely or to trashcan.
} delete_task_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
delete_task_data_reset (delete_task_data_t *data)
{
  free (data->task_id);

  memset (data, 0, sizeof (delete_task_data_t));
}

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
get_data_reset (get_data_t *data)
{
  free (data->actions);
  free (data->id);
  free (data->filter);
  free (data->filt_id);
  free (data->subtype);
  free (data->type);

  memset (data, 0, sizeof (get_data_t));
}

/**
 * @brief Reset command data.
 *
 * @param[in]  data              GET operation data.
 * @param[in]  type              Resource type.
 * @param[in]  attribute_names   XML attribute names.
 * @param[in]  attribute_values  XML attribute values.
 *
 * @param[in]  data  Command data.
 */
static void
get_data_parse_attributes (get_data_t *data, const gchar *type,
                           const gchar **attribute_names,
                           const gchar **attribute_values)
{
  gchar *name;
  const gchar *attribute;

  data->type = g_strdup (type);

  append_attribute (attribute_names, attribute_values, "actions",
                    &data->actions);

  append_attribute (attribute_names, attribute_values, "filter",
                    &data->filter);

  name = g_strdup_printf ("%s_id", type);
  append_attribute (attribute_names, attribute_values, name,
                    &data->id);
  g_free (name);

  append_attribute (attribute_names, attribute_values, "filt_id",
                    &data->filt_id);

  if (find_attribute (attribute_names, attribute_values,
                      "trash", &attribute))
    data->trash = strcmp (attribute, "0");
  else
    data->trash = 0;

  if (find_attribute (attribute_names, attribute_values,
                      "details", &attribute))
    data->details = strcmp (attribute, "0");
  else
    data->details = 0;
}

/**
 * @brief Command data for the get_agents command.
 */
typedef struct
{
  get_data_t get;        ///< Get args.
  char *format;          ///< Format requested: "installer", "howto_use", ....
} get_agents_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
get_agents_data_reset (get_agents_data_t *data)
{
  get_data_reset (&data->get);
  free (data->format);

  memset (data, 0, sizeof (get_agents_data_t));
}

/**
 * @brief Command data for the get_configs command.
 */
typedef struct
{
  int families;          ///< Boolean.  Whether to include config families.
  int preferences;       ///< Boolean.  Whether to include config preferences.
  get_data_t get;        ///< Get args.
  int tasks;             ///< Boolean.  Whether to include tasks that use scan config.
} get_configs_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
get_configs_data_reset (get_configs_data_t *data)
{
  get_data_reset (&data->get);
  memset (data, 0, sizeof (get_configs_data_t));
}

/**
 * @brief Command data for the get_dependencies command.
 */
typedef struct
{
  char *nvt_oid;  ///< OID of single NVT whose  dependencies to get.
} get_dependencies_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
get_dependencies_data_reset (get_dependencies_data_t *data)
{
  free (data->nvt_oid);

  memset (data, 0, sizeof (get_dependencies_data_t));
}

/**
 * @brief Command data for the get_alerts command.
 */
typedef struct
{
  get_data_t get;   ///< Get args.
  int tasks;        ///< Boolean.  Whether to include tasks that use alert.
} get_alerts_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
get_alerts_data_reset (get_alerts_data_t *data)
{
  get_data_reset (&data->get);
  memset (data, 0, sizeof (get_alerts_data_t));
}

/**
 * @brief Command data for the get_filters command.
 */
typedef struct
{
  get_data_t get;    ///< Get args.
  int alerts;        ///< Boolean.  Whether to include alerts that use filter.
} get_filters_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
get_filters_data_reset (get_filters_data_t *data)
{
  get_data_reset (&data->get);
  memset (data, 0, sizeof (get_filters_data_t));
}

/**
 * @brief Command data for the get_info command.
 */
typedef struct
{
  char *type;         ///< Requested information type.
  char *name;         ///< Name of the info
  get_data_t get;     ///< Get Args.
  int details;        ///< Boolean. Weather to include full details.
} get_info_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
get_info_data_reset (get_info_data_t *data)
{
  free (data->type);
  free (data->name);
  get_data_reset (&data->get);

  memset (data, 0, sizeof (get_info_data_t));
}

/**
 * @brief Command data for the get_lsc_credentials command.
 */
typedef struct
{
  char *format;      ///< Format requested: "key", "deb", ....
  get_data_t get;    ///< Get Args.
  int targets;       ///< Boolean.  Whether to return targets using credential.
} get_lsc_credentials_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
get_lsc_credentials_data_reset (get_lsc_credentials_data_t *data)
{
  get_data_reset (&data->get);
  memset (data, 0, sizeof (get_lsc_credentials_data_t));
}

/**
 * @brief Command data for the get_notes command.
 */
typedef struct
{
  get_data_t get;        ///< Get args.
  char *note_id;         ///< ID of single note to get.
  char *nvt_oid;         ///< OID of NVT to which to limit listing.
  char *task_id;         ///< ID of task to which to limit listing.
  int result;            ///< Boolean.  Whether to include associated results.
} get_notes_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
get_notes_data_reset (get_notes_data_t *data)
{
  free (data->note_id);
  free (data->nvt_oid);
  free (data->task_id);

  memset (data, 0, sizeof (get_notes_data_t));
}

/**
 * @brief Command data for the get_nvts command.
 */
typedef struct
{
  char *actions;         ///< Actions.
  char *config_id;       ///< ID of config to which to limit NVT selection.
  int details;           ///< Boolean.  Whether to include full NVT details.
  char *family;          ///< Name of family to which to limit NVT selection.
  char *nvt_oid;         ///< Name of single NVT to get.
  int preference_count;  ///< Boolean.  Whether to include NVT preference count.
  int preferences;       ///< Boolean.  Whether to include NVT preferences.
  char *sort_field;      ///< Field to sort results on.
  int sort_order;        ///< Result sort order: 0 descending, else ascending.
  int timeout;           ///< Boolean.  Whether to include timeout preference.
} get_nvts_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
get_nvts_data_reset (get_nvts_data_t *data)
{
  free (data->actions);
  free (data->config_id);
  free (data->family);
  free (data->nvt_oid);
  free (data->sort_field);

  memset (data, 0, sizeof (get_nvts_data_t));
}

/**
 * @brief Command data for the get_nvt_families command.
 */
typedef struct
{
  int sort_order;        ///< Result sort order: 0 descending, else ascending.
} get_nvt_families_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
get_nvt_families_data_reset (get_nvt_families_data_t *data)
{
  memset (data, 0, sizeof (get_nvt_families_data_t));
}

/**
 * @brief Command data for the get_nvt_feed_checksum command.
 */
typedef struct
{
  char *algorithm;  ///< Algorithm requested by client.
} get_nvt_feed_checksum_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
get_nvt_feed_checksum_data_reset (get_nvt_feed_checksum_data_t *data)
{
  free (data->algorithm);

  memset (data, 0, sizeof (get_nvt_feed_checksum_data_t));
}

/**
 * @brief Command data for the get_overrides command.
 */
typedef struct
{
  get_data_t get;      ///< Get args.
  char *override_id;   ///< ID of override to get.
  char *nvt_oid;       ///< OID of NVT to which to limit listing.
  char *task_id;       ///< ID of task to which to limit listing.
  int result;          ///< Boolean.  Whether to include associated results.
} get_overrides_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
get_overrides_data_reset (get_overrides_data_t *data)
{
  free (data->override_id);
  free (data->nvt_oid);
  free (data->task_id);

  memset (data, 0, sizeof (get_overrides_data_t));
}

/**
 * @brief Command data for the get_port_lists command.
 */
typedef struct
{
  int targets;         ///< Boolean. Include targets that use Port List or not.
  get_data_t get;      ///< Get args.
} get_port_lists_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
get_port_lists_data_reset (get_port_lists_data_t *data)
{
  get_data_reset (&data->get);
  memset (data, 0, sizeof (get_port_lists_data_t));
}

/**
 * @brief Command data for the get_preferences command.
 */
typedef struct
{
  char *config_id;  ///< Config whose preference values to get.
  char *nvt_oid;    ///< Single NVT whose preferences to get.
  char *preference; ///< Single preference to get.
} get_preferences_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
get_preferences_data_reset (get_preferences_data_t *data)
{
  free (data->config_id);
  free (data->nvt_oid);
  free (data->preference);

  memset (data, 0, sizeof (get_preferences_data_t));
}

/**
 * @brief Command data for the get_reports command.
 */
typedef struct
{
  get_data_t get;        ///< Get args.
  int apply_overrides;   ///< Boolean.  Whether to apply overrides to results.
  char *delta_report_id; ///< ID of report to compare single report to.
  char *delta_states;    ///< Delta states (Changed Gone New Same) to include.
  char *format_id;       ///< ID of report format.
  char *alert_id;        ///< ID of alert.
  char *report_id;       ///< ID of single report to get.
  int first_result;      ///< Skip over results before this result number.
  int max_results;       ///< Maximum number of results return.
  int host_first_result; ///< Skip over results before this result number.
  int host_max_results;  ///< Maximum number of results return.
  char *sort_field;      ///< Field to sort results on.
  int sort_order;        ///< Result sort order: 0 descending, else ascending.
  char *levels;          ///< Letter encoded threat level filter.
  char *host_levels;     ///< Letter encoded threat level filter, for hosts.
  char *search_phrase;   ///< Search phrase result filter.
  char *host_search_phrase;  ///< Search phrase result filter.
  char *min_cvss_base;   ///< Minimum CVSS base filter.
  int autofp;            ///< Boolean.  Whether to apply auto FP filter.
  int show_closed_cves;  ///< Boolean.  Whether to include Closed CVEs detail.
  int notes;             ///< Boolean.  Whether to include associated notes.
  int notes_details;     ///< Boolean.  Whether to include details of above.
  int overrides;         ///< Boolean.  Whether to include associated overrides.
  int overrides_details; ///< Boolean.  Whether to include details of above.
  int result_hosts_only; ///< Boolean.  Whether to include only resulted hosts.
  char *type;            ///< Type of report.
  char *host;            ///< Host for asset report.
  char *pos;             ///< Position of report from end.
} get_reports_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
get_reports_data_reset (get_reports_data_t *data)
{
  get_data_reset (&data->get);
  free (data->delta_report_id);
  free (data->delta_states);
  free (data->format_id);
  free (data->alert_id);
  free (data->report_id);
  free (data->sort_field);
  free (data->levels);
  free (data->host_levels);
  free (data->search_phrase);
  free (data->host_search_phrase);
  free (data->min_cvss_base);
  free (data->type);
  free (data->host);
  free (data->pos);

  memset (data, 0, sizeof (get_reports_data_t));
}

/**
 * @brief Command data for the get_report_formats command.
 */
typedef struct
{
  get_data_t get;        ///< Get args.
  int alerts;   ///< Boolean.  Whether to include alerts that use Report Format
  int params;            ///< Boolean.  Whether to include params.
} get_report_formats_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
get_report_formats_data_reset (get_report_formats_data_t *data)
{
  get_data_reset (&data->get);
  memset (data, 0, sizeof (get_report_formats_data_t));
}

/**
 * @brief Command data for the get_results command.
 */
typedef struct
{
  int apply_overrides;   ///< Boolean.  Whether to apply overrides to results.
  int autofp;            ///< Boolean.  Whether to apply auto FP filter.
  char *result_id;       ///< ID of single result to get.
  char *task_id;         ///< Task associated with results.
  int notes;             ///< Boolean.  Whether to include associated notes.
  int notes_details;     ///< Boolean.  Whether to include details of above.
  int overrides;         ///< Boolean.  Whether to include associated overrides.
  int overrides_details; ///< Boolean.  Whether to include details of above.
} get_results_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
get_results_data_reset (get_results_data_t *data)
{
  free (data->result_id);
  free (data->task_id);

  memset (data, 0, sizeof (get_results_data_t));
}

/**
 * @brief Command data for the get_schedules command.
 */
typedef struct
{
  get_data_t get;      ///< Get args.
  int tasks;           ///< Boolean.  Whether to include tasks that use this schedule.
} get_schedules_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
get_schedules_data_reset (get_schedules_data_t *data)
{
  get_data_reset (&data->get);
  memset (data, 0, sizeof (get_schedules_data_t));
}

/**
 * @brief Command data.
 */
typedef struct
{
  char *filter;        ///< Filter term.
  int first;           ///< Skip over rows before this number.
  int max;             ///< Maximum number of rows returned.
  char *sort_field;    ///< Field to sort results on.
  int sort_order;      ///< Result sort order: 0 descending, else ascending.
  char *setting_id;    ///< UUID of single setting to get.
} get_settings_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
get_settings_data_reset (get_settings_data_t *data)
{
  free (data->filter);
  free (data->setting_id);
  free (data->sort_field);

  memset (data, 0, sizeof (get_settings_data_t));
}

/**
 * @brief Command data for the get_slaves command.
 */
typedef struct
{
  get_data_t get;      ///< Get args.
  int tasks;           ///< Boolean.  Whether to include tasks that use this slave.
} get_slaves_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
get_slaves_data_reset (get_slaves_data_t *data)
{
  get_data_reset (&data->get);
  memset (data, 0, sizeof (get_slaves_data_t));
}

/**
 * @brief Command data for the get_system_reports command.
 */
typedef struct
{
  int brief;        ///< Boolean.  Whether respond in brief.
  char *name;       ///< Name of single report to get.
  char *duration;   ///< Duration into the past to report on.
  char *slave_id;   ///< Slave that reports apply to, 0 for local Manager.
} get_system_reports_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
get_system_reports_data_reset (get_system_reports_data_t *data)
{
  free (data->name);
  free (data->duration);
  free (data->slave_id);

  memset (data, 0, sizeof (get_system_reports_data_t));
}

/**
 * @brief Command data for the get_targets command.
 */
typedef struct
{
  get_data_t get;    ///< Get args.
  int tasks;         ///< Boolean.  Whether to include tasks that use target.
} get_targets_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
get_targets_data_reset (get_targets_data_t *data)
{
  get_data_reset (&data->get);
  memset (data, 0, sizeof (get_targets_data_t));
}

/**
 * @brief Command data for the modify_config command.
 */
typedef struct
{
  char *comment;                       ///< New comment for config.
  char *config_id;                     ///< ID of config to modify.
  array_t *families_growing_empty; ///< New family selection: growing, empty.
  array_t *families_growing_all;   ///< New family selection: growing, all NVTs.
  array_t *families_static_all;    ///< New family selection: static, all NVTs.
  int family_selection_family_all;     ///< All flag in FAMILY_SELECTION/FAMILY.
  char *family_selection_family_all_text; ///< Text version of above.
  int family_selection_family_growing; ///< FAMILY_SELECTION/FAMILY growing flag.
  char *family_selection_family_growing_text; ///< Text version of above.
  char *family_selection_family_name;  ///< FAMILY_SELECTION/FAMILY family name.
  int family_selection_growing;        ///< Whether families in selection grow.
  char *family_selection_growing_text; ///< Text version of above.
  char *name;                          ///< New name for config.
  array_t *nvt_selection;              ///< OID array. New NVT set for config.
  char *nvt_selection_family;          ///< Family of NVT selection.
  char *nvt_selection_nvt_oid;         ///< OID during NVT_selection/NVT.
  char *preference_name;               ///< Config preference to modify.
  char *preference_nvt_oid;            ///< OID of NVT of preference.
  char *preference_value;              ///< New value for preference.
} modify_config_data_t;

/**
 * @brief Command data for the get_tasks command.
 */
typedef struct
{
  get_data_t get;        ///< Get args.
  int rcfile;            ///< Boolean.  Whether to include RC defining task.
} get_tasks_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
get_tasks_data_reset (get_tasks_data_t *data)
{
  get_data_reset (&data->get);

  memset (data, 0, sizeof (get_tasks_data_t));
}

/**
 * @brief Command data for the help command.
 */
typedef struct
{
  char *format;       ///< Format.
  char *type;         ///< Type of help.
} help_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
help_data_reset (help_data_t *data)
{
  free (data->format);
  free (data->type);

  memset (data, 0, sizeof (help_data_t));
}

/**
 * @brief Command data for the modify_agent command.
 */
typedef struct
{
  char *comment;                 ///< Comment.
  char *name;                    ///< Name of agent.
  char *agent_id;                ///< agent UUID.
} modify_agent_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
modify_agent_data_reset (modify_agent_data_t *data)
{
  free (data->comment);
  free (data->name);
  free (data->agent_id);

  memset (data, 0, sizeof (modify_agent_data_t));
}

/**
 * @brief Command data for the modify_alert command.
 */
typedef struct
{
  char *alert_id;                ///< alert UUID.
  char *name;                    ///< Name of alert.
  char *comment;                 ///< Comment.
  char *event;                   ///< Event that will cause alert.
  array_t *event_data;           ///< Array of pointers. Extra data for event.
  char *filter_id;               ///< UUID of filter.
  char *condition;               ///< Condition for alert, e.g. "Always".
  array_t *condition_data;       ///< Array of pointers.  Extra data for condition.
  char *method;                  ///< Method of alert, e.g. "Email".
  array_t *method_data;          ///< Array of pointer.  Extra data for method.
  char *part_data;               ///< Second part of data during *_data: value.
  char *part_name;               ///< First part of data during *_data: name.
} modify_alert_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
modify_alert_data_reset (modify_alert_data_t *data)
{
  free (data->alert_id);
  free (data->name);
  free (data->comment);
  free (data->filter_id);
  free (data->event);
  array_free (data->event_data);
  free (data->condition);
  array_free (data->condition_data);
  free (data->method);
  array_free (data->method_data);

  memset (data, 0, sizeof (modify_alert_data_t));
}

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
modify_config_data_reset (modify_config_data_t *data)
{
  free (data->comment);
  free (data->config_id);
  array_free (data->families_growing_empty);
  array_free (data->families_growing_all);
  array_free (data->families_static_all);
  free (data->family_selection_family_all_text);
  free (data->family_selection_family_growing_text);
  free (data->family_selection_family_name);
  free (data->family_selection_growing_text);
  free (data->name);
  array_free (data->nvt_selection);
  free (data->nvt_selection_family);
  free (data->nvt_selection_nvt_oid);
  free (data->preference_name);
  free (data->preference_nvt_oid);
  free (data->preference_value);

  memset (data, 0, sizeof (modify_config_data_t));
}

/**
 * @brief Command data for the modify_filter command.
 */
typedef struct
{
  char *comment;                 ///< Comment.
  char *name;                    ///< Name of filter.
  char *filter_id;               ///< Filter UUID.
  char *term;                    ///< Term for filter.
  char *type;                    ///< Type of filter.
} modify_filter_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
modify_filter_data_reset (modify_filter_data_t *data)
{
  free (data->comment);
  free (data->name);
  free (data->filter_id);
  free (data->term);
  free (data->type);

  memset (data, 0, sizeof (modify_filter_data_t));
}

/**
 * @brief Command data for the modify_lsc_credential command.
 */
typedef struct
{
  char *lsc_credential_id;    ///< ID of credential to modify.
  char *name;                 ///< Name.
  char *comment;              ///< Comment.
  char *login;                ///< Login.
  char *password;             ///< Password.
} modify_lsc_credential_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
modify_lsc_credential_data_reset (modify_lsc_credential_data_t *data)
{
  free (data->lsc_credential_id);
  free (data->name);
  free (data->comment);
  free (data->login);
  free (data->password);

  memset (data, 0, sizeof (modify_lsc_credential_data_t));
}

/**
 * @brief Command data for the modify_port_list command.
 */
typedef struct
{
  char *comment;                 ///< Comment.
  char *name;                    ///< Name of Port List.
  char *port_list_id;            ///< UUID of Port List.
} modify_port_list_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
modify_port_list_data_reset (modify_port_list_data_t *data)
{
  free (data->comment);
  free (data->name);
  free (data->port_list_id);

  memset (data, 0, sizeof (modify_port_list_data_t));
}

/**
 * @brief Command data for the modify_report command.
 */
typedef struct
{
  char *comment;       ///< Comment.
  char *report_id;     ///< ID of report to modify.
} modify_report_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
modify_report_data_reset (modify_report_data_t *data)
{
  free (data->comment);
  free (data->report_id);

  memset (data, 0, sizeof (modify_report_data_t));
}

/**
 * @brief Command data for the modify_report_format command.
 */
typedef struct
{
  char *active;               ///< Boolean.  Whether report format is active.
  char *name;                 ///< Name.
  char *param_name;           ///< Param name.
  char *param_value;          ///< Param value.
  char *report_format_id;     ///< ID of report format to modify.
  char *summary;              ///< Summary.
} modify_report_format_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
modify_report_format_data_reset (modify_report_format_data_t *data)
{
  free (data->active);
  free (data->name);
  free (data->param_name);
  free (data->param_value);
  free (data->report_format_id);
  free (data->summary);

  memset (data, 0, sizeof (modify_report_format_data_t));
}

/**
 * @brief Command data for the modify_schedule command.
 */
typedef struct
{
  char *comment;                 ///< Comment.
  char *name;                    ///< Name of schedule.
  char *schedule_id;             ///< Schedule UUID.
  char *first_time_day_of_month; ///< Day of month schedule must first run.
  char *first_time_hour;         ///< Hour schedule must first run.
  char *first_time_minute;       ///< Minute schedule must first run.
  char *first_time_month;        ///< Month schedule must first run.
  char *first_time_year;         ///< Year schedule must first run.
  char *period;                  ///< Period of schedule (how often it runs).
  char *period_unit;             ///< Unit of period: "hour", "day", "week", ....
  char *duration;                ///< Duration of schedule (how long it runs for).
  char *duration_unit;           ///< Unit of duration: "hour", "day", "week", ....
  char *timezone;                ///< Timezone.
} modify_schedule_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
modify_schedule_data_reset (modify_schedule_data_t *data)
{
  free (data->comment);
  free (data->name);
  free (data->schedule_id);
  free (data->first_time_day_of_month);
  free (data->first_time_hour);
  free (data->first_time_minute);
  free (data->first_time_month);
  free (data->first_time_year);
  free (data->period);
  free (data->period_unit);
  free (data->duration);
  free (data->duration_unit);
  free (data->timezone);

  memset (data, 0, sizeof (modify_schedule_data_t));
}

/**
 * @brief Command data for the modify_slave command.
 */
typedef struct
{
  char *comment;                 ///< Comment.
  char *name;                    ///< Name of slave.
  char *slave_id;                ///< Slave UUID.
  char *host;                    ///< Slave hostname.
  char *port;                    ///< Slave port.
  char *login;                   ///< Slave login.
  char *password;                ///< Slave password.
} modify_slave_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
modify_slave_data_reset (modify_slave_data_t *data)
{
  free (data->comment);
  free (data->name);
  free (data->slave_id);
  free (data->host);
  free (data->port);
  free (data->login);
  free (data->password);

  memset (data, 0, sizeof (modify_slave_data_t));
}

/**
 * @brief Command data for the modify_setting command.
 */
typedef struct
{
  char *name;           ///< Name.
  char *setting_id;     ///< Setting.
  char *value;          ///< Value.
} modify_setting_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
modify_setting_data_reset (modify_setting_data_t *data)
{
  free (data->name);
  free (data->setting_id);
  free (data->value);

  memset (data, 0, sizeof (modify_setting_data_t));
}

/**
 * @brief Command data for the modify_target command.
 */
typedef struct
{
  char *comment;                 ///< Comment.
  char *hosts;                   ///< Hosts for target.
  char *name;                    ///< Name of target.
  char *port_list_id;            ///< Port list for target.
  char *ssh_lsc_credential_id;   ///< SSH LSC credential for target.
  char *ssh_port;                ///< Port for SSH LSC.
  char *smb_lsc_credential_id;   ///< SMB LSC credential for target.
  char *target_id;               ///< Target UUID.
  char *target_locator;          ///< Target locator (source name).
  char *target_locator_password; ///< Target locator credentials: password.
  char *target_locator_username; ///< Target locator credentials: username.
} modify_target_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
modify_target_data_reset (modify_target_data_t *data)
{
  free (data->comment);
  free (data->hosts);
  free (data->name);
  free (data->port_list_id);
  free (data->ssh_lsc_credential_id);
  free (data->ssh_port);
  free (data->smb_lsc_credential_id);
  free (data->target_id);
  free (data->target_locator);
  free (data->target_locator_password);
  free (data->target_locator_username);

  memset (data, 0, sizeof (modify_target_data_t));
}

/**
 * @brief Command data for the modify_task command.
 */
typedef struct
{
  char *action;        ///< What to do to file: "update" or "remove".
  char *comment;       ///< Comment.
  char *config_id;     ///< ID of new config for task.
  array_t *alerts;     ///< IDs of new alerts for task.
  char *file;          ///< File to attach to task.
  char *file_name;     ///< Name of file to attach to task.
  char *name;          ///< New name for task.
  char *observers;     ///< Space separated list of observer user names.
  name_value_t *preference;  ///< Current preference.
  array_t *preferences;   ///< Preferences.
  char *rcfile;        ///< New definition for task, as an RC file.
  char *schedule_id;   ///< ID of new schedule for task.
  char *slave_id;      ///< ID of new slave for task.
  char *target_id;     ///< ID of new target for task.
  char *task_id;       ///< ID of task to modify.
} modify_task_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
modify_task_data_reset (modify_task_data_t *data)
{
  free (data->action);
  array_free (data->alerts);
  free (data->comment);
  free (data->config_id);
  free (data->file);
  free (data->file_name);
  free (data->name);
  free (data->observers);
  if (data->preferences)
    {
      guint index = data->preferences->len;
      while (index--)
        {
          name_value_t *pair;
          pair = (name_value_t*) g_ptr_array_index (data->preferences, index);
          if (pair)
            {
              g_free (pair->name);
              g_free (pair->value);
            }
        }
    }
  array_free (data->preferences);
  free (data->rcfile);
  free (data->schedule_id);
  free (data->slave_id);
  free (data->target_id);
  free (data->task_id);

  memset (data, 0, sizeof (modify_task_data_t));
}

/**
 * @brief Command data for the modify_note command.
 */
typedef struct
{
  char *active;       ///< Whether the note is active.
  char *hosts;        ///< Hosts to which to limit override.
  char *note_id;      ///< ID of note to modify.
  char *nvt_oid;      ///< NVT to which to limit override.
  char *port;         ///< Port to which to limit override.
  char *result_id;    ///< ID of result to which to limit override.
  char *task_id;      ///< ID of task to which to limit override.
  char *text;         ///< Text of override.
  char *threat;       ///< Threat to which to limit override.
} modify_note_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
modify_note_data_reset (modify_note_data_t *data)
{
  free (data->active);
  free (data->hosts);
  free (data->note_id);
  free (data->nvt_oid);
  free (data->port);
  free (data->result_id);
  free (data->task_id);
  free (data->text);
  free (data->threat);

  memset (data, 0, sizeof (modify_note_data_t));
}

/**
 * @brief Command data for the modify_override command.
 */
typedef struct
{
  char *active;       ///< Whether the override is active.
  char *hosts;        ///< Hosts to which to limit override.
  char *new_threat;   ///< New threat value of overridden results.
  char *nvt_oid;      ///< NVT to which to limit override.
  char *override_id;  ///< ID of override to modify.
  char *port;         ///< Port to which to limit override.
  char *result_id;    ///< ID of result to which to limit override.
  char *task_id;      ///< ID of task to which to limit override.
  char *text;         ///< Text of override.
  char *threat;       ///< Threat to which to limit override.
} modify_override_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
modify_override_data_reset (modify_override_data_t *data)
{
  free (data->active);
  free (data->hosts);
  free (data->new_threat);
  free (data->nvt_oid);
  free (data->override_id);
  free (data->port);
  free (data->result_id);
  free (data->task_id);
  free (data->text);
  free (data->threat);

  memset (data, 0, sizeof (modify_override_data_t));
}

/**
 * @brief Command data for the pause_task command.
 */
typedef struct
{
  char *task_id;   ///< ID of task to pause.
} pause_task_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
pause_task_data_reset (pause_task_data_t *data)
{
  free (data->task_id);

  memset (data, 0, sizeof (pause_task_data_t));
}

/**
 * @brief Command data for the restore command.
 */
typedef struct
{
  char *id;   ///< ID of resource to pause.
} restore_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
restore_data_reset (restore_data_t *data)
{
  free (data->id);

  memset (data, 0, sizeof (restore_data_t));
}

/**
 * @brief Command data for the resume_or_start_task command.
 */
typedef struct
{
  char *task_id;   ///< ID of task to resume or start.
} resume_or_start_task_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
resume_or_start_task_data_reset (resume_or_start_task_data_t *data)
{
  free (data->task_id);

  memset (data, 0, sizeof (resume_or_start_task_data_t));
}

/**
 * @brief Command data for the resume_paused_task command.
 */
typedef struct
{
  char *task_id;   ///< ID of paused task to resume.
} resume_paused_task_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
resume_paused_task_data_reset (resume_paused_task_data_t *data)
{
  free (data->task_id);

  memset (data, 0, sizeof (resume_paused_task_data_t));
}

/**
 * @brief Command data for the resume_stopped_task command.
 */
typedef struct
{
  char *task_id;   ///< ID of stopped task to resume.
} resume_stopped_task_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
resume_stopped_task_data_reset (resume_stopped_task_data_t *data)
{
  free (data->task_id);

  memset (data, 0, sizeof (resume_stopped_task_data_t));
}

/**
 * @brief Command data for the start_task command.
 */
typedef struct
{
  char *task_id;   ///< ID of task to start.
} start_task_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
start_task_data_reset (start_task_data_t *data)
{
  free (data->task_id);

  memset (data, 0, sizeof (start_task_data_t));
}

/**
 * @brief Command data for the stop_task command.
 */
typedef struct
{
  char *task_id;   ///< ID of task to stop.
} stop_task_data_t;

/**
 * @brief Free members of a stop_task_data_t and set them to NULL.
 */
/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
stop_task_data_reset (stop_task_data_t *data)
{
  free (data->task_id);

  memset (data, 0, sizeof (stop_task_data_t));
}

/**
 * @brief Command data for the test_alert command.
 */
typedef struct
{
  char *alert_id;   ///< ID of alert to test.
} test_alert_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
test_alert_data_reset (test_alert_data_t *data)
{
  free (data->alert_id);

  memset (data, 0, sizeof (test_alert_data_t));
}

/**
 * @brief Command data for the verify_agent command.
 */
typedef struct
{
  char *agent_id;   ///< ID of agent to verify.
} verify_agent_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
verify_agent_data_reset (verify_agent_data_t *data)
{
  free (data->agent_id);

  memset (data, 0, sizeof (verify_agent_data_t));
}

/**
 * @brief Command data for the verify_report_format command.
 */
typedef struct
{
  char *report_format_id;   ///< ID of report format to verify.
} verify_report_format_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
verify_report_format_data_reset (verify_report_format_data_t *data)
{
  free (data->report_format_id);

  memset (data, 0, sizeof (verify_report_format_data_t));
}

/**
 * @brief Command data for the wizard command.
 */
typedef struct
{
  char *name;          ///< Name of the wizard.
  name_value_t *param; ///< Current param.
  array_t *params;     ///< Parameters.
} run_wizard_data_t;

/**
 * @brief Reset command data.
 *
 * @param[in]  data  Command data.
 */
static void
run_wizard_data_reset (run_wizard_data_t *data)
{
  free (data->name);
  if (data->params)
    {
      guint index = data->params->len;
      while (index--)
        {
          name_value_t *pair;
          pair = (name_value_t*) g_ptr_array_index (data->params, index);
          if (pair)
            {
              g_free (pair->name);
              g_free (pair->value);
            }
        }
    }
  array_free (data->params);

  memset (data, 0, sizeof (run_wizard_data_t));
}

/**
 * @brief Command data, as passed between OMP parser callbacks.
 */
typedef union
{
  create_agent_data_t create_agent;                   ///< create_agent
  create_config_data_t create_config;                 ///< create_config
  create_alert_data_t create_alert;                   ///< create_alert
  create_filter_data_t create_filter;                 ///< create_filter
  create_lsc_credential_data_t create_lsc_credential; ///< create_lsc_credential
  create_note_data_t create_note;                     ///< create_note
  create_override_data_t create_override;             ///< create_override
  create_port_list_data_t create_port_list;           ///< create_port_list
  create_port_range_data_t create_port_range;         ///< create_port_range
  create_report_data_t create_report;                 ///< create_report
  create_report_format_data_t create_report_format;   ///< create_report_format
  create_schedule_data_t create_schedule;             ///< create_schedule
  create_slave_data_t create_slave;                   ///< create_slave
  create_target_data_t create_target;                 ///< create_target
  create_task_data_t create_task;                     ///< create_task
  delete_agent_data_t delete_agent;                   ///< delete_agent
  delete_config_data_t delete_config;                 ///< delete_config
  delete_alert_data_t delete_alert;                   ///< delete_alert
  delete_filter_data_t delete_filter;                 ///< delete_filter
  delete_lsc_credential_data_t delete_lsc_credential; ///< delete_lsc_credential
  delete_note_data_t delete_note;                     ///< delete_note
  delete_override_data_t delete_override;             ///< delete_override
  delete_port_list_data_t delete_port_list;           ///< delete_port_list
  delete_port_range_data_t delete_port_range;         ///< delete_port_range
  delete_report_data_t delete_report;                 ///< delete_report
  delete_report_format_data_t delete_report_format;   ///< delete_report_format
  delete_schedule_data_t delete_schedule;             ///< delete_schedule
  delete_slave_data_t delete_slave;                   ///< delete_slave
  delete_target_data_t delete_target;                 ///< delete_target
  delete_task_data_t delete_task;                     ///< delete_task
  get_agents_data_t get_agents;                       ///< get_agents
  get_configs_data_t get_configs;                     ///< get_configs
  get_dependencies_data_t get_dependencies;           ///< get_dependencies
  get_alerts_data_t get_alerts;                       ///< get_alerts
  get_filters_data_t get_filters;                     ///< get_filters
  get_info_data_t get_info;                           ///< get_info
  get_lsc_credentials_data_t get_lsc_credentials;     ///< get_lsc_credentials
  get_notes_data_t get_notes;                         ///< get_notes
  get_nvts_data_t get_nvts;                           ///< get_nvts
  get_nvt_families_data_t get_nvt_families;           ///< get_nvt_families
  get_nvt_feed_checksum_data_t get_nvt_feed_checksum; ///< get_nvt_feed_checksum
  get_overrides_data_t get_overrides;                 ///< get_overrides
  get_port_lists_data_t get_port_lists;               ///< get_port_lists
  get_preferences_data_t get_preferences;             ///< get_preferences
  get_reports_data_t get_reports;                     ///< get_reports
  get_report_formats_data_t get_report_formats;       ///< get_report_formats
  get_results_data_t get_results;                     ///< get_results
  get_schedules_data_t get_schedules;                 ///< get_schedules
  get_settings_data_t get_settings;                   ///< get_settings
  get_slaves_data_t get_slaves;                       ///< get_slaves
  get_system_reports_data_t get_system_reports;       ///< get_system_reports
  get_targets_data_t get_targets;                     ///< get_targets
  get_tasks_data_t get_tasks;                         ///< get_tasks
  help_data_t help;                                   ///< help
  modify_agent_data_t modify_agent;                   ///< modify_agent
  modify_alert_data_t modify_alert;                   ///< modify_alert
  modify_config_data_t modify_config;                 ///< modify_config
  modify_filter_data_t modify_filter;                 ///< modify_filter
  modify_lsc_credential_data_t modify_lsc_credential; ///< modify_lsc_credential
  modify_port_list_data_t modify_port_list;           ///< modify_port_list
  modify_report_data_t modify_report;                 ///< modify_report
  modify_report_format_data_t modify_report_format;   ///< modify_report_format
  modify_schedule_data_t modify_schedule;             ///< modify_schedule
  modify_setting_data_t modify_setting;               ///< modify_setting
  modify_slave_data_t modify_slave;                   ///< modify_slave
  modify_target_data_t modify_target;                 ///< modify_target
  modify_task_data_t modify_task;                     ///< modify_task
  pause_task_data_t pause_task;                       ///< pause_task
  restore_data_t restore;                             ///< restore
  resume_or_start_task_data_t resume_or_start_task;   ///< resume_or_start_task
  resume_paused_task_data_t resume_paused_task;       ///< resume_paused_task
  resume_stopped_task_data_t resume_stopped_task;     ///< resume_stopped_task
  start_task_data_t start_task;                       ///< start_task
  stop_task_data_t stop_task;                         ///< stop_task
  test_alert_data_t test_alert;                       ///< test_alert
  verify_agent_data_t verify_agent;                   ///< verify_agent
  verify_report_format_data_t verify_report_format;   ///< verify_report_format
  run_wizard_data_t wizard;                           ///< run_wizard
} command_data_t;

/**
 * @brief Initialise command data.
 */
static void
command_data_init (command_data_t *data)
{
  memset (data, 0, sizeof (command_data_t));
}


/* Global variables. */

/**
 * @brief Parser callback data.
 */
command_data_t command_data;

/**
 * @brief Parser callback data for CREATE_AGENT.
 */
create_agent_data_t *create_agent_data
 = (create_agent_data_t*) &(command_data.create_agent);

/**
 * @brief Parser callback data for CREATE_CONFIG.
 */
create_config_data_t *create_config_data
 = (create_config_data_t*) &(command_data.create_config);

/**
 * @brief Parser callback data for CREATE_ALERT.
 */
create_alert_data_t *create_alert_data
 = (create_alert_data_t*) &(command_data.create_alert);

/**
 * @brief Parser callback data for CREATE_FILTER.
 */
create_filter_data_t *create_filter_data
 = (create_filter_data_t*) &(command_data.create_filter);

/**
 * @brief Parser callback data for CREATE_LSC_CREDENTIAL.
 */
create_lsc_credential_data_t *create_lsc_credential_data
 = (create_lsc_credential_data_t*) &(command_data.create_lsc_credential);

/**
 * @brief Parser callback data for CREATE_NOTE.
 */
create_note_data_t *create_note_data
 = (create_note_data_t*) &(command_data.create_note);

/**
 * @brief Parser callback data for CREATE_OVERRIDE.
 */
create_override_data_t *create_override_data
 = (create_override_data_t*) &(command_data.create_override);

/**
 * @brief Parser callback data for CREATE_PORT_LIST.
 */
create_port_list_data_t *create_port_list_data
 = (create_port_list_data_t*) &(command_data.create_port_list);

/**
 * @brief Parser callback data for CREATE_PORT_RANGE.
 */
create_port_range_data_t *create_port_range_data
 = (create_port_range_data_t*) &(command_data.create_port_range);

/**
 * @brief Parser callback data for CREATE_REPORT.
 */
create_report_data_t *create_report_data
 = (create_report_data_t*) &(command_data.create_report);

/**
 * @brief Parser callback data for CREATE_REPORT_FORMAT.
 */
create_report_format_data_t *create_report_format_data
 = (create_report_format_data_t*) &(command_data.create_report_format);

/**
 * @brief Parser callback data for CREATE_SCHEDULE.
 */
create_schedule_data_t *create_schedule_data
 = (create_schedule_data_t*) &(command_data.create_schedule);

/**
 * @brief Parser callback data for CREATE_SLAVE.
 */
create_slave_data_t *create_slave_data
 = (create_slave_data_t*) &(command_data.create_slave);

/**
 * @brief Parser callback data for CREATE_TARGET.
 */
create_target_data_t *create_target_data
 = (create_target_data_t*) &(command_data.create_target);

/**
 * @brief Parser callback data for CREATE_TASK.
 */
create_task_data_t *create_task_data
 = (create_task_data_t*) &(command_data.create_task);

/**
 * @brief Parser callback data for DELETE_AGENT.
 */
delete_agent_data_t *delete_agent_data
 = (delete_agent_data_t*) &(command_data.delete_agent);

/**
 * @brief Parser callback data for DELETE_CONFIG.
 */
delete_config_data_t *delete_config_data
 = (delete_config_data_t*) &(command_data.delete_config);

/**
 * @brief Parser callback data for DELETE_ALERT.
 */
delete_alert_data_t *delete_alert_data
 = (delete_alert_data_t*) &(command_data.delete_alert);

/**
 * @brief Parser callback data for DELETE_FILTER.
 */
delete_filter_data_t *delete_filter_data
 = (delete_filter_data_t*) &(command_data.delete_filter);

/**
 * @brief Parser callback data for DELETE_LSC_CREDENTIAL.
 */
delete_lsc_credential_data_t *delete_lsc_credential_data
 = (delete_lsc_credential_data_t*) &(command_data.delete_lsc_credential);

/**
 * @brief Parser callback data for DELETE_NOTE.
 */
delete_note_data_t *delete_note_data
 = (delete_note_data_t*) &(command_data.delete_note);

/**
 * @brief Parser callback data for DELETE_OVERRIDE.
 */
delete_override_data_t *delete_override_data
 = (delete_override_data_t*) &(command_data.delete_override);

/**
 * @brief Parser callback data for DELETE_PORT_LIST.
 */
delete_port_list_data_t *delete_port_list_data
 = (delete_port_list_data_t*) &(command_data.delete_port_list);

/**
 * @brief Parser callback data for DELETE_PORT_RANGE.
 */
delete_port_range_data_t *delete_port_range_data
 = (delete_port_range_data_t*) &(command_data.delete_port_range);

/**
 * @brief Parser callback data for DELETE_REPORT.
 */
delete_report_data_t *delete_report_data
 = (delete_report_data_t*) &(command_data.delete_report);

/**
 * @brief Parser callback data for DELETE_REPORT_FORMAT.
 */
delete_report_format_data_t *delete_report_format_data
 = (delete_report_format_data_t*) &(command_data.delete_report_format);

/**
 * @brief Parser callback data for DELETE_SCHEDULE.
 */
delete_schedule_data_t *delete_schedule_data
 = (delete_schedule_data_t*) &(command_data.delete_schedule);

/**
 * @brief Parser callback data for DELETE_SLAVE.
 */
delete_slave_data_t *delete_slave_data
 = (delete_slave_data_t*) &(command_data.delete_slave);

/**
 * @brief Parser callback data for DELETE_TARGET.
 */
delete_target_data_t *delete_target_data
 = (delete_target_data_t*) &(command_data.delete_target);

/**
 * @brief Parser callback data for DELETE_TASK.
 */
delete_task_data_t *delete_task_data
 = (delete_task_data_t*) &(command_data.delete_task);

/**
 * @brief Parser callback data for GET_AGENTS.
 */
get_agents_data_t *get_agents_data
 = &(command_data.get_agents);

/**
 * @brief Parser callback data for GET_CONFIGS.
 */
get_configs_data_t *get_configs_data
 = &(command_data.get_configs);

/**
 * @brief Parser callback data for GET_DEPENDENCIES.
 */
get_dependencies_data_t *get_dependencies_data
 = &(command_data.get_dependencies);

/**
 * @brief Parser callback data for GET_ALERTS.
 */
get_alerts_data_t *get_alerts_data
 = &(command_data.get_alerts);

/**
 * @brief Parser callback data for GET_FILTERS.
 */
get_filters_data_t *get_filters_data
 = &(command_data.get_filters);

/**
 * @brief Parser callback data for GET_INFO.
 */
get_info_data_t *get_info_data
 = &(command_data.get_info);

/**
 * @brief Parser callback data for GET_LSC_CREDENTIALS.
 */
get_lsc_credentials_data_t *get_lsc_credentials_data
 = &(command_data.get_lsc_credentials);

/**
 * @brief Parser callback data for GET_NOTES.
 */
get_notes_data_t *get_notes_data
 = &(command_data.get_notes);

/**
 * @brief Parser callback data for GET_NVTS.
 */
get_nvts_data_t *get_nvts_data
 = &(command_data.get_nvts);

/**
 * @brief Parser callback data for GET_NVT_FAMILIES.
 */
get_nvt_families_data_t *get_nvt_families_data
 = &(command_data.get_nvt_families);

/**
 * @brief Parser callback data for GET_NVT_FEED_CHECKSUM.
 */
get_nvt_feed_checksum_data_t *get_nvt_feed_checksum_data
 = &(command_data.get_nvt_feed_checksum);

/**
 * @brief Parser callback data for GET_OVERRIDES.
 */
get_overrides_data_t *get_overrides_data
 = &(command_data.get_overrides);

/**
 * @brief Parser callback data for GET_PORT_LISTS.
 */
get_port_lists_data_t *get_port_lists_data
 = &(command_data.get_port_lists);

/**
 * @brief Parser callback data for GET_PREFERENCES.
 */
get_preferences_data_t *get_preferences_data
 = &(command_data.get_preferences);

/**
 * @brief Parser callback data for GET_REPORTS.
 */
get_reports_data_t *get_reports_data
 = &(command_data.get_reports);

/**
 * @brief Parser callback data for GET_REPORT_FORMATS.
 */
get_report_formats_data_t *get_report_formats_data
 = &(command_data.get_report_formats);

/**
 * @brief Parser callback data for GET_RESULTS.
 */
get_results_data_t *get_results_data
 = &(command_data.get_results);

/**
 * @brief Parser callback data for GET_SCHEDULES.
 */
get_schedules_data_t *get_schedules_data
 = &(command_data.get_schedules);

/**
 * @brief Parser callback data for GET_SETTINGS.
 */
get_settings_data_t *get_settings_data
 = &(command_data.get_settings);

/**
 * @brief Parser callback data for GET_SLAVES.
 */
get_slaves_data_t *get_slaves_data
 = &(command_data.get_slaves);

/**
 * @brief Parser callback data for GET_SYSTEM_REPORTS.
 */
get_system_reports_data_t *get_system_reports_data
 = &(command_data.get_system_reports);

/**
 * @brief Parser callback data for GET_TARGETS.
 */
get_targets_data_t *get_targets_data
 = &(command_data.get_targets);

/**
 * @brief Parser callback data for GET_TASKS.
 */
get_tasks_data_t *get_tasks_data
 = &(command_data.get_tasks);

/**
 * @brief Parser callback data for HELP.
 */
help_data_t *help_data
 = &(command_data.help);

/**
 * @brief Parser callback data for CREATE_CONFIG (import).
 */
import_config_data_t *import_config_data
 = (import_config_data_t*) &(command_data.create_config.import);

/**
 * @brief Parser callback data for MODIFY_CONFIG.
 */
modify_config_data_t *modify_config_data
 = &(command_data.modify_config);

/**
 * @brief Parser callback data for MODIFY_AGENT.
 */
modify_agent_data_t *modify_agent_data
 = &(command_data.modify_agent);

/**
 * @brief Parser callback data for MODIFY_ALERT.
 */
modify_alert_data_t *modify_alert_data
 = &(command_data.modify_alert);

/**
 * @brief Parser callback data for MODIFY_FILTER.
 */
modify_filter_data_t *modify_filter_data
 = &(command_data.modify_filter);

/**
 * @brief Parser callback data for MODIFY_LSC_CREDENTIAL.
 */
modify_lsc_credential_data_t *modify_lsc_credential_data
 = &(command_data.modify_lsc_credential);

/**
 * @brief Parser callback data for MODIFY_NOTE.
 */
modify_note_data_t *modify_note_data
 = (modify_note_data_t*) &(command_data.create_note);

/**
 * @brief Parser callback data for MODIFY_OVERRIDE.
 */
modify_override_data_t *modify_override_data
 = (modify_override_data_t*) &(command_data.create_override);

/**
 * @brief Parser callback data for MODIFY_PORT_LIST.
 */
modify_port_list_data_t *modify_port_list_data
 = &(command_data.modify_port_list);

/**
 * @brief Parser callback data for MODIFY_REPORT.
 */
modify_report_data_t *modify_report_data
 = &(command_data.modify_report);

/**
 * @brief Parser callback data for MODIFY_REPORT_FORMAT.
 */
modify_report_format_data_t *modify_report_format_data
 = &(command_data.modify_report_format);

/**
 * @brief Parser callback data for MODIFY_SCHEDULE.
 */
modify_schedule_data_t *modify_schedule_data
 = &(command_data.modify_schedule);

/**
 * @brief Parser callback data for MODIFY_SETTING.
 */
modify_setting_data_t *modify_setting_data
 = &(command_data.modify_setting);

/**
 * @brief Parser callback data for MODIFY_SLAVE.
 */
modify_slave_data_t *modify_slave_data
 = &(command_data.modify_slave);

/**
 * @brief Parser callback data for MODIFY_TARGET.
 */
modify_target_data_t *modify_target_data
 = &(command_data.modify_target);

/**
 * @brief Parser callback data for MODIFY_TASK.
 */
modify_task_data_t *modify_task_data
 = &(command_data.modify_task);

/**
 * @brief Parser callback data for PAUSE_TASK.
 */
pause_task_data_t *pause_task_data
 = (pause_task_data_t*) &(command_data.pause_task);

/**
 * @brief Parser callback data for RESTORE.
 */
restore_data_t *restore_data
 = (restore_data_t*) &(command_data.restore);

/**
 * @brief Parser callback data for RESUME_OR_START_TASK.
 */
resume_or_start_task_data_t *resume_or_start_task_data
 = (resume_or_start_task_data_t*) &(command_data.resume_or_start_task);

/**
 * @brief Parser callback data for RESUME_PAUSED_TASK.
 */
resume_paused_task_data_t *resume_paused_task_data
 = (resume_paused_task_data_t*) &(command_data.resume_paused_task);

/**
 * @brief Parser callback data for RESUME_STOPPED_TASK.
 */
resume_stopped_task_data_t *resume_stopped_task_data
 = (resume_stopped_task_data_t*) &(command_data.resume_stopped_task);

/**
 * @brief Parser callback data for START_TASK.
 */
start_task_data_t *start_task_data
 = (start_task_data_t*) &(command_data.start_task);

/**
 * @brief Parser callback data for STOP_TASK.
 */
stop_task_data_t *stop_task_data
 = (stop_task_data_t*) &(command_data.stop_task);

/**
 * @brief Parser callback data for TEST_ALERT.
 */
test_alert_data_t *test_alert_data
 = (test_alert_data_t*) &(command_data.test_alert);

/**
 * @brief Parser callback data for VERIFY_AGENT.
 */
verify_agent_data_t *verify_agent_data
 = (verify_agent_data_t*) &(command_data.verify_agent);

/**
 * @brief Parser callback data for VERIFY_REPORT_FORMAT.
 */
verify_report_format_data_t *verify_report_format_data
 = (verify_report_format_data_t*) &(command_data.verify_report_format);

/**
 * @brief Parser callback data for WIZARD.
 */
run_wizard_data_t *run_wizard_data
 = (run_wizard_data_t*) &(command_data.wizard);

/**
 * @brief Hack for returning forked process status from the callbacks.
 */
int current_error;

/**
 * @brief Hack for returning fork status to caller.
 */
int forked;

/**
 * @brief Buffer of output to the client.
 */
char to_client[TO_CLIENT_BUFFER_SIZE];

/**
 * @brief The start of the data in the \ref to_client buffer.
 */
buffer_size_t to_client_start = 0;
/**
 * @brief The end of the data in the \ref to_client buffer.
 */
buffer_size_t to_client_end = 0;

/**
 * @brief Client input parsing context.
 */
static /*@null@*/ /*@only@*/ GMarkupParseContext*
xml_context = NULL;

/**
 * @brief Client input parser.
 */
static GMarkupParser xml_parser;


/* Client state. */

/**
 * @brief Possible states of the client.
 */
typedef enum
{
  CLIENT_TOP,
  CLIENT_AUTHENTIC,

  CLIENT_AUTHENTICATE,
  CLIENT_AUTHENTICATE_CREDENTIALS,
  CLIENT_AUTHENTICATE_CREDENTIALS_PASSWORD,
  CLIENT_AUTHENTICATE_CREDENTIALS_USERNAME,
  CLIENT_AUTHENTIC_COMMANDS,
  CLIENT_COMMANDS,
  CLIENT_CREATE_AGENT,
  CLIENT_CREATE_AGENT_NAME,
  CLIENT_CREATE_AGENT_COMMENT,
  CLIENT_CREATE_AGENT_COPY,
  CLIENT_CREATE_AGENT_INSTALLER,
  CLIENT_CREATE_AGENT_INSTALLER_FILENAME,
  CLIENT_CREATE_AGENT_INSTALLER_SIGNATURE,
  CLIENT_CREATE_AGENT_HOWTO_INSTALL,
  CLIENT_CREATE_AGENT_HOWTO_USE,
  CLIENT_CREATE_CONFIG,
  CLIENT_CREATE_CONFIG_COMMENT,
  CLIENT_CREATE_CONFIG_COPY,
  CLIENT_CREATE_CONFIG_NAME,
  CLIENT_CREATE_CONFIG_RCFILE,
  /* get_configs_response (GCR) is used for config export.  CLIENT_C_C is
   * for CLIENT_CREATE_CONFIG. */
  CLIENT_C_C_GCR,
  CLIENT_C_C_GCR_CONFIG,
  CLIENT_C_C_GCR_CONFIG_COMMENT,
  CLIENT_C_C_GCR_CONFIG_NAME,
  CLIENT_C_C_GCR_CONFIG_NVT_SELECTORS,
  CLIENT_C_C_GCR_CONFIG_NVT_SELECTORS_NVT_SELECTOR,
  CLIENT_C_C_GCR_CONFIG_NVT_SELECTORS_NVT_SELECTOR_NAME,
  CLIENT_C_C_GCR_CONFIG_NVT_SELECTORS_NVT_SELECTOR_INCLUDE,
  CLIENT_C_C_GCR_CONFIG_NVT_SELECTORS_NVT_SELECTOR_TYPE,
  CLIENT_C_C_GCR_CONFIG_NVT_SELECTORS_NVT_SELECTOR_FAMILY_OR_NVT,
  CLIENT_C_C_GCR_CONFIG_PREFERENCES,
  CLIENT_C_C_GCR_CONFIG_PREFERENCES_PREFERENCE,
  CLIENT_C_C_GCR_CONFIG_PREFERENCES_PREFERENCE_ALT,
  CLIENT_C_C_GCR_CONFIG_PREFERENCES_PREFERENCE_NAME,
  CLIENT_C_C_GCR_CONFIG_PREFERENCES_PREFERENCE_NVT,
  CLIENT_C_C_GCR_CONFIG_PREFERENCES_PREFERENCE_NVT_NAME,
  CLIENT_C_C_GCR_CONFIG_PREFERENCES_PREFERENCE_TYPE,
  CLIENT_C_C_GCR_CONFIG_PREFERENCES_PREFERENCE_VALUE,
  CLIENT_CREATE_ALERT,
  CLIENT_CREATE_ALERT_COMMENT,
  CLIENT_CREATE_ALERT_COPY,
  CLIENT_CREATE_ALERT_CONDITION,
  CLIENT_CREATE_ALERT_CONDITION_DATA,
  CLIENT_CREATE_ALERT_CONDITION_DATA_NAME,
  CLIENT_CREATE_ALERT_EVENT,
  CLIENT_CREATE_ALERT_EVENT_DATA,
  CLIENT_CREATE_ALERT_EVENT_DATA_NAME,
  CLIENT_CREATE_ALERT_FILTER,
  CLIENT_CREATE_ALERT_METHOD,
  CLIENT_CREATE_ALERT_METHOD_DATA,
  CLIENT_CREATE_ALERT_METHOD_DATA_NAME,
  CLIENT_CREATE_ALERT_NAME,
  CLIENT_CREATE_FILTER,
  CLIENT_CREATE_FILTER_COMMENT,
  CLIENT_CREATE_FILTER_COPY,
  CLIENT_CREATE_FILTER_NAME,
  CLIENT_CREATE_FILTER_NAME_MAKE_UNIQUE,
  CLIENT_CREATE_FILTER_TERM,
  CLIENT_CREATE_FILTER_TYPE,
  CLIENT_CREATE_LSC_CREDENTIAL,
  CLIENT_CREATE_LSC_CREDENTIAL_COPY,
  CLIENT_CREATE_LSC_CREDENTIAL_COMMENT,
  CLIENT_CREATE_LSC_CREDENTIAL_NAME,
  CLIENT_CREATE_LSC_CREDENTIAL_PASSWORD,
  CLIENT_CREATE_LSC_CREDENTIAL_LOGIN,
  CLIENT_CREATE_LSC_CREDENTIAL_KEY,
  CLIENT_CREATE_LSC_CREDENTIAL_KEY_PHRASE,
  CLIENT_CREATE_LSC_CREDENTIAL_KEY_PRIVATE,
  CLIENT_CREATE_LSC_CREDENTIAL_KEY_PUBLIC,
  CLIENT_CREATE_NOTE,
  CLIENT_CREATE_NOTE_ACTIVE,
  CLIENT_CREATE_NOTE_COPY,
  CLIENT_CREATE_NOTE_HOSTS,
  CLIENT_CREATE_NOTE_NVT,
  CLIENT_CREATE_NOTE_PORT,
  CLIENT_CREATE_NOTE_RESULT,
  CLIENT_CREATE_NOTE_TASK,
  CLIENT_CREATE_NOTE_TEXT,
  CLIENT_CREATE_NOTE_THREAT,
  CLIENT_CREATE_OVERRIDE,
  CLIENT_CREATE_OVERRIDE_ACTIVE,
  CLIENT_CREATE_OVERRIDE_COPY,
  CLIENT_CREATE_OVERRIDE_HOSTS,
  CLIENT_CREATE_OVERRIDE_NEW_THREAT,
  CLIENT_CREATE_OVERRIDE_NVT,
  CLIENT_CREATE_OVERRIDE_PORT,
  CLIENT_CREATE_OVERRIDE_RESULT,
  CLIENT_CREATE_OVERRIDE_TASK,
  CLIENT_CREATE_OVERRIDE_TEXT,
  CLIENT_CREATE_OVERRIDE_THREAT,
  CLIENT_CREATE_PORT_LIST,
  CLIENT_CREATE_PORT_LIST_COMMENT,
  CLIENT_CREATE_PORT_LIST_COPY,
  CLIENT_CREATE_PORT_LIST_NAME,
  CLIENT_CREATE_PORT_LIST_PORT_RANGE,
  /* get_port_lists (GPL) is used for port lists export.  CLIENT_CPL is
   * for CLIENT_CREATE_PORT_LIST. */
  CLIENT_CPL_GPLR,
  CLIENT_CPL_GPLR_PORT_LIST,
  CLIENT_CPL_GPLR_PORT_LIST_COMMENT,
  CLIENT_CPL_GPLR_PORT_LIST_IN_USE,
  CLIENT_CPL_GPLR_PORT_LIST_NAME,
  CLIENT_CPL_GPLR_PORT_LIST_PORT_RANGE,
  CLIENT_CPL_GPLR_PORT_LIST_PORT_RANGES,
  CLIENT_CPL_GPLR_PORT_LIST_PORT_RANGES_PORT_RANGE,
  CLIENT_CPL_GPLR_PORT_LIST_PORT_RANGES_PORT_RANGE_COMMENT,
  CLIENT_CPL_GPLR_PORT_LIST_PORT_RANGES_PORT_RANGE_END,
  CLIENT_CPL_GPLR_PORT_LIST_PORT_RANGES_PORT_RANGE_START,
  CLIENT_CPL_GPLR_PORT_LIST_PORT_RANGES_PORT_RANGE_TYPE,
  CLIENT_CPL_GPLR_PORT_LIST_TARGETS,
  CLIENT_CREATE_PORT_RANGE,
  CLIENT_CREATE_PORT_RANGE_COMMENT,
  CLIENT_CREATE_PORT_RANGE_END,
  CLIENT_CREATE_PORT_RANGE_PORT_LIST,
  CLIENT_CREATE_PORT_RANGE_START,
  CLIENT_CREATE_PORT_RANGE_TYPE,
  /* CREATE_REPORT. */
  CLIENT_CREATE_REPORT,
  CLIENT_CREATE_REPORT_REPORT,
  CLIENT_CREATE_REPORT_RR,
  CLIENT_CREATE_REPORT_RR_FILTERS,
  /* RR_H is for RR_HOST because it clashes with entities like HOST_START. */
  CLIENT_CREATE_REPORT_RR_H,
  CLIENT_CREATE_REPORT_RR_H_DETAIL,
  CLIENT_CREATE_REPORT_RR_H_DETAIL_NAME,
  CLIENT_CREATE_REPORT_RR_H_DETAIL_SOURCE,
  CLIENT_CREATE_REPORT_RR_H_DETAIL_SOURCE_DESC,
  CLIENT_CREATE_REPORT_RR_H_DETAIL_SOURCE_NAME,
  CLIENT_CREATE_REPORT_RR_H_DETAIL_SOURCE_TYPE,
  CLIENT_CREATE_REPORT_RR_H_DETAIL_VALUE,
  CLIENT_CREATE_REPORT_RR_H_END,
  CLIENT_CREATE_REPORT_RR_H_IP,
  CLIENT_CREATE_REPORT_RR_H_START,
  CLIENT_CREATE_REPORT_RR_HOST_COUNT,
  CLIENT_CREATE_REPORT_RR_HOST_END,
  CLIENT_CREATE_REPORT_RR_HOST_END_HOST,
  CLIENT_CREATE_REPORT_RR_HOST_START,
  CLIENT_CREATE_REPORT_RR_HOST_START_HOST,
  CLIENT_CREATE_REPORT_RR_HOSTS,
  CLIENT_CREATE_REPORT_RR_PORTS,
  CLIENT_CREATE_REPORT_RR_REPORT_FORMAT,
  CLIENT_CREATE_REPORT_RR_RESULTS,
  CLIENT_CREATE_REPORT_RR_RESULTS_RESULT,
  CLIENT_CREATE_REPORT_RR_RESULTS_RESULT_DESCRIPTION,
  CLIENT_CREATE_REPORT_RR_RESULTS_RESULT_DETECTION,
  CLIENT_CREATE_REPORT_RR_RESULTS_RESULT_HOST,
  CLIENT_CREATE_REPORT_RR_RESULTS_RESULT_NOTES,
  CLIENT_CREATE_REPORT_RR_RESULTS_RESULT_NVT,
  CLIENT_CREATE_REPORT_RR_RESULTS_RESULT_NVT_BID,
  CLIENT_CREATE_REPORT_RR_RESULTS_RESULT_NVT_CVE,
  CLIENT_CREATE_REPORT_RR_RESULTS_RESULT_NVT_CVSS_BASE,
  CLIENT_CREATE_REPORT_RR_RESULTS_RESULT_NVT_FAMILY,
  CLIENT_CREATE_REPORT_RR_RESULTS_RESULT_NVT_NAME,
  CLIENT_CREATE_REPORT_RR_RESULTS_RESULT_NVT_RISK_FACTOR,
  CLIENT_CREATE_REPORT_RR_RESULTS_RESULT_NVT_XREF,
  CLIENT_CREATE_REPORT_RR_RESULTS_RESULT_NVT_CERT,
  CLIENT_CREATE_REPORT_RR_RESULTS_RESULT_NVT_CERT_CERT_REF,
  CLIENT_CREATE_REPORT_RR_RESULTS_RESULT_ORIGINAL_THREAT,
  CLIENT_CREATE_REPORT_RR_RESULTS_RESULT_OVERRIDES,
  CLIENT_CREATE_REPORT_RR_RESULTS_RESULT_PORT,
  CLIENT_CREATE_REPORT_RR_RESULTS_RESULT_SUBNET,
  CLIENT_CREATE_REPORT_RR_RESULTS_RESULT_THREAT,
  CLIENT_CREATE_REPORT_RR_RESULT_COUNT,
  CLIENT_CREATE_REPORT_RR_SCAN_RUN_STATUS,
  CLIENT_CREATE_REPORT_RR_SCAN_END,
  CLIENT_CREATE_REPORT_RR_SCAN_START,
  CLIENT_CREATE_REPORT_RR_SORT,
  CLIENT_CREATE_REPORT_RR_TASK,
  CLIENT_CREATE_REPORT_TASK,
  CLIENT_CREATE_REPORT_TASK_NAME,
  CLIENT_CREATE_REPORT_TASK_COMMENT,
  /* CREATE_REPORT_FORMAT. */
  CLIENT_CREATE_REPORT_FORMAT,
  CLIENT_CREATE_REPORT_FORMAT_COPY,
  /* get_report_formats (GRF) is used for report format export.  CLIENT_CRF is
   * for CLIENT_CREATE_REPORT_FORMAT. */
  CLIENT_CRF_GRFR,
  CLIENT_CRF_GRFR_REPORT_FORMAT,
  CLIENT_CRF_GRFR_REPORT_FORMAT_CONTENT_TYPE,
  CLIENT_CRF_GRFR_REPORT_FORMAT_DESCRIPTION,
  CLIENT_CRF_GRFR_REPORT_FORMAT_EXTENSION,
  CLIENT_CRF_GRFR_REPORT_FORMAT_FILE,
  CLIENT_CRF_GRFR_REPORT_FORMAT_GLOBAL,
  CLIENT_CRF_GRFR_REPORT_FORMAT_NAME,
  CLIENT_CRF_GRFR_REPORT_FORMAT_PARAM,
  CLIENT_CRF_GRFR_REPORT_FORMAT_PARAM_DEFAULT,
  CLIENT_CRF_GRFR_REPORT_FORMAT_PARAM_NAME,
  CLIENT_CRF_GRFR_REPORT_FORMAT_PARAM_OPTIONS,
  CLIENT_CRF_GRFR_REPORT_FORMAT_PARAM_OPTIONS_OPTION,
  CLIENT_CRF_GRFR_REPORT_FORMAT_PARAM_TYPE,
  CLIENT_CRF_GRFR_REPORT_FORMAT_PARAM_TYPE_MAX,
  CLIENT_CRF_GRFR_REPORT_FORMAT_PARAM_TYPE_MIN,
  CLIENT_CRF_GRFR_REPORT_FORMAT_PARAM_VALUE,
  CLIENT_CRF_GRFR_REPORT_FORMAT_PREDEFINED,
  CLIENT_CRF_GRFR_REPORT_FORMAT_SIGNATURE,
  CLIENT_CRF_GRFR_REPORT_FORMAT_SUMMARY,
  CLIENT_CRF_GRFR_REPORT_FORMAT_TRUST,
  CLIENT_CREATE_SCHEDULE,
  CLIENT_CREATE_SCHEDULE_NAME,
  CLIENT_CREATE_SCHEDULE_COMMENT,
  CLIENT_CREATE_SCHEDULE_COPY,
  CLIENT_CREATE_SCHEDULE_FIRST_TIME,
  CLIENT_CREATE_SCHEDULE_FIRST_TIME_DAY_OF_MONTH,
  CLIENT_CREATE_SCHEDULE_FIRST_TIME_HOUR,
  CLIENT_CREATE_SCHEDULE_FIRST_TIME_MINUTE,
  CLIENT_CREATE_SCHEDULE_FIRST_TIME_MONTH,
  CLIENT_CREATE_SCHEDULE_FIRST_TIME_YEAR,
  CLIENT_CREATE_SCHEDULE_DURATION,
  CLIENT_CREATE_SCHEDULE_DURATION_UNIT,
  CLIENT_CREATE_SCHEDULE_PERIOD,
  CLIENT_CREATE_SCHEDULE_PERIOD_UNIT,
  CLIENT_CREATE_SLAVE,
  CLIENT_CREATE_SLAVE_COMMENT,
  CLIENT_CREATE_SLAVE_COPY,
  CLIENT_CREATE_SLAVE_HOST,
  CLIENT_CREATE_SLAVE_LOGIN,
  CLIENT_CREATE_SLAVE_NAME,
  CLIENT_CREATE_SLAVE_PASSWORD,
  CLIENT_CREATE_SLAVE_PORT,
  CLIENT_CREATE_TARGET,
  CLIENT_CREATE_TARGET_COMMENT,
  CLIENT_CREATE_TARGET_COPY,
  CLIENT_CREATE_TARGET_HOSTS,
  CLIENT_CREATE_TARGET_SSH_LSC_CREDENTIAL,
  CLIENT_CREATE_TARGET_SSH_LSC_CREDENTIAL_PORT,
  CLIENT_CREATE_TARGET_SMB_LSC_CREDENTIAL,
  CLIENT_CREATE_TARGET_NAME,
  CLIENT_CREATE_TARGET_NAME_MAKE_UNIQUE,
  CLIENT_CREATE_TARGET_PORT_RANGE,
  CLIENT_CREATE_TARGET_PORT_LIST,
  CLIENT_CREATE_TARGET_TARGET_LOCATOR,
  CLIENT_CREATE_TARGET_TARGET_LOCATOR_PASSWORD,
  CLIENT_CREATE_TARGET_TARGET_LOCATOR_USERNAME,
  CLIENT_CREATE_TASK,
  CLIENT_CREATE_TASK_COMMENT,
  CLIENT_CREATE_TASK_CONFIG,
  CLIENT_CREATE_TASK_COPY,
  CLIENT_CREATE_TASK_ALERT,
  CLIENT_CREATE_TASK_NAME,
  CLIENT_CREATE_TASK_OBSERVERS,
  CLIENT_CREATE_TASK_PREFERENCES,
  CLIENT_CREATE_TASK_PREFERENCES_PREFERENCE,
  CLIENT_CREATE_TASK_PREFERENCES_PREFERENCE_NAME,
  CLIENT_CREATE_TASK_PREFERENCES_PREFERENCE_VALUE,
  CLIENT_CREATE_TASK_RCFILE,
  CLIENT_CREATE_TASK_SCHEDULE,
  CLIENT_CREATE_TASK_SLAVE,
  CLIENT_CREATE_TASK_TARGET,
  CLIENT_DELETE_AGENT,
  CLIENT_DELETE_CONFIG,
  CLIENT_DELETE_ALERT,
  CLIENT_DELETE_FILTER,
  CLIENT_DELETE_LSC_CREDENTIAL,
  CLIENT_DELETE_NOTE,
  CLIENT_DELETE_OVERRIDE,
  CLIENT_DELETE_PORT_LIST,
  CLIENT_DELETE_PORT_RANGE,
  CLIENT_DELETE_REPORT,
  CLIENT_DELETE_REPORT_FORMAT,
  CLIENT_DELETE_SCHEDULE,
  CLIENT_DELETE_SLAVE,
  CLIENT_DELETE_TASK,
  CLIENT_DELETE_TARGET,
  CLIENT_EMPTY_TRASHCAN,
  CLIENT_GET_AGENTS,
  CLIENT_GET_CONFIGS,
  CLIENT_GET_DEPENDENCIES,
  CLIENT_GET_ALERTS,
  CLIENT_GET_FILTERS,
  CLIENT_GET_LSC_CREDENTIALS,
  CLIENT_GET_NOTES,
  CLIENT_GET_NVTS,
  CLIENT_GET_NVT_FAMILIES,
  CLIENT_GET_NVT_FEED_CHECKSUM,
  CLIENT_GET_OVERRIDES,
  CLIENT_GET_PORT_LISTS,
  CLIENT_GET_PREFERENCES,
  CLIENT_GET_REPORTS,
  CLIENT_GET_REPORT_FORMATS,
  CLIENT_GET_RESULTS,
  CLIENT_GET_SCHEDULES,
  CLIENT_GET_SETTINGS,
  CLIENT_GET_SLAVES,
  CLIENT_GET_SYSTEM_REPORTS,
  CLIENT_GET_TARGET_LOCATORS,
  CLIENT_GET_TARGETS,
  CLIENT_GET_TASKS,
  CLIENT_GET_VERSION,
  CLIENT_GET_VERSION_AUTHENTIC,
  CLIENT_GET_INFO,
  CLIENT_HELP,
  CLIENT_MODIFY_AGENT,
  CLIENT_MODIFY_AGENT_COMMENT,
  CLIENT_MODIFY_AGENT_NAME,
  CLIENT_MODIFY_ALERT,
  CLIENT_MODIFY_ALERT_NAME,
  CLIENT_MODIFY_ALERT_COMMENT,
  CLIENT_MODIFY_ALERT_FILTER,
  CLIENT_MODIFY_ALERT_EVENT,
  CLIENT_MODIFY_ALERT_EVENT_DATA,
  CLIENT_MODIFY_ALERT_EVENT_DATA_NAME,
  CLIENT_MODIFY_ALERT_CONDITION,
  CLIENT_MODIFY_ALERT_CONDITION_DATA,
  CLIENT_MODIFY_ALERT_CONDITION_DATA_NAME,
  CLIENT_MODIFY_ALERT_METHOD,
  CLIENT_MODIFY_ALERT_METHOD_DATA,
  CLIENT_MODIFY_ALERT_METHOD_DATA_NAME,
  CLIENT_MODIFY_LSC_CREDENTIAL,
  CLIENT_MODIFY_LSC_CREDENTIAL_NAME,
  CLIENT_MODIFY_LSC_CREDENTIAL_COMMENT,
  CLIENT_MODIFY_LSC_CREDENTIAL_LOGIN,
  CLIENT_MODIFY_LSC_CREDENTIAL_PASSWORD,
  CLIENT_MODIFY_REPORT,
  CLIENT_MODIFY_REPORT_COMMENT,
  CLIENT_MODIFY_REPORT_FORMAT,
  CLIENT_MODIFY_REPORT_FORMAT_ACTIVE,
  CLIENT_MODIFY_REPORT_FORMAT_NAME,
  CLIENT_MODIFY_REPORT_FORMAT_SUMMARY,
  CLIENT_MODIFY_REPORT_FORMAT_PARAM,
  CLIENT_MODIFY_REPORT_FORMAT_PARAM_NAME,
  CLIENT_MODIFY_REPORT_FORMAT_PARAM_VALUE,
  CLIENT_MODIFY_CONFIG,
  CLIENT_MODIFY_CONFIG_COMMENT,
  CLIENT_MODIFY_CONFIG_NAME,
  CLIENT_MODIFY_CONFIG_PREFERENCE,
  CLIENT_MODIFY_CONFIG_PREFERENCE_NAME,
  CLIENT_MODIFY_CONFIG_PREFERENCE_NVT,
  CLIENT_MODIFY_CONFIG_PREFERENCE_VALUE,
  CLIENT_MODIFY_CONFIG_FAMILY_SELECTION,
  CLIENT_MODIFY_CONFIG_FAMILY_SELECTION_FAMILY,
  CLIENT_MODIFY_CONFIG_FAMILY_SELECTION_FAMILY_ALL,
  CLIENT_MODIFY_CONFIG_FAMILY_SELECTION_FAMILY_GROWING,
  CLIENT_MODIFY_CONFIG_FAMILY_SELECTION_FAMILY_NAME,
  CLIENT_MODIFY_CONFIG_FAMILY_SELECTION_GROWING,
  CLIENT_MODIFY_CONFIG_NVT_SELECTION,
  CLIENT_MODIFY_CONFIG_NVT_SELECTION_FAMILY,
  CLIENT_MODIFY_CONFIG_NVT_SELECTION_NVT,
  CLIENT_MODIFY_FILTER,
  CLIENT_MODIFY_FILTER_COMMENT,
  CLIENT_MODIFY_FILTER_NAME,
  CLIENT_MODIFY_FILTER_TERM,
  CLIENT_MODIFY_FILTER_TYPE,
  CLIENT_MODIFY_NOTE,
  CLIENT_MODIFY_NOTE_ACTIVE,
  CLIENT_MODIFY_NOTE_HOSTS,
  CLIENT_MODIFY_NOTE_PORT,
  CLIENT_MODIFY_NOTE_RESULT,
  CLIENT_MODIFY_NOTE_TASK,
  CLIENT_MODIFY_NOTE_TEXT,
  CLIENT_MODIFY_NOTE_THREAT,
  CLIENT_MODIFY_OVERRIDE,
  CLIENT_MODIFY_OVERRIDE_ACTIVE,
  CLIENT_MODIFY_OVERRIDE_HOSTS,
  CLIENT_MODIFY_OVERRIDE_NEW_THREAT,
  CLIENT_MODIFY_OVERRIDE_PORT,
  CLIENT_MODIFY_OVERRIDE_RESULT,
  CLIENT_MODIFY_OVERRIDE_TASK,
  CLIENT_MODIFY_OVERRIDE_TEXT,
  CLIENT_MODIFY_OVERRIDE_THREAT,
  CLIENT_MODIFY_PORT_LIST,
  CLIENT_MODIFY_PORT_LIST_COMMENT,
  CLIENT_MODIFY_PORT_LIST_NAME,
  CLIENT_MODIFY_SCHEDULE,
  CLIENT_MODIFY_SCHEDULE_COMMENT,
  CLIENT_MODIFY_SCHEDULE_NAME,
  CLIENT_MODIFY_SCHEDULE_FIRST_TIME,
  CLIENT_MODIFY_SCHEDULE_FIRST_TIME_DAY_OF_MONTH,
  CLIENT_MODIFY_SCHEDULE_FIRST_TIME_HOUR,
  CLIENT_MODIFY_SCHEDULE_FIRST_TIME_MINUTE,
  CLIENT_MODIFY_SCHEDULE_FIRST_TIME_MONTH,
  CLIENT_MODIFY_SCHEDULE_FIRST_TIME_YEAR,
  CLIENT_MODIFY_SCHEDULE_DURATION,
  CLIENT_MODIFY_SCHEDULE_DURATION_UNIT,
  CLIENT_MODIFY_SCHEDULE_PERIOD,
  CLIENT_MODIFY_SCHEDULE_PERIOD_UNIT,
  CLIENT_MODIFY_SCHEDULE_TIMEZONE,
  CLIENT_MODIFY_SETTING,
  CLIENT_MODIFY_SETTING_NAME,
  CLIENT_MODIFY_SETTING_VALUE,
  CLIENT_MODIFY_SLAVE,
  CLIENT_MODIFY_SLAVE_COMMENT,
  CLIENT_MODIFY_SLAVE_NAME,
  CLIENT_MODIFY_SLAVE_HOST,
  CLIENT_MODIFY_SLAVE_PORT,
  CLIENT_MODIFY_SLAVE_LOGIN,
  CLIENT_MODIFY_SLAVE_PASSWORD,
  CLIENT_MODIFY_TARGET,
  CLIENT_MODIFY_TARGET_COMMENT,
  CLIENT_MODIFY_TARGET_HOSTS,
  CLIENT_MODIFY_TARGET_SSH_LSC_CREDENTIAL,
  CLIENT_MODIFY_TARGET_SSH_LSC_CREDENTIAL_PORT,
  CLIENT_MODIFY_TARGET_SMB_LSC_CREDENTIAL,
  CLIENT_MODIFY_TARGET_NAME,
  CLIENT_MODIFY_TARGET_PORT_LIST,
  CLIENT_MODIFY_TARGET_TARGET_LOCATOR,
  CLIENT_MODIFY_TARGET_TARGET_LOCATOR_PASSWORD,
  CLIENT_MODIFY_TARGET_TARGET_LOCATOR_USERNAME,
  CLIENT_MODIFY_TASK,
  CLIENT_MODIFY_TASK_COMMENT,
  CLIENT_MODIFY_TASK_ALERT,
  CLIENT_MODIFY_TASK_CONFIG,
  CLIENT_MODIFY_TASK_FILE,
  CLIENT_MODIFY_TASK_NAME,
  CLIENT_MODIFY_TASK_OBSERVERS,
  CLIENT_MODIFY_TASK_PREFERENCES,
  CLIENT_MODIFY_TASK_PREFERENCES_PREFERENCE,
  CLIENT_MODIFY_TASK_PREFERENCES_PREFERENCE_NAME,
  CLIENT_MODIFY_TASK_PREFERENCES_PREFERENCE_VALUE,
  CLIENT_MODIFY_TASK_RCFILE,
  CLIENT_MODIFY_TASK_SCHEDULE,
  CLIENT_MODIFY_TASK_SLAVE,
  CLIENT_MODIFY_TASK_TARGET,
  CLIENT_PAUSE_TASK,
  CLIENT_RESTORE,
  CLIENT_RESUME_OR_START_TASK,
  CLIENT_RESUME_PAUSED_TASK,
  CLIENT_RESUME_STOPPED_TASK,
  CLIENT_RUN_WIZARD,
  CLIENT_RUN_WIZARD_NAME,
  CLIENT_RUN_WIZARD_PARAMS,
  CLIENT_RUN_WIZARD_PARAMS_PARAM,
  CLIENT_RUN_WIZARD_PARAMS_PARAM_NAME,
  CLIENT_RUN_WIZARD_PARAMS_PARAM_VALUE,
  CLIENT_START_TASK,
  CLIENT_STOP_TASK,
  CLIENT_TEST_ALERT,
  CLIENT_VERIFY_AGENT,
  CLIENT_VERIFY_REPORT_FORMAT
} client_state_t;

/**
 * @brief The state of the client.
 */
static client_state_t client_state = CLIENT_TOP;

/**
 * @brief Set the client state.
 */
static void
set_client_state (client_state_t state)
{
  client_state = state;
  tracef ("   client state set: %i\n", client_state);
}


/* Communication. */

/**
 * @brief Send a response message to the client.
 *
 * @param[in]  msg                       The message, a string.
 * @param[in]  user_send_to_client       Function to send to client.
 * @param[in]  user_send_to_client_data  Argument to \p user_send_to_client.
 *
 * @return TRUE if send to client failed, else FALSE.
 */
static gboolean
send_to_client (const char* msg,
                int (*user_send_to_client) (const char*, void*),
                void* user_send_to_client_data)
{
  if (user_send_to_client && msg)
    return user_send_to_client (msg, user_send_to_client_data);
  return FALSE;
}

/**
 * @brief Send an XML element error response message to the client.
 *
 * @param[in]  command  Command name.
 * @param[in]  element  Element name.
 * @param[in]  write_to_client       Function to write to client.
 * @param[in]  write_to_client_data  Argument to \p write_to_client.
 *
 * @return TRUE if out of space in to_client, else FALSE.
 */
static gboolean
send_element_error_to_client (const char* command, const char* element,
                              int (*write_to_client) (const char*, void*),
                              void* write_to_client_data)
{
  gchar *msg;
  gboolean ret;

  /** @todo Set gerror so parsing terminates. */
  msg = g_strdup_printf ("<%s_response status=\""
                         STATUS_ERROR_SYNTAX
                         "\" status_text=\"Bogus element: %s\"/>",
                         command,
                         element);
  ret = send_to_client (msg, write_to_client, write_to_client_data);
  g_free (msg);
  return ret;
}

/**
 * @brief Send an XML find error response message to the client.
 *
 * @param[in]  command  Command name.
 * @param[in]  type     Resource type.
 * @param[in]  id       Resource ID.
 * @param[in]  write_to_client       Function to write to client.
 * @param[in]  write_to_client_data  Argument to \p write_to_client.
 *
 * @return TRUE if out of space in to_client, else FALSE.
 */
static gboolean
send_find_error_to_client (const char* command, const char* type,
                           const char* id,
                           int (*write_to_client) (const char*, void*),
                           void* write_to_client_data)
{
  gchar *msg;
  gboolean ret;

  msg = g_strdup_printf ("<%s_response status=\""
                         STATUS_ERROR_MISSING
                         "\" status_text=\"Failed to find %s '%s'\"/>",
                         command, type, id);
  ret = send_to_client (msg, write_to_client, write_to_client_data);
  g_free (msg);
  return ret;
}

/**
 * @brief Set an out of space parse error on a GError.
 *
 * @param [out]  error  The error.
 */
static void
error_send_to_client (GError** error)
{
  tracef ("   send_to_client out of space in to_client\n");
  g_set_error (error, G_MARKUP_ERROR, G_MARKUP_ERROR_PARSE,
               "Manager out of space for reply to client.");
}

/**
 * @brief Set an internal error on a GError.
 *
 * @param [out]  error  The error.
 */
static void
internal_error_send_to_client (GError** error)
{
  g_set_error (error, G_MARKUP_ERROR, G_MARKUP_ERROR_PARSE,
               "Internal Error.");
}


/* XML parser handlers. */

/**
 * @brief Expand to XML for a STATUS_ERROR_SYNTAX response.
 *
 * @param  tag   Name of the command generating the response.
 * @param  text  Text for the status_text attribute of the response.
 */
#define XML_ERROR_SYNTAX(tag, text)                      \
 "<" tag "_response"                                     \
 " status=\"" STATUS_ERROR_SYNTAX "\""                   \
 " status_text=\"" text "\"/>"

/**
 * @brief Expand to XML for a STATUS_ERROR_SYNTAX response.
 *
 * This is a variant of the \ref XML_ERROR_SYNTAX macro to allow for a
 * runtime defined syntax_text attribute value.
 *
 * @param  tag   Name of the command generating the response.
 * @param text   Value for the status_text attribute of the response.
 *               The function takes care of proper quoting.
 *
 * @return A malloced XML string.  The caller must use g_free to
 *         release it.
 */
static char *
make_xml_error_syntax (const char *tag, const char *text)
{
  char *textbuf;
  char *ret;

  textbuf = g_markup_escape_text (text, -1);
  ret = g_strdup_printf ("<%s_response status=\"" STATUS_ERROR_SYNTAX "\""
                         " status_text=\"%s\"/>", tag, textbuf);
  g_free (textbuf);
  return ret;
}


/**
 * @brief Expand to XML for a STATUS_ERROR_ACCESS response.
 *
 * @param  tag  Name of the command generating the response.
 */
#define XML_ERROR_ACCESS(tag)                            \
 "<" tag "_response"                                     \
 " status=\"" STATUS_ERROR_ACCESS "\""                   \
 " status_text=\"" STATUS_ERROR_ACCESS_TEXT "\"/>"

/**
 * @brief Expand to XML for a STATUS_SERVICE_UNAVAILABLE response.
 *
 * @param  tag   Name of the command generating the response.
 */
#define XML_ERROR_UNAVAILABLE(tag)                        \
 "<" tag "_response"                                      \
 " status=\"" STATUS_SERVICE_UNAVAILABLE "\""             \
 " status_text=\"" STATUS_SERVICE_UNAVAILABLE_TEXT "\"/>"

/**
 * @brief Expand to XML for a STATUS_ERROR_MISSING response.
 *
 * @param  tag  Name of the command generating the response.
 */
#define XML_ERROR_MISSING(tag)                           \
 "<" tag "_response"                                     \
 " status=\"" STATUS_ERROR_MISSING "\""                  \
 " status_text=\"" STATUS_ERROR_MISSING_TEXT "\"/>"

/**
 * @brief Expand to XML for a STATUS_ERROR_AUTH_FAILED response.
 *
 * @param  tag  Name of the command generating the response.
 */
#define XML_ERROR_AUTH_FAILED(tag)                       \
 "<" tag "_response"                                     \
 " status=\"" STATUS_ERROR_AUTH_FAILED "\""              \
 " status_text=\"" STATUS_ERROR_AUTH_FAILED_TEXT "\"/>"

/**
 * @brief Expand to XML for a STATUS_OK response.
 *
 * @param  tag  Name of the command generating the response.
 */
#define XML_OK(tag)                                      \
 "<" tag "_response"                                     \
 " status=\"" STATUS_OK "\""                             \
 " status_text=\"" STATUS_OK_TEXT "\"/>"

/**
 * @brief Expand to XML for a STATUS_OK_CREATED response.
 *
 * @param  tag  Name of the command generating the response.
 */
#define XML_OK_CREATED(tag)                              \
 "<" tag "_response"                                     \
 " status=\"" STATUS_OK_CREATED "\""                     \
 " status_text=\"" STATUS_OK_CREATED_TEXT "\"/>"

/**
 * @brief Expand to XML for a STATUS_OK_CREATED response with %s for ID.
 *
 * @param  tag  Name of the command generating the response.
 */
#define XML_OK_CREATED_ID(tag)                           \
 "<" tag "_response"                                     \
 " status=\"" STATUS_OK_CREATED "\""                     \
 " status_text=\"" STATUS_OK_CREATED_TEXT "\""           \
 " id=\"%s\"/>"

/**
 * @brief Expand to XML for a STATUS_OK_REQUESTED response.
 *
 * @param  tag  Name of the command generating the response.
 */
#define XML_OK_REQUESTED(tag)                            \
 "<" tag "_response"                                     \
 " status=\"" STATUS_OK_REQUESTED "\""                   \
 " status_text=\"" STATUS_OK_REQUESTED_TEXT "\"/>"

/**
 * @brief Expand to XML for a STATUS_INTERNAL_ERROR response.
 *
 * @param  tag  Name of the command generating the response.
 */
#define XML_INTERNAL_ERROR(tag)                          \
 "<" tag "_response"                                     \
 " status=\"" STATUS_INTERNAL_ERROR "\""                 \
 " status_text=\"" STATUS_INTERNAL_ERROR_TEXT "\"/>"

/**
 * @brief Expand to XML for a STATUS_SERVICE_DOWN response.
 *
 * @param  tag  Name of the command generating the response.
 */
#define XML_SERVICE_DOWN(tag)                            \
 "<" tag "_response"                                     \
 " status=\"" STATUS_SERVICE_DOWN "\""                   \
 " status_text=\"" STATUS_SERVICE_DOWN_TEXT "\"/>"

/** @cond STATIC */

/**
 * @brief Send start of GET response.
 *
 * @param[in]  type                  Type.
 * @param[in]  get                   GET data.
 * @param[in]  write_to_client       Function that sends to clients.
 * @param[in]  write_to_client_data  Data for write_to_client.
 */
int
send_get_start (const char *type, get_data_t *get,
                int (*write_to_client) (const char*, void*),
                void* write_to_client_data)
{
  gchar *msg;

  if (strcmp (type, "info"))
    msg = g_markup_printf_escaped ("<get_%ss_response"
                                   " status=\"" STATUS_OK "\""
                                   " status_text=\"" STATUS_OK_TEXT "\">",
                                   type);
  else
    msg = g_markup_printf_escaped ("<get_%s_response"
                                   " status=\"" STATUS_OK "\""
                                   " status_text=\"" STATUS_OK_TEXT "\">",
                                   type);


  if (send_to_client (msg, write_to_client, write_to_client_data))
    {
      g_free (msg);
      return 1;
    }
  g_free (msg);
  return 0;
}

/**
 * @brief Send common part of GET response for a single resource.
 *
 * @param[in]  type                  Type.
 * @param[in]  get                   GET data.
 * @param[in]  iterator              Iterator.
 * @param[in]  write_to_client       Function that sends to clients.
 * @param[in]  write_to_client_data  Data for write_to_client.
 * @param[in]  writable              Whether the resource is writable.
 * @param[in]  in_use                Whether the resource is in use.
 */
int
send_get_common (const char *type, get_data_t *get, iterator_t *iterator,
                 int (*write_to_client) (const char *, void*),
                 void* write_to_client_data, int writable, int in_use)
{
  gchar* msg;

  msg = g_markup_printf_escaped ("<%s id=\"%s\">"
                                 "<name>%s</name>"
                                 "<comment>%s</comment>"
                                 "<creation_time>%s</creation_time>"
                                 "<modification_time>%s</modification_time>"
                                 "<writable>%i</writable>"
                                 "<in_use>%i</in_use>",
                                 type,
                                 get_iterator_uuid (iterator)
                                  ? get_iterator_uuid (iterator)
                                  : "",
                                 get_iterator_name (iterator)
                                  ? get_iterator_name (iterator)
                                  : "",
                                 get_iterator_comment (iterator)
                                  ? get_iterator_comment (iterator)
                                  : "",
                                 get_iterator_creation_time (iterator)
                                  ? get_iterator_creation_time (iterator)
                                  : "",
                                 get_iterator_modification_time (iterator)
                                  ? get_iterator_modification_time (iterator)
                                  : "",
                                 writable,
                                 in_use);

  if (send_to_client (msg, write_to_client, write_to_client_data))
    {
      g_free (msg);
      return 1;
    }
  g_free (msg);
  return 0;
}

/**
 * @brief Send end of GET response.
 *
 * @param[in]  type                  Type.
 * @param[in]  get                   GET data.
 * @param[in]  count                 Page count.
 * @param[in]  filtered              Filtered count.
 * @param[in]  full                  Full count.
 * @param[in]  write_to_client       Function that sends to clients.
 * @param[in]  write_to_client_data  Data for write_to_client.
 */
int
send_get_end (const char *type, get_data_t *get, int count, int filtered,
              int full, int (*write_to_client) (const char *, void*),
              void* write_to_client_data)
{
  gchar *msg, *sort_field, *filter;
  int first, max, sort_order;
  GString *type_many;

  if (get->filt_id && strcmp (get->filt_id, "0"))
    {
      filter = filter_term (get->filt_id);
      if (filter == NULL)
        return 2;
    }
  else
    filter = NULL;

  manage_filter_controls (filter ? filter : get->filter,
                          &first, &max, &sort_field, &sort_order);

  type_many = g_string_new (type);

  if (strcmp (type, "info") != 0)
    g_string_append (type_many, "s");

  msg = g_markup_printf_escaped ("<filters id=\"%s\">"
                                 "<term>%s</term>"
                                 "</filters>"
                                 "<sort>"
                                 "<field>%s<order>%s</order></field>"
                                 "</sort>"
                                 "<%s start=\"%i\" max=\"%i\"/>"
                                 "<%s_count>"
                                 "%i"
                                 "<filtered>%i</filtered>"
                                 "<page>%i</page>"
                                 "</%s_count>"
                                 "</get_%s_response>",
                                 get->filt_id ? get->filt_id : "",
                                 filter || get->filter
                                  ? manage_clean_filter (filter
                                                          ? filter
                                                          : get->filter)
                                  : "",
                                 sort_field,
                                 sort_order ? "ascending" : "descending",
                                 type_many->str,
                                 first,
                                 max,
                                 type,
                                 full,
                                 filtered,
                                 count,
                                 type,
                                 type_many->str);
  g_string_free (type_many, TRUE);
  g_free (sort_field);
  g_free (filter);

  if (send_to_client (msg, write_to_client, write_to_client_data))
    {
      g_free (msg);
      return 1;
    }
  g_free (msg);
  return 0;
}

/**
 * @brief Send start of GET response to client, returning on fail.
 *
 * @param[in]  type  Type of resource.
 * @param[in]  get   GET data.
 */
#define SEND_GET_START(type, get)                                            \
  do                                                                         \
    {                                                                        \
      if (send_get_start (type, get, write_to_client, write_to_client_data)) \
        {                                                                    \
          error_send_to_client (error);                                      \
          return;                                                            \
        }                                                                    \
    }                                                                        \
  while (0)

/**
 * @brief Send common part of GET response to client, returning on fail.
 *
 * @param[in]  type      Type of resource.
 * @param[in]  get       GET data.
 * @param[in]  iterator  Iterator.
 */
#define SEND_GET_COMMON(type, get, iterator)                                   \
  do                                                                           \
    {                                                                          \
      if (send_get_common (G_STRINGIFY (type), get, iterator,                  \
                           write_to_client, write_to_client_data,              \
                           (get)->trash                                        \
                            ? trash_ ## type ## _writable                      \
                               (get_iterator_resource                          \
                                 (iterator))                                   \
                            : type ## _writable                                \
                               (get_iterator_resource                          \
                                 (iterator)),                                  \
                           (get)->trash                                        \
                            ? trash_ ## type ## _in_use                        \
                               (get_iterator_resource                          \
                                 (iterator))                                   \
                            : type ## _in_use                                  \
                               (get_iterator_resource                          \
                                 (iterator))))                                 \
        {                                                                      \
          error_send_to_client (error);                                        \
          return;                                                              \
        }                                                                      \
    }                                                                          \
  while (0)

/**
 * @brief Send end of GET response to client, returning on fail.
 *
 * @param[in]  type  Type of resource.
 * @param[in]  get   GET data.
 */
#define SEND_GET_END(type, get, count, filtered)                             \
  do                                                                         \
    {                                                                        \
      if (send_get_end (type, get, count, filtered,                          \
                        resource_count (type, get),                          \
                        write_to_client,                                     \
                        write_to_client_data))                               \
        {                                                                    \
          error_send_to_client (error);                                      \
          return;                                                            \
        }                                                                    \
    }                                                                        \
  while (0)

/**
 * @brief Send response message to client, returning on fail.
 *
 * Queue a message in \ref to_client with \ref send_to_client.  On failure
 * call \ref error_send_to_client on a GError* called "error" and do a return.
 *
 * @param[in]   msg    The message, a string.
 */
#define SEND_TO_CLIENT_OR_FAIL(msg)                                          \
  do                                                                         \
    {                                                                        \
      if (send_to_client (msg, write_to_client, write_to_client_data))       \
        {                                                                    \
          error_send_to_client (error);                                      \
          return;                                                            \
        }                                                                    \
    }                                                                        \
  while (0)

/**
 * @brief Send response message to client, returning on fail.
 *
 * Queue a message in \ref to_client with \ref send_to_client.  On failure
 * call \ref error_send_to_client on a GError* called "error" and do a return.
 *
 * @param[in]   format    Format string for message.
 * @param[in]   args      Arguments for format string.
 */
#define SENDF_TO_CLIENT_OR_FAIL(format, args...)                             \
  do                                                                         \
    {                                                                        \
      gchar* msg = g_markup_printf_escaped (format , ## args);               \
      if (send_to_client (msg, write_to_client, write_to_client_data))       \
        {                                                                    \
          g_free (msg);                                                      \
          error_send_to_client (error);                                      \
          return;                                                            \
        }                                                                    \
      g_free (msg);                                                          \
    }                                                                        \
  while (0)

/** @endcond */


/**
 * @brief Insert else clause for omp_xml_handle_start_element.
 *
 * @param[in]  op  Operation.
 */
#define ELSE_ERROR(op)                                          \
  else if (omp_parser->importing)                               \
    {                                                           \
      if (omp_parser->read_over == 0)                           \
        {                                                       \
          omp_parser->read_over = 1;                            \
          omp_parser->parent_state = client_state;              \
        }                                                       \
    }                                                           \
  else                                                          \
    {                                                           \
      if ((strcmp (op, "create_task") == 0)                     \
          && create_task_data->task)                            \
        request_delete_task (&create_task_data->task);          \
      if (send_element_error_to_client (op, element_name,       \
                                        write_to_client,        \
                                        write_to_client_data))  \
        {                                                       \
          error_send_to_client (error);                         \
          return;                                               \
        }                                                       \
      set_client_state (CLIENT_AUTHENTIC);                      \
      g_set_error (error,                                       \
                   G_MARKUP_ERROR,                              \
                   G_MARKUP_ERROR_UNKNOWN_ELEMENT,              \
                   "Error");                                    \
    }                                                           \
  break


/** @todo Free globals when tags open, in case of duplicate tags. */
/**
 * @brief Handle the start of an OMP XML element.
 *
 * React to the start of an XML element according to the current value
 * of \ref client_state, usually adjusting \ref client_state to indicate
 * the change (with \ref set_client_state).  Call \ref send_to_client to
 * queue any responses for the client.
 *
 * Set error parameter on encountering an error.
 *
 * @param[in]  context           Parser context.
 * @param[in]  element_name      XML element name.
 * @param[in]  attribute_names   XML attribute names.
 * @param[in]  attribute_values  XML attribute values.
 * @param[in]  user_data         OMP parser.
 * @param[in]  error             Error parameter.
 */
static void
omp_xml_handle_start_element (/*@unused@*/ GMarkupParseContext* context,
                              const gchar *element_name,
                              const gchar **attribute_names,
                              const gchar **attribute_values,
                              gpointer user_data,
                              GError **error)
{
  omp_parser_t *omp_parser = (omp_parser_t*) user_data;
  int (*write_to_client) (const char *, void*)
    = (int (*) (const char *, void*)) omp_parser->client_writer;
  void* write_to_client_data = (void*) omp_parser->client_writer_data;

  tracef ("   XML  start: %s (%i)\n", element_name, client_state);

  if (omp_parser->read_over)
    omp_parser->read_over++;
  else switch (client_state)
    {
      case CLIENT_TOP:
        if (strcasecmp ("GET_VERSION", element_name) == 0)
          {
            set_client_state (CLIENT_GET_VERSION);
            break;
          }
        /*@fallthrough@*/
      case CLIENT_COMMANDS:
        if (strcasecmp ("AUTHENTICATE", element_name) == 0)
          {
            set_client_state (CLIENT_AUTHENTICATE);
          }
        else if (strcasecmp ("COMMANDS", element_name) == 0)
          {
            SENDF_TO_CLIENT_OR_FAIL
             ("<commands_response"
              " status=\"" STATUS_OK "\" status_text=\"" STATUS_OK_TEXT "\">");
            set_client_state (CLIENT_COMMANDS);
          }
        else
          {
            /** @todo If a real OMP command, return STATUS_ERROR_MUST_AUTH. */
            if (send_to_client
                 (XML_ERROR_SYNTAX ("omp",
                                    "First command must be AUTHENTICATE,"
                                    " COMMANDS or GET_VERSION"),
                  write_to_client,
                  write_to_client_data))
              {
                error_send_to_client (error);
                return;
              }
            if (client_state == CLIENT_COMMANDS)
              send_to_client ("</commands_response>",
                              write_to_client,
                              write_to_client_data);
            g_set_error (error, G_MARKUP_ERROR, G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                         "Must authenticate first.");
          }
        break;

      case CLIENT_AUTHENTIC:
      case CLIENT_AUTHENTIC_COMMANDS:
        if (command_disabled (omp_parser, element_name))
          {
            SEND_TO_CLIENT_OR_FAIL (XML_ERROR_UNAVAILABLE ("omp"));
            g_set_error (error,
                         G_MARKUP_ERROR,
                         G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                         "Command Unavailable");
          }
        else if (strcasecmp ("AUTHENTICATE", element_name) == 0)
          {
            if (save_tasks ()) abort ();
            free_tasks ();
            free_credentials (&current_credentials);
            set_client_state (CLIENT_AUTHENTICATE);
          }
        else if (strcasecmp ("COMMANDS", element_name) == 0)
          {
            SEND_TO_CLIENT_OR_FAIL
             ("<commands_response"
              " status=\"" STATUS_OK "\" status_text=\"" STATUS_OK_TEXT "\">");
            set_client_state (CLIENT_AUTHENTIC_COMMANDS);
          }
        else if (strcasecmp ("CREATE_AGENT", element_name) == 0)
          {
            openvas_append_string (&create_agent_data->comment, "");
            openvas_append_string (&create_agent_data->installer, "");
            openvas_append_string (&create_agent_data->installer_filename, "");
            openvas_append_string (&create_agent_data->installer_signature, "");
            openvas_append_string (&create_agent_data->howto_install, "");
            openvas_append_string (&create_agent_data->howto_use, "");
            set_client_state (CLIENT_CREATE_AGENT);
          }
        else if (strcasecmp ("CREATE_CONFIG", element_name) == 0)
          {
            openvas_append_string (&create_config_data->comment, "");
            openvas_append_string (&create_config_data->name, "");
            set_client_state (CLIENT_CREATE_CONFIG);
          }
        else if (strcasecmp ("CREATE_ALERT", element_name) == 0)
          {
            create_alert_data->condition_data = make_array ();
            create_alert_data->event_data = make_array ();
            create_alert_data->method_data = make_array ();

            openvas_append_string (&create_alert_data->part_data, "");
            openvas_append_string (&create_alert_data->part_name, "");
            openvas_append_string (&create_alert_data->comment, "");
            openvas_append_string (&create_alert_data->name, "");
            openvas_append_string (&create_alert_data->condition, "");
            openvas_append_string (&create_alert_data->method, "");
            openvas_append_string (&create_alert_data->event, "");

            set_client_state (CLIENT_CREATE_ALERT);
          }
        else if (strcasecmp ("CREATE_FILTER", element_name) == 0)
          {
            openvas_append_string (&create_filter_data->comment, "");
            openvas_append_string (&create_filter_data->term, "");
            set_client_state (CLIENT_CREATE_FILTER);
          }
        else if (strcasecmp ("CREATE_LSC_CREDENTIAL", element_name) == 0)
          {
            openvas_append_string (&create_lsc_credential_data->comment, "");
            openvas_append_string (&create_lsc_credential_data->login, "");
            openvas_append_string (&create_lsc_credential_data->name, "");
            set_client_state (CLIENT_CREATE_LSC_CREDENTIAL);
          }
        else if (strcasecmp ("CREATE_NOTE", element_name) == 0)
          set_client_state (CLIENT_CREATE_NOTE);
        else if (strcasecmp ("CREATE_OVERRIDE", element_name) == 0)
          set_client_state (CLIENT_CREATE_OVERRIDE);
        else if (strcasecmp ("CREATE_PORT_LIST", element_name) == 0)
          set_client_state (CLIENT_CREATE_PORT_LIST);
        else if (strcasecmp ("CREATE_PORT_RANGE", element_name) == 0)
          set_client_state (CLIENT_CREATE_PORT_RANGE);
        else if (strcasecmp ("CREATE_REPORT", element_name) == 0)
          set_client_state (CLIENT_CREATE_REPORT);
        else if (strcasecmp ("CREATE_REPORT_FORMAT", element_name) == 0)
          set_client_state (CLIENT_CREATE_REPORT_FORMAT);
        else if (strcasecmp ("CREATE_SLAVE", element_name) == 0)
          {
            openvas_append_string (&create_slave_data->comment, "");
            openvas_append_string (&create_slave_data->password, "");
            set_client_state (CLIENT_CREATE_SLAVE);
          }
        else if (strcasecmp ("CREATE_SCHEDULE", element_name) == 0)
          set_client_state (CLIENT_CREATE_SCHEDULE);
        else if (strcasecmp ("CREATE_TARGET", element_name) == 0)
          {
            openvas_append_string (&create_target_data->comment, "");
            openvas_append_string (&create_target_data->hosts, "");
            set_client_state (CLIENT_CREATE_TARGET);
          }
        else if (strcasecmp ("CREATE_TASK", element_name) == 0)
          {
            create_task_data->task = make_task (NULL, 0, NULL);
            create_task_data->alerts = make_array ();
            set_client_state (CLIENT_CREATE_TASK);
          }
        else if (strcasecmp ("DELETE_AGENT", element_name) == 0)
          {
            const gchar* attribute;
            append_attribute (attribute_names, attribute_values,
                              "agent_id", &delete_agent_data->agent_id);
            if (find_attribute (attribute_names, attribute_values,
                                "ultimate", &attribute))
              delete_agent_data->ultimate = strcmp (attribute, "0");
            else
              delete_agent_data->ultimate = 0;
            set_client_state (CLIENT_DELETE_AGENT);
          }
        else if (strcasecmp ("DELETE_CONFIG", element_name) == 0)
          {
            const gchar* attribute;
            append_attribute (attribute_names, attribute_values,
                              "config_id", &delete_config_data->config_id);
            if (find_attribute (attribute_names, attribute_values,
                                "ultimate", &attribute))
              delete_config_data->ultimate = strcmp (attribute, "0");
            else
              delete_config_data->ultimate = 0;
            set_client_state (CLIENT_DELETE_CONFIG);
          }
        else if (strcasecmp ("DELETE_ALERT", element_name) == 0)
          {
            const gchar* attribute;
            append_attribute (attribute_names, attribute_values,
                              "alert_id",
                              &delete_alert_data->alert_id);
            if (find_attribute (attribute_names, attribute_values,
                                "ultimate", &attribute))
              delete_alert_data->ultimate = strcmp (attribute, "0");
            else
              delete_alert_data->ultimate = 0;
            set_client_state (CLIENT_DELETE_ALERT);
          }
        else if (strcasecmp ("DELETE_FILTER", element_name) == 0)
          {
            const gchar* attribute;
            append_attribute (attribute_names, attribute_values, "filter_id",
                              &delete_filter_data->filter_id);
            if (find_attribute (attribute_names, attribute_values,
                                "ultimate", &attribute))
              delete_filter_data->ultimate = strcmp (attribute, "0");
            else
              delete_filter_data->ultimate = 0;
            set_client_state (CLIENT_DELETE_FILTER);
          }
        else if (strcasecmp ("DELETE_LSC_CREDENTIAL", element_name) == 0)
          {
            const gchar* attribute;
            append_attribute (attribute_names, attribute_values,
                              "lsc_credential_id",
                              &delete_lsc_credential_data->lsc_credential_id);
            if (find_attribute (attribute_names, attribute_values,
                                "ultimate", &attribute))
              delete_lsc_credential_data->ultimate
               = strcmp (attribute, "0");
            else
              delete_lsc_credential_data->ultimate = 0;
            set_client_state (CLIENT_DELETE_LSC_CREDENTIAL);
          }
        else if (strcasecmp ("DELETE_NOTE", element_name) == 0)
          {
            const gchar* attribute;
            append_attribute (attribute_names, attribute_values, "note_id",
                              &delete_note_data->note_id);
            if (find_attribute (attribute_names, attribute_values,
                                "ultimate", &attribute))
              delete_note_data->ultimate = strcmp (attribute, "0");
            else
              delete_note_data->ultimate = 0;
            set_client_state (CLIENT_DELETE_NOTE);
          }
        else if (strcasecmp ("DELETE_OVERRIDE", element_name) == 0)
          {
            const gchar* attribute;
            append_attribute (attribute_names, attribute_values, "override_id",
                              &delete_override_data->override_id);
            if (find_attribute (attribute_names, attribute_values,
                                "ultimate", &attribute))
              delete_override_data->ultimate = strcmp (attribute, "0");
            else
              delete_override_data->ultimate = 0;
            set_client_state (CLIENT_DELETE_OVERRIDE);
          }
        else if (strcasecmp ("DELETE_PORT_LIST", element_name) == 0)
          {
            const gchar* attribute;
            append_attribute (attribute_names, attribute_values, "port_list_id",
                              &delete_port_list_data->port_list_id);
            if (find_attribute (attribute_names, attribute_values,
                                "ultimate", &attribute))
              delete_port_list_data->ultimate = strcmp (attribute, "0");
            else
              delete_port_list_data->ultimate = 0;
            set_client_state (CLIENT_DELETE_PORT_LIST);
          }
        else if (strcasecmp ("DELETE_PORT_RANGE", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "port_range_id",
                              &delete_port_range_data->port_range_id);
            set_client_state (CLIENT_DELETE_PORT_RANGE);
          }
        else if (strcasecmp ("DELETE_REPORT", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "report_id",
                              &delete_report_data->report_id);
            set_client_state (CLIENT_DELETE_REPORT);
          }
        else if (strcasecmp ("DELETE_REPORT_FORMAT", element_name) == 0)
          {
            const gchar* attribute;
            append_attribute (attribute_names, attribute_values, "report_format_id",
                              &delete_report_format_data->report_format_id);
            if (find_attribute (attribute_names, attribute_values,
                                "ultimate", &attribute))
              delete_report_format_data->ultimate = strcmp (attribute,
                                                            "0");
            else
              delete_report_format_data->ultimate = 0;
            set_client_state (CLIENT_DELETE_REPORT_FORMAT);
          }
        else if (strcasecmp ("DELETE_SCHEDULE", element_name) == 0)
          {
            const gchar* attribute;
            append_attribute (attribute_names, attribute_values, "schedule_id",
                              &delete_schedule_data->schedule_id);
            if (find_attribute (attribute_names, attribute_values,
                                "ultimate", &attribute))
              delete_schedule_data->ultimate = strcmp (attribute, "0");
            else
              delete_schedule_data->ultimate = 0;
            set_client_state (CLIENT_DELETE_SCHEDULE);
          }
        else if (strcasecmp ("DELETE_SLAVE", element_name) == 0)
          {
            const gchar* attribute;
            append_attribute (attribute_names, attribute_values, "slave_id",
                              &delete_slave_data->slave_id);
            if (find_attribute (attribute_names, attribute_values,
                                "ultimate", &attribute))
              delete_slave_data->ultimate = strcmp (attribute, "0");
            else
              delete_slave_data->ultimate = 0;
            set_client_state (CLIENT_DELETE_SLAVE);
          }
        else if (strcasecmp ("DELETE_TARGET", element_name) == 0)
          {
            const gchar* attribute;
            append_attribute (attribute_names, attribute_values, "target_id",
                              &delete_target_data->target_id);
            if (find_attribute (attribute_names, attribute_values,
                                "ultimate", &attribute))
              delete_target_data->ultimate = strcmp (attribute, "0");
            else
              delete_target_data->ultimate = 0;
            set_client_state (CLIENT_DELETE_TARGET);
          }
        else if (strcasecmp ("DELETE_TASK", element_name) == 0)
          {
            const gchar* attribute;
            append_attribute (attribute_names, attribute_values, "task_id",
                              &delete_task_data->task_id);
            if (find_attribute (attribute_names, attribute_values,
                                "ultimate", &attribute))
              delete_task_data->ultimate = strcmp (attribute, "0");
            else
              delete_task_data->ultimate = 0;
            set_client_state (CLIENT_DELETE_TASK);
          }
        else if (strcasecmp ("EMPTY_TRASHCAN", element_name) == 0)
          set_client_state (CLIENT_EMPTY_TRASHCAN);
        else if (strcasecmp ("GET_AGENTS", element_name) == 0)
          {
            get_data_parse_attributes (&get_agents_data->get, "agent",
                                       attribute_names,
                                       attribute_values);
            append_attribute (attribute_names, attribute_values, "format",
                              &get_agents_data->format);
            set_client_state (CLIENT_GET_AGENTS);
          }
        else if (strcasecmp ("GET_CONFIGS", element_name) == 0)
          {
            const gchar* attribute;

            get_data_parse_attributes (&get_configs_data->get,
                                       "config",
                                       attribute_names,
                                       attribute_values);

            if (find_attribute (attribute_names, attribute_values,
                                "tasks", &attribute))
              get_configs_data->tasks = strcmp (attribute, "0");
            else
              get_configs_data->tasks = 0;

            if (find_attribute (attribute_names, attribute_values,
                                "families", &attribute))
              get_configs_data->families = strcmp (attribute, "0");
            else
              get_configs_data->families = 0;

            if (find_attribute (attribute_names, attribute_values,
                                "preferences", &attribute))
              get_configs_data->preferences = strcmp (attribute, "0");
            else
              get_configs_data->preferences = 0;

            set_client_state (CLIENT_GET_CONFIGS);
          }
        else if (strcasecmp ("GET_DEPENDENCIES", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "nvt_oid",
                              &get_dependencies_data->nvt_oid);
            set_client_state (CLIENT_GET_DEPENDENCIES);
          }
        else if (strcasecmp ("GET_ALERTS", element_name) == 0)
          {
            const gchar* attribute;

            get_data_parse_attributes (&get_alerts_data->get,
                                       "alert",
                                       attribute_names,
                                       attribute_values);
            if (find_attribute (attribute_names, attribute_values,
                                "tasks", &attribute))
              get_alerts_data->tasks = strcmp (attribute, "0");
            else
              get_alerts_data->tasks = 0;

            set_client_state (CLIENT_GET_ALERTS);
          }
        else if (strcasecmp ("GET_FILTERS", element_name) == 0)
          {
            const gchar* attribute;
            get_data_parse_attributes (&get_filters_data->get, "filter",
                                       attribute_names,
                                       attribute_values);
            if (find_attribute (attribute_names, attribute_values,
                                "alerts", &attribute))
              get_filters_data->alerts = strcmp (attribute, "0");
            else
              get_filters_data->alerts = 0;
            set_client_state (CLIENT_GET_FILTERS);
          }
        else if (strcasecmp ("GET_LSC_CREDENTIALS", element_name) == 0)
          {
            const gchar* attribute;

            get_data_parse_attributes (&get_lsc_credentials_data->get,
                                       "lsc_credential",
                                       attribute_names,
                                       attribute_values);
            if (find_attribute (attribute_names, attribute_values,
                                "targets", &attribute))
              get_lsc_credentials_data->targets = strcmp (attribute, "0");
            else
              get_lsc_credentials_data->targets = 0;
            append_attribute (attribute_names, attribute_values, "format",
                              &get_lsc_credentials_data->format);
            set_client_state (CLIENT_GET_LSC_CREDENTIALS);
          }
        else if (strcasecmp ("GET_NOTES", element_name) == 0)
          {
            const gchar* attribute;

            get_data_parse_attributes (&get_notes_data->get, "note",
                                       attribute_names,
                                       attribute_values);

            append_attribute (attribute_names, attribute_values, "note_id",
                              &get_notes_data->note_id);

            append_attribute (attribute_names, attribute_values, "nvt_oid",
                              &get_notes_data->nvt_oid);

            append_attribute (attribute_names, attribute_values, "task_id",
                              &get_notes_data->task_id);

            if (find_attribute (attribute_names, attribute_values,
                                "result", &attribute))
              get_notes_data->result = strcmp (attribute, "0");
            else
              get_notes_data->result = 0;

            set_client_state (CLIENT_GET_NOTES);
          }
        else if (strcasecmp ("GET_NVT_FEED_CHECKSUM", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "algorithm",
                              &get_nvt_feed_checksum_data->algorithm);
            set_client_state (CLIENT_GET_NVT_FEED_CHECKSUM);
          }
        else if (strcasecmp ("GET_NVTS", element_name) == 0)
          {
            const gchar* attribute;
            append_attribute (attribute_names, attribute_values, "actions",
                              &get_nvts_data->actions);
            append_attribute (attribute_names, attribute_values, "nvt_oid",
                              &get_nvts_data->nvt_oid);
            append_attribute (attribute_names, attribute_values, "config_id",
                              &get_nvts_data->config_id);
            if (find_attribute (attribute_names, attribute_values,
                                "details", &attribute))
              get_nvts_data->details = strcmp (attribute, "0");
            else
              get_nvts_data->details = 0;
            append_attribute (attribute_names, attribute_values, "family",
                              &get_nvts_data->family);
            if (find_attribute (attribute_names, attribute_values,
                                "preferences", &attribute))
              get_nvts_data->preferences = strcmp (attribute, "0");
            else
              get_nvts_data->preferences = 0;
            if (find_attribute (attribute_names, attribute_values,
                                "preference_count", &attribute))
              get_nvts_data->preference_count = strcmp (attribute, "0");
            else
              get_nvts_data->preference_count = 0;
            if (find_attribute (attribute_names, attribute_values,
                                "timeout", &attribute))
              get_nvts_data->timeout = strcmp (attribute, "0");
            else
              get_nvts_data->timeout = 0;
            append_attribute (attribute_names, attribute_values, "sort_field",
                              &get_nvts_data->sort_field);
            if (find_attribute (attribute_names, attribute_values,
                                "sort_order", &attribute))
              get_nvts_data->sort_order = strcmp (attribute,
                                                         "descending");
            else
              get_nvts_data->sort_order = 1;
            set_client_state (CLIENT_GET_NVTS);
          }
        else if (strcasecmp ("GET_NVT_FAMILIES", element_name) == 0)
          {
            const gchar* attribute;
            if (find_attribute (attribute_names, attribute_values,
                                "sort_order", &attribute))
              get_nvt_families_data->sort_order = strcmp (attribute,
                                                          "descending");
            else
              get_nvt_families_data->sort_order = 1;
            set_client_state (CLIENT_GET_NVT_FAMILIES);
          }
        else if (strcasecmp ("GET_OVERRIDES", element_name) == 0)
          {
            const gchar* attribute;

            get_data_parse_attributes (&get_overrides_data->get, "override",
                                       attribute_names,
                                       attribute_values);

            append_attribute (attribute_names, attribute_values, "override_id",
                              &get_overrides_data->override_id);

            append_attribute (attribute_names, attribute_values, "nvt_oid",
                              &get_overrides_data->nvt_oid);

            append_attribute (attribute_names, attribute_values, "task_id",
                              &get_overrides_data->task_id);

            if (find_attribute (attribute_names, attribute_values,
                                "result", &attribute))
              get_overrides_data->result = strcmp (attribute, "0");
            else
              get_overrides_data->result = 0;

            set_client_state (CLIENT_GET_OVERRIDES);
          }
        else if (strcasecmp ("GET_PORT_LISTS", element_name) == 0)
          {
            const gchar* attribute;

            get_data_parse_attributes (&get_port_lists_data->get,
                                       "port_list",
                                       attribute_names,
                                       attribute_values);
            if (find_attribute (attribute_names, attribute_values,
                                "targets", &attribute))
              get_port_lists_data->targets = strcmp (attribute, "0");
            else
              get_port_lists_data->targets = 0;
            set_client_state (CLIENT_GET_PORT_LISTS);
          }
        else if (strcasecmp ("GET_PREFERENCES", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "nvt_oid",
                              &get_preferences_data->nvt_oid);
            append_attribute (attribute_names, attribute_values, "config_id",
                              &get_preferences_data->config_id);
            append_attribute (attribute_names, attribute_values, "preference",
                              &get_preferences_data->preference);
            set_client_state (CLIENT_GET_PREFERENCES);
          }
        else if (strcasecmp ("GET_REPORTS", element_name) == 0)
          {
            const gchar* attribute;

            get_data_parse_attributes (&get_agents_data->get, "report",
                                       attribute_names,
                                       attribute_values);

            append_attribute (attribute_names, attribute_values, "report_id",
                              &get_reports_data->report_id);

            append_attribute (attribute_names, attribute_values,
                              "delta_report_id",
                              &get_reports_data->delta_report_id);

            append_attribute (attribute_names, attribute_values, "alert_id",
                              &get_reports_data->alert_id);

            append_attribute (attribute_names, attribute_values, "format_id",
                              &get_reports_data->format_id);

            if (find_attribute (attribute_names, attribute_values,
                                "first_result", &attribute))
              /* Subtract 1 to switch from 1 to 0 indexing. */
              get_reports_data->first_result = atoi (attribute) - 1;
            else
              get_reports_data->first_result = 0;

            if (find_attribute (attribute_names, attribute_values,
                                "host_first_result", &attribute))
              /* Subtract 1 to switch from 1 to 0 indexing. */
              get_reports_data->host_first_result = atoi (attribute) - 1;
            else
              get_reports_data->host_first_result = 0;

            if (find_attribute (attribute_names, attribute_values,
                                "max_results", &attribute))
              get_reports_data->max_results = atoi (attribute);
            else
              get_reports_data->max_results = -1;

            if (find_attribute (attribute_names, attribute_values,
                                "host_max_results", &attribute))
              get_reports_data->host_max_results = atoi (attribute);
            else
              get_reports_data->host_max_results = -1;

            append_attribute (attribute_names, attribute_values, "sort_field",
                              &get_reports_data->sort_field);

            if (find_attribute (attribute_names, attribute_values,
                                "sort_order", &attribute))
              get_reports_data->sort_order = strcmp (attribute, "descending");
            else
              {
                if (get_reports_data->sort_field == NULL
                    || (strcmp (get_reports_data->sort_field, "type") == 0))
                  /* Normally it makes more sense to order type descending. */
                  get_reports_data->sort_order = 0;
                else
                  get_reports_data->sort_order = 1;
              }

            append_attribute (attribute_names, attribute_values, "levels",
                              &get_reports_data->levels);

            append_attribute (attribute_names, attribute_values, "host_levels",
                              &get_reports_data->host_levels);

            append_attribute (attribute_names, attribute_values, "delta_states",
                              &get_reports_data->delta_states);

            append_attribute (attribute_names, attribute_values,
                              "search_phrase",
                              &get_reports_data->search_phrase);

            if (find_attribute (attribute_names, attribute_values,
                                "autofp", &attribute))
              get_reports_data->autofp = strcmp (attribute, "0");
            else
              get_reports_data->autofp = 0;

            if (find_attribute (attribute_names, attribute_values,
                                "show_closed_cves", &attribute))
              get_reports_data->show_closed_cves = strcmp (attribute, "0");
            else
              get_reports_data->show_closed_cves = 0;

            if (find_attribute (attribute_names, attribute_values,
                                "notes", &attribute))
              get_reports_data->notes = strcmp (attribute, "0");
            else
              get_reports_data->notes = 0;

            if (find_attribute (attribute_names, attribute_values,
                                "notes_details", &attribute))
              get_reports_data->notes_details = strcmp (attribute, "0");
            else
              get_reports_data->notes_details = 0;

            if (find_attribute (attribute_names, attribute_values,
                                "overrides", &attribute))
              get_reports_data->overrides = strcmp (attribute, "0");
            else
              get_reports_data->overrides = 0;

            if (find_attribute (attribute_names, attribute_values,
                                "overrides_details", &attribute))
              get_reports_data->overrides_details = strcmp (attribute, "0");
            else
              get_reports_data->overrides_details = 0;

            if (find_attribute (attribute_names, attribute_values,
                                "apply_overrides", &attribute))
              get_reports_data->apply_overrides = strcmp (attribute, "0");
            else
              get_reports_data->apply_overrides = 0;

            append_attribute (attribute_names, attribute_values,
                              "min_cvss_base",
                              &get_reports_data->min_cvss_base);

            if (find_attribute (attribute_names, attribute_values,
                                "result_hosts_only", &attribute))
              get_reports_data->result_hosts_only = strcmp (attribute, "0");
            else
              get_reports_data->result_hosts_only = 1;

            if (find_attribute (attribute_names, attribute_values,
                                "type", &attribute))
              openvas_append_string (&get_reports_data->type, attribute);
            else
              get_reports_data->type = g_strdup ("scan");

            append_attribute (attribute_names,
                              attribute_values,
                              "host",
                              &get_reports_data->host);

            append_attribute (attribute_names,
                              attribute_values,
                              "pos",
                              &get_reports_data->pos);

            set_client_state (CLIENT_GET_REPORTS);
          }
        else if (strcasecmp ("GET_REPORT_FORMATS", element_name) == 0)
          {
            const gchar* attribute;

            get_data_parse_attributes (&get_report_formats_data->get,
                                       "report_format",
                                       attribute_names,
                                       attribute_values);
            if (find_attribute (attribute_names, attribute_values,
                                "alerts", &attribute))
              get_report_formats_data->alerts = strcmp (attribute, "0");
            else
              get_report_formats_data->alerts = 0;

            if (find_attribute (attribute_names, attribute_values,
                                "params", &attribute))
              get_report_formats_data->params = strcmp (attribute, "0");
            else
              get_report_formats_data->params = 0;

            set_client_state (CLIENT_GET_REPORT_FORMATS);
          }
        else if (strcasecmp ("GET_RESULTS", element_name) == 0)
          {
            const gchar* attribute;

            append_attribute (attribute_names, attribute_values, "result_id",
                              &get_results_data->result_id);

            append_attribute (attribute_names, attribute_values, "task_id",
                              &get_results_data->task_id);

            if (find_attribute (attribute_names, attribute_values,
                                "notes", &attribute))
              get_results_data->notes = strcmp (attribute, "0");
            else
              get_results_data->notes = 0;

            if (find_attribute (attribute_names, attribute_values,
                                "notes_details", &attribute))
              get_results_data->notes_details = strcmp (attribute, "0");
            else
              get_results_data->notes_details = 0;

            if (find_attribute (attribute_names, attribute_values,
                                "overrides", &attribute))
              get_results_data->overrides = strcmp (attribute, "0");
            else
              get_results_data->overrides = 0;

            if (find_attribute (attribute_names, attribute_values,
                                "overrides_details", &attribute))
              get_results_data->overrides_details = strcmp (attribute, "0");
            else
              get_results_data->overrides_details = 0;

            if (find_attribute (attribute_names, attribute_values,
                                "apply_overrides", &attribute))
              get_results_data->apply_overrides = strcmp (attribute, "0");
            else
              get_results_data->apply_overrides = 0;

            if (find_attribute (attribute_names, attribute_values,
                                "autofp", &attribute))
              get_results_data->autofp = strcmp (attribute, "0");
            else
              get_results_data->autofp = 0;

            set_client_state (CLIENT_GET_RESULTS);
          }
        else if (strcasecmp ("GET_SCHEDULES", element_name) == 0)
          {
            const gchar *attribute;
            get_data_parse_attributes (&get_schedules_data->get, "schedule",
                                       attribute_names,
                                       attribute_values);
            if (find_attribute (attribute_names, attribute_values,
                                "tasks", &attribute))
              get_schedules_data->tasks = strcmp (attribute, "0");
            else
              get_schedules_data->tasks = 0;
            set_client_state (CLIENT_GET_SCHEDULES);
          }
        else if (strcasecmp ("GET_SETTINGS", element_name) == 0)
          {
            const gchar* attribute;

            append_attribute (attribute_names, attribute_values, "setting_id",
                              &get_settings_data->setting_id);

            append_attribute (attribute_names, attribute_values, "filter",
                              &get_settings_data->filter);

            if (find_attribute (attribute_names, attribute_values,
                                "first", &attribute))
              /* Subtract 1 to switch from 1 to 0 indexing. */
              get_settings_data->first = atoi (attribute) - 1;
            else
              get_settings_data->first = 0;
            if (get_settings_data->first < 0)
              get_settings_data->first = 0;

            if (find_attribute (attribute_names, attribute_values,
                                "max", &attribute))
              get_settings_data->max = atoi (attribute);
            else
              get_settings_data->max = -1;
            if (get_settings_data->max < 1)
              get_settings_data->max = -1;

            append_attribute (attribute_names, attribute_values, "sort_field",
                              &get_settings_data->sort_field);

            if (find_attribute (attribute_names, attribute_values,
                                "sort_order", &attribute))
              get_settings_data->sort_order = strcmp (attribute, "descending");
            else
              get_settings_data->sort_order = 1;

            set_client_state (CLIENT_GET_SETTINGS);
          }
        else if (strcasecmp ("GET_SLAVES", element_name) == 0)
          {
            const gchar* attribute;
            get_data_parse_attributes (&get_slaves_data->get, "slave",
                                       attribute_names,
                                       attribute_values);
            if (find_attribute (attribute_names, attribute_values,
                                "tasks", &attribute))
              get_slaves_data->tasks = strcmp (attribute, "0");
            else
              get_slaves_data->tasks = 0;
            set_client_state (CLIENT_GET_SLAVES);
          }
        else if (strcasecmp ("GET_TARGET_LOCATORS", element_name) == 0)
          {
            set_client_state (CLIENT_GET_TARGET_LOCATORS);
          }
        else if (strcasecmp ("GET_SYSTEM_REPORTS", element_name) == 0)
          {
            const gchar* attribute;
            append_attribute (attribute_names, attribute_values, "name",
                              &get_system_reports_data->name);
            append_attribute (attribute_names, attribute_values, "duration",
                              &get_system_reports_data->duration);
            append_attribute (attribute_names, attribute_values, "slave_id",
                              &get_system_reports_data->slave_id);
            if (find_attribute (attribute_names, attribute_values,
                                "brief", &attribute))
              get_system_reports_data->brief = strcmp (attribute, "0");
            else
              get_system_reports_data->brief = 0;
            set_client_state (CLIENT_GET_SYSTEM_REPORTS);
          }
        else if (strcasecmp ("GET_TARGETS", element_name) == 0)
          {
            const gchar *attribute;
            get_data_parse_attributes (&get_targets_data->get, "target",
                                       attribute_names,
                                       attribute_values);
            if (find_attribute (attribute_names, attribute_values,
                                "tasks", &attribute))
              get_targets_data->tasks = strcmp (attribute, "0");
            else
              get_targets_data->tasks = 0;
            set_client_state (CLIENT_GET_TARGETS);
          }
        else if (strcasecmp ("GET_TASKS", element_name) == 0)
          {
            const gchar* attribute;

            get_data_parse_attributes (&get_tasks_data->get, "task",
                                       attribute_names,
                                       attribute_values);

            if (find_attribute (attribute_names, attribute_values,
                                "rcfile", &attribute))
              get_tasks_data->rcfile = atoi (attribute);
            else
              get_tasks_data->rcfile = 0;

            set_client_state (CLIENT_GET_TASKS);
          }
        else if (strcasecmp ("GET_INFO", element_name) == 0)
          {
            const gchar* attribute;
            const gchar* typebuf;
            get_data_parse_attributes (&get_info_data->get, "info",
                                       attribute_names,
                                       attribute_values);
            append_attribute (attribute_names, attribute_values, "name",
                              &get_info_data->name);
            if (find_attribute (attribute_names, attribute_values,
                                "details", &attribute))
              get_info_data->details = strcmp (attribute, "0");
            else
              get_info_data->details = 0;

            if (find_attribute (attribute_names, attribute_values,
                                "type", &typebuf))
              get_info_data->type = g_ascii_strdown (typebuf, -1);
            set_client_state (CLIENT_GET_INFO);
          }
        else if (strcasecmp ("GET_VERSION", element_name) == 0)
          set_client_state (CLIENT_GET_VERSION_AUTHENTIC);
        else if (strcasecmp ("HELP", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "format",
                              &help_data->format);
            append_attribute (attribute_names, attribute_values, "type",
                              &help_data->type);
            set_client_state (CLIENT_HELP);
          }
        else if (strcasecmp ("MODIFY_AGENT", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "agent_id",
                              &modify_agent_data->agent_id);
            set_client_state (CLIENT_MODIFY_AGENT);
          }
        else if (strcasecmp ("MODIFY_ALERT", element_name) == 0)
          {
            modify_alert_data->event_data = make_array ();
            openvas_append_string (&modify_alert_data->event, "");
            modify_alert_data->condition_data = make_array ();
            openvas_append_string (&modify_alert_data->condition, "");
            modify_alert_data->method_data = make_array ();
            openvas_append_string (&modify_alert_data->method, "");

            append_attribute (attribute_names, attribute_values, "alert_id",
                              &modify_alert_data->alert_id);
            set_client_state (CLIENT_MODIFY_ALERT);
          }
        else if (strcasecmp ("MODIFY_CONFIG", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "config_id",
                              &modify_config_data->config_id);
            set_client_state (CLIENT_MODIFY_CONFIG);
          }
        else if (strcasecmp ("MODIFY_FILTER", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "filter_id",
                              &modify_filter_data->filter_id);
            set_client_state (CLIENT_MODIFY_FILTER);
          }
        else if (strcasecmp ("MODIFY_PORT_LIST", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values,
                              "port_list_id",
                              &modify_port_list_data->port_list_id);
            set_client_state (CLIENT_MODIFY_PORT_LIST);
          }
        else if (strcasecmp ("MODIFY_LSC_CREDENTIAL", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values,
                              "lsc_credential_id",
                              &modify_lsc_credential_data->lsc_credential_id);
            set_client_state (CLIENT_MODIFY_LSC_CREDENTIAL);
          }
        else if (strcasecmp ("MODIFY_NOTE", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "note_id",
                              &modify_note_data->note_id);
            set_client_state (CLIENT_MODIFY_NOTE);
          }
        else if (strcasecmp ("MODIFY_OVERRIDE", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "override_id",
                              &modify_override_data->override_id);
            set_client_state (CLIENT_MODIFY_OVERRIDE);
          }
        else if (strcasecmp ("MODIFY_REPORT", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "report_id",
                              &modify_report_data->report_id);
            set_client_state (CLIENT_MODIFY_REPORT);
          }
        else if (strcasecmp ("MODIFY_REPORT_FORMAT", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values,
                              "report_format_id",
                              &modify_report_format_data->report_format_id);
            set_client_state (CLIENT_MODIFY_REPORT_FORMAT);
          }
        else if (strcasecmp ("MODIFY_SCHEDULE", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "schedule_id",
                              &modify_schedule_data->schedule_id);
            set_client_state (CLIENT_MODIFY_SCHEDULE);
          }
        else if (strcasecmp ("MODIFY_SETTING", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values,
                              "setting_id",
                              &modify_setting_data->setting_id);
            set_client_state (CLIENT_MODIFY_SETTING);
          }
        else if (strcasecmp ("MODIFY_SLAVE", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "slave_id",
                              &modify_slave_data->slave_id);
            set_client_state (CLIENT_MODIFY_SLAVE);
          }
        else if (strcasecmp ("MODIFY_TARGET", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "target_id",
                              &modify_target_data->target_id);
            set_client_state (CLIENT_MODIFY_TARGET);
          }
        else if (strcasecmp ("MODIFY_TASK", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "task_id",
                              &modify_task_data->task_id);
            modify_task_data->alerts = make_array ();
            set_client_state (CLIENT_MODIFY_TASK);
          }
        else if (strcasecmp ("PAUSE_TASK", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "task_id",
                              &pause_task_data->task_id);
            set_client_state (CLIENT_PAUSE_TASK);
          }
        else if (strcasecmp ("RESTORE", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "id",
                              &restore_data->id);
            set_client_state (CLIENT_RESTORE);
          }
        else if (strcasecmp ("RESUME_OR_START_TASK", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "task_id",
                              &resume_or_start_task_data->task_id);
            set_client_state (CLIENT_RESUME_OR_START_TASK);
          }
        else if (strcasecmp ("RESUME_PAUSED_TASK", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "task_id",
                              &resume_paused_task_data->task_id);
            set_client_state (CLIENT_RESUME_PAUSED_TASK);
          }
        else if (strcasecmp ("RESUME_STOPPED_TASK", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "task_id",
                              &resume_paused_task_data->task_id);
            set_client_state (CLIENT_RESUME_STOPPED_TASK);
          }
        else if (strcasecmp ("RUN_WIZARD", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "name",
                              &run_wizard_data->name);
            set_client_state (CLIENT_RUN_WIZARD);
          }
        else if (strcasecmp ("START_TASK", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "task_id",
                              &start_task_data->task_id);
            set_client_state (CLIENT_START_TASK);
          }
        else if (strcasecmp ("STOP_TASK", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "task_id",
                              &stop_task_data->task_id);
            set_client_state (CLIENT_STOP_TASK);
          }
        else if (strcasecmp ("TEST_ALERT", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values,
                              "alert_id",
                              &test_alert_data->alert_id);
            set_client_state (CLIENT_TEST_ALERT);
          }
        else if (strcasecmp ("VERIFY_AGENT", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "agent_id",
                              &verify_agent_data->agent_id);
            set_client_state (CLIENT_VERIFY_AGENT);
          }
        else if (strcasecmp ("VERIFY_REPORT_FORMAT", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "report_format_id",
                              &verify_report_format_data->report_format_id);
            set_client_state (CLIENT_VERIFY_REPORT_FORMAT);
          }
        else
          {
            if (send_to_client (XML_ERROR_SYNTAX ("omp", "Bogus command name"),
                                write_to_client,
                                write_to_client_data))
              {
                error_send_to_client (error);
                return;
              }
            g_set_error (error,
                         G_MARKUP_ERROR,
                         G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                         "Error");
          }
        break;

      case CLIENT_AUTHENTICATE:
        if (strcasecmp ("CREDENTIALS", element_name) == 0)
          {
            /* Init, so it's the empty string when the entity is empty. */
            append_to_credentials_password (&current_credentials, "", 0);
            set_client_state (CLIENT_AUTHENTICATE_CREDENTIALS);
          }
        ELSE_ERROR ("authenticate");
        break;
      case CLIENT_AUTHENTICATE_CREDENTIALS:
        if (strcasecmp ("USERNAME", element_name) == 0)
          set_client_state (CLIENT_AUTHENTICATE_CREDENTIALS_USERNAME);
        else if (strcasecmp ("PASSWORD", element_name) == 0)
          set_client_state (CLIENT_AUTHENTICATE_CREDENTIALS_PASSWORD);
        ELSE_ERROR ("authenticate");

      case CLIENT_CREATE_SCHEDULE:
        if (strcasecmp ("COMMENT", element_name) == 0)
          set_client_state (CLIENT_CREATE_SCHEDULE_COMMENT);
        else if (strcasecmp ("COPY", element_name) == 0)
          set_client_state (CLIENT_CREATE_SCHEDULE_COPY);
        else if (strcasecmp ("DURATION", element_name) == 0)
          set_client_state (CLIENT_CREATE_SCHEDULE_DURATION);
        else if (strcasecmp ("FIRST_TIME", element_name) == 0)
          set_client_state (CLIENT_CREATE_SCHEDULE_FIRST_TIME);
        else if (strcasecmp ("NAME", element_name) == 0)
          set_client_state (CLIENT_CREATE_SCHEDULE_NAME);
        else if (strcasecmp ("PERIOD", element_name) == 0)
          set_client_state (CLIENT_CREATE_SCHEDULE_PERIOD);
        ELSE_ERROR ("create_schedule");

      case CLIENT_CREATE_SCHEDULE_FIRST_TIME:
        if (strcasecmp ("DAY_OF_MONTH", element_name) == 0)
          set_client_state (CLIENT_CREATE_SCHEDULE_FIRST_TIME_DAY_OF_MONTH);
        else if (strcasecmp ("HOUR", element_name) == 0)
          set_client_state (CLIENT_CREATE_SCHEDULE_FIRST_TIME_HOUR);
        else if (strcasecmp ("MINUTE", element_name) == 0)
          set_client_state (CLIENT_CREATE_SCHEDULE_FIRST_TIME_MINUTE);
        else if (strcasecmp ("MONTH", element_name) == 0)
          set_client_state (CLIENT_CREATE_SCHEDULE_FIRST_TIME_MONTH);
        else if (strcasecmp ("YEAR", element_name) == 0)
          set_client_state (CLIENT_CREATE_SCHEDULE_FIRST_TIME_YEAR);
        ELSE_ERROR ("create_schedule");

      case CLIENT_CREATE_SCHEDULE_DURATION:
        if (strcasecmp ("UNIT", element_name) == 0)
          set_client_state (CLIENT_CREATE_SCHEDULE_DURATION_UNIT);
        ELSE_ERROR ("create_schedule");

      case CLIENT_CREATE_SCHEDULE_PERIOD:
        if (strcasecmp ("UNIT", element_name) == 0)
          set_client_state (CLIENT_CREATE_SCHEDULE_PERIOD_UNIT);
        ELSE_ERROR ("create_schedule");

      case CLIENT_MODIFY_AGENT:
        if (strcasecmp ("COMMENT", element_name) == 0)
          {
            openvas_append_string (&modify_agent_data->comment, "");
            set_client_state (CLIENT_MODIFY_AGENT_COMMENT);
          }
        else if (strcasecmp ("NAME", element_name) == 0)
          {
            openvas_append_string (&modify_agent_data->name, "");
            set_client_state (CLIENT_MODIFY_AGENT_NAME);
          }
        ELSE_ERROR ("modify_agent");

      case CLIENT_MODIFY_ALERT:
        if (strcasecmp ("NAME", element_name) == 0)
          {
            openvas_append_string (&modify_alert_data->name, "");
            set_client_state (CLIENT_MODIFY_ALERT_NAME);
          }
        else if (strcasecmp ("COMMENT", element_name) == 0)
          {
            openvas_append_string (&modify_alert_data->comment, "");
            set_client_state (CLIENT_MODIFY_ALERT_COMMENT);
          }
        else if (strcasecmp ("EVENT", element_name) == 0)
          set_client_state (CLIENT_MODIFY_ALERT_EVENT);
        else if (strcasecmp ("FILTER", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "id",
                              &modify_alert_data->filter_id);
            set_client_state (CLIENT_MODIFY_ALERT_FILTER);
          }
        else if (strcasecmp ("CONDITION", element_name) == 0)
          set_client_state (CLIENT_MODIFY_ALERT_CONDITION);
        else if (strcasecmp ("METHOD", element_name) == 0)
          set_client_state (CLIENT_MODIFY_ALERT_METHOD);
        ELSE_ERROR ("modify_alert");

      case CLIENT_MODIFY_ALERT_EVENT:
        if (strcasecmp ("DATA", element_name) == 0)
          set_client_state (CLIENT_MODIFY_ALERT_EVENT_DATA);
        ELSE_ERROR ("modify_alert");

      case CLIENT_MODIFY_ALERT_EVENT_DATA:
        if (strcasecmp ("NAME", element_name) == 0)
          set_client_state (CLIENT_MODIFY_ALERT_EVENT_DATA_NAME);
        ELSE_ERROR ("modify_alert");

      case CLIENT_MODIFY_ALERT_CONDITION:
        if (strcasecmp ("DATA", element_name) == 0)
          set_client_state (CLIENT_MODIFY_ALERT_CONDITION_DATA);
        ELSE_ERROR ("modify_alert");

      case CLIENT_MODIFY_ALERT_CONDITION_DATA:
        if (strcasecmp ("NAME", element_name) == 0)
          set_client_state (CLIENT_MODIFY_ALERT_CONDITION_DATA_NAME);
        ELSE_ERROR ("modify_alert");

      case CLIENT_MODIFY_ALERT_METHOD:
        if (strcasecmp ("DATA", element_name) == 0)
          set_client_state (CLIENT_MODIFY_ALERT_METHOD_DATA);
        ELSE_ERROR ("modify_alert");

      case CLIENT_MODIFY_ALERT_METHOD_DATA:
        if (strcasecmp ("NAME", element_name) == 0)
          set_client_state (CLIENT_MODIFY_ALERT_METHOD_DATA_NAME);
        ELSE_ERROR ("modify_alert");

      case CLIENT_MODIFY_CONFIG:
        if (strcasecmp ("COMMENT", element_name) == 0)
          {
            openvas_free_string_var (&modify_config_data->comment);
            openvas_append_string (&modify_config_data->comment, "");
            set_client_state (CLIENT_MODIFY_CONFIG_COMMENT);
          }
        else if (strcasecmp ("FAMILY_SELECTION", element_name) == 0)
          {
            modify_config_data->families_growing_all = make_array ();
            modify_config_data->families_static_all = make_array ();
            modify_config_data->families_growing_empty = make_array ();
            /* For GROWING entity, in case missing. */
            modify_config_data->family_selection_growing = 0;
            set_client_state (CLIENT_MODIFY_CONFIG_FAMILY_SELECTION);
          }
        else if (strcasecmp ("NAME", element_name) == 0)
          set_client_state (CLIENT_MODIFY_CONFIG_NAME);
        else if (strcasecmp ("NVT_SELECTION", element_name) == 0)
          {
            modify_config_data->nvt_selection = make_array ();
            set_client_state (CLIENT_MODIFY_CONFIG_NVT_SELECTION);
          }
        else if (strcasecmp ("PREFERENCE", element_name) == 0)
          {
            openvas_free_string_var (&modify_config_data->preference_name);
            openvas_free_string_var (&modify_config_data->preference_nvt_oid);
            openvas_free_string_var (&modify_config_data->preference_value);
            set_client_state (CLIENT_MODIFY_CONFIG_PREFERENCE);
          }
        ELSE_ERROR ("modify_config");

      case CLIENT_MODIFY_CONFIG_NVT_SELECTION:
        if (strcasecmp ("FAMILY", element_name) == 0)
          set_client_state (CLIENT_MODIFY_CONFIG_NVT_SELECTION_FAMILY);
        else if (strcasecmp ("NVT", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "oid",
                              &modify_config_data->nvt_selection_nvt_oid);
            set_client_state (CLIENT_MODIFY_CONFIG_NVT_SELECTION_NVT);
          }
        ELSE_ERROR ("modify_config");

      case CLIENT_MODIFY_CONFIG_FAMILY_SELECTION:
        if (strcasecmp ("FAMILY", element_name) == 0)
          {
            /* For ALL entity, in case missing. */
            modify_config_data->family_selection_family_all = 0;
            /* For GROWING entity, in case missing. */
            modify_config_data->family_selection_family_growing = 0;
            set_client_state (CLIENT_MODIFY_CONFIG_FAMILY_SELECTION_FAMILY);
          }
        else if (strcasecmp ("GROWING", element_name) == 0)
          set_client_state (CLIENT_MODIFY_CONFIG_FAMILY_SELECTION_GROWING);
        ELSE_ERROR ("modify_config");

      case CLIENT_MODIFY_CONFIG_FAMILY_SELECTION_FAMILY:
        if (strcasecmp ("ALL", element_name) == 0)
          set_client_state
           (CLIENT_MODIFY_CONFIG_FAMILY_SELECTION_FAMILY_ALL);
        else if (strcasecmp ("GROWING", element_name) == 0)
          set_client_state
           (CLIENT_MODIFY_CONFIG_FAMILY_SELECTION_FAMILY_GROWING);
        else if (strcasecmp ("NAME", element_name) == 0)
          set_client_state (CLIENT_MODIFY_CONFIG_FAMILY_SELECTION_FAMILY_NAME);
        ELSE_ERROR ("modify_config");

      case CLIENT_MODIFY_CONFIG_PREFERENCE:
        if (strcasecmp ("NAME", element_name) == 0)
          set_client_state (CLIENT_MODIFY_CONFIG_PREFERENCE_NAME);
        else if (strcasecmp ("NVT", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "oid",
                              &modify_config_data->preference_nvt_oid);
            set_client_state (CLIENT_MODIFY_CONFIG_PREFERENCE_NVT);
          }
        else if (strcasecmp ("VALUE", element_name) == 0)
          set_client_state (CLIENT_MODIFY_CONFIG_PREFERENCE_VALUE);
        ELSE_ERROR ("modify_config");

      case CLIENT_MODIFY_FILTER:
        if (strcasecmp ("COMMENT", element_name) == 0)
          {
            openvas_append_string (&modify_filter_data->comment, "");
            set_client_state (CLIENT_MODIFY_FILTER_COMMENT);
          }
        else if (strcasecmp ("NAME", element_name) == 0)
          {
            openvas_append_string (&modify_filter_data->name, "");
            set_client_state (CLIENT_MODIFY_FILTER_NAME);
          }
        else if (strcasecmp ("TERM", element_name) == 0)
          {
            openvas_append_string (&modify_filter_data->term, "");
            set_client_state (CLIENT_MODIFY_FILTER_TERM);
          }
        else if (strcasecmp ("TYPE", element_name) == 0)
          {
            openvas_append_string (&modify_filter_data->type, "");
            set_client_state (CLIENT_MODIFY_FILTER_TYPE);
          }
        ELSE_ERROR ("modify_filter");

      case CLIENT_MODIFY_PORT_LIST:
        if (strcasecmp ("NAME", element_name) == 0)
          set_client_state (CLIENT_MODIFY_PORT_LIST_NAME);
        else if (strcasecmp ("COMMENT", element_name) == 0)
          {
            openvas_free_string_var (&modify_port_list_data->comment);
            openvas_append_string (&modify_port_list_data->comment, "");
            set_client_state (CLIENT_MODIFY_PORT_LIST_COMMENT);
          }
        ELSE_ERROR ("modify_port_list");

      case CLIENT_MODIFY_LSC_CREDENTIAL:
        if (strcasecmp ("NAME", element_name) == 0)
          set_client_state (CLIENT_MODIFY_LSC_CREDENTIAL_NAME);
        else if (strcasecmp ("COMMENT", element_name) == 0)
          {
            openvas_free_string_var (&modify_lsc_credential_data->comment);
            openvas_append_string (&modify_lsc_credential_data->comment, "");
            set_client_state (CLIENT_MODIFY_LSC_CREDENTIAL_COMMENT);
          }
        else if (strcasecmp ("LOGIN", element_name) == 0)
          set_client_state (CLIENT_MODIFY_LSC_CREDENTIAL_LOGIN);
        else if (strcasecmp ("PASSWORD", element_name) == 0)
          {
            openvas_free_string_var (&modify_lsc_credential_data->password);
            openvas_append_string (&modify_lsc_credential_data->password, "");
            set_client_state (CLIENT_MODIFY_LSC_CREDENTIAL_PASSWORD);
          }
        ELSE_ERROR ("modify_lsc_credential");

      case CLIENT_MODIFY_REPORT:
        if (strcasecmp ("COMMENT", element_name) == 0)
          set_client_state (CLIENT_MODIFY_REPORT_COMMENT);
        ELSE_ERROR ("modify_report");

      case CLIENT_MODIFY_REPORT_FORMAT:
        if (strcasecmp ("ACTIVE", element_name) == 0)
          set_client_state (CLIENT_MODIFY_REPORT_FORMAT_ACTIVE);
        else if (strcasecmp ("NAME", element_name) == 0)
          set_client_state (CLIENT_MODIFY_REPORT_FORMAT_NAME);
        else if (strcasecmp ("SUMMARY", element_name) == 0)
          set_client_state (CLIENT_MODIFY_REPORT_FORMAT_SUMMARY);
        else if (strcasecmp ("PARAM", element_name) == 0)
          set_client_state (CLIENT_MODIFY_REPORT_FORMAT_PARAM);
        ELSE_ERROR ("modify_report_format");

      case CLIENT_MODIFY_REPORT_FORMAT_PARAM:
        if (strcasecmp ("NAME", element_name) == 0)
          set_client_state (CLIENT_MODIFY_REPORT_FORMAT_PARAM_NAME);
        else if (strcasecmp ("VALUE", element_name) == 0)
          set_client_state (CLIENT_MODIFY_REPORT_FORMAT_PARAM_VALUE);
        ELSE_ERROR ("modify_report_format");

      case CLIENT_MODIFY_SCHEDULE:
        if (strcasecmp ("COMMENT", element_name) == 0)
          {
            openvas_append_string (&modify_schedule_data->comment, "");
            set_client_state (CLIENT_MODIFY_SCHEDULE_COMMENT);
          }
        else if (strcasecmp ("NAME", element_name) == 0)
          {
            openvas_append_string (&modify_schedule_data->name, "");
            set_client_state (CLIENT_MODIFY_SCHEDULE_NAME);
          }
        else if (strcasecmp ("DURATION", element_name) == 0)
          set_client_state (CLIENT_MODIFY_SCHEDULE_DURATION);
        else if (strcasecmp ("FIRST_TIME", element_name) == 0)
          set_client_state (CLIENT_MODIFY_SCHEDULE_FIRST_TIME);
        else if (strcasecmp ("NAME", element_name) == 0)
          set_client_state (CLIENT_MODIFY_SCHEDULE_NAME);
        else if (strcasecmp ("PERIOD", element_name) == 0)
          set_client_state (CLIENT_MODIFY_SCHEDULE_PERIOD);
        else if (strcasecmp ("TIMEZONE", element_name) == 0)
          set_client_state (CLIENT_MODIFY_SCHEDULE_TIMEZONE);
        ELSE_ERROR ("modify_schedule");

      case CLIENT_MODIFY_SCHEDULE_FIRST_TIME:
        if (strcasecmp ("DAY_OF_MONTH", element_name) == 0)
          set_client_state (CLIENT_MODIFY_SCHEDULE_FIRST_TIME_DAY_OF_MONTH);
        else if (strcasecmp ("HOUR", element_name) == 0)
          set_client_state (CLIENT_MODIFY_SCHEDULE_FIRST_TIME_HOUR);
        else if (strcasecmp ("MINUTE", element_name) == 0)
          set_client_state (CLIENT_MODIFY_SCHEDULE_FIRST_TIME_MINUTE);
        else if (strcasecmp ("MONTH", element_name) == 0)
          set_client_state (CLIENT_MODIFY_SCHEDULE_FIRST_TIME_MONTH);
        else if (strcasecmp ("YEAR", element_name) == 0)
          set_client_state (CLIENT_MODIFY_SCHEDULE_FIRST_TIME_YEAR);
        ELSE_ERROR ("modify_schedule");

      case CLIENT_MODIFY_SCHEDULE_DURATION:
        if (strcasecmp ("UNIT", element_name) == 0)
          set_client_state (CLIENT_MODIFY_SCHEDULE_DURATION_UNIT);
        ELSE_ERROR ("modify_schedule");

      case CLIENT_MODIFY_SCHEDULE_PERIOD:
        if (strcasecmp ("UNIT", element_name) == 0)
          set_client_state (CLIENT_MODIFY_SCHEDULE_PERIOD_UNIT);
        ELSE_ERROR ("modify_schedule");

      case CLIENT_MODIFY_SETTING:
        if (strcasecmp ("NAME", element_name) == 0)
          set_client_state (CLIENT_MODIFY_SETTING_NAME);
        else if (strcasecmp ("VALUE", element_name) == 0)
          {
            openvas_append_string (&modify_setting_data->value, "");
            set_client_state (CLIENT_MODIFY_SETTING_VALUE);
          }
        ELSE_ERROR ("modify_setting");

      case CLIENT_MODIFY_SLAVE:
        if (strcasecmp ("COMMENT", element_name) == 0)
          {
            openvas_append_string (&modify_slave_data->comment, "");
            set_client_state (CLIENT_MODIFY_SLAVE_COMMENT);
          }
        else if (strcasecmp ("NAME", element_name) == 0)
          {
            openvas_append_string (&modify_slave_data->name, "");
            set_client_state (CLIENT_MODIFY_SLAVE_NAME);
          }
        else if (strcasecmp ("HOST", element_name) == 0)
          {
            openvas_append_string (&modify_slave_data->host, "");
            set_client_state (CLIENT_MODIFY_SLAVE_HOST);
          }
        else if (strcasecmp ("PORT", element_name) == 0)
          {
            openvas_append_string (&modify_slave_data->port, "");
            set_client_state (CLIENT_MODIFY_SLAVE_PORT);
          }
        else if (strcasecmp ("LOGIN", element_name) == 0)
          {
            openvas_append_string (&modify_slave_data->login, "");
            set_client_state (CLIENT_MODIFY_SLAVE_LOGIN);
          }
        else if (strcasecmp ("PASSWORD", element_name) == 0)
          {
            openvas_append_string (&modify_slave_data->password, "");
            set_client_state (CLIENT_MODIFY_SLAVE_PASSWORD);
          }
        ELSE_ERROR ("modify_slave");

      case CLIENT_MODIFY_TARGET:
        if (strcasecmp ("COMMENT", element_name) == 0)
          set_client_state (CLIENT_MODIFY_TARGET_COMMENT);
        else if (strcasecmp ("HOSTS", element_name) == 0)
          set_client_state (CLIENT_MODIFY_TARGET_HOSTS);
        else if (strcasecmp ("PORT_LIST", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "id",
                              &modify_target_data->port_list_id);
            set_client_state (CLIENT_MODIFY_TARGET_PORT_LIST);
          }
        else if (strcasecmp ("SSH_LSC_CREDENTIAL", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "id",
                              &modify_target_data->ssh_lsc_credential_id);
            set_client_state (CLIENT_MODIFY_TARGET_SSH_LSC_CREDENTIAL);
          }
        else if (strcasecmp ("SMB_LSC_CREDENTIAL", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "id",
                              &modify_target_data->smb_lsc_credential_id);
            set_client_state (CLIENT_MODIFY_TARGET_SMB_LSC_CREDENTIAL);
          }
        else if (strcasecmp ("NAME", element_name) == 0)
          set_client_state (CLIENT_MODIFY_TARGET_NAME);
        else if (strcasecmp ("TARGET_LOCATOR", element_name) == 0)
          set_client_state (CLIENT_MODIFY_TARGET_TARGET_LOCATOR);
        ELSE_ERROR ("modify_target");

      case CLIENT_MODIFY_TARGET_SSH_LSC_CREDENTIAL:
        if (strcasecmp ("PORT", element_name) == 0)
          set_client_state (CLIENT_MODIFY_TARGET_SSH_LSC_CREDENTIAL_PORT);
        ELSE_ERROR ("modify_target");

      case CLIENT_MODIFY_TARGET_TARGET_LOCATOR:
        if (strcasecmp ("PASSWORD", element_name) == 0)
          set_client_state (CLIENT_MODIFY_TARGET_TARGET_LOCATOR_PASSWORD);
        else if (strcasecmp ("USERNAME", element_name) == 0)
          set_client_state (CLIENT_MODIFY_TARGET_TARGET_LOCATOR_USERNAME);
        ELSE_ERROR ("modify_target");

      case CLIENT_MODIFY_TASK:
        if (strcasecmp ("COMMENT", element_name) == 0)
          {
            openvas_append_string (&modify_task_data->comment, "");
            set_client_state (CLIENT_MODIFY_TASK_COMMENT);
          }
        else if (strcasecmp ("ALERT", element_name) == 0)
          {
            const gchar* attribute;
            if (find_attribute (attribute_names, attribute_values, "id",
                                &attribute))
              array_add (modify_task_data->alerts, g_strdup (attribute));
            set_client_state (CLIENT_MODIFY_TASK_ALERT);
          }
        else if (strcasecmp ("CONFIG", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "id",
                              &modify_task_data->config_id);
            set_client_state (CLIENT_MODIFY_TASK_CONFIG);
          }
        else if (strcasecmp ("NAME", element_name) == 0)
          set_client_state (CLIENT_MODIFY_TASK_NAME);
        else if (strcasecmp ("OBSERVERS", element_name) == 0)
          {
            openvas_append_string (&modify_task_data->observers, "");
            set_client_state (CLIENT_MODIFY_TASK_OBSERVERS);
          }
        else if (strcasecmp ("PREFERENCES", element_name) == 0)
          {
            modify_task_data->preferences = make_array ();
            set_client_state (CLIENT_MODIFY_TASK_PREFERENCES);
          }
        else if (strcasecmp ("RCFILE", element_name) == 0)
          set_client_state (CLIENT_MODIFY_TASK_RCFILE);
        else if (strcasecmp ("SCHEDULE", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "id",
                              &modify_task_data->schedule_id);
            set_client_state (CLIENT_MODIFY_TASK_SCHEDULE);
          }
        else if (strcasecmp ("SLAVE", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "id",
                              &modify_task_data->slave_id);
            set_client_state (CLIENT_MODIFY_TASK_SLAVE);
          }
        else if (strcasecmp ("TARGET", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "id",
                              &modify_task_data->target_id);
            set_client_state (CLIENT_MODIFY_TASK_TARGET);
          }
        else if (strcasecmp ("FILE", element_name) == 0)
          {
            const gchar* attribute;
            append_attribute (attribute_names, attribute_values, "name",
                              &modify_task_data->file_name);
            if (find_attribute (attribute_names, attribute_values,
                                "action", &attribute))
              openvas_append_string (&modify_task_data->action, attribute);
            else
              openvas_append_string (&modify_task_data->action, "update");
            set_client_state (CLIENT_MODIFY_TASK_FILE);
          }
        ELSE_ERROR ("modify_task");

      case CLIENT_MODIFY_TASK_PREFERENCES:
        if (strcasecmp ("PREFERENCE", element_name) == 0)
          {
            assert (modify_task_data->preference == NULL);
            modify_task_data->preference = g_malloc (sizeof (name_value_t));
            modify_task_data->preference->name = NULL;
            modify_task_data->preference->value = NULL;
            set_client_state (CLIENT_MODIFY_TASK_PREFERENCES_PREFERENCE);
          }
        ELSE_ERROR ("modify_task");

      case CLIENT_MODIFY_TASK_PREFERENCES_PREFERENCE:
        if (strcasecmp ("SCANNER_NAME", element_name) == 0)
          set_client_state (CLIENT_MODIFY_TASK_PREFERENCES_PREFERENCE_NAME);
        else if (strcasecmp ("VALUE", element_name) == 0)
          set_client_state (CLIENT_MODIFY_TASK_PREFERENCES_PREFERENCE_VALUE);
        ELSE_ERROR ("modify_task");

      case CLIENT_CREATE_AGENT:
        if (strcasecmp ("COMMENT", element_name) == 0)
          set_client_state (CLIENT_CREATE_AGENT_COMMENT);
        else if (strcasecmp ("COPY", element_name) == 0)
          set_client_state (CLIENT_CREATE_AGENT_COPY);
        else if (strcasecmp ("HOWTO_INSTALL", element_name) == 0)
          set_client_state (CLIENT_CREATE_AGENT_HOWTO_INSTALL);
        else if (strcasecmp ("HOWTO_USE", element_name) == 0)
          set_client_state (CLIENT_CREATE_AGENT_HOWTO_USE);
        else if (strcasecmp ("INSTALLER", element_name) == 0)
          set_client_state (CLIENT_CREATE_AGENT_INSTALLER);
        else if (strcasecmp ("NAME", element_name) == 0)
          {
            openvas_append_string (&create_agent_data->name, "");
            set_client_state (CLIENT_CREATE_AGENT_NAME);
          }
        ELSE_ERROR ("create_agent");
      case CLIENT_CREATE_AGENT_INSTALLER:
        if (strcasecmp ("FILENAME", element_name) == 0)
          set_client_state (CLIENT_CREATE_AGENT_INSTALLER_FILENAME);
        else if (strcasecmp ("SIGNATURE", element_name) == 0)
          set_client_state (CLIENT_CREATE_AGENT_INSTALLER_SIGNATURE);
        ELSE_ERROR ("create_agent");

      case CLIENT_CREATE_CONFIG:
        if (strcasecmp ("COMMENT", element_name) == 0)
          set_client_state (CLIENT_CREATE_CONFIG_COMMENT);
        else if (strcasecmp ("COPY", element_name) == 0)
          set_client_state (CLIENT_CREATE_CONFIG_COPY);
        else if (strcasecmp ("GET_CONFIGS_RESPONSE", element_name) == 0)
          {
            omp_parser->importing = 1;
            import_config_data->import = 1;
            set_client_state (CLIENT_C_C_GCR);
          }
        else if (strcasecmp ("NAME", element_name) == 0)
          set_client_state (CLIENT_CREATE_CONFIG_NAME);
        else if (strcasecmp ("RCFILE", element_name) == 0)
          set_client_state (CLIENT_CREATE_CONFIG_RCFILE);
        ELSE_ERROR ("create_config");

      case CLIENT_C_C_GCR:
        if (strcasecmp ("CONFIG", element_name) == 0)
          {
            /* Reset here in case there was a previous config element. */
            create_config_data_reset (create_config_data);
            import_config_data->import = 1;
            set_client_state (CLIENT_C_C_GCR_CONFIG);
          }
        ELSE_ERROR ("create_config");

      case CLIENT_C_C_GCR_CONFIG:
        if (strcasecmp ("COMMENT", element_name) == 0)
          set_client_state (CLIENT_C_C_GCR_CONFIG_COMMENT);
        else if (strcasecmp ("NAME", element_name) == 0)
          set_client_state (CLIENT_C_C_GCR_CONFIG_NAME);
        else if (strcasecmp ("NVT_SELECTORS", element_name) == 0)
          {
            /* Reset array, in case there was a previous nvt_selectors element. */
            array_reset (&import_config_data->nvt_selectors);
            set_client_state (CLIENT_C_C_GCR_CONFIG_NVT_SELECTORS);
          }
        else if (strcasecmp ("PREFERENCES", element_name) == 0)
          {
            /* Reset array, in case there was a previous preferences element. */
            array_reset (&import_config_data->preferences);
            set_client_state (CLIENT_C_C_GCR_CONFIG_PREFERENCES);
          }
        ELSE_ERROR ("create_config");

      case CLIENT_C_C_GCR_CONFIG_NVT_SELECTORS:
        if (strcasecmp ("NVT_SELECTOR", element_name) == 0)
          set_client_state (CLIENT_C_C_GCR_CONFIG_NVT_SELECTORS_NVT_SELECTOR);
        ELSE_ERROR ("create_config");

      case CLIENT_C_C_GCR_CONFIG_NVT_SELECTORS_NVT_SELECTOR:
        if (strcasecmp ("INCLUDE", element_name) == 0)
          set_client_state
           (CLIENT_C_C_GCR_CONFIG_NVT_SELECTORS_NVT_SELECTOR_INCLUDE);
        else if (strcasecmp ("NAME", element_name) == 0)
          set_client_state
           (CLIENT_C_C_GCR_CONFIG_NVT_SELECTORS_NVT_SELECTOR_NAME);
        else if (strcasecmp ("TYPE", element_name) == 0)
          set_client_state
           (CLIENT_C_C_GCR_CONFIG_NVT_SELECTORS_NVT_SELECTOR_TYPE);
        else if (strcasecmp ("FAMILY_OR_NVT", element_name) == 0)
          set_client_state
           (CLIENT_C_C_GCR_CONFIG_NVT_SELECTORS_NVT_SELECTOR_FAMILY_OR_NVT);
        ELSE_ERROR ("create_config");

      case CLIENT_C_C_GCR_CONFIG_PREFERENCES:
        if (strcasecmp ("PREFERENCE", element_name) == 0)
          {
            array_reset (&import_config_data->preference_alts);
            set_client_state (CLIENT_C_C_GCR_CONFIG_PREFERENCES_PREFERENCE);
          }
        ELSE_ERROR ("create_config");

      case CLIENT_C_C_GCR_CONFIG_PREFERENCES_PREFERENCE:
        if (strcasecmp ("ALT", element_name) == 0)
          set_client_state
           (CLIENT_C_C_GCR_CONFIG_PREFERENCES_PREFERENCE_ALT);
        else if (strcasecmp ("NAME", element_name) == 0)
          set_client_state
           (CLIENT_C_C_GCR_CONFIG_PREFERENCES_PREFERENCE_NAME);
        else if (strcasecmp ("NVT", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "oid",
                              &import_config_data->preference_nvt_oid);
            set_client_state
             (CLIENT_C_C_GCR_CONFIG_PREFERENCES_PREFERENCE_NVT);
          }
        else if (strcasecmp ("TYPE", element_name) == 0)
          set_client_state
           (CLIENT_C_C_GCR_CONFIG_PREFERENCES_PREFERENCE_TYPE);
        else if (strcasecmp ("VALUE", element_name) == 0)
          set_client_state
           (CLIENT_C_C_GCR_CONFIG_PREFERENCES_PREFERENCE_VALUE);
        ELSE_ERROR ("create_config");

      case CLIENT_C_C_GCR_CONFIG_PREFERENCES_PREFERENCE_NVT:
        if (strcasecmp ("NAME", element_name) == 0)
          set_client_state
           (CLIENT_C_C_GCR_CONFIG_PREFERENCES_PREFERENCE_NVT_NAME);
        ELSE_ERROR ("create_config");

      case CLIENT_CREATE_ALERT:
        if (strcasecmp ("COMMENT", element_name) == 0)
          set_client_state (CLIENT_CREATE_ALERT_COMMENT);
        else if (strcasecmp ("COPY", element_name) == 0)
          set_client_state (CLIENT_CREATE_ALERT_COPY);
        else if (strcasecmp ("CONDITION", element_name) == 0)
          set_client_state (CLIENT_CREATE_ALERT_CONDITION);
        else if (strcasecmp ("EVENT", element_name) == 0)
          set_client_state (CLIENT_CREATE_ALERT_EVENT);
        else if (strcasecmp ("FILTER", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "id",
                              &create_alert_data->filter_id);
            set_client_state (CLIENT_CREATE_ALERT_FILTER);
          }
        else if (strcasecmp ("METHOD", element_name) == 0)
          set_client_state (CLIENT_CREATE_ALERT_METHOD);
        else if (strcasecmp ("NAME", element_name) == 0)
          set_client_state (CLIENT_CREATE_ALERT_NAME);
        ELSE_ERROR ("create_alert");

      case CLIENT_CREATE_ALERT_CONDITION:
        if (strcasecmp ("DATA", element_name) == 0)
          set_client_state (CLIENT_CREATE_ALERT_CONDITION_DATA);
        ELSE_ERROR ("create_alert");

      case CLIENT_CREATE_ALERT_CONDITION_DATA:
        if (strcasecmp ("NAME", element_name) == 0)
          set_client_state (CLIENT_CREATE_ALERT_CONDITION_DATA_NAME);
        ELSE_ERROR ("create_alert");

      case CLIENT_CREATE_ALERT_EVENT:
        if (strcasecmp ("DATA", element_name) == 0)
          set_client_state (CLIENT_CREATE_ALERT_EVENT_DATA);
        ELSE_ERROR ("create_alert");

      case CLIENT_CREATE_ALERT_EVENT_DATA:
        if (strcasecmp ("NAME", element_name) == 0)
          set_client_state (CLIENT_CREATE_ALERT_EVENT_DATA_NAME);
        ELSE_ERROR ("create_alert");

      case CLIENT_CREATE_ALERT_METHOD:
        if (strcasecmp ("DATA", element_name) == 0)
          set_client_state (CLIENT_CREATE_ALERT_METHOD_DATA);
        ELSE_ERROR ("create_alert");

      case CLIENT_CREATE_ALERT_METHOD_DATA:
        if (strcasecmp ("NAME", element_name) == 0)
          set_client_state (CLIENT_CREATE_ALERT_METHOD_DATA_NAME);
        ELSE_ERROR ("create_alert");

      case CLIENT_CREATE_FILTER:
        if (strcasecmp ("COMMENT", element_name) == 0)
          set_client_state (CLIENT_CREATE_FILTER_COMMENT);
        else if (strcasecmp ("COPY", element_name) == 0)
          set_client_state (CLIENT_CREATE_FILTER_COPY);
        else if (strcasecmp ("NAME", element_name) == 0)
          {
            openvas_append_string (&create_filter_data->name, "");
            set_client_state (CLIENT_CREATE_FILTER_NAME);
          }
        else if (strcasecmp ("TERM", element_name) == 0)
          set_client_state (CLIENT_CREATE_FILTER_TERM);
        else if (strcasecmp ("TYPE", element_name) == 0)
          set_client_state (CLIENT_CREATE_FILTER_TYPE);
        ELSE_ERROR ("create_filter");

      case CLIENT_CREATE_FILTER_NAME:
        if (strcasecmp ("MAKE_UNIQUE", element_name) == 0)
          set_client_state (CLIENT_CREATE_FILTER_NAME_MAKE_UNIQUE);
        ELSE_ERROR ("create_filter");

      case CLIENT_CREATE_LSC_CREDENTIAL:
        if (strcasecmp ("COMMENT", element_name) == 0)
          set_client_state (CLIENT_CREATE_LSC_CREDENTIAL_COMMENT);
        else if (strcasecmp ("KEY", element_name) == 0)
          {
            create_lsc_credential_data->key = 1;
            set_client_state (CLIENT_CREATE_LSC_CREDENTIAL_KEY);
          }
        else if (strcasecmp ("LOGIN", element_name) == 0)
          set_client_state (CLIENT_CREATE_LSC_CREDENTIAL_LOGIN);
        else if (strcasecmp ("COPY", element_name) == 0)
          set_client_state (CLIENT_CREATE_LSC_CREDENTIAL_COPY);
        else if (strcasecmp ("NAME", element_name) == 0)
          set_client_state (CLIENT_CREATE_LSC_CREDENTIAL_NAME);
        else if (strcasecmp ("PASSWORD", element_name) == 0)
          {
            openvas_append_string (&create_lsc_credential_data->password, "");
            set_client_state (CLIENT_CREATE_LSC_CREDENTIAL_PASSWORD);
          }
        ELSE_ERROR ("create_lsc_credential");

      case CLIENT_CREATE_LSC_CREDENTIAL_KEY:
        if (strcasecmp ("PHRASE", element_name) == 0)
          {
            openvas_append_string (&create_lsc_credential_data->key_phrase, "");
            set_client_state (CLIENT_CREATE_LSC_CREDENTIAL_KEY_PHRASE);
          }
        else if (strcasecmp ("PRIVATE", element_name) == 0)
          set_client_state (CLIENT_CREATE_LSC_CREDENTIAL_KEY_PRIVATE);
        else if (strcasecmp ("PUBLIC", element_name) == 0)
          set_client_state (CLIENT_CREATE_LSC_CREDENTIAL_KEY_PUBLIC);
        ELSE_ERROR ("create_lsc_credential");

      case CLIENT_CREATE_NOTE:
        if (strcasecmp ("ACTIVE", element_name) == 0)
          set_client_state (CLIENT_CREATE_NOTE_ACTIVE);
        else if (strcasecmp ("COPY", element_name) == 0)
          set_client_state (CLIENT_CREATE_NOTE_COPY);
        else if (strcasecmp ("HOSTS", element_name) == 0)
          set_client_state (CLIENT_CREATE_NOTE_HOSTS);
        else if (strcasecmp ("NVT", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "oid",
                              &create_note_data->nvt_oid);
            set_client_state (CLIENT_CREATE_NOTE_NVT);
          }
        else if (strcasecmp ("PORT", element_name) == 0)
          set_client_state (CLIENT_CREATE_NOTE_PORT);
        else if (strcasecmp ("RESULT", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "id",
                              &create_note_data->result_id);
            if (create_note_data->result_id
                && create_note_data->result_id[0] == '\0')
              {
                g_free (create_note_data->result_id);
                create_note_data->result_id = NULL;
              }
            set_client_state (CLIENT_CREATE_NOTE_RESULT);
          }
        else if (strcasecmp ("TASK", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "id",
                              &create_note_data->task_id);
            if (create_note_data->task_id
                && create_note_data->task_id[0] == '\0')
              {
                g_free (create_note_data->task_id);
                create_note_data->task_id = NULL;
              }
            set_client_state (CLIENT_CREATE_NOTE_TASK);
          }
        else if (strcasecmp ("TEXT", element_name) == 0)
          set_client_state (CLIENT_CREATE_NOTE_TEXT);
        else if (strcasecmp ("THREAT", element_name) == 0)
          set_client_state (CLIENT_CREATE_NOTE_THREAT);
        ELSE_ERROR ("create_note");

      case CLIENT_CREATE_PORT_LIST:
        if (strcasecmp ("COMMENT", element_name) == 0)
          set_client_state (CLIENT_CREATE_PORT_LIST_COMMENT);
        else if (strcasecmp ("COPY", element_name) == 0)
          set_client_state (CLIENT_CREATE_PORT_LIST_COPY);
        else if (strcasecmp ("GET_PORT_LISTS_RESPONSE", element_name) == 0)
          {
            omp_parser->importing = 1;
            create_port_list_data->import = 1;
            set_client_state (CLIENT_CPL_GPLR);
          }
        else if (strcasecmp ("PORT_RANGE", element_name) == 0)
          {
            openvas_append_string (&create_port_list_data->port_range, "");
            set_client_state (CLIENT_CREATE_PORT_LIST_PORT_RANGE);
          }
        else if (strcasecmp ("NAME", element_name) == 0)
          set_client_state (CLIENT_CREATE_PORT_LIST_NAME);
        ELSE_ERROR ("create_port_list");

      case CLIENT_CPL_GPLR:
        if (strcasecmp ("PORT_LIST", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "id",
                              &create_port_list_data->id);
            set_client_state (CLIENT_CPL_GPLR_PORT_LIST);
          }
        ELSE_ERROR ("create_port_list");

      case CLIENT_CPL_GPLR_PORT_LIST:
        if (strcasecmp ("COMMENT", element_name) == 0)
          set_client_state (CLIENT_CPL_GPLR_PORT_LIST_COMMENT);
        else if (strcasecmp ("IN_USE", element_name) == 0)
          set_client_state (CLIENT_CPL_GPLR_PORT_LIST_IN_USE);
        else if (strcasecmp ("NAME", element_name) == 0)
          set_client_state (CLIENT_CPL_GPLR_PORT_LIST_NAME);
        else if (strcasecmp ("PORT_RANGE", element_name) == 0)
          set_client_state (CLIENT_CPL_GPLR_PORT_LIST_PORT_RANGE);
        else if (strcasecmp ("PORT_RANGES", element_name) == 0)
          {
            create_port_list_data->ranges = make_array ();
            set_client_state (CLIENT_CPL_GPLR_PORT_LIST_PORT_RANGES);
          }
        else if (strcasecmp ("TARGETS", element_name) == 0)
          {
            omp_parser->read_over = 1;
            set_client_state (CLIENT_CPL_GPLR_PORT_LIST_TARGETS);
          }
        ELSE_ERROR ("create_port_list");

      case CLIENT_CPL_GPLR_PORT_LIST_PORT_RANGES:
        if (strcasecmp ("PORT_RANGE", element_name) == 0)
          {
            assert (create_port_list_data->range == NULL);
            create_port_list_data->range
             = g_malloc0 (sizeof (create_port_list_range_t));
            append_attribute (attribute_names, attribute_values, "id",
                              &(create_port_list_data->range->id));
            set_client_state (CLIENT_CPL_GPLR_PORT_LIST_PORT_RANGES_PORT_RANGE);
          }
        ELSE_ERROR ("create_port_list");

      case CLIENT_CPL_GPLR_PORT_LIST_PORT_RANGES_PORT_RANGE:
        if (strcasecmp ("COMMENT", element_name) == 0)
          {
            openvas_append_string (&create_port_list_data->range->comment, "");
            set_client_state (CLIENT_CPL_GPLR_PORT_LIST_PORT_RANGES_PORT_RANGE_COMMENT);
          }
        else if (strcasecmp ("END", element_name) == 0)
          {
            openvas_append_string (&create_port_list_data->range->end, "");
            set_client_state (CLIENT_CPL_GPLR_PORT_LIST_PORT_RANGES_PORT_RANGE_END);
          }
        else if (strcasecmp ("START", element_name) == 0)
          {
            openvas_append_string (&create_port_list_data->range->start, "");
            set_client_state (CLIENT_CPL_GPLR_PORT_LIST_PORT_RANGES_PORT_RANGE_START);
          }
        else if (strcasecmp ("TYPE", element_name) == 0)
          {
            openvas_append_string (&create_port_list_data->range->type, "");
            set_client_state (CLIENT_CPL_GPLR_PORT_LIST_PORT_RANGES_PORT_RANGE_TYPE);
          }
        ELSE_ERROR ("create_port_list");

      case CLIENT_CREATE_PORT_RANGE:
        if (strcasecmp ("COMMENT", element_name) == 0)
          set_client_state (CLIENT_CREATE_PORT_RANGE_COMMENT);
        else if (strcasecmp ("END", element_name) == 0)
          set_client_state (CLIENT_CREATE_PORT_RANGE_END);
        else if (strcasecmp ("PORT_LIST", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "id",
                              &create_port_range_data->port_list_id);
            set_client_state (CLIENT_CREATE_PORT_RANGE_PORT_LIST);
          }
        else if (strcasecmp ("START", element_name) == 0)
          set_client_state (CLIENT_CREATE_PORT_RANGE_START);
        else if (strcasecmp ("TYPE", element_name) == 0)
          set_client_state (CLIENT_CREATE_PORT_RANGE_TYPE);
        ELSE_ERROR ("create_port_range");

      case CLIENT_CREATE_REPORT:
        if (strcasecmp ("REPORT", element_name) == 0)
          {
            const gchar* attribute;

            omp_parser->importing = 1;

            append_attribute (attribute_names, attribute_values,
                              "type", &create_report_data->type);

            if (find_attribute (attribute_names, attribute_values, "format_id",
                                &attribute))
              {
                /* Assume this is the wrapper REPORT. */
                create_report_data->wrapper = 1;
                set_client_state (CLIENT_CREATE_REPORT_REPORT);
              }
            else
              {
                /* Assume the report is immediately inside the CREATE_REPORT. */
                create_report_data->wrapper = 0;
                create_report_data->details = make_array ();
                create_report_data->host_ends = make_array ();
                create_report_data->host_starts = make_array ();
                create_report_data->results = make_array ();
                set_client_state (CLIENT_CREATE_REPORT_RR);
              }
          }
        else if (strcasecmp ("TASK", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "id",
                              &create_report_data->task_id);
            set_client_state (CLIENT_CREATE_REPORT_TASK);
          }
        ELSE_ERROR ("create_report");

      case CLIENT_CREATE_REPORT_REPORT:
        if (strcasecmp ("REPORT", element_name) == 0)
          {
            create_report_data->details = make_array ();
            create_report_data->host_ends = make_array ();
            create_report_data->host_starts = make_array ();
            create_report_data->results = make_array ();
            set_client_state (CLIENT_CREATE_REPORT_RR);
          }
        ELSE_ERROR ("create_report");

      case CLIENT_CREATE_REPORT_RR:
        if (strcasecmp ("FILTERS", element_name) == 0)
          {
            omp_parser->read_over = 1;
            set_client_state (CLIENT_CREATE_REPORT_RR_FILTERS);
          }
        else if (strcasecmp ("HOST", element_name) == 0)
          {
            set_client_state (CLIENT_CREATE_REPORT_RR_H);
          }
        else if (strcasecmp ("HOST_COUNT", element_name) == 0)
          {
            omp_parser->read_over = 1;
            set_client_state (CLIENT_CREATE_REPORT_RR_HOST_COUNT);
          }
        else if (strcasecmp ("HOST_END", element_name) == 0)
          set_client_state (CLIENT_CREATE_REPORT_RR_HOST_END);
        else if (strcasecmp ("HOST_START", element_name) == 0)
          set_client_state (CLIENT_CREATE_REPORT_RR_HOST_START);
        else if (strcasecmp ("HOSTS", element_name) == 0)
          {
            omp_parser->read_over = 1;
            set_client_state (CLIENT_CREATE_REPORT_RR_HOSTS);
          }
        else if (strcasecmp ("PORTS", element_name) == 0)
          {
            omp_parser->read_over = 1;
            set_client_state (CLIENT_CREATE_REPORT_RR_PORTS);
          }
        else if (strcasecmp ("REPORT_FORMAT", element_name) == 0)
          {
            omp_parser->read_over = 1;
            set_client_state (CLIENT_CREATE_REPORT_RR_REPORT_FORMAT);
          }
        else if (strcasecmp ("RESULTS", element_name) == 0)
          set_client_state (CLIENT_CREATE_REPORT_RR_RESULTS);
        else if (strcasecmp ("RESULT_COUNT", element_name) == 0)
          {
            omp_parser->read_over = 1;
            set_client_state (CLIENT_CREATE_REPORT_RR_RESULT_COUNT);
          }
        else if (strcasecmp ("SCAN_RUN_STATUS", element_name) == 0)
          {
            omp_parser->read_over = 1;
            set_client_state
             (CLIENT_CREATE_REPORT_RR_SCAN_RUN_STATUS);
          }
        else if (strcasecmp ("SCAN_END", element_name) == 0)
          {
            set_client_state (CLIENT_CREATE_REPORT_RR_SCAN_END);
          }
        else if (strcasecmp ("SCAN_START", element_name) == 0)
          {
            set_client_state (CLIENT_CREATE_REPORT_RR_SCAN_START);
          }
        else if (strcasecmp ("SORT", element_name) == 0)
          {
            omp_parser->read_over = 1;
            set_client_state (CLIENT_CREATE_REPORT_RR_SORT);
          }
        else if (strcasecmp ("TASK", element_name) == 0)
          {
            omp_parser->read_over = 1;
            set_client_state (CLIENT_CREATE_REPORT_RR_TASK);
          }
        ELSE_ERROR ("create_report");

      case CLIENT_CREATE_REPORT_RR_HOST_END:
        if (strcasecmp ("HOST", element_name) == 0)
          set_client_state (CLIENT_CREATE_REPORT_RR_HOST_END_HOST);
        ELSE_ERROR ("create_report");

      case CLIENT_CREATE_REPORT_RR_HOST_START:
        if (strcasecmp ("HOST", element_name) == 0)
          set_client_state (CLIENT_CREATE_REPORT_RR_HOST_START_HOST);
        ELSE_ERROR ("create_report");

      case CLIENT_CREATE_REPORT_RR_H:
        if (strcasecmp ("IP", element_name) == 0)
          {
            set_client_state (CLIENT_CREATE_REPORT_RR_H_IP);
          }
        else if (strcasecmp ("DETAIL", element_name) == 0)
          {
            set_client_state (CLIENT_CREATE_REPORT_RR_H_DETAIL);
          }
        else if (strcasecmp ("END", element_name) == 0)
          {
            set_client_state (CLIENT_CREATE_REPORT_RR_H_END);
          }
        else if (strcasecmp ("START", element_name) == 0)
          {
            set_client_state (CLIENT_CREATE_REPORT_RR_H_START);
          }
        ELSE_ERROR ("create_report");

      case CLIENT_CREATE_REPORT_RR_H_DETAIL:
        if (strcasecmp ("NAME", element_name) == 0)
          {
            set_client_state (CLIENT_CREATE_REPORT_RR_H_DETAIL_NAME);
          }
        else if (strcasecmp ("VALUE", element_name) == 0)
          {
            set_client_state (CLIENT_CREATE_REPORT_RR_H_DETAIL_VALUE);
          }
        else if (strcasecmp ("SOURCE", element_name) == 0)
          {
            set_client_state (CLIENT_CREATE_REPORT_RR_H_DETAIL_SOURCE);
          }
        ELSE_ERROR ("create_report");

      case CLIENT_CREATE_REPORT_RR_H_DETAIL_SOURCE:
        if (strcasecmp ("DESCRIPTION", element_name) == 0)
          {
            set_client_state (CLIENT_CREATE_REPORT_RR_H_DETAIL_SOURCE_DESC);
          }
        else if (strcasecmp ("NAME", element_name) == 0)
          {
            set_client_state (CLIENT_CREATE_REPORT_RR_H_DETAIL_SOURCE_NAME);
          }
        else if (strcasecmp ("TYPE", element_name) == 0)
          {
            set_client_state (CLIENT_CREATE_REPORT_RR_H_DETAIL_SOURCE_TYPE);
          }
        ELSE_ERROR ("create_report");

      case CLIENT_CREATE_REPORT_RR_RESULTS:
        if (strcasecmp ("RESULT", element_name) == 0)
          set_client_state
           (CLIENT_CREATE_REPORT_RR_RESULTS_RESULT);
        ELSE_ERROR ("create_report");

      case CLIENT_CREATE_REPORT_RR_RESULTS_RESULT:
        if (strcasecmp ("DESCRIPTION", element_name) == 0)
          set_client_state
           (CLIENT_CREATE_REPORT_RR_RESULTS_RESULT_DESCRIPTION);
        else if (strcasecmp ("DETECTION", element_name) == 0)
          {
            omp_parser->read_over = 1;
            set_client_state (CLIENT_CREATE_REPORT_RR_RESULTS_RESULT_DETECTION);
          }
        else if (strcasecmp ("HOST", element_name) == 0)
          set_client_state (CLIENT_CREATE_REPORT_RR_RESULTS_RESULT_HOST);
        else if (strcasecmp ("NOTES", element_name) == 0)
          {
            omp_parser->read_over = 1;
            set_client_state (CLIENT_CREATE_REPORT_RR_RESULTS_RESULT_NOTES);
          }
        else if (strcasecmp ("NVT", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "oid",
                              &create_report_data->result_nvt_oid);
            set_client_state (CLIENT_CREATE_REPORT_RR_RESULTS_RESULT_NVT);
          }
        else if (strcasecmp ("ORIGINAL_THREAT", element_name) == 0)
          set_client_state
           (CLIENT_CREATE_REPORT_RR_RESULTS_RESULT_ORIGINAL_THREAT);
        else if (strcasecmp ("OVERRIDES", element_name) == 0)
          {
            omp_parser->read_over = 1;
            set_client_state (CLIENT_CREATE_REPORT_RR_RESULTS_RESULT_OVERRIDES);
          }
        else if (strcasecmp ("PORT", element_name) == 0)
          set_client_state (CLIENT_CREATE_REPORT_RR_RESULTS_RESULT_PORT);
        else if (strcasecmp ("SUBNET", element_name) == 0)
          set_client_state (CLIENT_CREATE_REPORT_RR_RESULTS_RESULT_SUBNET);
        else if (strcasecmp ("THREAT", element_name) == 0)
          set_client_state (CLIENT_CREATE_REPORT_RR_RESULTS_RESULT_THREAT);
        ELSE_ERROR ("create_report");

      case CLIENT_CREATE_REPORT_RR_RESULTS_RESULT_NVT:
        if (strcasecmp ("BID", element_name) == 0)
          set_client_state (CLIENT_CREATE_REPORT_RR_RESULTS_RESULT_NVT_BID);
        else if (strcasecmp ("CVE", element_name) == 0)
          set_client_state (CLIENT_CREATE_REPORT_RR_RESULTS_RESULT_NVT_CVE);
        else if (strcasecmp ("CVSS_BASE", element_name) == 0)
          set_client_state
           (CLIENT_CREATE_REPORT_RR_RESULTS_RESULT_NVT_CVSS_BASE);
        else if (strcasecmp ("FAMILY", element_name) == 0)
          set_client_state (CLIENT_CREATE_REPORT_RR_RESULTS_RESULT_NVT_FAMILY);
        else if (strcasecmp ("NAME", element_name) == 0)
          set_client_state (CLIENT_CREATE_REPORT_RR_RESULTS_RESULT_NVT_NAME);
        else if (strcasecmp ("RISK_FACTOR", element_name) == 0)
          set_client_state
           (CLIENT_CREATE_REPORT_RR_RESULTS_RESULT_NVT_RISK_FACTOR);
        else if (strcasecmp ("XREF", element_name) == 0)
          set_client_state (CLIENT_CREATE_REPORT_RR_RESULTS_RESULT_NVT_XREF);
        else if (strcasecmp ("CERT", element_name) == 0)
          set_client_state (CLIENT_CREATE_REPORT_RR_RESULTS_RESULT_NVT_CERT);
        ELSE_ERROR ("create_report");

      case (CLIENT_CREATE_REPORT_RR_RESULTS_RESULT_NVT_CERT):
        if (strcasecmp ("CERT_REF", element_name) == 0)
          set_client_state
              (CLIENT_CREATE_REPORT_RR_RESULTS_RESULT_NVT_CERT_CERT_REF);
        ELSE_ERROR ("create_report");

      case CLIENT_CREATE_REPORT_TASK:
        if (strcasecmp ("COMMENT", element_name) == 0)
          set_client_state (CLIENT_CREATE_REPORT_TASK_COMMENT);
        else if (strcasecmp ("NAME", element_name) == 0)
          set_client_state (CLIENT_CREATE_REPORT_TASK_NAME);
        ELSE_ERROR ("create_report");

      case CLIENT_CREATE_REPORT_FORMAT:
        if (strcasecmp ("GET_REPORT_FORMATS_RESPONSE", element_name) == 0)
          {
            omp_parser->importing = 1;
            create_report_format_data->import = 1;
            set_client_state (CLIENT_CRF_GRFR);
          }
        else if (strcasecmp ("COPY", element_name) == 0)
          set_client_state (CLIENT_CREATE_REPORT_FORMAT_COPY);
        ELSE_ERROR ("create_report_format");

      case CLIENT_CRF_GRFR:
        if (strcasecmp ("REPORT_FORMAT", element_name) == 0)
          {
            create_report_format_data->files = make_array ();
            create_report_format_data->params = make_array ();
            create_report_format_data->params_options = make_array ();
            append_attribute (attribute_names, attribute_values, "id",
                              &create_report_format_data->id);
            set_client_state (CLIENT_CRF_GRFR_REPORT_FORMAT);
          }
        ELSE_ERROR ("create_report_format");

      case CLIENT_CRF_GRFR_REPORT_FORMAT:
        if (strcasecmp ("CONTENT_TYPE", element_name) == 0)
          set_client_state (CLIENT_CRF_GRFR_REPORT_FORMAT_CONTENT_TYPE);
        else if (strcasecmp ("DESCRIPTION", element_name) == 0)
          set_client_state (CLIENT_CRF_GRFR_REPORT_FORMAT_DESCRIPTION);
        else if (strcasecmp ("EXTENSION", element_name) == 0)
          set_client_state (CLIENT_CRF_GRFR_REPORT_FORMAT_EXTENSION);
        else if (strcasecmp ("GLOBAL", element_name) == 0)
          set_client_state (CLIENT_CRF_GRFR_REPORT_FORMAT_GLOBAL);
        else if (strcasecmp ("FILE", element_name) == 0)
          {
            assert (create_report_format_data->file == NULL);
            assert (create_report_format_data->file_name == NULL);
            openvas_append_string (&create_report_format_data->file, "");
            append_attribute (attribute_names, attribute_values, "name",
                              &create_report_format_data->file_name);
            set_client_state (CLIENT_CRF_GRFR_REPORT_FORMAT_FILE);
          }
        else if (strcasecmp ("NAME", element_name) == 0)
          set_client_state (CLIENT_CRF_GRFR_REPORT_FORMAT_NAME);
        else if (strcasecmp ("PARAM", element_name) == 0)
          {
            assert (create_report_format_data->param_name == NULL);
            assert (create_report_format_data->param_type == NULL);
            assert (create_report_format_data->param_value == NULL);
            openvas_append_string (&create_report_format_data->param_name, "");
            openvas_append_string (&create_report_format_data->param_value, "");
            create_report_format_data->param_options = make_array ();
            set_client_state (CLIENT_CRF_GRFR_REPORT_FORMAT_PARAM);
          }
        else if (strcasecmp ("PREDEFINED", element_name) == 0)
          set_client_state (CLIENT_CRF_GRFR_REPORT_FORMAT_PREDEFINED);
        else if (strcasecmp ("SIGNATURE", element_name) == 0)
          set_client_state (CLIENT_CRF_GRFR_REPORT_FORMAT_SIGNATURE);
        else if (strcasecmp ("SUMMARY", element_name) == 0)
          set_client_state (CLIENT_CRF_GRFR_REPORT_FORMAT_SUMMARY);
        else if (strcasecmp ("TRUST", element_name) == 0)
          set_client_state (CLIENT_CRF_GRFR_REPORT_FORMAT_TRUST);
        ELSE_ERROR ("create_report_format");

      case CLIENT_CRF_GRFR_REPORT_FORMAT_PARAM:
        if (strcasecmp ("DEFAULT", element_name) == 0)
          {
            openvas_append_string (&create_report_format_data->param_default,
                                   "");
            set_client_state (CLIENT_CRF_GRFR_REPORT_FORMAT_PARAM_DEFAULT);
          }
        else if (strcasecmp ("NAME", element_name) == 0)
          set_client_state (CLIENT_CRF_GRFR_REPORT_FORMAT_PARAM_NAME);
        else if (strcasecmp ("OPTIONS", element_name) == 0)
          set_client_state (CLIENT_CRF_GRFR_REPORT_FORMAT_PARAM_OPTIONS);
        else if (strcasecmp ("TYPE", element_name) == 0)
          {
            openvas_append_string (&create_report_format_data->param_type, "");
            set_client_state (CLIENT_CRF_GRFR_REPORT_FORMAT_PARAM_TYPE);
          }
        else if (strcasecmp ("VALUE", element_name) == 0)
          set_client_state (CLIENT_CRF_GRFR_REPORT_FORMAT_PARAM_VALUE);
        ELSE_ERROR ("create_report_format");

      case CLIENT_CRF_GRFR_REPORT_FORMAT_PARAM_OPTIONS:
        if (strcasecmp ("OPTION", element_name) == 0)
          {
            openvas_append_string (&create_report_format_data->param_option,
                                   "");
            set_client_state
             (CLIENT_CRF_GRFR_REPORT_FORMAT_PARAM_OPTIONS_OPTION);
          }
        ELSE_ERROR ("create_report_format");

      case CLIENT_CRF_GRFR_REPORT_FORMAT_PARAM_TYPE:
        if (strcasecmp ("MAX", element_name) == 0)
          {
            set_client_state
             (CLIENT_CRF_GRFR_REPORT_FORMAT_PARAM_TYPE_MAX);
          }
        else if (strcasecmp ("MIN", element_name) == 0)
          {
            set_client_state
             (CLIENT_CRF_GRFR_REPORT_FORMAT_PARAM_TYPE_MIN);
          }
        ELSE_ERROR ("create_report_format");

      case CLIENT_CREATE_OVERRIDE:
        if (strcasecmp ("ACTIVE", element_name) == 0)
          set_client_state (CLIENT_CREATE_OVERRIDE_ACTIVE);
        else if (strcasecmp ("COPY", element_name) == 0)
          set_client_state (CLIENT_CREATE_OVERRIDE_COPY);
        else if (strcasecmp ("HOSTS", element_name) == 0)
          set_client_state (CLIENT_CREATE_OVERRIDE_HOSTS);
        else if (strcasecmp ("NEW_THREAT", element_name) == 0)
          set_client_state (CLIENT_CREATE_OVERRIDE_NEW_THREAT);
        else if (strcasecmp ("NVT", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "oid",
                              &create_override_data->nvt_oid);
            set_client_state (CLIENT_CREATE_OVERRIDE_NVT);
          }
        else if (strcasecmp ("PORT", element_name) == 0)
          set_client_state (CLIENT_CREATE_OVERRIDE_PORT);
        else if (strcasecmp ("RESULT", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "id",
                              &create_override_data->result_id);
            if (create_override_data->result_id
                && create_override_data->result_id[0] == '\0')
              {
                g_free (create_override_data->result_id);
                create_override_data->result_id = NULL;
              }
            set_client_state (CLIENT_CREATE_OVERRIDE_RESULT);
          }
        else if (strcasecmp ("TASK", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "id",
                              &create_override_data->task_id);
            if (create_override_data->task_id
                && create_override_data->task_id[0] == '\0')
              {
                g_free (create_override_data->task_id);
                create_override_data->task_id = NULL;
              }
            set_client_state (CLIENT_CREATE_OVERRIDE_TASK);
          }
        else if (strcasecmp ("TEXT", element_name) == 0)
          set_client_state (CLIENT_CREATE_OVERRIDE_TEXT);
        else if (strcasecmp ("THREAT", element_name) == 0)
          set_client_state (CLIENT_CREATE_OVERRIDE_THREAT);
        ELSE_ERROR ("create_override");

      case CLIENT_CREATE_SLAVE:
        if (strcasecmp ("COMMENT", element_name) == 0)
          set_client_state (CLIENT_CREATE_SLAVE_COMMENT);
        else if (strcasecmp ("COPY", element_name) == 0)
          set_client_state (CLIENT_CREATE_SLAVE_COPY);
        else if (strcasecmp ("HOST", element_name) == 0)
          set_client_state (CLIENT_CREATE_SLAVE_HOST);
        else if (strcasecmp ("LOGIN", element_name) == 0)
          set_client_state (CLIENT_CREATE_SLAVE_LOGIN);
        else if (strcasecmp ("NAME", element_name) == 0)
          set_client_state (CLIENT_CREATE_SLAVE_NAME);
        else if (strcasecmp ("PASSWORD", element_name) == 0)
          set_client_state (CLIENT_CREATE_SLAVE_PASSWORD);
        else if (strcasecmp ("PORT", element_name) == 0)
          set_client_state (CLIENT_CREATE_SLAVE_PORT);
        ELSE_ERROR ("create_slave");

      case CLIENT_CREATE_TARGET:
        if (strcasecmp ("COMMENT", element_name) == 0)
          set_client_state (CLIENT_CREATE_TARGET_COMMENT);
        else if (strcasecmp ("COPY", element_name) == 0)
          set_client_state (CLIENT_CREATE_TARGET_COPY);
        else if (strcasecmp ("HOSTS", element_name) == 0)
          set_client_state (CLIENT_CREATE_TARGET_HOSTS);
        else if (strcasecmp ("PORT_LIST", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "id",
                              &create_target_data->port_list_id);
            set_client_state (CLIENT_CREATE_TARGET_PORT_LIST);
          }
        else if (strcasecmp ("PORT_RANGE", element_name) == 0)
          {
            openvas_append_string (&create_target_data->port_range, "");
            set_client_state (CLIENT_CREATE_TARGET_PORT_RANGE);
          }
        else if (strcasecmp ("SSH_LSC_CREDENTIAL", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "id",
                              &create_target_data->ssh_lsc_credential_id);
            set_client_state (CLIENT_CREATE_TARGET_SSH_LSC_CREDENTIAL);
          }
        else if (strcasecmp ("SMB_LSC_CREDENTIAL", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "id",
                              &create_target_data->smb_lsc_credential_id);
            set_client_state (CLIENT_CREATE_TARGET_SMB_LSC_CREDENTIAL);
          }
        else if (strcasecmp ("NAME", element_name) == 0)
          {
            openvas_append_string (&create_target_data->name, "");
            set_client_state (CLIENT_CREATE_TARGET_NAME);
          }
        else if (strcasecmp ("TARGET_LOCATOR", element_name) == 0)
          set_client_state (CLIENT_CREATE_TARGET_TARGET_LOCATOR);
        ELSE_ERROR ("create_target");

      case CLIENT_CREATE_TARGET_NAME:
        if (strcasecmp ("MAKE_UNIQUE", element_name) == 0)
          set_client_state (CLIENT_CREATE_TARGET_NAME_MAKE_UNIQUE);
        ELSE_ERROR ("create_target");

      case CLIENT_CREATE_TARGET_SSH_LSC_CREDENTIAL:
        if (strcasecmp ("PORT", element_name) == 0)
          set_client_state (CLIENT_CREATE_TARGET_SSH_LSC_CREDENTIAL_PORT);
        ELSE_ERROR ("create_target");

      case CLIENT_CREATE_TARGET_TARGET_LOCATOR:
        if (strcasecmp ("PASSWORD", element_name) == 0)
          set_client_state (CLIENT_CREATE_TARGET_TARGET_LOCATOR_PASSWORD);
        else if (strcasecmp ("USERNAME", element_name) == 0)
          set_client_state (CLIENT_CREATE_TARGET_TARGET_LOCATOR_USERNAME);
        ELSE_ERROR ("create_target");

      case CLIENT_CREATE_TASK:
        if (strcasecmp ("COPY", element_name) == 0)
          set_client_state (CLIENT_CREATE_TASK_COPY);
        else if (strcasecmp ("RCFILE", element_name) == 0)
          {
            /* Initialise the task description. */
            if (create_task_data->task)
              add_task_description_line (create_task_data->task, "", 0);
            set_client_state (CLIENT_CREATE_TASK_RCFILE);
          }
        else if (strcasecmp ("PREFERENCES", element_name) == 0)
          {
            create_task_data->preferences = make_array ();
            set_client_state (CLIENT_CREATE_TASK_PREFERENCES);
          }
        else if (strcasecmp ("NAME", element_name) == 0)
          set_client_state (CLIENT_CREATE_TASK_NAME);
        else if (strcasecmp ("COMMENT", element_name) == 0)
          set_client_state (CLIENT_CREATE_TASK_COMMENT);
        else if (strcasecmp ("CONFIG", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "id",
                              &create_task_data->config_id);
            set_client_state (CLIENT_CREATE_TASK_CONFIG);
          }
        else if (strcasecmp ("ALERT", element_name) == 0)
          {
            const gchar* attribute;
            if (find_attribute (attribute_names, attribute_values, "id",
                                &attribute))
              array_add (create_task_data->alerts, g_strdup (attribute));
            set_client_state (CLIENT_CREATE_TASK_ALERT);
          }
        else if (strcasecmp ("OBSERVERS", element_name) == 0)
          set_client_state (CLIENT_CREATE_TASK_OBSERVERS);
        else if (strcasecmp ("SCHEDULE", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "id",
                              &create_task_data->schedule_id);
            set_client_state (CLIENT_CREATE_TASK_SCHEDULE);
          }
        else if (strcasecmp ("SLAVE", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "id",
                              &create_task_data->slave_id);
            set_client_state (CLIENT_CREATE_TASK_SLAVE);
          }
        else if (strcasecmp ("TARGET", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "id",
                              &create_task_data->target_id);
            set_client_state (CLIENT_CREATE_TASK_TARGET);
          }
        ELSE_ERROR ("create_task");

      case CLIENT_CREATE_TASK_PREFERENCES:
        if (strcasecmp ("PREFERENCE", element_name) == 0)
          {
            assert (create_task_data->preference == NULL);
            create_task_data->preference = g_malloc (sizeof (name_value_t));
            create_task_data->preference->name = NULL;
            create_task_data->preference->value = NULL;
            set_client_state (CLIENT_CREATE_TASK_PREFERENCES_PREFERENCE);
          }
        ELSE_ERROR ("create_task");

      case CLIENT_CREATE_TASK_PREFERENCES_PREFERENCE:
        if (strcasecmp ("SCANNER_NAME", element_name) == 0)
          set_client_state (CLIENT_CREATE_TASK_PREFERENCES_PREFERENCE_NAME);
        else if (strcasecmp ("VALUE", element_name) == 0)
          set_client_state (CLIENT_CREATE_TASK_PREFERENCES_PREFERENCE_VALUE);
        ELSE_ERROR ("create_task");

      case CLIENT_MODIFY_NOTE:
        if (strcasecmp ("ACTIVE", element_name) == 0)
          set_client_state (CLIENT_MODIFY_NOTE_ACTIVE);
        else if (strcasecmp ("HOSTS", element_name) == 0)
          set_client_state (CLIENT_MODIFY_NOTE_HOSTS);
        else if (strcasecmp ("PORT", element_name) == 0)
          set_client_state (CLIENT_MODIFY_NOTE_PORT);
        else if (strcasecmp ("RESULT", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "id",
                              &modify_note_data->result_id);
            if (modify_note_data->result_id
                && modify_note_data->result_id[0] == '\0')
              {
                g_free (modify_note_data->result_id);
                modify_note_data->result_id = NULL;
              }
            set_client_state (CLIENT_MODIFY_NOTE_RESULT);
          }
        else if (strcasecmp ("TASK", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "id",
                              &modify_note_data->task_id);
            if (modify_note_data->task_id
                && modify_note_data->task_id[0] == '\0')
              {
                g_free (modify_note_data->task_id);
                modify_note_data->task_id = NULL;
              }
            set_client_state (CLIENT_MODIFY_NOTE_TASK);
          }
        else if (strcasecmp ("TEXT", element_name) == 0)
          set_client_state (CLIENT_MODIFY_NOTE_TEXT);
        else if (strcasecmp ("THREAT", element_name) == 0)
          set_client_state (CLIENT_MODIFY_NOTE_THREAT);
        ELSE_ERROR ("modify_note");

      case CLIENT_MODIFY_OVERRIDE:
        if (strcasecmp ("ACTIVE", element_name) == 0)
          set_client_state (CLIENT_MODIFY_OVERRIDE_ACTIVE);
        else if (strcasecmp ("HOSTS", element_name) == 0)
          set_client_state (CLIENT_MODIFY_OVERRIDE_HOSTS);
        else if (strcasecmp ("NEW_THREAT", element_name) == 0)
          set_client_state (CLIENT_MODIFY_OVERRIDE_NEW_THREAT);
        else if (strcasecmp ("PORT", element_name) == 0)
          set_client_state (CLIENT_MODIFY_OVERRIDE_PORT);
        else if (strcasecmp ("RESULT", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "id",
                              &modify_override_data->result_id);
            if (modify_override_data->result_id
                && modify_override_data->result_id[0] == '\0')
              {
                g_free (modify_override_data->result_id);
                modify_override_data->result_id = NULL;
              }
            set_client_state (CLIENT_MODIFY_OVERRIDE_RESULT);
          }
        else if (strcasecmp ("TASK", element_name) == 0)
          {
            append_attribute (attribute_names, attribute_values, "id",
                              &modify_override_data->task_id);
            if (modify_override_data->task_id
                && modify_override_data->task_id[0] == '\0')
              {
                g_free (modify_override_data->task_id);
                modify_override_data->task_id = NULL;
              }
            set_client_state (CLIENT_MODIFY_OVERRIDE_TASK);
          }
        else if (strcasecmp ("TEXT", element_name) == 0)
          set_client_state (CLIENT_MODIFY_OVERRIDE_TEXT);
        else if (strcasecmp ("THREAT", element_name) == 0)
          set_client_state (CLIENT_MODIFY_OVERRIDE_THREAT);
        ELSE_ERROR ("modify_override");

      case CLIENT_RUN_WIZARD:
        if (strcasecmp ("NAME", element_name) == 0)
          {
            set_client_state (CLIENT_RUN_WIZARD_NAME);
          }
        else if (strcasecmp ("PARAMS", element_name) == 0)
          {
            run_wizard_data->params = make_array ();
            set_client_state (CLIENT_RUN_WIZARD_PARAMS);
          }
        ELSE_ERROR ("run_wizard");

      case CLIENT_RUN_WIZARD_PARAMS:
        if (strcasecmp ("PARAM", element_name) == 0)
          {
            assert (run_wizard_data->param == NULL);
            run_wizard_data->param = g_malloc (sizeof (name_value_t));
            run_wizard_data->param->name = NULL;
            run_wizard_data->param->value = NULL;
            set_client_state (CLIENT_RUN_WIZARD_PARAMS_PARAM);
          }
        ELSE_ERROR ("run_wizard");

      case CLIENT_RUN_WIZARD_PARAMS_PARAM:
        if (strcasecmp ("NAME", element_name) == 0)
          {
            set_client_state (CLIENT_RUN_WIZARD_PARAMS_PARAM_NAME);
          }
        else if (strcasecmp ("VALUE", element_name) == 0)
          {
            set_client_state (CLIENT_RUN_WIZARD_PARAMS_PARAM_VALUE);
          }
        ELSE_ERROR ("run_wizard");

      default:
        /* Send a generic response. */
        if (send_element_error_to_client ("omp", element_name,
                                          write_to_client,
                                          write_to_client_data))
          {
            error_send_to_client (error);
            return;
          }
        set_client_state (CLIENT_AUTHENTIC);
        g_set_error (error,
                     G_MARKUP_ERROR,
                     G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                     "Error");
        break;
    }

  return;
}

/**
 * @brief Send XML for a requirement of a plugin.
 *
 * @param[in]  element  The required plugin.
 * @param[in]  data     Array of two pointers: write_to_client and
 *                      write_to_client_data.
 *
 * @return 0 if out of space in to_client buffer, else 1.
 */
static gint
send_requirement (gconstpointer element, gconstpointer data)
{
  gboolean fail;
  gchar* text = g_markup_escape_text ((char*) element,
                                      strlen ((char*) element));
  char* oid = nvt_oid (text);
  gchar* msg = g_strdup_printf ("<nvt oid=\"%s\"><name>%s</name></nvt>",
                                oid ? oid : "",
                                text);
  int (*write_to_client) (const char *, void*)
    = (int (*) (const char *, void*)) *((void**)data);
  void* write_to_client_data = *(((void**)data) + 1);

  free (oid);
  g_free (text);

  fail = send_to_client (msg, write_to_client, write_to_client_data);
  g_free (msg);
  return fail ? 0 : 1;
}

/**
 * @brief Send XML for a plugin dependency.
 *
 * @param[in]  key    The dependency hashtable key.
 * @param[in]  value  The dependency hashtable value.
 * @param[in]  data   Array of two pointers: write_to_client and
 *                    write_to_client_data.
 *
 * @return TRUE if out of space in to_client buffer, else FALSE.
 */
static gboolean
send_dependency (gpointer key, gpointer value, gpointer data)
{
  gchar* key_text = g_markup_escape_text ((char*) key, strlen ((char*) key));
  char *oid = nvt_oid (key_text);
  gchar* msg = g_strdup_printf ("<nvt oid=\"%s\"><name>%s</name><requires>",
                                oid ? oid : "",
                                key_text);
  int (*write_to_client) (const char *, void*)
    = (int (*) (const char *, void*)) *((void**)data);
  void* write_to_client_data = *(((void**)data) + 1);

  g_free (oid);
  g_free (key_text);

  if (send_to_client (msg, write_to_client, write_to_client_data))
    {
      g_free (msg);
      return TRUE;
    }

  if (g_slist_find_custom ((GSList*) value, data, send_requirement))
    {
      g_free (msg);
      return TRUE;
    }

  if (send_to_client ("</requires></nvt>",
                      write_to_client,
                      write_to_client_data))
    {
      g_free (msg);
      return TRUE;
    }

  g_free (msg);
  return FALSE;
}

/**
 * @brief Send XML for an NVT.
 *
 * The caller must send the closing NVT tag.
 *
 * @param[in]  nvts        The NVT.
 * @param[in]  details     If true, detailed XML, else simple XML.
 * @param[in]  pref_count  Preference count.  Used if details is true.
 * @param[in]  timeout     Timeout.  Used if details is true.
 * @param[in]  write_to_client       Function to write to client.
 * @param[in]  write_to_client_data  Argument to \p write_to_client.
 *
 * @return TRUE if out of space in to_client buffer, else FALSE.
 */
static gboolean
send_nvt (iterator_t *nvts, int details, int pref_count, const char *timeout,
          int (*write_to_client) (const char *, void*),
          void* write_to_client_data)
{
  gchar *msg;

  msg = get_nvti_xml (nvts, details, pref_count, timeout, 0);
  if (send_to_client (msg, write_to_client, write_to_client_data))
    {
      g_free (msg);
      return TRUE;
    }
  g_free (msg);
  return FALSE;
}

/**
 * @brief Send XML for the reports of a task.
 *
 * @param[in]  task             The task.
 * @param[in]  apply_overrides  Whether to apply overrides.
 * @param[in]  write_to_client       Function to write to client.
 * @param[in]  write_to_client_data  Argument to \p write_to_client.
 *
 * @return 0 success, -4 out of space in to_client,
 *         -5 failed to get report counts, -6 failed to get timestamp.
 */
static int
send_reports (task_t task, int apply_overrides,
              int (*write_to_client) (const char*, void*),
              void* write_to_client_data)
{
  iterator_t iterator;
  report_t index;

  if (send_to_client ("<reports>", write_to_client, write_to_client_data))
    return -4;

  init_report_iterator (&iterator, task, 0);
  while (next_report (&iterator, &index))
    {
      gchar *uuid, *timestamp, *msg;
      int debugs, false_positives, holes, infos, logs, warnings, run_status;

      uuid = report_uuid (index);

      if (report_counts (uuid, &debugs, &holes, &infos, &logs, &warnings,
                         &false_positives, apply_overrides, 0))
        {
          free (uuid);
          return -5;
        }

      if (report_timestamp (uuid, &timestamp))
        {
          free (uuid);
          return -6;
        }

      tracef ("     %s\n", uuid);

      report_scan_run_status (index, &run_status);
      msg = g_strdup_printf ("<report"
                             " id=\"%s\">"
                             "<timestamp>%s</timestamp>"
                             "<scan_run_status>%s</scan_run_status>"
                             "<result_count>"
                             "<debug>%i</debug>"
                             "<hole>%i</hole>"
                             "<info>%i</info>"
                             "<log>%i</log>"
                             "<warning>%i</warning>"
                             "<false_positive>%i</false_positive>"
                             "</result_count>"
                             "</report>",
                             uuid,
                             timestamp,
                             run_status_name
                              (run_status ? run_status
                                          : TASK_STATUS_INTERNAL_ERROR),
                             debugs,
                             holes,
                             infos,
                             logs,
                             warnings,
                             false_positives);
      g_free (timestamp);
      if (send_to_client (msg, write_to_client, write_to_client_data))
        {
          g_free (msg);
          free (uuid);
          return -4;
        }
      g_free (msg);
      free (uuid);
    }
  cleanup_iterator (&iterator);

  if (send_to_client ("</reports>", write_to_client, write_to_client_data))
    return -4;

  return 0;
}

/**
 * @brief Convert \n's to real newline's.
 *
 * @param[in]  text  The text in which to insert newlines.
 *
 * @return A newly allocated version of text.
 */
static gchar*
convert_to_newlines (const char *text)
{
  char *nptr, *new;

  new = g_malloc (strlen (text) + 1);
  nptr = new;
  while (*text)
    if (*text == '\\')
      {
         /* Convert "\\n" to '\n' */
         if (*(text+1) == 'n')
           {
             text += 2;
             *nptr++ = '\n';
           }
         /* Skip "\\r" */
         else if (*(text+1) == 'r')
           text += 2;
         else
           *nptr++ = *text++;
      }
    else
      *nptr++ = *text++;
  *nptr = '\0';

  return new;
}

/**
 * @brief Format XML into a buffer.
 *
 * @param[in]  buffer  Buffer.
 * @param[in]  format  Format string for XML.
 * @param[in]  ...     Arguments for format string.
 */
static void
buffer_xml_append_printf (GString *buffer, const char *format, ...)
{
  va_list args;
  gchar *msg;
  va_start (args, format);
  msg = g_markup_vprintf_escaped (format, args);
  va_end (args);
  g_string_append (buffer, msg);
  g_free (msg);
}

/**
 * @brief Buffer XML for some notes.
 *
 * @param[in]  buffer                 Buffer into which to buffer notes.
 * @param[in]  notes                  Notes iterator.
 * @param[in]  include_notes_details  Whether to include details of notes.
 * @param[in]  include_result         Whether to include associated result.
 * @param[out] count                  Number of notes.
 */
static void
buffer_notes_xml (GString *buffer, iterator_t *notes, int include_notes_details,
                  int include_result, int *count)
{
  while (next (notes))
    {
      char *uuid_task, *uuid_result;

      if (count)
        (*count)++;

      if (note_iterator_task (notes))
        task_uuid (note_iterator_task (notes),
                   &uuid_task);
      else
        uuid_task = NULL;

      if (note_iterator_result (notes))
        result_uuid (note_iterator_result (notes),
                     &uuid_result);
      else
        uuid_result = NULL;

      if (include_notes_details == 0)
        {
          const char *text = note_iterator_text (notes);
          gchar *excerpt = g_strndup (text, 60);
          /* This must match send_get_common. */
          buffer_xml_append_printf (buffer,
                                    "<note id=\"%s\">"
                                    "<nvt oid=\"%s\">"
                                    "<name>%s</name>"
                                    "</nvt>"
                                    "<creation_time>%s</creation_time>"
                                    "<modification_time>%s</modification_time>"
                                    "<writable>1</writable>"
                                    "<in_use>0</in_use>"
                                    "<active>%i</active>"
                                    "<text excerpt=\"%i\">%s</text>"
                                    "<orphan>%i</orphan>"
                                    "</note>",
                                    get_iterator_uuid (notes),
                                    note_iterator_nvt_oid (notes),
                                    note_iterator_nvt_name (notes),
                                    get_iterator_creation_time (notes),
                                    get_iterator_modification_time (notes),
                                    note_iterator_active (notes),
                                    strlen (excerpt) < strlen (text),
                                    excerpt,
                                    ((note_iterator_task (notes)
                                      && (uuid_task == NULL))
                                     || (note_iterator_result (notes)
                                         && (uuid_result == NULL))));
          g_free (excerpt);
        }
      else
        {
          char *name_task;
          int trash_task;
          time_t end_time;

          if (uuid_task)
            {
              name_task = task_name (note_iterator_task (notes));
              trash_task = task_in_trash (note_iterator_task (notes));
            }
          else
            {
              name_task = NULL;
              trash_task = 0;
            }

          end_time = note_iterator_end_time (notes);

          /* This must match send_get_common. */
          buffer_xml_append_printf
           (buffer,
            "<note id=\"%s\">"
            "<nvt oid=\"%s\"><name>%s</name></nvt>"
            "<creation_time>%s</creation_time>"
            "<modification_time>%s</modification_time>"
            "<writable>1</writable>"
            "<in_use>0</in_use>"
            "<active>%i</active>"
            "<end_time>%s</end_time>"
            "<text>%s</text>"
            "<hosts>%s</hosts>"
            "<port>%s</port>"
            "<threat>%s</threat>"

            "<task id=\"%s\"><name>%s</name><trash>%i</trash></task>"
            "<orphan>%i</orphan>",
            get_iterator_uuid (notes),
            note_iterator_nvt_oid (notes),
            note_iterator_nvt_name (notes),
            get_iterator_creation_time (notes),
            get_iterator_modification_time (notes),
            note_iterator_active (notes),
            end_time > 1 ? iso_time (&end_time) : "",
            note_iterator_text (notes),
            note_iterator_hosts (notes)
             ? note_iterator_hosts (notes) : "",
            note_iterator_port (notes)
             ? note_iterator_port (notes) : "",
            note_iterator_threat (notes)
             ? note_iterator_threat (notes) : "",
            uuid_task ? uuid_task : "",
            name_task ? name_task : "",
            trash_task,
            ((note_iterator_task (notes) && (uuid_task == NULL))
             || (note_iterator_result (notes) && (uuid_result == NULL))));

          free (name_task);

          if (include_result && note_iterator_result (notes))
            {
              iterator_t results;

              init_result_iterator (&results, 0,
                                    note_iterator_result (notes),
                                    0, 1, 1, NULL, NULL, 1, NULL, 0, NULL, 0);
              while (next (&results))
                buffer_results_xml (buffer,
                                    &results,
                                    0,
                                    0,  /* Notes. */
                                    0,  /* Note details. */
                                    0,  /* Overrides. */
                                    0,  /* Override details. */
                                    NULL,
                                    NULL,
                                    0);
              cleanup_iterator (&results);

              buffer_xml_append_printf (buffer, "</note>");
            }
          else
            buffer_xml_append_printf (buffer,
                                      "<result id=\"%s\"/>"
                                      "</note>",
                                      uuid_result ? uuid_result : "");
        }
      free (uuid_task);
      free (uuid_result);
    }
}

/**
 * @brief Buffer XML for some overrides.
 *
 * @param[in]  buffer                     Buffer into which to buffer overrides.
 * @param[in]  overrides                  Overrides iterator.
 * @param[in]  include_overrides_details  Whether to include details of overrides.
 * @param[in]  include_result             Whether to include associated result.
 * @param[out] count                      Number of overrides.
 */
static void
buffer_overrides_xml (GString *buffer, iterator_t *overrides,
                      int include_overrides_details, int include_result,
                      int *count)
{
  while (next (overrides))
    {
      char *uuid_task, *uuid_result;

      if (count)
        (*count)++;

      if (override_iterator_task (overrides))
        task_uuid (override_iterator_task (overrides),
                   &uuid_task);
      else
        uuid_task = NULL;

      if (override_iterator_result (overrides))
        result_uuid (override_iterator_result (overrides),
                     &uuid_result);
      else
        uuid_result = NULL;

      if (include_overrides_details == 0)
        {
          const char *text = override_iterator_text (overrides);
          gchar *excerpt = g_strndup (text, 60);
          /* This must match send_get_common. */
          buffer_xml_append_printf (buffer,
                                    "<override id=\"%s\">"
                                    "<nvt oid=\"%s\">"
                                    "<name>%s</name>"
                                    "</nvt>"
                                    "<creation_time>%s</creation_time>"
                                    "<modification_time>%s</modification_time>"
                                    "<writable>1</writable>"
                                    "<in_use>0</in_use>"
                                    "<active>%i</active>"
                                    "<text excerpt=\"%i\">%s</text>"
                                    "<new_threat>%s</new_threat>"
                                    "<orphan>%i</orphan>"
                                    "</override>",
                                    get_iterator_uuid (overrides),
                                    override_iterator_nvt_oid (overrides),
                                    override_iterator_nvt_name (overrides),
                                    get_iterator_creation_time (overrides),
                                    get_iterator_modification_time (overrides),
                                    override_iterator_active (overrides),
                                    strlen (excerpt) < strlen (text),
                                    excerpt,
                                    override_iterator_new_threat (overrides),
                                    ((override_iterator_task (overrides)
                                      && (uuid_task == NULL))
                                     || (override_iterator_result (overrides)
                                         && (uuid_result == NULL))));
          g_free (excerpt);
        }
      else
        {
          char *name_task;
          int trash_task;
          time_t end_time;

          if (uuid_task)
            {
              name_task = task_name (override_iterator_task (overrides));
              trash_task = task_in_trash (override_iterator_task (overrides));
            }
          else
            {
              name_task = NULL;
              trash_task = 0;
            }

          end_time = override_iterator_end_time (overrides);

          /* This must match send_get_common. */
          buffer_xml_append_printf
           (buffer,
            "<override id=\"%s\">"
            "<nvt oid=\"%s\"><name>%s</name></nvt>"
            "<creation_time>%s</creation_time>"
            "<modification_time>%s</modification_time>"
            "<writable>1</writable>"
            "<in_use>0</in_use>"
            "<active>%i</active>"
            "<end_time>%s</end_time>"
            "<text>%s</text>"
            "<hosts>%s</hosts>"
            "<port>%s</port>"
            "<threat>%s</threat>"
            "<new_threat>%s</new_threat>"
            "<task id=\"%s\"><name>%s</name><trash>%i</trash></task>"
            "<orphan>%i</orphan>",
            get_iterator_uuid (overrides),
            override_iterator_nvt_oid (overrides),
            override_iterator_nvt_name (overrides),
            get_iterator_creation_time (overrides),
            get_iterator_modification_time (overrides),
            override_iterator_active (overrides),
            end_time > 1 ? iso_time (&end_time) : "",
            override_iterator_text (overrides),
            override_iterator_hosts (overrides)
             ? override_iterator_hosts (overrides) : "",
            override_iterator_port (overrides)
             ? override_iterator_port (overrides) : "",
            override_iterator_threat (overrides)
             ? override_iterator_threat (overrides) : "",
            override_iterator_new_threat (overrides),
            uuid_task ? uuid_task : "",
            name_task ? name_task : "",
            trash_task,
            ((override_iterator_task (overrides) && (uuid_task == NULL))
             || (override_iterator_result (overrides) && (uuid_result == NULL))));

          free (name_task);

          if (include_result && override_iterator_result (overrides))
            {
              iterator_t results;

              init_result_iterator (&results, 0,
                                    override_iterator_result (overrides),
                                    0, 1, 1, NULL, NULL, 1, NULL, 0, NULL, 0);
              while (next (&results))
                buffer_results_xml (buffer,
                                    &results,
                                    0,
                                    0,  /* Overrides. */
                                    0,  /* Override details. */
                                    0,  /* Overrides. */
                                    0,  /* Override details. */
                                    NULL,
                                    NULL,
                                    0);
              cleanup_iterator (&results);

              buffer_xml_append_printf (buffer, "</override>");
            }
          else
            buffer_xml_append_printf (buffer,
                                      "<result id=\"%s\"/>"
                                      "</override>",
                                      uuid_result ? uuid_result : "");
        }
      free (uuid_task);
      free (uuid_result);
    }
}

/* External for manage.c. */
/**
 * @brief Buffer XML for the NVT preference of a config.
 *
 * @param[in]  buffer  Buffer.
 * @param[in]  prefs   NVT preference iterator.
 * @param[in]  config  Config.
 */
void
buffer_config_preference_xml (GString *buffer, iterator_t *prefs,
                              config_t config)
{
  char *real_name, *type, *value, *nvt;
  char *oid = NULL;

  real_name = nvt_preference_iterator_real_name (prefs);
  type = nvt_preference_iterator_type (prefs);
  value = nvt_preference_iterator_config_value (prefs, config);
  nvt = nvt_preference_iterator_nvt (prefs);

  if (nvt) oid = nvt_oid (nvt);

  buffer_xml_append_printf (buffer,
                            "<preference>"
                            "<nvt oid=\"%s\"><name>%s</name></nvt>"
                            "<name>%s</name>"
                            "<type>%s</type>",
                            oid ? oid : "",
                            nvt ? nvt : "",
                            real_name ? real_name : "",
                            type ? type : "");

  if (value
      && type
      && (strcmp (type, "radio") == 0))
    {
      /* Handle the other possible values. */
      char *pos = strchr (value, ';');
      if (pos) *pos = '\0';
      buffer_xml_append_printf (buffer, "<value>%s</value>", value);
      while (pos)
        {
          char *pos2 = strchr (++pos, ';');
          if (pos2) *pos2 = '\0';
          buffer_xml_append_printf (buffer, "<alt>%s</alt>", pos);
          pos = pos2;
        }
    }
  else if (value
           && type
           && (strcmp (type, "password") == 0))
    buffer_xml_append_printf (buffer, "<value></value>");
  else
    buffer_xml_append_printf (buffer, "<value>%s</value>", value ? value : "");

  buffer_xml_append_printf (buffer, "</preference>");

  free (real_name);
  free (type);
  free (value);
  free (nvt);
  free (oid);
}

/**
 * @brief Compare two string with the "diff" command.
 *
 * @param[in]  one     First string.
 * @param[in]  two     Second string.
 *
 * @return Output of "diff", or NULL on error.
 */
gchar *
strdiff (const gchar *one, const gchar *two)
{
  gchar **cmd, *ret, *one_file, *two_file, *old_lc_all, *old_language;
  gint exit_status;
  gchar *standard_out = NULL;
  gchar *standard_err = NULL;
  char dir[] = "/tmp/openvasmd-strdiff-XXXXXX";
  GError *error = NULL;

  if (mkdtemp (dir) == NULL)
    return NULL;

  one_file = g_build_filename (dir, "Report 1", NULL);

  g_file_set_contents (one_file, one, strlen (one), &error);
  if (error)
    {
      g_warning ("%s", error->message);
      g_error_free (error);
      openvas_file_remove_recurse (dir);
      g_free (one_file);
      return NULL;
    }

  two_file = g_build_filename (dir, "Report 2", NULL);

  g_file_set_contents (two_file, two, strlen (two), &error);
  if (error)
    {
      g_warning ("%s", error->message);
      g_error_free (error);
      openvas_file_remove_recurse (dir);
      g_free (one_file);
      g_free (two_file);
      return NULL;
    }

  old_lc_all = getenv ("LC_ALL") ? g_strdup (getenv ("LC_ALL")) : NULL;
  if (setenv ("LC_ALL", "C", 1) == -1)
    {
      g_warning ("%s: failed to set LC_ALL\n", __FUNCTION__);
      return NULL;
    }

  old_language = getenv ("LANGUAGE") ? g_strdup (getenv ("LANGUAGE")) : NULL;
  if (setenv ("LANGUAGE", "C", 1) == -1)
    {
      g_warning ("%s: failed to set LANGUAGE\n", __FUNCTION__);
      return NULL;
    }

  cmd = (gchar **) g_malloc (5 * sizeof (gchar *));

  cmd[0] = g_strdup ("diff");
  cmd[1] = g_strdup ("-u");
  cmd[2] = g_strdup ("Report 1");
  cmd[3] = g_strdup ("Report 2");
  cmd[4] = NULL;
  g_debug ("%s: Spawning in %s: %s \"%s\" \"%s\"\n",
           __FUNCTION__, dir,
           cmd[0], cmd[1], cmd[2]);
  if ((g_spawn_sync (dir,
                     cmd,
                     NULL,                 /* Environment. */
                     G_SPAWN_SEARCH_PATH,
                     NULL,                 /* Setup func. */
                     NULL,
                     &standard_out,
                     &standard_err,
                     &exit_status,
                     NULL) == FALSE)
      || (WIFEXITED (exit_status) == 0)
      || WEXITSTATUS (exit_status))
    {
      if (WEXITSTATUS (exit_status) == 1)
        ret = standard_out;
      else
        {
          g_debug ("%s: failed to run diff: %d (WIF %i, WEX %i)",
                   __FUNCTION__,
                   exit_status,
                   WIFEXITED (exit_status),
                   WEXITSTATUS (exit_status));
          g_debug ("%s: stdout: %s\n", __FUNCTION__, standard_out);
          g_debug ("%s: stderr: %s\n", __FUNCTION__, standard_err);
          ret = NULL;
          g_free (standard_out);
        }
    }
  else
    ret = standard_out;

  if (old_lc_all && (setenv ("LC_ALL", old_lc_all, 1) == -1))
    {
      g_warning ("%s: failed to reset LC_ALL\n", __FUNCTION__);
      ret = NULL;
    }
  else if (old_language && (setenv ("LANGUAGE", old_language, 1) == -1))
    {
      g_warning ("%s: failed to reset LANGUAGE\n", __FUNCTION__);
      ret = NULL;
    }

  g_free (old_lc_all);
  g_free (old_language);
  g_free (cmd[0]);
  g_free (cmd[1]);
  g_free (cmd[2]);
  g_free (cmd[3]);
  g_free (cmd);
  g_free (standard_err);
  g_free (one_file);
  g_free (two_file);
  openvas_file_remove_recurse (dir);

  return ret;
}

/**
 * @brief Buffer XML for notes of a result.
 *
 * @param[in]  buffer                 Buffer into which to buffer results.
 * @param[in]  result                 Result.
 * @param[in]  task                   Task associated with result.
 * @param[in]  include_notes_details  Whether to include details of notes.
 */
static void
buffer_result_notes_xml (GString *buffer, result_t result, task_t task,
                         int include_notes_details)
{
  g_string_append (buffer, "<notes>");

  if (task)
    {
      get_data_t get;
      iterator_t notes;
      memset (&get, '\0', sizeof (get));
      get.filter = "sort-reverse=created";  /* Most recent first. */
      init_note_iterator (&notes,
                          &get,
                          0,
                          result,
                          task);
      buffer_notes_xml (buffer,
                        &notes,
                        include_notes_details,
                        0,
                        NULL);
      cleanup_iterator (&notes);
    }

  g_string_append (buffer, "</notes>");
}

/**
 * @brief Buffer XML for overrides of a result.
 *
 * @param[in]  buffer                 Buffer into which to buffer results.
 * @param[in]  result                 Result.
 * @param[in]  task                   Task associated with result.
 * @param[in]  include_overrides_details  Whether to include details of overrides.
 */
static void
buffer_result_overrides_xml (GString *buffer, result_t result, task_t task,
                             int include_overrides_details)
{
  g_string_append (buffer, "<overrides>");

  if (task)
    {
      get_data_t get;
      iterator_t overrides;
      memset (&get, '\0', sizeof (get));
      get.filter = "sort-reverse=created";  /* Most recent first. */
      init_override_iterator (&overrides,
                              &get,
                              0,
                              result,
                              task);
      buffer_overrides_xml (buffer,
                            &overrides,
                            include_overrides_details,
                            0,
                            NULL);
      cleanup_iterator (&overrides);
    }

  g_string_append (buffer, "</overrides>");
}

/**
 * @brief Add a detail block to a XML buffer.
 */
#define ADD_DETAIL(buff, dname, dvalue) do { \
                                buffer_xml_append_printf (buff,   \
                                                          "<detail>"          \
                                                          "<name>%s</name>"   \
                                                          "<value>%s</value>" \
                                                          "</detail>",        \
                                                          dname,              \
                                                          dvalue);            \
                                } while (0)

/** @todo Exported for manage_sql.c. */
/**
 * @brief Buffer XML for some results.
 *
 * @param[in]  buffer                 Buffer into which to buffer results.
 * @param[in]  results                Result iterator.
 * @param[in]  task                   Task associated with results.  Only
 *                                    needed with include_notes or
 *                                    include_overrides.
 * @param[in]  include_notes          Whether to include notes.
 * @param[in]  include_notes_details  Whether to include details of notes.
 * @param[in]  include_overrides          Whether to include overrides.
 * @param[in]  include_overrides_details  Whether to include details of overrides.
 * @param[in]  delta_state            Delta state of result, or NULL.
 * @param[in]  delta_results          Iterator for delta result to include, or
 *                                    NULL.
 * @param[in]  changed                Whether the result is a "changed" delta.
 */
void
buffer_results_xml (GString *buffer, iterator_t *results, task_t task,
                    int include_notes, int include_notes_details,
                    int include_overrides, int include_overrides_details,
                    const char *delta_state, iterator_t *delta_results,
                    int changed)
{
  const char *descr = result_iterator_descr (results);
  gchar *nl_descr = descr ? convert_to_newlines (descr) : NULL;
  const char *name = result_iterator_nvt_name (results);
  const char *oid = result_iterator_nvt_oid (results);
  const char *family = result_iterator_nvt_family (results);
  const char *cvss_base = result_iterator_nvt_cvss_base (results);
  const char *risk_factor = result_iterator_nvt_risk_factor (results);
  const char *cve = result_iterator_nvt_cve (results);
  const char *bid = result_iterator_nvt_bid (results);
  const char *tags = result_iterator_nvt_tag (results);
  iterator_t cert_refs_iterator;
  const char *xref = result_iterator_nvt_xref (results);
  result_t result = result_iterator_result (results);
  char *uuid;
  char *detect_ref, *detect_cpe, *detect_loc, *detect_oid, *detect_name;

  result_uuid (result, &uuid);

  buffer_xml_append_printf (buffer, "<result id=\"%s\">", uuid);

  detect_ref = detect_cpe = detect_loc = detect_oid = detect_name = NULL;
  if (result_detection_reference (result, &detect_ref, &detect_cpe, &detect_loc,
                                  &detect_oid, &detect_name) == 0)
    {
      buffer_xml_append_printf (buffer,
                                "<detection>"
                                "<result id=\"%s\">"
                                "<details>",
                                detect_ref);

      ADD_DETAIL(buffer, "product", detect_cpe);
      ADD_DETAIL(buffer, "location", detect_loc);
      ADD_DETAIL(buffer, "source_oid", detect_oid);
      ADD_DETAIL(buffer, "source_name", detect_name);

      buffer_xml_append_printf (buffer,
                                "</details>"
                                "</result>"
                                "</detection>");
    }
  g_free (detect_ref);
  g_free (detect_cpe);
  g_free (detect_loc);
  g_free (detect_oid);
  g_free (detect_name);

  buffer_xml_append_printf
   (buffer,
    "<subnet>%s</subnet>"
    "<host>%s</host>"
    "<port>%s</port>"
    "<nvt oid=\"%s\">"
    "<name>%s</name>"
    "<family>%s</family>"
    "<cvss_base>%s</cvss_base>"
    "<risk_factor>%s</risk_factor>"
    "<cve>%s</cve>"
    "<bid>%s</bid>"
    "<tags>%s</tags>"
    "<cert>",
    result_iterator_subnet (results),
    result_iterator_host (results),
    result_iterator_port (results),
    result_iterator_nvt_oid (results),
    name ? name : "",
    family ? family : "",
    cvss_base ? cvss_base : "",
    risk_factor ? risk_factor : "",
    cve ? cve : "",
    bid ? bid : "",
    tags ? tags : "");

  if (manage_cert_loaded ())
    {
      init_nvt_dfn_cert_adv_iterator (&cert_refs_iterator, oid, 0, 0);
      while (next (&cert_refs_iterator))
        {
          g_string_append_printf (buffer,
                                  "<cert_ref type=\"DFN-CERT\" id=\"%s\"/>",
                                  get_iterator_name(&cert_refs_iterator));
        }
      cleanup_iterator (&cert_refs_iterator);
    }
  else
    {
      g_string_append_printf (buffer, "<warning>"
                                      "database not available"
                                      "</warning>");
    }

  buffer_xml_append_printf
   (buffer,
    "</cert>"
    "<xref>%s</xref>"
    "</nvt>"
    "<threat>%s</threat>"
    "<description>%s</description>",
    xref ? xref : "",
    manage_result_type_threat (result_iterator_type (results)),
    descr ? nl_descr : "");

  if (include_overrides)
    buffer_xml_append_printf (buffer,
                              "<original_threat>%s</original_threat>",
                              manage_result_type_threat
                               (result_iterator_original_type (results)));

  free (uuid);

  if (include_notes)
    buffer_result_notes_xml (buffer, result,
                             task, include_notes_details);

  if (include_overrides)
    buffer_result_overrides_xml (buffer, result,
                                 task, include_overrides_details);

  if (delta_state || delta_results)
    {
      g_string_append (buffer, "<delta>");
      if (delta_state)
        g_string_append_printf (buffer, "%s", delta_state);
      if (changed && delta_results)
        {
          gchar *diff, *delta_nl_descr;
          const char *delta_descr;
          buffer_results_xml (buffer, delta_results, task, include_notes,
                              include_notes_details, include_overrides,
                              include_overrides_details, delta_state, NULL, 0);
          delta_descr = result_iterator_descr (delta_results);
          delta_nl_descr = delta_descr ? convert_to_newlines (delta_descr)
                                       : NULL;
          diff = strdiff (descr ? nl_descr : "",
                          delta_descr ? delta_nl_descr : "");
          g_free (delta_nl_descr);
          if (diff)
            {
              gchar **split, *diff_xml;
              /* Remove the leading filename lines. */
              split = g_strsplit ((gchar*) diff, "\n", 3);
              if (split[0] && split[1] && split[2])
                diff_xml = g_markup_escape_text (split[2], strlen (split[2]));
              else
                diff_xml = g_markup_escape_text (diff, strlen (diff));
              g_strfreev (split);
              g_string_append_printf (buffer, "<diff>%s</diff>", diff_xml);
              g_free (diff_xml);
              g_free (diff);
            }
          else
            g_string_append (buffer, "<diff>Error creating diff.</diff>");
        }

      if (delta_results)
        {
          if (include_notes)
            buffer_result_notes_xml (buffer,
                                     result_iterator_result (delta_results),
                                     task,
                                     include_notes_details);

          if (include_overrides)
            buffer_result_overrides_xml (buffer,
                                         result_iterator_result (delta_results),
                                         task,
                                         include_overrides_details);
        }
      g_string_append (buffer, "</delta>");
    }

  if (descr) g_free (nl_descr);

  g_string_append (buffer, "</result>");
}

#undef ADD_DETAIL

/**
 * @brief Convert ranges to manage ranges.
 *
 * @param[in]  ranges  Ranges buffered in CREATE_PORT_LIST.
 *
 * @return Array of manage ranges on success, else NULL.
 */
static array_t *
convert_to_manage_ranges (array_t *ranges)
{
  if (ranges)
    {
      guint index;
      array_t *manage_ranges;

      manage_ranges = make_array ();

      index = ranges->len;
      while (index--)
        {
          create_port_list_range_t *range;
          range = (create_port_list_range_t*) g_ptr_array_index (ranges,
                                                                 index);
          if (range)
            {
              range_t *manage_range;

              manage_range = g_malloc0 (sizeof (range_t));
              manage_range->comment = range->comment;
              manage_range->end = atoi (range->end);
              manage_range->id = range->id;
              manage_range->start = atoi (range->start);
              if (strcasecmp (range->type, "TCP") == 0)
                manage_range->type = PORT_PROTOCOL_TCP;
              else if (strcasecmp (range->type, "UDP") == 0)
                manage_range->type = PORT_PROTOCOL_UDP;
              else
                manage_range->type = PORT_PROTOCOL_OTHER;
              manage_range->exclude = 0;

              array_add (manage_ranges, manage_range);
            }
        }
      return manage_ranges;
    }
  return NULL;
}

/**
 * @brief Insert else clause for omp_xml_handle_start_element.
 *
 * @param[in]  parent   Parent element.
 * @param[in]  element  Element.
 */
#define CLOSE(parent, element)                                           \
  case parent ## _ ## element:                                           \
    assert (strcasecmp (G_STRINGIFY (element), element_name) == 0);      \
    set_client_state (parent);                                           \
    break

/**
 * @brief Insert else clause for omp_xml_handle_start_element.
 *
 * Stop the parser from reading over elements at the same time.
 *
 * @param[in]  parent   Parent element.
 * @param[in]  element  Element.
 */
#define CLOSE_READ_OVER(parent, element)                                 \
  case parent ## _ ## element:                                           \
    assert (strcasecmp (G_STRINGIFY (element), element_name) == 0);      \
    omp_parser->read_over = 0;                                           \
    set_client_state (parent);                                           \
    break

/**
 * @brief Handle the end of an OMP XML element.
 *
 * @param[in]  resources  Resource iterator.
 * @param[in]  get        GET command data.
 * @param[out] first      First.
 * @param[out] count      Count.
 * @param[in]  init       Init function, to reset the iterator.
 *
 * @return What to do next: 0 continue, 1 end, -1 fail.
 */
static int
get_next (iterator_t *resources, get_data_t *get, int *first, int *count,
          int (*init) (iterator_t*, const get_data_t *))
{
  if (next (resources) == FALSE)
   {
     gchar *new_filter;

     if (*first == 0)
       return 1;

     if (*first == 1 || *count > 0)
       return 1;

     /* Reset the iterator with first 1, and start again. */
     cleanup_iterator (resources);
     new_filter = g_strdup_printf ("first=1 %s", get->filter);
     g_free (get->filter);
     get->filter = new_filter;
     if (init (resources, get))
       return -1;
     *count = 0;
     *first = 1;
     if (next (resources) == FALSE)
       return 1;
   }
 return 0;
}

/**
 * @brief Handle the end of an OMP XML element.
 *
 * React to the end of an XML element according to the current value
 * of \ref client_state, usually adjusting \ref client_state to indicate
 * the change (with \ref set_client_state).  Call \ref send_to_client to queue
 * any responses for the client.  Call the task utilities to adjust the
 * tasks (for example \ref start_task, \ref stop_task, \ref set_task_parameter,
 * \ref delete_task and \ref find_task ).
 *
 * Set error parameter on encountering an error.
 *
 * @param[in]  context           Parser context.
 * @param[in]  element_name      XML element name.
 * @param[in]  user_data         OMP parser.
 * @param[in]  error             Error parameter.
 */
static void
omp_xml_handle_end_element (/*@unused@*/ GMarkupParseContext* context,
                            const gchar *element_name,
                            gpointer user_data,
                            GError **error)
{
  omp_parser_t *omp_parser = (omp_parser_t*) user_data;
  int (*write_to_client) (const char *, void*)
    = (int (*) (const char *, void*)) omp_parser->client_writer;
  void* write_to_client_data = (void*) omp_parser->client_writer_data;

  tracef ("   XML    end: %s\n", element_name);

  if (omp_parser->read_over > 1)
    {
      omp_parser->read_over--;
    }
  else if ((omp_parser->read_over == 1) && omp_parser->parent_state)
    {
      client_state = omp_parser->parent_state;
      omp_parser->parent_state = 0;
      omp_parser->read_over = 0;
    }
  else switch (client_state)
    {
      case CLIENT_TOP:
        assert (0);
        break;

      case CLIENT_AUTHENTICATE:
        switch (authenticate (&current_credentials))
          {
            case 0:   /* Authentication succeeded. */
              if (load_tasks ())
                {
                  g_warning ("%s: failed to load tasks\n", __FUNCTION__);
                  g_set_error (error, G_MARKUP_ERROR, G_MARKUP_ERROR_PARSE,
                               "Manager failed to load tasks.");
                  free_credentials (&current_credentials);
                  SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("authenticate"));
                  set_client_state (CLIENT_TOP);
                }
              else
                {
                  const char *timezone;

                  timezone = (current_credentials.timezone
                              && strlen (current_credentials.timezone))
                               ? current_credentials.timezone
                               : "UTC";

                  if (setenv ("TZ", timezone, 1) == -1)
                    {
                      free_credentials (&current_credentials);
                      SEND_TO_CLIENT_OR_FAIL
                       (XML_INTERNAL_ERROR ("authenticate"));
                      set_client_state (CLIENT_TOP);
                      break;
                    }
                  tzset ();

                  SENDF_TO_CLIENT_OR_FAIL
                   ("<authenticate_response"
                    " status=\"" STATUS_OK "\""
                    " status_text=\"" STATUS_OK_TEXT "\">"
                    "<role>%s</role>"
                    "<timezone>%s</timezone>"
                    "</authenticate_response>",
                    current_credentials.role
                      ? current_credentials.role
                      : "",
                    timezone);

                  set_client_state (CLIENT_AUTHENTIC);
                }
              break;
            case 1:   /* Authentication failed. */
              free_credentials (&current_credentials);
              SEND_TO_CLIENT_OR_FAIL (XML_ERROR_AUTH_FAILED ("authenticate"));
              set_client_state (CLIENT_TOP);
              break;
            case -1:  /* Error while authenticating. */
            default:
              free_credentials (&current_credentials);
              SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("authenticate"));
              set_client_state (CLIENT_TOP);
              break;
          }
        break;

      case CLIENT_AUTHENTICATE_CREDENTIALS:
        assert (strcasecmp ("CREDENTIALS", element_name) == 0);
        set_client_state (CLIENT_AUTHENTICATE);
        break;

      case CLIENT_AUTHENTICATE_CREDENTIALS_USERNAME:
        assert (strcasecmp ("USERNAME", element_name) == 0);
        set_client_state (CLIENT_AUTHENTICATE_CREDENTIALS);
        break;

      case CLIENT_AUTHENTICATE_CREDENTIALS_PASSWORD:
        assert (strcasecmp ("PASSWORD", element_name) == 0);
        set_client_state (CLIENT_AUTHENTICATE_CREDENTIALS);
        break;

      case CLIENT_AUTHENTIC:
      case CLIENT_COMMANDS:
      case CLIENT_AUTHENTIC_COMMANDS:
        assert (strcasecmp ("COMMANDS", element_name) == 0);
        SENDF_TO_CLIENT_OR_FAIL ("</commands_response>");
        break;

      case CLIENT_GET_PREFERENCES:
        {
          iterator_t prefs;
          nvt_t nvt = 0;
          config_t config = 0;
          if (get_preferences_data->nvt_oid
              && find_nvt (get_preferences_data->nvt_oid, &nvt))
            SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("get_preferences"));
          else if (get_preferences_data->nvt_oid && nvt == 0)
            {
              if (send_find_error_to_client ("get_preferences",
                                             "NVT",
                                             get_preferences_data->nvt_oid,
                                             write_to_client,
                                             write_to_client_data))
                {
                  error_send_to_client (error);
                  return;
                }
            }
          else if (get_preferences_data->config_id
                   && find_config (get_preferences_data->config_id, &config))
            SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("get_preferences"));
          else if (get_preferences_data->config_id && config == 0)
            {
              if (send_find_error_to_client ("get_preferences",
                                             "config",
                                             get_preferences_data->config_id,
                                             write_to_client,
                                             write_to_client_data))
                {
                  error_send_to_client (error);
                  return;
                }
            }
          else
            {
              char *nvt_name = manage_nvt_name (nvt);
              SEND_TO_CLIENT_OR_FAIL ("<get_preferences_response"
                                      " status=\"" STATUS_OK "\""
                                      " status_text=\"" STATUS_OK_TEXT "\">");
              init_nvt_preference_iterator (&prefs, nvt_name);
              free (nvt_name);
              if (get_preferences_data->preference)
                while (next (&prefs))
                  {
                    char *name = strstr (nvt_preference_iterator_name (&prefs), "]:");
                    if (name
                        && (strcmp (name + 2,
                                    get_preferences_data->preference)
                            == 0))
                      {
                        if (config)
                          {
                            GString *buffer = g_string_new ("");
                            buffer_config_preference_xml (buffer, &prefs, config);
                            SEND_TO_CLIENT_OR_FAIL (buffer->str);
                            g_string_free (buffer, TRUE);
                          }
                        else
                          SENDF_TO_CLIENT_OR_FAIL ("<preference>"
                                                   "<name>%s</name>"
                                                   "<value>%s</value>"
                                                   "</preference>",
                                                   nvt_preference_iterator_name (&prefs),
                                                   nvt_preference_iterator_value (&prefs));
                        break;
                      }
                  }
              else
                while (next (&prefs))
                  if (config)
                    {
                      GString *buffer = g_string_new ("");
                      buffer_config_preference_xml (buffer, &prefs, config);
                      SEND_TO_CLIENT_OR_FAIL (buffer->str);
                      g_string_free (buffer, TRUE);
                    }
                  else
                    SENDF_TO_CLIENT_OR_FAIL ("<preference>"
                                             "<name>%s</name>"
                                             "<value>%s</value>"
                                             "</preference>",
                                             nvt_preference_iterator_name (&prefs),
                                             nvt_preference_iterator_value (&prefs));
              cleanup_iterator (&prefs);
              SEND_TO_CLIENT_OR_FAIL ("</get_preferences_response>");
            }
          get_preferences_data_reset (get_preferences_data);
          set_client_state (CLIENT_AUTHENTIC);
          break;
        }

      case CLIENT_GET_DEPENDENCIES:
        if (scanner.plugins_dependencies)
          {
            nvt_t nvt = 0;

            if (get_dependencies_data->nvt_oid
                && find_nvt (get_dependencies_data->nvt_oid, &nvt))
              SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("get_dependencies"));
            else if (get_dependencies_data->nvt_oid && nvt == 0)
              {
                if (send_find_error_to_client ("get_dependencies",
                                               "NVT",
                                               get_dependencies_data->nvt_oid,
                                               write_to_client,
                                               write_to_client_data))
                  {
                    error_send_to_client (error);
                    return;
                  }
              }
            else
              {
                void* data[2];

                data[0] = (int (*) (void*)) write_to_client;
                data[1] = (void*) write_to_client_data;

                SEND_TO_CLIENT_OR_FAIL ("<get_dependencies_response"
                                        " status=\"" STATUS_OK "\""
                                        " status_text=\"" STATUS_OK_TEXT "\">");
                if (nvt)
                  {
                    char *name = manage_nvt_name (nvt);

                    if (name)
                      {
                        gpointer value;
                        value = g_hash_table_lookup
                                 (scanner.plugins_dependencies,
                                  name);
                        if (value && send_dependency (name, value, data))
                          {
                            g_free (name);
                            error_send_to_client (error);
                            return;
                          }
                        g_free (name);
                      }
                  }
                else if (g_hash_table_find (scanner.plugins_dependencies,
                                            send_dependency,
                                            data))
                  {
                    error_send_to_client (error);
                    return;
                  }
                SEND_TO_CLIENT_OR_FAIL ("</get_dependencies_response>");
              }
          }
        else
          SEND_TO_CLIENT_OR_FAIL (XML_SERVICE_DOWN ("get_dependencies"));
        get_dependencies_data_reset (get_dependencies_data);
        set_client_state (CLIENT_AUTHENTIC);
        break;

      case CLIENT_GET_NOTES:
        {
          nvt_t nvt = 0;
          task_t task = 0;

          assert (strcasecmp ("GET_NOTES", element_name) == 0);

          if (get_notes_data->note_id && get_notes_data->nvt_oid)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("get_notes",
                                "Only one of NVT and the note_id attribute"
                                " may be given"));
          else if (get_notes_data->note_id && get_notes_data->task_id)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("get_notes",
                                "Only one of the note_id and task_id"
                                " attributes may be given"));
          else if (get_notes_data->task_id
                   && find_task (get_notes_data->task_id, &task))
            SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("get_notes"));
          else if (get_notes_data->task_id && task == 0)
            {
              if (send_find_error_to_client ("get_notes",
                                             "task",
                                             get_notes_data->task_id,
                                             write_to_client,
                                             write_to_client_data))
                {
                  error_send_to_client (error);
                  return;
                }
            }
          else if (get_notes_data->nvt_oid
                   && find_nvt (get_notes_data->nvt_oid, &nvt))
            SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("get_notes"));
          else if (get_notes_data->nvt_oid && nvt == 0)
            {
              if (send_find_error_to_client ("get_notes",
                                             "NVT",
                                             get_notes_data->nvt_oid,
                                             write_to_client,
                                             write_to_client_data))
                {
                  error_send_to_client (error);
                  return;
                }
            }
          else
            {
              iterator_t notes;
              GString *buffer;
              int count, filtered, ret, first;
              get_data_t * get;

              get = &get_notes_data->get;
              if ((!get->filter && !get->filt_id)
                  || (get->filt_id && strcmp (get->filt_id, "-2") == 0))
                {
                  char *user_filter = setting_filter ("Notes");

                  if (user_filter && strlen (user_filter))
                    {
                      get->filt_id = user_filter;
                      get->filter = filter_term (user_filter);
                    }
                  else
                    get->filt_id = g_strdup("0");
                }


              ret = init_note_iterator (&notes, &get_notes_data->get, nvt, 0,
                                        task);
              if (ret)
                {
                  switch (ret)
                    {
                      case 1:
                        if (send_find_error_to_client ("get_notes",
                                                       "note",
                                                       get_notes_data->get.id,
                                                       write_to_client,
                                                       write_to_client_data))
                          {
                            error_send_to_client (error);
                            return;
                          }
                        break;
                      case 2:
                        if (send_find_error_to_client
                             ("get_notes",
                              "filter",
                              get_notes_data->get.filt_id,
                              write_to_client,
                              write_to_client_data))
                          {
                            error_send_to_client (error);
                            return;
                          }
                        break;
                      case -1:
                        SEND_TO_CLIENT_OR_FAIL
                         (XML_INTERNAL_ERROR ("get_notes"));
                        break;
                    }
                  get_notes_data_reset (get_notes_data);
                  set_client_state (CLIENT_AUTHENTIC);
                  break;
                }

              count = 0;
              manage_filter_controls (get->filter, &first, NULL, NULL, NULL);
              SEND_GET_START ("note", &get_notes_data->get);

              buffer = g_string_new ("");

              buffer_notes_xml (buffer, &notes, get_notes_data->get.details,
                                get_notes_data->result, &count);

              SEND_TO_CLIENT_OR_FAIL (buffer->str);
              g_string_free (buffer, TRUE);

              cleanup_iterator (&notes);
              filtered = get_notes_data->get.id
                          ? 1
                          : note_count (&get_notes_data->get, nvt, 0, task);
              SEND_GET_END ("note", &get_notes_data->get, count, filtered);
            }
          get_notes_data_reset (get_notes_data);
          set_client_state (CLIENT_AUTHENTIC);
          break;
        }

      case CLIENT_GET_NVT_FEED_CHECKSUM:
        {
          char *md5sum;
          if (get_nvt_feed_checksum_data->algorithm
              && strcasecmp (get_nvt_feed_checksum_data->algorithm, "md5"))
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("get_nvt_feed_checksum",
                                "GET_NVT_FEED_CHECKSUM algorithm must be md5"));
          else if ((md5sum = nvts_md5sum ()))
            {
              SEND_TO_CLIENT_OR_FAIL ("<get_nvt_feed_checksum_response"
                                      " status=\"" STATUS_OK "\""
                                      " status_text=\"" STATUS_OK_TEXT "\">"
                                      "<checksum algorithm=\"md5\">");
              SEND_TO_CLIENT_OR_FAIL (md5sum);
              free (md5sum);
              SEND_TO_CLIENT_OR_FAIL ("</checksum>"
                                      "</get_nvt_feed_checksum_response>");
            }
          else
            SEND_TO_CLIENT_OR_FAIL (XML_SERVICE_DOWN ("get_nvt_feed_checksum"));
          get_nvt_feed_checksum_data_reset (get_nvt_feed_checksum_data);
          set_client_state (CLIENT_AUTHENTIC);
          break;
        }

      case CLIENT_GET_NVTS:
        {
          char *md5sum = nvts_md5sum ();
          if (md5sum)
            {
              config_t config = (config_t) 0;
              nvt_t nvt = 0;

              free (md5sum);

              if (get_nvts_data->nvt_oid && get_nvts_data->family)
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("get_nvts",
                                    "Too many parameters at once"));
              else if ((get_nvts_data->details == 0)
                       && get_nvts_data->preference_count)
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("get_nvts",
                                    "GET_NVTS preference_count attribute"
                                    " requires the details attribute"));
              else if (((get_nvts_data->details == 0)
                        || (get_nvts_data->config_id == NULL))
                       && get_nvts_data->preferences)
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("get_nvts",
                                    "GET_NVTS preferences attribute"
                                    " requires the details and config_id"
                                    " attributes"));
              else if (((get_nvts_data->details == 0)
                        || (get_nvts_data->config_id == NULL))
                       && get_nvts_data->timeout)
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("get_nvts",
                                    "GET_NVTS timeout attribute"
                                    " requires the details and config_id"
                                    " attributes"));
              else if (get_nvts_data->nvt_oid
                       && find_nvt (get_nvts_data->nvt_oid, &nvt))
                SEND_TO_CLIENT_OR_FAIL
                 (XML_INTERNAL_ERROR ("get_nvts"));
              else if (get_nvts_data->nvt_oid && nvt == 0)
                {
                  if (send_find_error_to_client ("get_nvts",
                                                 "NVT",
                                                 get_nvts_data->nvt_oid,
                                                 write_to_client,
                                                 write_to_client_data))
                    {
                      error_send_to_client (error);
                      return;
                    }
                }
              else if (get_nvts_data->config_id
                       && find_config_for_actions (get_nvts_data->config_id,
                                                   &config,
                                                   get_nvts_data->actions))
                SEND_TO_CLIENT_OR_FAIL
                 (XML_INTERNAL_ERROR ("get_nvts"));
              else if (get_nvts_data->config_id && (config == 0))
                {
                  if (send_find_error_to_client
                       ("get_nvts",
                        "config",
                        get_nvts_data->config_id,
                        write_to_client,
                        write_to_client_data))
                    {
                      error_send_to_client (error);
                      return;
                    }
                }
              else
                {
                  iterator_t nvts;

                  SENDF_TO_CLIENT_OR_FAIL
                   ("<get_nvts_response"
                    " status=\"" STATUS_OK "\""
                    " status_text=\"" STATUS_OK_TEXT "\">");

                  init_nvt_iterator (&nvts,
                                     nvt,
                                     get_nvts_data->nvt_oid
                                      /* Presume the NVT is in the config (if
                                       * a config was given). */
                                      ? 0
                                      : config,
                                     get_nvts_data->family,
                                     NULL,
                                     get_nvts_data->sort_order,
                                     get_nvts_data->sort_field);
                  if (get_nvts_data->details)
                    while (next (&nvts))
                      {
                        int pref_count = -1;
                        char *timeout = NULL;

                        if (get_nvts_data->timeout)
                          timeout = config_nvt_timeout (config,
                                                        nvt_iterator_oid (&nvts));

                        if (get_nvts_data->preference_count)
                          {
                            const char *nvt_name = nvt_iterator_name (&nvts);
                            pref_count = nvt_preference_count (nvt_name);
                          }
                        if (send_nvt (&nvts, 1, pref_count, timeout,
                                      write_to_client, write_to_client_data))
                          {
                            cleanup_iterator (&nvts);
                            error_send_to_client (error);
                            return;
                          }

                        if (get_nvts_data->preferences)
                          {
                            iterator_t prefs;
                            const char *nvt_name = nvt_iterator_name (&nvts);

                            if (timeout == NULL)
                              timeout = config_nvt_timeout
                                         (config,
                                          nvt_iterator_oid (&nvts));

                            /* Send the preferences for the NVT. */

                            SENDF_TO_CLIENT_OR_FAIL ("<preferences>"
                                                     "<timeout>%s</timeout>",
                                                     timeout ? timeout : "");
                            free (timeout);

                            init_nvt_preference_iterator (&prefs, nvt_name);
                            while (next (&prefs))
                              {
                                GString *buffer = g_string_new ("");
                                buffer_config_preference_xml (buffer, &prefs, config);
                                SEND_TO_CLIENT_OR_FAIL (buffer->str);
                                g_string_free (buffer, TRUE);
                              }
                            cleanup_iterator (&prefs);

                            SEND_TO_CLIENT_OR_FAIL ("</preferences>");
                          }

                        SEND_TO_CLIENT_OR_FAIL ("</nvt>");
                      }
                  else
                    while (next (&nvts))
                      {
                        if (send_nvt (&nvts, 0, -1, NULL, write_to_client,
                                      write_to_client_data))
                          {
                            cleanup_iterator (&nvts);
                            error_send_to_client (error);
                            return;
                          }
                        SEND_TO_CLIENT_OR_FAIL ("</nvt>");
                      }
                  cleanup_iterator (&nvts);

                  SEND_TO_CLIENT_OR_FAIL ("</get_nvts_response>");
                }
            }
          else
            SEND_TO_CLIENT_OR_FAIL (XML_SERVICE_DOWN ("get_nvts"));
        }
        get_nvts_data_reset (get_nvts_data);
        set_client_state (CLIENT_AUTHENTIC);
        break;

      case CLIENT_GET_NVT_FAMILIES:
        {
          iterator_t families;

          SEND_TO_CLIENT_OR_FAIL ("<get_nvt_families_response"
                                  " status=\"" STATUS_OK "\""
                                  " status_text=\"" STATUS_OK_TEXT "\">"
                                  "<families>");

          init_family_iterator (&families,
                                1,
                                NULL,
                                get_nvt_families_data->sort_order);
          while (next (&families))
            {
              int family_max;
              const char *family;

              family = family_iterator_name (&families);
              if (family)
                family_max = family_nvt_count (family);
              else
                family_max = -1;

              SENDF_TO_CLIENT_OR_FAIL
               ("<family>"
                "<name>%s</name>"
                /* The total number of NVT's in the family. */
                "<max_nvt_count>%i</max_nvt_count>"
                "</family>",
                family ? family : "",
                family_max);
            }
          cleanup_iterator (&families);

          SEND_TO_CLIENT_OR_FAIL ("</families>"
                                  "</get_nvt_families_response>");
        }
        get_nvt_families_data_reset (get_nvt_families_data);
        set_client_state (CLIENT_AUTHENTIC);
        break;

      case CLIENT_GET_OVERRIDES:
        {
          nvt_t nvt = 0;
          task_t task = 0;

          assert (strcasecmp ("GET_OVERRIDES", element_name) == 0);

          if (get_overrides_data->override_id && get_overrides_data->nvt_oid)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("get_overrides",
                                "Only one of NVT and the override_id attribute"
                                " may be given"));
          else if (get_overrides_data->override_id
                   && get_overrides_data->task_id)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("get_overrides",
                                "Only one of the override_id and task_id"
                                " attributes may be given"));
          else if (get_overrides_data->task_id
                   && find_task (get_overrides_data->task_id, &task))
            SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("get_overrides"));
          else if (get_overrides_data->task_id && task == 0)
            {
              if (send_find_error_to_client ("get_overrides",
                                             "task",
                                             get_overrides_data->task_id,
                                             write_to_client,
                                             write_to_client_data))
                {
                  error_send_to_client (error);
                  return;
                }
            }
          else if (get_overrides_data->nvt_oid
                   && find_nvt (get_overrides_data->nvt_oid, &nvt))
            SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("get_overrides"));
          else if (get_overrides_data->nvt_oid && nvt == 0)
            {
              if (send_find_error_to_client ("get_overrides",
                                             "NVT",
                                             get_overrides_data->nvt_oid,
                                             write_to_client,
                                             write_to_client_data))
                {
                  error_send_to_client (error);
                  return;
                }
            }
          else
            {
              iterator_t overrides;
              GString *buffer;
              int count, filtered, ret, first;
              get_data_t * get;

              get = &get_overrides_data->get;
              if ((!get->filter && !get->filt_id)
                  || (get->filt_id && strcmp (get->filt_id, "-2") == 0))
                {
                  char *user_filter = setting_filter ("Overrides");

                  if (user_filter && strlen (user_filter))
                    {
                      get->filt_id = user_filter;
                      get->filter = filter_term (user_filter);
                    }
                  else
                    get->filt_id = g_strdup("0");
                }

              ret = init_override_iterator (&overrides,
                                            &get_overrides_data->get, nvt, 0,
                                            task);
              if (ret)
                {
                  switch (ret)
                    {
                      case 1:
                        if (send_find_error_to_client
                             ("get_overrides",
                              "override",
                              get_overrides_data->get.id,
                              write_to_client,
                              write_to_client_data))
                          {
                            error_send_to_client (error);
                            return;
                          }
                        break;
                      case 2:
                        if (send_find_error_to_client
                             ("get_overrides",
                              "filter",
                              get_overrides_data->get.filt_id,
                              write_to_client,
                              write_to_client_data))
                          {
                            error_send_to_client (error);
                            return;
                          }
                        break;
                      case -1:
                        SEND_TO_CLIENT_OR_FAIL
                         (XML_INTERNAL_ERROR ("get_overrides"));
                        break;
                    }
                  get_overrides_data_reset (get_overrides_data);
                  set_client_state (CLIENT_AUTHENTIC);
                  break;
                }

              count = 0;
              manage_filter_controls (get->filter, &first, NULL, NULL, NULL);
              SEND_GET_START ("override", &get_overrides_data->get);

              buffer = g_string_new ("");

              buffer_overrides_xml (buffer, &overrides,
                                    get_overrides_data->get.details,
                                    get_overrides_data->result, &count);

              SEND_TO_CLIENT_OR_FAIL (buffer->str);
              g_string_free (buffer, TRUE);

              cleanup_iterator (&overrides);
              filtered = get_overrides_data->get.id
                          ? 1
                          : override_count (&get_overrides_data->get, nvt, 0,
                                            task);
              SEND_GET_END ("override", &get_overrides_data->get, count,
                            filtered);
            }
          get_overrides_data_reset (get_overrides_data);
          set_client_state (CLIENT_AUTHENTIC);
          break;
        }

      case CLIENT_GET_PORT_LISTS:
        {
          iterator_t port_lists;
          int count, filtered, ret, first;
          get_data_t * get;

          assert (strcasecmp ("GET_PORT_LISTS", element_name) == 0);

          get = &get_port_lists_data->get;
          if ((!get->filter && !get->filt_id)
              || (get->filt_id && strcmp (get->filt_id, "-2") == 0))
            {
              char *user_filter = setting_filter ("Port Lists");

              if (user_filter && strlen (user_filter))
                {
                  get->filt_id = user_filter;
                  get->filter = filter_term (user_filter);
                }
              else
                get->filt_id = g_strdup("0");
            }

          ret = init_port_list_iterator (&port_lists,
                                         &get_port_lists_data->get);
          if (ret)
            {
              switch (ret)
                {
                  case 1:
                    if (send_find_error_to_client ("get_port_lists",
                                                   "port_list",
                                                   get_port_lists_data->get.id,
                                                   write_to_client,
                                                   write_to_client_data))
                      {
                        error_send_to_client (error);
                        return;
                      }
                    break;
                  case 2:
                    if (send_find_error_to_client
                         ("get_port_lists",
                          "port_list",
                          get_port_lists_data->get.filt_id,
                          write_to_client,
                          write_to_client_data))
                      {
                        error_send_to_client (error);
                        return;
                      }
                    break;
                  case -1:
                    SEND_TO_CLIENT_OR_FAIL
                     (XML_INTERNAL_ERROR ("get_port_lists"));
                    break;
                }
              get_port_lists_data_reset (get_port_lists_data);
              set_client_state (CLIENT_AUTHENTIC);
              break;
            }

          count = 0;
          manage_filter_controls (get->filter, &first, NULL, NULL, NULL);
          SEND_GET_START ("port_list", &get_port_lists_data->get);

          assert (strcasecmp ("GET_PORT_LISTS", element_name) == 0);
          while(1)
            {
              ret = get_next (&port_lists, get, &first, &count,
                              init_port_list_iterator);
              if (ret == 1)
                break;
              if (ret == -1)
                {
                  internal_error_send_to_client (error);
                  return;
                }

              SEND_GET_COMMON (port_list, &get_port_lists_data->get,
                               &port_lists);

              SENDF_TO_CLIENT_OR_FAIL ("<port_count>"
                                       "<all>%i</all>"
                                       "<tcp>%i</tcp>"
                                       "<udp>%i</udp>"
                                       "</port_count>",
                                       port_list_iterator_count_all
                                        (&port_lists),
                                       port_list_iterator_count_tcp
                                        (&port_lists),
                                       port_list_iterator_count_udp
                                        (&port_lists));

              if (get_port_lists_data->get.details)
                {
                  iterator_t ranges;

                  SEND_TO_CLIENT_OR_FAIL ("<port_ranges>");

                  init_port_range_iterator (&ranges,
                                            port_list_iterator_port_list (&port_lists),
                                            0, 1,
                                            NULL);
                  while (next (&ranges))
                    SENDF_TO_CLIENT_OR_FAIL
                     ("<port_range id=\"%s\">"
                      "<start>%s</start>"
                      "<end>%s</end>"
                      "<type>%s</type>"
                      "<comment>%s</comment>"
                      "</port_range>",
                      port_range_iterator_uuid (&ranges),
                      port_range_iterator_start (&ranges),
                      port_range_iterator_end (&ranges)
                       ? port_range_iterator_end (&ranges)
                       : port_range_iterator_start (&ranges),
                      port_range_iterator_type (&ranges),
                      port_range_iterator_comment (&ranges));
                  cleanup_iterator (&ranges);

                  SENDF_TO_CLIENT_OR_FAIL ("</port_ranges>");
                }

              if (get_port_lists_data->targets)
                {
                  iterator_t targets;

                  SEND_TO_CLIENT_OR_FAIL ("<targets>");

                  init_port_list_target_iterator (&targets,
                                                  port_list_iterator_port_list
                                                   (&port_lists), 0);
                  while (next (&targets))
                    SENDF_TO_CLIENT_OR_FAIL
                     ("<target id=\"%s\">"
                      "<name>%s</name>"
                      "</target>",
                      port_list_target_iterator_uuid (&targets),
                      port_list_target_iterator_name (&targets));
                  cleanup_iterator (&targets);

                  SEND_TO_CLIENT_OR_FAIL ("</targets>");
                }

              SEND_TO_CLIENT_OR_FAIL ("</port_list>");

              count++;
            }

          cleanup_iterator (&port_lists);
          filtered = get_port_lists_data->get.id
                      ? 1
                      : port_list_count (&get_port_lists_data->get);
          SEND_GET_END ("port_list", &get_port_lists_data->get, count,
                        filtered);

          get_port_lists_data_reset (get_port_lists_data);
          set_client_state (CLIENT_AUTHENTIC);
          break;
        }

      case CLIENT_DELETE_NOTE:
        assert (strcasecmp ("DELETE_NOTE", element_name) == 0);
        if (delete_note_data->note_id)
          switch (delete_note (delete_note_data->note_id,
                               delete_note_data->ultimate))
            {
              case 0:
                SEND_TO_CLIENT_OR_FAIL (XML_OK ("delete_note"));
                g_log ("event note", G_LOG_LEVEL_MESSAGE,
                       "Note %s has been deleted",
                       delete_note_data->note_id);
                break;
              case 2:
                if (send_find_error_to_client ("delete_note",
                                               "note",
                                               delete_note_data->note_id,
                                               write_to_client,
                                               write_to_client_data))
                  {
                    error_send_to_client (error);
                    return;
                  }
                g_log ("event note", G_LOG_LEVEL_MESSAGE,
                       "Note %s could not be deleted",
                       delete_note_data->note_id);
                break;
              default:
                SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("delete_note"));
                g_log ("event note", G_LOG_LEVEL_MESSAGE,
                       "Note %s could not be deleted",
                       delete_note_data->note_id);
            }
        else
          SEND_TO_CLIENT_OR_FAIL
           (XML_ERROR_SYNTAX ("delete_note",
                              "DELETE_NOTE requires a note_id attribute"));
        delete_note_data_reset (delete_note_data);
        set_client_state (CLIENT_AUTHENTIC);
        break;

      case CLIENT_DELETE_OVERRIDE:
        assert (strcasecmp ("DELETE_OVERRIDE", element_name) == 0);
        if (delete_override_data->override_id)
          switch (delete_override (delete_override_data->override_id,
                               delete_override_data->ultimate))
            {
              case 0:
                SEND_TO_CLIENT_OR_FAIL (XML_OK ("delete_override"));
                g_log ("event override", G_LOG_LEVEL_MESSAGE,
                       "Override %s has been deleted",
                       delete_override_data->override_id);
                break;
              case 2:
                if (send_find_error_to_client
                     ("delete_override",
                      "override",
                      delete_override_data->override_id,
                      write_to_client,
                      write_to_client_data))
                  {
                    error_send_to_client (error);
                    return;
                  }
                g_log ("event override", G_LOG_LEVEL_MESSAGE,
                       "Override %s could not be deleted",
                       delete_override_data->override_id);
                break;
              default:
                SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR
                                         ("delete_override"));
                g_log ("event override", G_LOG_LEVEL_MESSAGE,
                       "Override %s could not be deleted",
                       delete_override_data->override_id);
            }
        else
          SEND_TO_CLIENT_OR_FAIL
           (XML_ERROR_SYNTAX ("delete_override",
                              "DELETE_OVERRIDE requires a override_id"
                              " attribute"));
        delete_override_data_reset (delete_override_data);
        set_client_state (CLIENT_AUTHENTIC);
        break;

      case CLIENT_DELETE_REPORT:
        assert (strcasecmp ("DELETE_REPORT", element_name) == 0);
        if (delete_report_data->report_id)
          {
            report_t report;

            /** @todo Check syntax of delete_report_data->report_id and reply with
             *        STATUS_ERROR_SYNTAX.
             *
             *        This is a common situation.  If it changes here then all
             *        the commands must change.
             */
            if (find_report (delete_report_data->report_id, &report))
              SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("delete_report"));
            else if (report == 0)
              {
                if (send_find_error_to_client ("delete_report",
                                               "report",
                                               delete_report_data->report_id,
                                               write_to_client,
                                               write_to_client_data))
                  {
                    error_send_to_client (error);
                    return;
                  }
              }
            else switch (manage_delete_report (report))
              {
                case 0:
                  SEND_TO_CLIENT_OR_FAIL (XML_OK ("delete_report"));
                  break;
                case 1:
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_ERROR_SYNTAX ("delete_report",
                                      "Attempt to delete a hidden report"));
                  break;
                case 2:
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_ERROR_SYNTAX ("delete_report",
                                      "Report is in use"));
                  break;
                default:
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_INTERNAL_ERROR ("delete_report"));
                  break;
              }
          }
        else
          SEND_TO_CLIENT_OR_FAIL
           (XML_ERROR_SYNTAX ("delete_report",
                              "DELETE_REPORT requires a report_id attribute"));
        delete_report_data_reset (delete_report_data);
        set_client_state (CLIENT_AUTHENTIC);
        break;

      case CLIENT_DELETE_REPORT_FORMAT:
        assert (strcasecmp ("DELETE_REPORT_FORMAT", element_name) == 0);
        if (delete_report_format_data->report_format_id)
          {
            switch (delete_report_format
                     (delete_report_format_data->report_format_id,
                      delete_report_format_data->ultimate))
              {
                case 0:
                  SEND_TO_CLIENT_OR_FAIL (XML_OK ("delete_report_format"));
                  break;
                case 2:
                  if (send_find_error_to_client
                       ("delete_report_format",
                        "report format",
                        delete_report_format_data->report_format_id,
                        write_to_client,
                        write_to_client_data))
                    {
                      error_send_to_client (error);
                      return;
                    }
                  break;
                case 3:
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_ERROR_SYNTAX ("delete_report_format",
                                      "Attempt to delete a predefined report"
                                      " format"));
                  break;
                default:
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_INTERNAL_ERROR ("delete_report_format"));
                  break;
              }
          }
        else
          SEND_TO_CLIENT_OR_FAIL
           (XML_ERROR_SYNTAX ("delete_report_format",
                              "DELETE_REPORT_FORMAT requires a report_format_id"
                              " attribute"));
        delete_report_format_data_reset (delete_report_format_data);
        set_client_state (CLIENT_AUTHENTIC);
        break;

      case CLIENT_DELETE_SCHEDULE:
        assert (strcasecmp ("DELETE_SCHEDULE", element_name) == 0);
        if (delete_schedule_data->schedule_id)
          {
            switch (delete_schedule (delete_schedule_data->schedule_id,
                                     delete_schedule_data->ultimate))
              {
                case 0:
                  SEND_TO_CLIENT_OR_FAIL (XML_OK ("delete_schedule"));
                  g_log ("event schedule", G_LOG_LEVEL_MESSAGE,
                         "Schedule %s has been deleted",
                         delete_schedule_data->schedule_id);
                  break;
                case 1:
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_ERROR_SYNTAX ("delete_schedule",
                                      "Schedule is in use"));
                  g_log ("event schedule", G_LOG_LEVEL_MESSAGE,
                         "Schedule %s could not be deleted",
                         delete_schedule_data->schedule_id);
                  break;
                case 2:
                  if (send_find_error_to_client
                       ("delete_schedule",
                        "schedule",
                        delete_schedule_data->schedule_id,
                        write_to_client,
                        write_to_client_data))
                    {
                      error_send_to_client (error);
                      return;
                    }
                  g_log ("event schedule", G_LOG_LEVEL_MESSAGE,
                         "Schedule %s could not be deleted",
                         delete_schedule_data->schedule_id);
                  break;
                default:
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_INTERNAL_ERROR ("delete_schedule"));
                  g_log ("event schedule", G_LOG_LEVEL_MESSAGE,
                         "Schedule %s could not be deleted",
                         delete_schedule_data->schedule_id);
                  break;
              }
          }
        else
          SEND_TO_CLIENT_OR_FAIL
           (XML_ERROR_SYNTAX ("delete_schedule",
                              "DELETE_SCHEDULE requires a schedule_id"
                              " attribute"));
        delete_schedule_data_reset (delete_schedule_data);
        set_client_state (CLIENT_AUTHENTIC);
        break;

      case CLIENT_GET_REPORTS:
        assert (strcasecmp ("GET_REPORTS", element_name) == 0);
        if (current_credentials.username == NULL)
          {
            get_reports_data_reset (get_reports_data);
            SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("get_reports"));
            set_client_state (CLIENT_AUTHENTIC);
            break;
          }

        report_t request_report = 0, delta_report = 0, report;
        report_format_t report_format;
        iterator_t results;
        float min_cvss_base;
        iterator_t reports;
        get_data_t * get;

        /** @todo Some checks only required when type is "scan". */

        if (strcmp (get_reports_data->type, "scan")
            && strcmp (get_reports_data->type, "assets")
            && strcmp (get_reports_data->type, "prognostic"))
          {
            get_reports_data_reset (get_reports_data);
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("get_reports",
                                "GET_REPORTS type must be scan, assets or"
                                " prognostic"));
            set_client_state (CLIENT_AUTHENTIC);
            break;
          }

        if (strcmp (get_reports_data->type, "prognostic") == 0
            && manage_scap_loaded () == 0)
          {
            get_reports_data_reset (get_reports_data);
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("get_reports",
                                "GET_REPORTS with type prognostic requires the"
                                " SCAP database"));
            set_client_state (CLIENT_AUTHENTIC);
            break;
          }

        if ((strcmp (get_reports_data->type, "scan") == 0)
            && get_reports_data->report_id
            && find_report_for_actions (get_reports_data->report_id,
                                        &request_report,
                                        "g"))
          {
            get_reports_data_reset (get_reports_data);
            SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("get_reports"));
            set_client_state (CLIENT_AUTHENTIC);
            break;
          }

        if (get_reports_data->delta_report_id
            && strcmp (get_reports_data->delta_report_id, "0")
            && find_report_for_actions (get_reports_data->delta_report_id,
                                        &delta_report,
                                        "g"))
          {
            get_reports_data_reset (get_reports_data);
            SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("get_reports"));
            set_client_state (CLIENT_AUTHENTIC);
            break;
          }

        if (get_reports_data->format_id == NULL)
          get_reports_data->format_id
           = g_strdup ("a994b278-1f62-11e1-96ac-406186ea4fc5");

        if (find_report_format (get_reports_data->format_id, &report_format))
          {
            get_reports_data_reset (get_reports_data);
            SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("get_reports"));
            set_client_state (CLIENT_AUTHENTIC);
            break;
          }

        if (report_format == 0)
          {
            if (send_find_error_to_client ("get_reports",
                                           "report format",
                                           get_reports_data->format_id,
                                           write_to_client,
                                           write_to_client_data))
              {
                error_send_to_client (error);
                return;
              }
            get_reports_data_reset (get_reports_data);
            set_client_state (CLIENT_AUTHENTIC);
            break;
          }

        if ((strcmp (get_reports_data->type, "scan") == 0)
            && get_reports_data->report_id
            && request_report == 0)
          {
            if (send_find_error_to_client ("get_reports",
                                           "report",
                                           get_reports_data->report_id,
                                           write_to_client,
                                           write_to_client_data))
              {
                error_send_to_client (error);
                return;
              }
            get_reports_data_reset (get_reports_data);
            set_client_state (CLIENT_AUTHENTIC);
            break;
          }

        if ((strcmp (get_reports_data->type, "scan") == 0)
            && get_reports_data->delta_report_id
            && strcmp (get_reports_data->delta_report_id, "0")
            && delta_report == 0)
          {
            if (send_find_error_to_client ("get_reports",
                                           "report",
                                           get_reports_data->delta_report_id,
                                           write_to_client,
                                           write_to_client_data))
              {
                error_send_to_client (error);
                return;
              }
            get_reports_data_reset (get_reports_data);
            set_client_state (CLIENT_AUTHENTIC);
            break;
          }

        if (((strcmp (get_reports_data->type, "scan") == 0)
             || (strcmp (get_reports_data->type, "prognostic") == 0))
            && get_reports_data->min_cvss_base
            && strlen (get_reports_data->min_cvss_base)
            && (sscanf (get_reports_data->min_cvss_base,
                        "%f",
                        &min_cvss_base)
                != 1))
          {
            get_reports_data_reset (get_reports_data);
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("get_reports",
                                "GET_REPORTS min_cvss_base must be a float"
                                " or the empty string"));
            set_client_state (CLIENT_AUTHENTIC);
            break;
          }

        if (report_format_active (report_format) == 0)
          {
            get_reports_data_reset (get_reports_data);
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("get_reports",
                                "GET_REPORTS report format must be active"));
            set_client_state (CLIENT_AUTHENTIC);
            break;
          }

        if ((report_format_predefined (report_format) == 0)
            && (report_format_trust (report_format) > 1))
          {
            get_reports_data_reset (get_reports_data);
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("get_reports",
                                "GET_REPORTS report format must be predefined"
                                " or trusted"));
            set_client_state (CLIENT_AUTHENTIC);
            break;
          }

        if (strcmp (get_reports_data->type, "assets") == 0)
          {
            gchar *extension, *content_type;
            int ret, pos;

            /* An asset report. */

            get = &get_reports_data->get;
            if (get->filt_id && strcmp (get->filt_id, "-2") == 0)
              {
                g_free (get->filt_id);
                get->filt_id = g_strdup("0");
              }

            if (get_reports_data->alert_id == NULL)
              SEND_TO_CLIENT_OR_FAIL
               ("<get_reports_response"
                " status=\"" STATUS_OK "\""
                " status_text=\"" STATUS_OK_TEXT "\">");

            content_type = report_format_content_type (report_format);
            extension = report_format_extension (report_format);

            SENDF_TO_CLIENT_OR_FAIL
             ("<report"
              " type=\"assets\""
              " format_id=\"%s\""
              " extension=\"%s\""
              " content_type=\"%s\">",
              get_reports_data->format_id,
              extension,
              content_type);

            g_free (extension);
            g_free (content_type);

            pos = get_reports_data->pos ? atoi (get_reports_data->pos) : 1;
            ret = manage_send_report (0,
                                      0,
                                      report_format,
                                      &get_reports_data->get,
                                      get_reports_data->sort_order,
                                      get_reports_data->sort_field,
                                      get_reports_data->result_hosts_only,
                                      NULL,
                                      get_reports_data->levels,
                                      get_reports_data->delta_states,
                                      get_reports_data->apply_overrides,
                                      get_reports_data->search_phrase,
                                      get_reports_data->autofp,
                                      get_reports_data->show_closed_cves,
                                      get_reports_data->notes,
                                      get_reports_data->notes_details,
                                      get_reports_data->overrides,
                                      get_reports_data->overrides_details,
                                      get_reports_data->first_result,
                                      get_reports_data->max_results,
                                      /* Special case the XML report, bah. */
                                      strcmp
                                       (get_reports_data->format_id,
                                        "a994b278-1f62-11e1-96ac-406186ea4fc5"),
                                      send_to_client,
                                      write_to_client,
                                      write_to_client_data,
                                      get_reports_data->alert_id,
                                      "assets",
                                      get_reports_data->host,
                                      pos,
                                      NULL, NULL, 0, 0, NULL);

            if (ret)
              {
                internal_error_send_to_client (error);
                get_reports_data_reset (get_reports_data);
                set_client_state (CLIENT_AUTHENTIC);
                return;
              }

            SEND_TO_CLIENT_OR_FAIL ("</report>"
                                    "</get_reports_response>");

            get_reports_data_reset (get_reports_data);
            set_client_state (CLIENT_AUTHENTIC);
            break;
          }

        if (strcmp (get_reports_data->type, "prognostic") == 0)
          {
            gchar *extension, *content_type;
            int ret, pos;

            /* A prognostic report. */

            get = &get_reports_data->get;
            if (get->filt_id && strcmp (get->filt_id, "-2") == 0)
              {
                g_free (get->filt_id);
                get->filt_id = g_strdup("0");
              }

            if (get_reports_data->alert_id == NULL)
              SEND_TO_CLIENT_OR_FAIL
               ("<get_reports_response"
                " status=\"" STATUS_OK "\""
                " status_text=\"" STATUS_OK_TEXT "\">");

            content_type = report_format_content_type (report_format);
            extension = report_format_extension (report_format);

            SENDF_TO_CLIENT_OR_FAIL
             ("<report"
              " type=\"prognostic\""
              " format_id=\"%s\""
              " extension=\"%s\""
              " content_type=\"%s\">",
              get_reports_data->format_id,
              extension,
              content_type);

            g_free (extension);
            g_free (content_type);

            pos = get_reports_data->pos ? atoi (get_reports_data->pos) : 1;
            ret = manage_send_report (0,
                                      0,
                                      report_format,
                                      &get_reports_data->get,
                                      get_reports_data->sort_order,
                                      get_reports_data->sort_field,
                                      get_reports_data->result_hosts_only,
                                      get_reports_data->min_cvss_base,
                                      get_reports_data->levels,
                                      get_reports_data->delta_states,
                                      get_reports_data->apply_overrides,
                                      get_reports_data->search_phrase,
                                      get_reports_data->autofp,
                                      get_reports_data->show_closed_cves,
                                      get_reports_data->notes,
                                      get_reports_data->notes_details,
                                      get_reports_data->overrides,
                                      get_reports_data->overrides_details,
                                      get_reports_data->first_result,
                                      get_reports_data->max_results,
                                      /* Special case the XML report, bah. */
                                      strcmp
                                       (get_reports_data->format_id,
                                        "a994b278-1f62-11e1-96ac-406186ea4fc5"),
                                      send_to_client,
                                      write_to_client,
                                      write_to_client_data,
                                      get_reports_data->alert_id,
                                      "prognostic",
                                      get_reports_data->host,
                                      pos,
                                      get_reports_data->host_search_phrase,
                                      get_reports_data->host_levels,
                                      get_reports_data->host_first_result,
                                      get_reports_data->host_max_results,
                                      NULL);

            if (ret)
              {
                internal_error_send_to_client (error);
                get_reports_data_reset (get_reports_data);
                set_client_state (CLIENT_AUTHENTIC);
                return;
              }

            SEND_TO_CLIENT_OR_FAIL ("</report>"
                                    "</get_reports_response>");

            get_reports_data_reset (get_reports_data);
            set_client_state (CLIENT_AUTHENTIC);
            break;
          }

        /* The usual scan report. */

        if (request_report == 0 && get_reports_data->alert_id == NULL)
          SEND_TO_CLIENT_OR_FAIL
           ("<get_reports_response"
            " status=\"" STATUS_OK "\""
            " status_text=\"" STATUS_OK_TEXT "\">");

        get = &get_reports_data->get;
        if ((!get->filter && !get->filt_id)
            || (get->filt_id && strcmp (get->filt_id, "-2") == 0))
          {
            char *user_filter = setting_filter ("Reports");

            if (user_filter && strlen (user_filter))
              {
                get->filt_id = user_filter;
                get->filter = filter_term (user_filter);
              }
            else
              get->filt_id = g_strdup("0");
          }

        init_report_iterator (&reports, 0, request_report);
        while (next_report (&reports, &report))
          {
            gchar *extension, *content_type;
            int ret;
            GString *prefix;

            prefix = g_string_new ("");
            content_type = report_format_content_type (report_format);
            extension = report_format_extension (report_format);

            if (request_report && get_reports_data->alert_id == NULL)
              g_string_append (prefix,
                               "<get_reports_response"
                               " status=\"" STATUS_OK "\""
                               " status_text=\"" STATUS_OK_TEXT "\">");

            if (get_reports_data->alert_id == NULL)
              g_string_append_printf (prefix,
                                      "<report"
                                      " type=\"scan\""
                                      " id=\"%s\""
                                      " format_id=\"%s\""
                                      " extension=\"%s\""
                                      " content_type=\"%s\">",
                                      report_iterator_uuid (&reports),
                                      get_reports_data->format_id,
                                      extension,
                                      content_type);

            g_free (extension);
            g_free (content_type);

            /* If there's just one report then cleanup the iterator early.  This
             * closes the iterator transaction, allowing manage_schedule to lock
             * the db during generation of large reports. */
            if (request_report)
              cleanup_iterator (&reports);

            ret = manage_send_report (report,
                                      delta_report,
                                      report_format,
                                      &get_reports_data->get,
                                      get_reports_data->sort_order,
                                      get_reports_data->sort_field,
                                      get_reports_data->result_hosts_only,
                                      get_reports_data->min_cvss_base,
                                      get_reports_data->levels,
                                      get_reports_data->delta_states,
                                      get_reports_data->apply_overrides,
                                      get_reports_data->search_phrase,
                                      get_reports_data->autofp,
                                      get_reports_data->show_closed_cves,
                                      get_reports_data->notes,
                                      get_reports_data->notes_details,
                                      get_reports_data->overrides,
                                      get_reports_data->overrides_details,
                                      get_reports_data->first_result,
                                      get_reports_data->max_results,
                                      /* Special case the XML report, bah. */
                                      strcmp
                                       (get_reports_data->format_id,
                                        "a994b278-1f62-11e1-96ac-406186ea4fc5"),
                                      send_to_client,
                                      write_to_client,
                                      write_to_client_data,
                                      get_reports_data->alert_id,
                                      get_reports_data->type,
                                      NULL, 0, NULL, NULL, 0, 0, prefix->str);
            g_string_free (prefix, TRUE);
            if (ret)
              {
                if (get_reports_data->alert_id)
                  switch (ret)
                    {
                      case 0:
                        break;
                      case 1:
                        if (send_find_error_to_client
                             ("get_reports",
                              "alert",
                              get_reports_data->alert_id,
                              write_to_client,
                              write_to_client_data))
                          {
                            error_send_to_client (error);
                            return;
                          }
                        /* Close the connection with the client, as part of the
                         * response may have been sent before the error
                         * occurred. */
                        internal_error_send_to_client (error);
                        if (request_report == 0)
                          cleanup_iterator (&reports);
                        get_reports_data_reset (get_reports_data);
                        set_client_state (CLIENT_AUTHENTIC);
                        return;
                        break;
                      case 2:
                        if (send_find_error_to_client
                             ("get_reports",
                              "filter",
                              get_reports_data->get.filt_id,
                              write_to_client,
                              write_to_client_data))
                          {
                            error_send_to_client (error);
                            return;
                          }
                        /* This error always occurs before anything is sent
                         * to the client, so the connection can stay up. */
                        if (request_report == 0)
                          cleanup_iterator (&reports);
                        get_reports_data_reset (get_reports_data);
                        set_client_state (CLIENT_AUTHENTIC);
                        return;
                        break;
                      default:
                      case -1:
                        SEND_TO_CLIENT_OR_FAIL
                         (XML_INTERNAL_ERROR ("get_reports"));
                        /* Close the connection with the client, as part of the
                         * response may have been sent before the error
                         * occurred. */
                        internal_error_send_to_client (error);
                        if (request_report == 0)
                          cleanup_iterator (&reports);
                        get_reports_data_reset (get_reports_data);
                        set_client_state (CLIENT_AUTHENTIC);
                        return;
                        break;
                    }
                else if (ret == 2)
                  {
                    if (send_find_error_to_client
                         ("get_reports",
                          "filter",
                          get_reports_data->get.filt_id,
                          write_to_client,
                          write_to_client_data))
                      {
                        error_send_to_client (error);
                        return;
                      }
                    /* This error always occurs before anything is sent
                     * to the client, so the connection can stay up. */
                    if (request_report == 0)
                      cleanup_iterator (&reports);
                    get_reports_data_reset (get_reports_data);
                    set_client_state (CLIENT_AUTHENTIC);
                    return;
                    break;
                  }
                else
                  {
                    /* Close the connection with the client, as part of the
                     * response may have been sent before the error
                     * occurred. */
                    internal_error_send_to_client (error);
                    if (request_report == 0)
                      cleanup_iterator (&reports);
                    get_reports_data_reset (get_reports_data);
                    set_client_state (CLIENT_AUTHENTIC);
                    return;
                  }
              }
            if (get_reports_data->alert_id == NULL)
              SEND_TO_CLIENT_OR_FAIL ("</report>");

            if (request_report)
              /* Just to be safe, because iterator has been freed. */
              break;
          }
        if (request_report == 0)
          cleanup_iterator (&reports);

        if (get_reports_data->alert_id)
          SEND_TO_CLIENT_OR_FAIL (XML_OK ("get_reports"));
        else
          SEND_TO_CLIENT_OR_FAIL ("</get_reports_response>");

        get_reports_data_reset (get_reports_data);
        set_client_state (CLIENT_AUTHENTIC);
        break;

      case CLIENT_GET_REPORT_FORMATS:
        {
          assert (strcasecmp ("GET_REPORT_FORMATS", element_name) == 0);

          if (get_report_formats_data->params &&
              get_report_formats_data->get.trash)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("get_report_formats",
                                "GET_REPORT_FORMATS params given with trash"));
          else
            {
              iterator_t report_formats;
              int count, filtered, ret, first;
              get_data_t * get;

              get = &get_report_formats_data->get;
              if ((!get->filter && !get->filt_id)
                  || (get->filt_id && strcmp (get->filt_id, "-2") == 0))
                {
                  char *user_filter = setting_filter ("Report Formats");

                  if (user_filter && strlen (user_filter))
                    {
                      get->filt_id = user_filter;
                      get->filter = filter_term (user_filter);
                    }
                  else
                    get->filt_id = g_strdup("0");
                }

              ret = init_report_format_iterator (&report_formats,
                                                 &get_report_formats_data->get);
              if (ret)
                {
                  switch (ret)
                    {
                      case 1:
                        if (send_find_error_to_client ("get_report_formats",
                                                       "report_format",
                                                       get_report_formats_data->get.id,
                                                       write_to_client,
                                                       write_to_client_data))
                          {
                            error_send_to_client (error);
                            return;
                          }
                        break;
                      case 2:
                        if (send_find_error_to_client
                             ("get_report_formats",
                              "filter",
                              get_report_formats_data->get.filt_id,
                              write_to_client,
                              write_to_client_data))
                          {
                            error_send_to_client (error);
                            return;
                          }
                        break;
                      case -1:
                        SEND_TO_CLIENT_OR_FAIL
                         (XML_INTERNAL_ERROR ("get_report_formats"));
                        break;
                    }
                  get_report_formats_data_reset (get_report_formats_data);
                  set_client_state (CLIENT_AUTHENTIC);
                  break;
                }

              count = 0;
              manage_filter_controls (get->filter, &first, NULL, NULL, NULL);
              SEND_GET_START ("report_format", &get_report_formats_data->get);
              while (1)
                {
                  time_t trust_time;

                  ret = get_next (&report_formats, get, &first, &count,
                                  init_report_format_iterator);
                  if (ret == 1)
                    break;
                  if (ret == -1)
                    {
                      internal_error_send_to_client (error);
                      return;
                    }

                  SEND_GET_COMMON (report_format,
                                   &get_report_formats_data->get,
                                   &report_formats);

                  trust_time = report_format_iterator_trust_time
                                (&report_formats);

                  SENDF_TO_CLIENT_OR_FAIL
                   ("<extension>%s</extension>"
                    "<content_type>%s</content_type>"
                    "<summary>%s</summary>"
                    "<description>%s</description>"
                    "<global>%i</global>"
                    "<predefined>%i</predefined>",
                    report_format_iterator_extension (&report_formats),
                    report_format_iterator_content_type (&report_formats),
                    report_format_iterator_summary (&report_formats),
                    report_format_iterator_description (&report_formats),
                    report_format_global
                     (report_format_iterator_report_format
                      (&report_formats)),
                    get_report_formats_data->get.trash
                      ? 0
                      : report_format_predefined
                         (report_format_iterator_report_format
                           (&report_formats)));

                  if (get_report_formats_data->alerts)
                    {
                      iterator_t alerts;

                      SEND_TO_CLIENT_OR_FAIL ("<alerts>");
                      init_report_format_alert_iterator (&alerts,
                                                  get_iterator_resource
                                                   (&report_formats));
                      while (next (&alerts))
                        SENDF_TO_CLIENT_OR_FAIL
                         ("<alert id=\"%s\">"
                          "<name>%s</name>"
                          "</alert>",
                          report_format_alert_iterator_uuid (&alerts),
                          report_format_alert_iterator_name (&alerts));
                      cleanup_iterator (&alerts);
                      SEND_TO_CLIENT_OR_FAIL ("</alerts>");
                    }

                  if (get_report_formats_data->params
                      || get_report_formats_data->get.details)
                    {
                      iterator_t params;
                      init_report_format_param_iterator
                       (&params,
                        report_format_iterator_report_format (&report_formats),
                        get_report_formats_data->get.trash,
                        1,
                        NULL);
                      while (next (&params))
                        {
                          long long int min, max;
                          iterator_t options;

                          SENDF_TO_CLIENT_OR_FAIL
                           ("<param>"
                            "<name>%s</name>"
                            "<type>%s",
                            report_format_param_iterator_name (&params),
                            report_format_param_iterator_type_name (&params));

                          min = report_format_param_iterator_type_min (&params);
                          if (min > LLONG_MIN)
                            SENDF_TO_CLIENT_OR_FAIL ("<min>%lli</min>", min);

                          max = report_format_param_iterator_type_max (&params);
                          if (max < LLONG_MAX)
                            SENDF_TO_CLIENT_OR_FAIL ("<max>%lli</max>", max);

                          SENDF_TO_CLIENT_OR_FAIL
                           ("</type>"
                            "<value>%s</value>"
                            "<default>%s</default>",
                            report_format_param_iterator_value (&params),
                            report_format_param_iterator_fallback (&params));

                          if (report_format_param_iterator_type (&params)
                              == REPORT_FORMAT_PARAM_TYPE_SELECTION)
                            {
                              SEND_TO_CLIENT_OR_FAIL ("<options>");
                              init_param_option_iterator
                               (&options,
                                report_format_param_iterator_param
                                 (&params),
                                1,
                                NULL);
                              while (next (&options))
                                SENDF_TO_CLIENT_OR_FAIL
                                 ("<option>%s</option>",
                                  param_option_iterator_value (&options));
                              cleanup_iterator (&options);
                              SEND_TO_CLIENT_OR_FAIL ("</options>");
                            }

                          SEND_TO_CLIENT_OR_FAIL ("</param>");
                        }
                      cleanup_iterator (&params);
                    }

                  if (get_report_formats_data->get.details)
                    {
                      file_iterator_t files;
                      init_report_format_file_iterator
                       (&files,
                        report_format_iterator_report_format (&report_formats));
                      while (next_file (&files))
                        {
                          gchar *content = file_iterator_content_64 (&files);
                          SENDF_TO_CLIENT_OR_FAIL
                           ("<file name=\"%s\">%s</file>",
                            file_iterator_name (&files),
                            content);
                          g_free (content);
                        }
                      cleanup_file_iterator (&files);

                      SENDF_TO_CLIENT_OR_FAIL
                       ("<signature>%s</signature>",
                        report_format_iterator_signature (&report_formats));
                    }
                  else
                    SENDF_TO_CLIENT_OR_FAIL
                     ("<trust>%s<time>%s</time></trust>"
                      "<active>%i</active>",
                      report_format_iterator_trust (&report_formats),
                      iso_time (&trust_time),
                      report_format_iterator_active (&report_formats));

                  SEND_TO_CLIENT_OR_FAIL ("</report_format>");
                  count++;
                }
              cleanup_iterator (&report_formats);
              filtered = get_report_formats_data->get.id
                          ? 1
                          : report_format_count (&get_report_formats_data->get);
              SEND_GET_END ("report_format", &get_report_formats_data->get,
                            count, filtered);
            }
          get_report_formats_data_reset (get_report_formats_data);
          set_client_state (CLIENT_AUTHENTIC);
          break;
        }

      case CLIENT_GET_TARGET_LOCATORS:
        {
          assert (strcasecmp ("GET_TARGET_LOCATORS", element_name) == 0);
          GSList* sources = resource_request_sources (RESOURCE_TYPE_TARGET);
          GSList* source = sources;

          SEND_TO_CLIENT_OR_FAIL ("<get_target_locators_response"
                                  " status=\"" STATUS_OK "\""
                                  " status_text=\"" STATUS_OK_TEXT "\">");

          while (source)
            {
              SENDF_TO_CLIENT_OR_FAIL ("<target_locator>"
                                       "<name>%s</name>"
                                       "</target_locator>",
                                       (char*) source->data);
              source = g_slist_next (source);
            }

          SEND_TO_CLIENT_OR_FAIL ("</get_target_locators_response>");

          /* Clean up. */
          openvas_string_list_free (sources);

          set_client_state (CLIENT_AUTHENTIC);

          break;
        }

      case CLIENT_GET_RESULTS:
        {
          result_t result = 0;
          task_t task = 0;

          assert (strcasecmp ("GET_RESULTS", element_name) == 0);

          if (current_credentials.username == NULL)
            {
              get_results_data_reset (get_results_data);
              SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("get_results"));
              set_client_state (CLIENT_AUTHENTIC);
              break;
            }

          if (get_results_data->notes
              && (get_results_data->task_id == NULL))
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("get_results",
                                "GET_RESULTS must have a task_id attribute"
                                " if the notes attribute is true"));
          else if ((get_results_data->overrides
                    || get_results_data->apply_overrides)
                   && (get_results_data->task_id == NULL))
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("get_results",
                                "GET_RESULTS must have a task_id attribute"
                                " if either of the overrides attributes is"
                                " true"));
          else if (get_results_data->result_id
                   && find_result_for_actions (get_results_data->result_id,
                                               &result,
                                               "g"))
            SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("get_results"));
          else if (get_results_data->result_id && result == 0)
            {
              if (send_find_error_to_client ("get_results",
                                             "result",
                                             get_results_data->result_id,
                                             write_to_client,
                                             write_to_client_data))
                {
                  error_send_to_client (error);
                  return;
                }
            }
          else if (get_results_data->task_id
                   && find_task_for_actions (get_results_data->task_id,
                                             &task,
                                             "g"))
            SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("get_results"));
          else if (get_results_data->task_id && task == 0)
            {
              if (send_find_error_to_client ("get_results",
                                             "task",
                                             get_results_data->task_id,
                                             write_to_client,
                                             write_to_client_data))
                {
                  error_send_to_client (error);
                  return;
                }
            }
          else
            {
              SEND_TO_CLIENT_OR_FAIL ("<get_results_response"
                                      " status=\"" STATUS_OK "\""
                                      " status_text=\"" STATUS_OK_TEXT "\">"
                                      "<results>");
              init_result_iterator (&results, 0, result, 0, 1, 1, NULL,
                                    NULL, get_results_data->autofp, NULL, 0,
                                    NULL, get_results_data->apply_overrides);
              while (next (&results))
                {
                  GString *buffer = g_string_new ("");
                  buffer_results_xml (buffer,
                                      &results,
                                      task,
                                      get_results_data->notes,
                                      get_results_data->notes_details,
                                      get_results_data->overrides,
                                      get_results_data->overrides_details,
                                      NULL,
                                      NULL,
                                      0);
                  SEND_TO_CLIENT_OR_FAIL (buffer->str);
                  g_string_free (buffer, TRUE);
                }
              cleanup_iterator (&results);
              SEND_TO_CLIENT_OR_FAIL ("</results>"
                                      "</get_results_response>");
            }

          get_results_data_reset (get_results_data);
          set_client_state (CLIENT_AUTHENTIC);
          break;
        }

      case CLIENT_GET_VERSION:
      case CLIENT_GET_VERSION_AUTHENTIC:
        SEND_TO_CLIENT_OR_FAIL ("<get_version_response"
                                " status=\"" STATUS_OK "\""
                                " status_text=\"" STATUS_OK_TEXT "\">"
                                "<version>4.0</version>"
                                "</get_version_response>");
        if (client_state)
          set_client_state (CLIENT_AUTHENTIC);
        else
          set_client_state (CLIENT_TOP);
        break;

      case CLIENT_GET_SCHEDULES:
        {
          assert (strcasecmp ("GET_SCHEDULES", element_name) == 0);

          if (get_schedules_data->tasks && get_schedules_data->get.trash)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("get_schedules",
                                "GET_SCHEDULES tasks given with trash"));
          else
            {
              iterator_t schedules;
              int count, filtered, ret, first;
              get_data_t * get;

              get = &get_schedules_data->get;
              if ((!get->filter && !get->filt_id)
                  || (get->filt_id && strcmp (get->filt_id, "-2") == 0))
                {
                  char *user_filter = setting_filter ("Schedules");

                  if (user_filter && strlen (user_filter))
                    {
                      get->filt_id = user_filter;
                      get->filter = filter_term (user_filter);
                    }
                  else
                    get->filt_id = g_strdup("0");
                }

              ret = init_schedule_iterator (&schedules, &get_schedules_data->get);
              if (ret)
                {
                  switch (ret)
                    {
                      case 1:
                        if (send_find_error_to_client ("get_schedules",
                                                       "schedule",
                                                       get_schedules_data->get.id,
                                                       write_to_client,
                                                       write_to_client_data))
                          {
                            error_send_to_client (error);
                            return;
                          }
                        break;
                      case 2:
                        if (send_find_error_to_client
                             ("get_schedules",
                              "filter",
                              get_schedules_data->get.filt_id,
                              write_to_client,
                              write_to_client_data))
                          {
                            error_send_to_client (error);
                            return;
                          }
                        break;
                      case -1:
                        SEND_TO_CLIENT_OR_FAIL
                         (XML_INTERNAL_ERROR ("get_schedules"));
                        break;
                    }
                  get_schedules_data_reset (get_schedules_data);
                  set_client_state (CLIENT_AUTHENTIC);
                  break;
                }

              count = 0;
              manage_filter_controls (get->filter, &first, NULL, NULL, NULL);
              SEND_GET_START ("schedule", &get_schedules_data->get);
              while (1)
                {
                  time_t first_time, next_time;
                  gchar *iso;
                  const char *timezone;
                  char *simple_period_unit, *simple_duration_unit;
                  int period, period_minutes, period_hours, period_days;
                  int period_weeks, period_months, duration, duration_minutes;
                  int duration_hours, duration_days, duration_weeks;
                  int simple_period, simple_duration;

                  ret = get_next (&schedules, get, &first, &count,
                                  init_schedule_iterator);
                  if (ret == 1)
                    break;
                  if (ret == -1)
                    {
                      internal_error_send_to_client (error);
                      return;
                    }

                  SEND_GET_COMMON (schedule, &get_schedules_data->get, &schedules);

                  timezone = schedule_iterator_timezone (&schedules);
                  first_time = schedule_iterator_first_time (&schedules);
                  next_time = schedule_iterator_next_time (&schedules);

                  first_time += schedule_iterator_initial_offset (&schedules)
                                 - time_offset (timezone, first_time);
                  if (next_time)
                    next_time += schedule_iterator_initial_offset (&schedules)
                                   - time_offset (timezone, next_time);

                  /* Duplicate static string because there's an iso_time_tz below. */
                  iso = g_strdup (iso_time_tz (&first_time, timezone));

                  period = schedule_iterator_period (&schedules);
                  if (period)
                    {
                      period_minutes = period / 60;
                      period_hours = period_minutes / 60;
                      period_days = period_hours / 24;
                      period_weeks = period_days / 7;
                    }
                  simple_period_unit = "";
                  if (period == 0)
                    simple_period = 0;
                  else if (period_weeks && (period % (60 * 60 * 24 * 7) == 0))
                    {
                      simple_period = period_weeks;
                      simple_period_unit = "week";
                    }
                  else if (period_days && (period % (60 * 60 * 24) == 0))
                    {
                      simple_period = period_days;
                      simple_period_unit = "day";
                    }
                  else if (period_hours && (period % (60 * 60) == 0))
                    {
                      simple_period = period_hours;
                      simple_period_unit = "hour";
                    }
                  /* The granularity of the "simple" GSA interface stops at hours. */
                  else
                    simple_period = 0;

                  period_months = schedule_iterator_period_months (&schedules);
                  if (period_months && (period_months < 25))
                    {
                      simple_period = period_months;
                      simple_period_unit = "month";
                    }

                  duration = schedule_iterator_duration (&schedules);
                  if (duration)
                    {
                      duration_minutes = duration / 60;
                      duration_hours = duration_minutes / 60;
                      duration_days = duration_hours / 24;
                      duration_weeks = duration_days / 7;
                    }
                  simple_duration_unit = "";
                  if (duration == 0)
                    simple_duration = 0;
                  else if (duration_weeks
                           && (duration % (60 * 60 * 24 * 7) == 0))
                    {
                      simple_duration = duration_weeks;
                      simple_duration_unit = "week";
                    }
                  else if (duration_days
                           && (duration % (60 * 60 * 24) == 0))
                    {
                      simple_duration = duration_days;
                      simple_duration_unit = "day";
                    }
                  else if (duration_hours
                           && (duration % (60 * 60) == 0))
                    {
                      simple_duration = duration_hours;
                      simple_duration_unit = "hour";
                    }
                  /* The granularity of the "simple" GSA interface stops at hours. */
                  else
                    simple_duration = 0;

                  SENDF_TO_CLIENT_OR_FAIL
                   ("<first_time>%s</first_time>"
                    "<next_time>%s</next_time>"
                    "<period>%ld</period>"
                    "<period_months>%ld</period_months>"
                    "<simple_period>%i<unit>%s</unit></simple_period>"
                    "<duration>%ld</duration>"
                    "<simple_duration>%i<unit>%s</unit></simple_duration>"
                    "<timezone>%s</timezone>",
                    iso,
                    (next_time == 0 ? "over" : iso_time_tz (&next_time, timezone)),
                    schedule_iterator_period (&schedules),
                    schedule_iterator_period_months (&schedules),
                    simple_period,
                    simple_period_unit,
                    schedule_iterator_duration (&schedules),
                    simple_duration,
                    simple_duration_unit,
                    schedule_iterator_timezone (&schedules)
                     ? schedule_iterator_timezone (&schedules)
                     : "UTC");

                  g_free (iso);
                  if (get_schedules_data->tasks)
                    {
                      iterator_t tasks;

                      SEND_TO_CLIENT_OR_FAIL ("<tasks>");
                      init_schedule_task_iterator (&tasks,
                                                 schedule_iterator_schedule
                                                  (&schedules));
                      while (next (&tasks))
                        SENDF_TO_CLIENT_OR_FAIL ("<task id=\"%s\">"
                                                 "<name>%s</name>"
                                                 "</task>",
                                                 schedule_task_iterator_uuid (&tasks),
                                                 schedule_task_iterator_name (&tasks));
                      cleanup_iterator (&tasks);
                      SEND_TO_CLIENT_OR_FAIL ("</tasks>");
                    }
                  SEND_TO_CLIENT_OR_FAIL ("</schedule>");
                  count++;
                }
              cleanup_iterator (&schedules);
              filtered = get_schedules_data->get.id
                          ? 1
                          : schedule_count (&get_schedules_data->get);
              SEND_GET_END ("schedule", &get_schedules_data->get, count, filtered);
            }
          get_schedules_data_reset (get_schedules_data);
          set_client_state (CLIENT_AUTHENTIC);
          break;
        }

      case CLIENT_DELETE_AGENT:
        assert (strcasecmp ("DELETE_AGENT", element_name) == 0);
        if (delete_agent_data->agent_id)
          {
            switch (delete_agent (delete_agent_data->agent_id,
                                  delete_agent_data->ultimate))
              {
                case 0:
                  SEND_TO_CLIENT_OR_FAIL (XML_OK ("delete_agent"));
                  break;
                case 1:
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_ERROR_SYNTAX ("delete_agent",
                                      "Agent is in use"));
                  break;
                case 2:
                  if (send_find_error_to_client ("delete_agent",
                                                 "agent",
                                                 delete_agent_data->agent_id,
                                                 write_to_client,
                                                 write_to_client_data))
                    {
                      error_send_to_client (error);
                      return;
                    }
                  break;
                default:
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_INTERNAL_ERROR ("delete_agent"));
              }
          }
        else
          SEND_TO_CLIENT_OR_FAIL
           (XML_ERROR_SYNTAX ("delete_agent",
                              "DELETE_AGENT requires an agent_id"
                              " attribute"));
        delete_agent_data_reset (delete_agent_data);
        set_client_state (CLIENT_AUTHENTIC);
        break;

      case CLIENT_DELETE_CONFIG:
        assert (strcasecmp ("DELETE_CONFIG", element_name) == 0);
        if (delete_config_data->config_id)
          {
            switch (delete_config (delete_config_data->config_id,
                                   delete_config_data->ultimate))
              {
                case 0:
                  SEND_TO_CLIENT_OR_FAIL (XML_OK ("delete_config"));
                  g_log ("event config", G_LOG_LEVEL_MESSAGE,
                         "Scan config %s has been deleted",
                         delete_config_data->config_id);
                  break;
                case 1:
                  SEND_TO_CLIENT_OR_FAIL (XML_ERROR_SYNTAX ("delete_config",
                                                            "Config is in use"));
                  g_log ("event config", G_LOG_LEVEL_MESSAGE,
                         "Scan config %s could not be deleted",
                         delete_config_data->config_id);
                  break;
                case 2:
                  if (send_find_error_to_client ("delete_config",
                                                 "config",
                                                 delete_config_data->config_id,
                                                 write_to_client,
                                                 write_to_client_data))
                    {
                      error_send_to_client (error);
                      return;
                    }
                  g_log ("event config", G_LOG_LEVEL_MESSAGE,
                         "Scan config %s could not be deleted",
                         delete_config_data->config_id);
                  break;
                default:
                  SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("delete_config"));
                  g_log ("event config", G_LOG_LEVEL_MESSAGE,
                         "Scan config %s could not be deleted",
                         delete_config_data->config_id);
              }
          }
        else
          SEND_TO_CLIENT_OR_FAIL
           (XML_ERROR_SYNTAX ("delete_config",
                              "DELETE_CONFIG requires a config_id attribute"));
        delete_config_data_reset (delete_config_data);
        set_client_state (CLIENT_AUTHENTIC);
        break;

      case CLIENT_DELETE_ALERT:
        assert (strcasecmp ("DELETE_ALERT", element_name) == 0);
        if (delete_alert_data->alert_id)
          {
            switch (delete_alert (delete_alert_data->alert_id,
                                      delete_alert_data->ultimate))
              {
                case 0:
                  SEND_TO_CLIENT_OR_FAIL (XML_OK ("delete_alert"));
                  g_log ("event alert", G_LOG_LEVEL_MESSAGE,
                         "Alert %s has been deleted",
                         delete_alert_data->alert_id);
                  break;
                case 1:
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_ERROR_SYNTAX ("delete_alert",
                                      "Alert is in use"));
                  g_log ("event alert", G_LOG_LEVEL_MESSAGE,
                         "Alert %s could not be deleted",
                         delete_alert_data->alert_id);
                  break;
                case 2:
                  if (send_find_error_to_client
                       ("delete_alert",
                        "alert",
                        delete_alert_data->alert_id,
                        write_to_client,
                        write_to_client_data))
                    {
                      error_send_to_client (error);
                      return;
                    }
                  g_log ("event alert", G_LOG_LEVEL_MESSAGE,
                         "Alert %s could not be deleted",
                         delete_alert_data->alert_id);
                  break;
                default:
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_INTERNAL_ERROR ("delete_alert"));
                  g_log ("event alert", G_LOG_LEVEL_MESSAGE,
                         "Alert %s could not be deleted",
                         delete_alert_data->alert_id);
              }
          }
        else
          SEND_TO_CLIENT_OR_FAIL
           (XML_ERROR_SYNTAX ("delete_alert",
                              "DELETE_ALERT requires an alert_id"
                              " attribute"));
        delete_alert_data_reset (delete_alert_data);
        set_client_state (CLIENT_AUTHENTIC);
        break;

      case CLIENT_DELETE_FILTER:
        assert (strcasecmp ("DELETE_FILTER", element_name) == 0);
        if (delete_filter_data->filter_id)
          switch (delete_filter (delete_filter_data->filter_id,
                                 delete_filter_data->ultimate))
            {
              case 0:
                SEND_TO_CLIENT_OR_FAIL (XML_OK ("delete_filter"));
                g_log ("event filter", G_LOG_LEVEL_MESSAGE,
                       "Filter %s has been deleted",
                       delete_filter_data->filter_id);
                break;
              case 1:
                SEND_TO_CLIENT_OR_FAIL (XML_ERROR_SYNTAX ("delete_filter",
                                                          "Filter is in use"));
                g_log ("event filter", G_LOG_LEVEL_MESSAGE,
                       "Filter %s could not be deleted",
                       delete_filter_data->filter_id);
                break;
              case 2:
                if (send_find_error_to_client ("delete_filter",
                                               "filter",
                                               delete_filter_data->filter_id,
                                               write_to_client,
                                               write_to_client_data))
                  {
                    error_send_to_client (error);
                    return;
                  }
                g_log ("event filter", G_LOG_LEVEL_MESSAGE,
                       "Filter %s could not be deleted",
                       delete_filter_data->filter_id);
                break;
              case 3:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("delete_filter",
                                    "Attempt to delete a predefined"
                                    " filter"));
                break;
              default:
                SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("delete_filter"));
                g_log ("event filter", G_LOG_LEVEL_MESSAGE,
                       "Filter %s could not be deleted",
                       delete_filter_data->filter_id);
            }
        else
          SEND_TO_CLIENT_OR_FAIL
           (XML_ERROR_SYNTAX ("delete_filter",
                              "DELETE_FILTER requires a filter_id attribute"));
        delete_filter_data_reset (delete_filter_data);
        set_client_state (CLIENT_AUTHENTIC);
        break;

      case CLIENT_DELETE_LSC_CREDENTIAL:
        assert (strcasecmp ("DELETE_LSC_CREDENTIAL", element_name) == 0);
        if (delete_lsc_credential_data->lsc_credential_id)
          switch (delete_lsc_credential
                   (delete_lsc_credential_data->lsc_credential_id,
                    delete_lsc_credential_data->ultimate))
            {
              case 0:
                SEND_TO_CLIENT_OR_FAIL (XML_OK ("delete_lsc_credential"));
                break;
              case 1:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("delete_lsc_credential",
                                    "LSC credential is in use"));
                break;
              case 2:
                if (send_find_error_to_client
                     ("delete_lsc_credential",
                      "LSC credential",
                      delete_lsc_credential_data->lsc_credential_id,
                      write_to_client,
                      write_to_client_data))

                  {
                    error_send_to_client (error);
                    return;
                  }
                break;
              default:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_INTERNAL_ERROR ("delete_lsc_credential"));
            }
        else
          SEND_TO_CLIENT_OR_FAIL
           (XML_ERROR_SYNTAX ("delete_lsc_credential",
                              "DELETE_LSC_CREDENTIAL requires an"
                              " lsc_credential_id attribute"));
        delete_lsc_credential_data_reset (delete_lsc_credential_data);
        set_client_state (CLIENT_AUTHENTIC);
        break;

      case CLIENT_DELETE_PORT_LIST:
        assert (strcasecmp ("DELETE_PORT_LIST", element_name) == 0);
        if (delete_port_list_data->port_list_id)
          switch (delete_port_list (delete_port_list_data->port_list_id,
                                    delete_port_list_data->ultimate))
            {
              case 0:
                SEND_TO_CLIENT_OR_FAIL (XML_OK ("delete_port_list"));
                g_log ("event port_list", G_LOG_LEVEL_MESSAGE,
                       "Port_List %s has been deleted",
                       delete_port_list_data->port_list_id);
                break;
              case 1:
                SEND_TO_CLIENT_OR_FAIL (XML_ERROR_SYNTAX ("delete_port_list",
                                                          "Port list is in use"));
                g_log ("event port_list", G_LOG_LEVEL_MESSAGE,
                       "Port list %s could not be deleted",
                       delete_port_list_data->port_list_id);
                break;
              case 2:
                if (send_find_error_to_client ("delete_port_list",
                                               "port_list",
                                               delete_port_list_data->port_list_id,
                                               write_to_client,
                                               write_to_client_data))
                  {
                    error_send_to_client (error);
                    return;
                  }
                g_log ("event port_list", G_LOG_LEVEL_MESSAGE,
                       "Port list %s could not be deleted",
                       delete_port_list_data->port_list_id);
                break;
              case 3:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("delete_port_list",
                                    "Attempt to delete a predefined port"
                                    " list"));
                g_log ("event port_list", G_LOG_LEVEL_MESSAGE,
                       "Port list %s could not be deleted",
                       delete_port_list_data->port_list_id);
                break;
              default:
                SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("delete_port_list"));
                g_log ("event port_list", G_LOG_LEVEL_MESSAGE,
                       "Port list %s could not be deleted",
                       delete_port_list_data->port_list_id);
            }
        else
          SEND_TO_CLIENT_OR_FAIL
           (XML_ERROR_SYNTAX ("delete_port_list",
                              "DELETE_PORT_LIST requires a port_list_id attribute"));
        delete_port_list_data_reset (delete_port_list_data);
        set_client_state (CLIENT_AUTHENTIC);
        break;

      case CLIENT_DELETE_PORT_RANGE:
        assert (strcasecmp ("DELETE_PORT_RANGE", element_name) == 0);
        if (delete_port_range_data->port_range_id)
          switch (delete_port_range (delete_port_range_data->port_range_id))
            {
              case 0:
                SEND_TO_CLIENT_OR_FAIL (XML_OK ("delete_port_range"));
                g_log ("event port_range", G_LOG_LEVEL_MESSAGE,
                       "Port_Range %s has been deleted",
                       delete_port_range_data->port_range_id);
                break;
              case 1:
                if (send_find_error_to_client ("delete_port_range",
                                               "port_range",
                                               delete_port_range_data->port_range_id,
                                               write_to_client,
                                               write_to_client_data))
                  {
                    error_send_to_client (error);
                    return;
                  }
                g_log ("event port_range", G_LOG_LEVEL_MESSAGE,
                       "Port range %s could not be deleted",
                       delete_port_range_data->port_range_id);
                break;
              case 2:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("delete_port_range",
                                    "Port range belongs to predefined port"
                                    " list"));
                g_log ("event port_range", G_LOG_LEVEL_MESSAGE,
                       "Port range %s could not be deleted",
                       delete_port_range_data->port_range_id);
                break;
              default:
                SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("delete_port_range"));
                g_log ("event port_range", G_LOG_LEVEL_MESSAGE,
                       "Port range %s could not be deleted",
                       delete_port_range_data->port_range_id);
            }
        else
          SEND_TO_CLIENT_OR_FAIL
           (XML_ERROR_SYNTAX ("delete_port_range",
                              "DELETE_PORT_RANGE requires a port_range_id attribute"));
        delete_port_range_data_reset (delete_port_range_data);
        set_client_state (CLIENT_AUTHENTIC);
        break;

      case CLIENT_DELETE_SLAVE:
        assert (strcasecmp ("DELETE_SLAVE", element_name) == 0);
        if (delete_slave_data->slave_id)
          {
            switch (delete_slave (delete_slave_data->slave_id,
                                  delete_slave_data->ultimate))
              {
                case 0:
                  SEND_TO_CLIENT_OR_FAIL (XML_OK ("delete_slave"));
                  g_log ("event slave", G_LOG_LEVEL_MESSAGE,
                         "Slave %s has been deleted",
                         delete_slave_data->slave_id);
                  break;
                case 1:
                  SEND_TO_CLIENT_OR_FAIL (XML_ERROR_SYNTAX ("delete_slave",
                                                            "Slave is in use"));
                  g_log ("event slave", G_LOG_LEVEL_MESSAGE,
                         "Slave %s could not be deleted",
                         delete_slave_data->slave_id);
                  break;
                case 2:
                  if (send_find_error_to_client ("delete_slave",
                                                 "slave",
                                                 delete_slave_data->slave_id,
                                                 write_to_client,
                                                 write_to_client_data))
                    {
                      error_send_to_client (error);
                      return;
                    }
                  break;
                default:
                  SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("delete_slave"));
                  g_log ("event slave", G_LOG_LEVEL_MESSAGE,
                         "Slave %s could not be deleted",
                         delete_slave_data->slave_id);
              }
          }
        else
          SEND_TO_CLIENT_OR_FAIL
           (XML_ERROR_SYNTAX ("delete_slave",
                              "DELETE_SLAVE requires a slave_id attribute"));
        delete_slave_data_reset (delete_slave_data);
        set_client_state (CLIENT_AUTHENTIC);
        break;

      case CLIENT_DELETE_TARGET:
        assert (strcasecmp ("DELETE_TARGET", element_name) == 0);
        if (delete_target_data->target_id)
          switch (delete_target (delete_target_data->target_id,
                                 delete_target_data->ultimate))
            {
              case 0:
                SEND_TO_CLIENT_OR_FAIL (XML_OK ("delete_target"));
                g_log ("event target", G_LOG_LEVEL_MESSAGE,
                       "Target %s has been deleted",
                       delete_target_data->target_id);
                break;
              case 1:
                SEND_TO_CLIENT_OR_FAIL (XML_ERROR_SYNTAX ("delete_target",
                                                          "Target is in use"));
                g_log ("event target", G_LOG_LEVEL_MESSAGE,
                       "Target %s could not be deleted",
                       delete_target_data->target_id);
                break;
              case 2:
                if (send_find_error_to_client ("delete_target",
                                               "target",
                                               delete_target_data->target_id,
                                               write_to_client,
                                               write_to_client_data))
                  {
                    error_send_to_client (error);
                    return;
                  }
                g_log ("event target", G_LOG_LEVEL_MESSAGE,
                       "Target %s could not be deleted",
                       delete_target_data->target_id);
                break;
              case 3:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("delete_target",
                                    "Attempt to delete a predefined"
                                    " target"));
                break;
              default:
                SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("delete_target"));
                g_log ("event target", G_LOG_LEVEL_MESSAGE,
                       "Target %s could not be deleted",
                       delete_target_data->target_id);
            }
        else
          SEND_TO_CLIENT_OR_FAIL
           (XML_ERROR_SYNTAX ("delete_target",
                              "DELETE_TARGET requires a target_id attribute"));
        delete_target_data_reset (delete_target_data);
        set_client_state (CLIENT_AUTHENTIC);
        break;

      case CLIENT_DELETE_TASK:
        if (delete_task_data->task_id)
          {
            switch (request_delete_task_uuid (delete_task_data->task_id,
                                              delete_task_data->ultimate))
              {
                case 0:    /* Deleted. */
                  SEND_TO_CLIENT_OR_FAIL (XML_OK ("delete_task"));
                  g_log ("event task", G_LOG_LEVEL_MESSAGE,
                         "Task %s has been deleted",
                         delete_task_data->task_id);
                  break;
                case 1:    /* Delete requested. */
                  SEND_TO_CLIENT_OR_FAIL (XML_OK_REQUESTED ("delete_task"));
                  g_log ("event task", G_LOG_LEVEL_MESSAGE,
                         "Deletion of task %s has been requested",
                         delete_task_data->task_id);
                  break;
                case 2:    /* Hidden task. */
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_ERROR_SYNTAX ("delete_task",
                                      "Attempt to delete a hidden task"));
                  g_log ("event task", G_LOG_LEVEL_MESSAGE,
                         "Task %s could not be deleted",
                         delete_task_data->task_id);
                  break;
                case 3:  /* Failed to find task. */
                  if (send_find_error_to_client
                       ("delete_task",
                        "task",
                        delete_task_data->task_id,
                        write_to_client,
                        write_to_client_data))
                    {
                      error_send_to_client (error);
                      return;
                    }
                  break;
                default:   /* Programming error. */
                  assert (0);
                case -1:
                  /* to_scanner is full. */
                  /** @todo Or some other error occurred. */
                  /** @todo Consider reverting parsing for retry. */
                  /** @todo process_omp_client_input must return -2. */
                  tracef ("delete_task failed\n");
                  abort ();
                  break;
              }
          }
        else
          SEND_TO_CLIENT_OR_FAIL
           (XML_ERROR_SYNTAX ("delete_task",
                              "DELETE_TASK requires a task_id attribute"));
        delete_task_data_reset (delete_task_data);
        set_client_state (CLIENT_AUTHENTIC);
        break;

      case CLIENT_HELP:
        if (help_data->format == NULL
            || (strcmp (help_data->format, "text") == 0))
          {
            command_t *commands;
            SEND_TO_CLIENT_OR_FAIL ("<help_response"
                                    " status=\"" STATUS_OK "\""
                                    " status_text=\"" STATUS_OK_TEXT "\">\n");
            commands = omp_commands;
            while ((*commands).name)
              {
                if (command_disabled (omp_parser, (*commands).name) == 0)
                  {
                    int count;
                    SENDF_TO_CLIENT_OR_FAIL ("    %s",
                                             (*commands).name);
                    for (count = 23 - strlen ((*commands).name);
                         count > 0;
                         count--)
                      SEND_TO_CLIENT_OR_FAIL (" ");
                    SENDF_TO_CLIENT_OR_FAIL ("%s\n",
                                             (*commands).summary);
                  }
                commands++;
              }
            SEND_TO_CLIENT_OR_FAIL ("</help_response>");
          }
        else if (help_data->type && (strcmp (help_data->type, "brief") == 0))
          {
            command_t *commands;
            SEND_TO_CLIENT_OR_FAIL ("<help_response"
                                    " status=\"" STATUS_OK "\""
                                    " status_text=\"" STATUS_OK_TEXT "\">\n"
                                    "<schema"
                                    " format=\"XML\""
                                    " extension=\"xml\""
                                    " content_type=\"text/xml\">");
            commands = omp_commands;
            while ((*commands).name)
              {
                if (command_disabled (omp_parser, (*commands).name) == 0)
                  SENDF_TO_CLIENT_OR_FAIL ("<command>"
                                           "<name>%s</name>"
                                           "<summary>%s</summary>"
                                           "</command>",
                                           (*commands).name,
                                           (*commands).summary);
                commands++;
              }
            SEND_TO_CLIENT_OR_FAIL ("</schema>"
                                    "</help_response>");
          }
        else
          {
            gchar *extension, *content_type, *output;
            gsize output_len;

            switch (manage_schema (help_data->format,
                                   &output,
                                   &output_len,
                                   &extension,
                                   &content_type))
              {
                case 0:
                  break;
                case 1:
                  assert (help_data->format);
                  if (send_find_error_to_client ("help",
                                                 "schema_format",
                                                 help_data->format,
                                                 write_to_client,
                                                 write_to_client_data))
                    {
                      error_send_to_client (error);
                      return;
                    }
                  help_data_reset (help_data);
                  set_client_state (CLIENT_AUTHENTIC);
                  return;
                  break;
                case 2:
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_ERROR_SYNTAX ("help",
                                      "Brief help is only available in XML."));
                  help_data_reset (help_data);
                  set_client_state (CLIENT_AUTHENTIC);
                  return;
                  break;
                default:
                  SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("help"));
                  help_data_reset (help_data);
                  set_client_state (CLIENT_AUTHENTIC);
                  return;
                  break;
              }

            SENDF_TO_CLIENT_OR_FAIL ("<help_response"
                                     " status=\"" STATUS_OK "\""
                                     " status_text=\"" STATUS_OK_TEXT "\">"
                                     "<schema"
                                     " format=\"%s\""
                                     " extension=\"%s\""
                                     " content_type=\"%s\">",
                                     help_data->format
                                      ? help_data->format
                                      : "XML",
                                     extension,
                                     content_type);
            g_free (extension);
            g_free (content_type);

            if (output && strlen (output))
              {
                /* Encode and send the output. */

                if (help_data->format
                    && strcasecmp (help_data->format, "XML"))
                  {
                    gchar *base64;

                    base64 = g_base64_encode ((guchar*) output, output_len);
                    if (send_to_client (base64,
                                        write_to_client,
                                        write_to_client_data))
                      {
                        g_free (output);
                        g_free (base64);
                        error_send_to_client (error);
                        return;
                      }
                    g_free (base64);
                  }
                else
                  {
                    /* Special case the XML schema, bah. */
                    if (send_to_client (output,
                                        write_to_client,
                                        write_to_client_data))
                      {
                        g_free (output);
                        error_send_to_client (error);
                        return;
                      }
                  }
              }
            g_free (output);
            SEND_TO_CLIENT_OR_FAIL ("</schema>"
                                    "</help_response>");
          }
        help_data_reset (help_data);
        set_client_state (CLIENT_AUTHENTIC);
        break;

      case CLIENT_MODIFY_CONFIG:
        {
          config_t config;
          if (modify_config_data->config_id == NULL
              || strlen (modify_config_data->config_id) == 0)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("modify_config",
                                "MODIFY_CONFIG requires a config_id"
                                " attribute"));
          else if ((modify_config_data->nvt_selection_family
                    /* This array implies FAMILY_SELECTION. */
                    && modify_config_data->families_static_all)
                   || ((modify_config_data->nvt_selection_family
                        || modify_config_data->families_static_all)
                       && (modify_config_data->preference_name
                           || modify_config_data->preference_value
                           || modify_config_data->preference_nvt_oid)))
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("modify_config",
                                "MODIFY_CONFIG requires either a PREFERENCE or"
                                " an NVT_SELECTION or a FAMILY_SELECTION"));
          else if (find_config (modify_config_data->config_id, &config))
            SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("modify_config"));
          else if (config == 0)
            {
              if (send_find_error_to_client ("modify_config",
                                             "config",
                                             modify_config_data->config_id,
                                             write_to_client,
                                             write_to_client_data))
                {
                  error_send_to_client (error);
                  return;
                }
            }
          else if (modify_config_data->nvt_selection_family)
            {
              switch (manage_set_config_nvts
                       (config,
                        modify_config_data->nvt_selection_family,
                        modify_config_data->nvt_selection))
                {
                  case 0:
                    SEND_TO_CLIENT_OR_FAIL (XML_OK ("modify_config"));
                    g_log ("event config", G_LOG_LEVEL_MESSAGE,
                           "Scan config %s has been modified",
                           modify_config_data->config_id);
                    break;
                  case 1:
                    SEND_TO_CLIENT_OR_FAIL
                     (XML_ERROR_SYNTAX ("modify_config", "Config is in use"));
                    g_log ("event config", G_LOG_LEVEL_MESSAGE,
                           "Scan config %s could not be modified",
                           modify_config_data->config_id);
                    break;
#if 0
                  case -1:
                    SEND_TO_CLIENT_OR_FAIL
                     (XML_ERROR_SYNTAX ("modify_config",
                                        "MODIFY_CONFIG PREFERENCE requires at"
                                        " least one of the VALUE and NVT"
                                        " elements"));
                    break;
#endif
                  default:
                    SEND_TO_CLIENT_OR_FAIL
                     (XML_INTERNAL_ERROR ("modify_config"));
                    g_log ("event config", G_LOG_LEVEL_MESSAGE,
                           "Scan config %s could not be modified",
                           modify_config_data->config_id);
                    break;
                }
            }
          else if (modify_config_data->families_static_all)
            {
              /* There was a FAMILY_SELECTION. */

              switch (manage_set_config_families
                       (config,
                        modify_config_data->families_growing_all,
                        modify_config_data->families_static_all,
                        modify_config_data->families_growing_empty,
                        modify_config_data->family_selection_growing))
                {
                  case 0:
                    SEND_TO_CLIENT_OR_FAIL (XML_OK ("modify_config"));
                    g_log ("event config", G_LOG_LEVEL_MESSAGE,
                           "Scan config %s has been modified",
                           modify_config_data->config_id);
                    break;
                  case 1:
                    SEND_TO_CLIENT_OR_FAIL
                     (XML_ERROR_SYNTAX ("modify_config", "Config is in use"));
                    g_log ("event config", G_LOG_LEVEL_MESSAGE,
                           "Scan config %s could not be modified",
                           modify_config_data->config_id);
                    break;
#if 0
                  case -1:
                    SEND_TO_CLIENT_OR_FAIL
                     (XML_ERROR_SYNTAX ("modify_config",
                                        "MODIFY_CONFIG PREFERENCE requires at"
                                        " least one of the VALUE and NVT"
                                        " elements"));
                    break;
#endif
                  default:
                    SEND_TO_CLIENT_OR_FAIL
                     (XML_INTERNAL_ERROR ("modify_config"));
                    g_log ("event config", G_LOG_LEVEL_MESSAGE,
                           "Scan config %s could not be modified",
                           modify_config_data->config_id);
                    break;
                }
            }
          else if (modify_config_data->name && modify_config_data->comment)
            switch (manage_set_config_name_comment (config,
                                                    modify_config_data->name,
                                                    modify_config_data->comment))
              {
                case 0:
                  SEND_TO_CLIENT_OR_FAIL (XML_OK ("modify_config"));
                  break;
                case 1:
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_ERROR_SYNTAX ("modify_config",
                                      "MODIFY_CONFIG name must be unique"));
                  break;
                case -1:
                  SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("modify_config"));
                  break;
                default:
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_INTERNAL_ERROR ("modify_config"));
                  break;
              }
          else if (modify_config_data->name)
            switch (manage_set_config_name (config, modify_config_data->name))
              {
                case 0:
                  SEND_TO_CLIENT_OR_FAIL (XML_OK ("modify_config"));
                  break;
                case 1:
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_ERROR_SYNTAX ("modify_config",
                                      "MODIFY_CONFIG name must be unique"));
                  break;
                case -1:
                  SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("modify_config"));
                  break;
                default:
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_INTERNAL_ERROR ("modify_config"));
                  break;
              }
          else if (modify_config_data->comment)
            switch (manage_set_config_comment (config,
                                               modify_config_data->comment))
              {
                case 0:
                  SEND_TO_CLIENT_OR_FAIL (XML_OK ("modify_config"));
                  break;
                case -1:
                  SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("modify_config"));
                  break;
                default:
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_INTERNAL_ERROR ("modify_config"));
                  break;
              }
          else if (modify_config_data->preference_name == NULL
                   || strlen (modify_config_data->preference_name) == 0)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("modify_config",
                                "MODIFY_CONFIG PREFERENCE requires a NAME"
                                " element"));
          else switch (manage_set_config_preference
                        (config,
                         modify_config_data->preference_nvt_oid,
                         modify_config_data->preference_name,
                         modify_config_data->preference_value))
            {
              case 0:
                SEND_TO_CLIENT_OR_FAIL (XML_OK ("modify_config"));
                break;
              case 1:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("modify_config", "Config is in use"));
                break;
              case 2:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("modify_config", "Empty radio value"));
                break;
              case -1:
                SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("modify_config"));
                break;
              default:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_INTERNAL_ERROR ("modify_config"));
                break;
            }
        }
        modify_config_data_reset (modify_config_data);
        set_client_state (CLIENT_AUTHENTIC);
        break;
      CLOSE (CLIENT_MODIFY_CONFIG, COMMENT);
      case CLIENT_MODIFY_CONFIG_FAMILY_SELECTION:
        assert (strcasecmp ("FAMILY_SELECTION", element_name) == 0);
        assert (modify_config_data->families_growing_all);
        assert (modify_config_data->families_static_all);
        assert (modify_config_data->families_growing_empty);
        array_terminate (modify_config_data->families_growing_all);
        array_terminate (modify_config_data->families_static_all);
        array_terminate (modify_config_data->families_growing_empty);
        set_client_state (CLIENT_MODIFY_CONFIG);
        break;
      CLOSE (CLIENT_MODIFY_CONFIG, NAME);
      case CLIENT_MODIFY_CONFIG_NVT_SELECTION:
        assert (strcasecmp ("NVT_SELECTION", element_name) == 0);
        assert (modify_config_data->nvt_selection);
        array_terminate (modify_config_data->nvt_selection);
        set_client_state (CLIENT_MODIFY_CONFIG);
        break;
      CLOSE (CLIENT_MODIFY_CONFIG, PREFERENCE);

      case CLIENT_MODIFY_CONFIG_FAMILY_SELECTION_FAMILY:
        assert (strcasecmp ("FAMILY", element_name) == 0);
        if (modify_config_data->family_selection_family_name)
          {
            if (modify_config_data->family_selection_family_growing)
              {
                if (modify_config_data->family_selection_family_all)
                  /* Growing 1 and select all 1. */
                  array_add (modify_config_data->families_growing_all,
                             modify_config_data->family_selection_family_name);
                else
                  /* Growing 1 and select all 0. */
                  array_add (modify_config_data->families_growing_empty,
                             modify_config_data->family_selection_family_name);
              }
            else
              {
                if (modify_config_data->family_selection_family_all)
                  /* Growing 0 and select all 1. */
                  array_add (modify_config_data->families_static_all,
                             modify_config_data->family_selection_family_name);
                /* Else growing 0 and select all 0. */
              }
          }
        modify_config_data->family_selection_family_name = NULL;
        set_client_state (CLIENT_MODIFY_CONFIG_FAMILY_SELECTION);
        break;
      case CLIENT_MODIFY_CONFIG_FAMILY_SELECTION_GROWING:
        assert (strcasecmp ("GROWING", element_name) == 0);
        if (modify_config_data->family_selection_growing_text)
          {
            modify_config_data->family_selection_growing
             = atoi (modify_config_data->family_selection_growing_text);
            openvas_free_string_var
             (&modify_config_data->family_selection_growing_text);
          }
        else
          modify_config_data->family_selection_growing = 0;
        set_client_state (CLIENT_MODIFY_CONFIG_FAMILY_SELECTION);
        break;

      case CLIENT_MODIFY_CONFIG_FAMILY_SELECTION_FAMILY_ALL:
        assert (strcasecmp ("ALL", element_name) == 0);
        if (modify_config_data->family_selection_family_all_text)
          {
            modify_config_data->family_selection_family_all
             = atoi (modify_config_data->family_selection_family_all_text);
            openvas_free_string_var
             (&modify_config_data->family_selection_family_all_text);
          }
        else
          modify_config_data->family_selection_family_all = 0;
        set_client_state (CLIENT_MODIFY_CONFIG_FAMILY_SELECTION_FAMILY);
        break;
      CLOSE (CLIENT_MODIFY_CONFIG_FAMILY_SELECTION_FAMILY, NAME);
      case CLIENT_MODIFY_CONFIG_FAMILY_SELECTION_FAMILY_GROWING:
        assert (strcasecmp ("GROWING", element_name) == 0);
        if (modify_config_data->family_selection_family_growing_text)
          {
            modify_config_data->family_selection_family_growing
             = atoi (modify_config_data->family_selection_family_growing_text);
            openvas_free_string_var
             (&modify_config_data->family_selection_family_growing_text);
          }
        else
          modify_config_data->family_selection_family_growing = 0;
        set_client_state (CLIENT_MODIFY_CONFIG_FAMILY_SELECTION_FAMILY);
        break;

      CLOSE (CLIENT_MODIFY_CONFIG_NVT_SELECTION, FAMILY);
      case CLIENT_MODIFY_CONFIG_NVT_SELECTION_NVT:
        assert (strcasecmp ("NVT", element_name) == 0);
        if (modify_config_data->nvt_selection_nvt_oid)
          array_add (modify_config_data->nvt_selection,
                     modify_config_data->nvt_selection_nvt_oid);
        modify_config_data->nvt_selection_nvt_oid = NULL;
        set_client_state (CLIENT_MODIFY_CONFIG_NVT_SELECTION);
        break;

      CLOSE (CLIENT_MODIFY_CONFIG_PREFERENCE, NAME);
      CLOSE (CLIENT_MODIFY_CONFIG_PREFERENCE, NVT);
      case CLIENT_MODIFY_CONFIG_PREFERENCE_VALUE:
        assert (strcasecmp ("VALUE", element_name) == 0);
        /* Init, so it's the empty string when the value is empty. */
        openvas_append_string (&modify_config_data->preference_value, "");
        set_client_state (CLIENT_MODIFY_CONFIG_PREFERENCE);
        break;

      case CLIENT_MODIFY_LSC_CREDENTIAL:
        {
          lsc_credential_t lsc_credential = 0;

          if (modify_lsc_credential_data->lsc_credential_id == NULL)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("modify_lsc_credential",
                                "MODIFY_LSC_CREDENTIAL requires a"
                                " lsc_credential_id attribute"));
          else if (find_lsc_credential
                    (modify_lsc_credential_data->lsc_credential_id,
                     &lsc_credential))
            SEND_TO_CLIENT_OR_FAIL
             (XML_INTERNAL_ERROR ("modify_lsc_credential"));
          else if (lsc_credential == 0)
            {
              if (send_find_error_to_client
                   ("modify_lsc_credential",
                    "LSC credential",
                    modify_lsc_credential_data->lsc_credential_id,
                    write_to_client,
                    write_to_client_data))
                {
                  error_send_to_client (error);
                  return;
                }
            }
          else if ((modify_lsc_credential_data->login
                    || modify_lsc_credential_data->password)
                   && lsc_credential_packaged (lsc_credential))
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("modify_lsc_credential",
                                "Attempt to change login or password of"
                                " packaged LSC credential"));
          else
            {
              if (modify_lsc_credential_data->name)
                set_lsc_credential_name (lsc_credential,
                                         modify_lsc_credential_data->name);
              if (modify_lsc_credential_data->comment)
                set_lsc_credential_comment
                 (lsc_credential,
                  modify_lsc_credential_data->comment);
              if (modify_lsc_credential_data->login)
                set_lsc_credential_login (lsc_credential,
                                          modify_lsc_credential_data->login);
              if (modify_lsc_credential_data->password)
                set_lsc_credential_password
                 (lsc_credential,
                  modify_lsc_credential_data->password);
              SEND_TO_CLIENT_OR_FAIL (XML_OK ("modify_lsc_credential"));
            }
        }
        modify_lsc_credential_data_reset (modify_lsc_credential_data);
        set_client_state (CLIENT_AUTHENTIC);
        break;
      CLOSE (CLIENT_MODIFY_LSC_CREDENTIAL, NAME);
      CLOSE (CLIENT_MODIFY_LSC_CREDENTIAL, COMMENT);
      CLOSE (CLIENT_MODIFY_LSC_CREDENTIAL, LOGIN);
      CLOSE (CLIENT_MODIFY_LSC_CREDENTIAL, PASSWORD);

      case CLIENT_MODIFY_REPORT:
        {
          report_t report = 0;

          if (modify_report_data->report_id == NULL)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("modify_report",
                                "MODIFY_REPORT requires a report_id attribute"));
          else if (modify_report_data->comment == NULL)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("modify_report",
                                "MODIFY_REPORT requires a COMMENT element"));
          else if (find_report (modify_report_data->report_id, &report))
            SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("modify_report"));
          else if (report == 0)
            {
              if (send_find_error_to_client ("modify_report",
                                             "report",
                                             modify_report_data->report_id,
                                             write_to_client,
                                             write_to_client_data))
                {
                  error_send_to_client (error);
                  return;
                }
            }
          else
            {
              int ret = set_report_parameter
                         (report,
                          "COMMENT",
                          modify_report_data->comment);
              switch (ret)
                {
                  case 0:
                    SEND_TO_CLIENT_OR_FAIL (XML_OK ("modify_report"));
                    break;
                  case -2: /* Parameter name error. */
                    SEND_TO_CLIENT_OR_FAIL
                     (XML_ERROR_SYNTAX ("modify_report",
                                        "Bogus MODIFY_REPORT parameter"));
                    break;
                  case -3: /* Failed to write to disk. */
                  default:
                    SEND_TO_CLIENT_OR_FAIL
                     (XML_INTERNAL_ERROR ("modify_report"));
                    break;
                }
            }
          SEND_TO_CLIENT_OR_FAIL (XML_OK ("modify_report"));
        }
        modify_report_data_reset (modify_report_data);
        set_client_state (CLIENT_AUTHENTIC);
        break;
      CLOSE (CLIENT_MODIFY_REPORT, COMMENT);

      case CLIENT_MODIFY_REPORT_FORMAT:
        {
          report_format_t report_format = 0;

          if (modify_report_format_data->report_format_id == NULL)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("modify_report_format",
                                "MODIFY_REPORT_FORMAT requires a"
                                " report_format_id attribute"));
          else if (find_report_format
                    (modify_report_format_data->report_format_id,
                     &report_format))
            SEND_TO_CLIENT_OR_FAIL
             (XML_INTERNAL_ERROR ("modify_report_format"));
          else if (report_format == 0)
            {
              if (send_find_error_to_client
                   ("modify_report_format",
                    "report format",
                    modify_report_format_data->report_format_id,
                    write_to_client,
                    write_to_client_data))
                {
                  error_send_to_client (error);
                  return;
                }
            }
          else
            {
              if (modify_report_format_data->active)
                set_report_format_active
                 (report_format,
                  strcmp (modify_report_format_data->active, "0"));
              if (modify_report_format_data->name)
                set_report_format_name (report_format,
                                        modify_report_format_data->name);
              if (modify_report_format_data->summary)
                set_report_format_summary (report_format,
                                           modify_report_format_data->summary);
              if (modify_report_format_data->param_name)
                {
                  switch (set_report_format_param
                           (report_format,
                            modify_report_format_data->param_name,
                            modify_report_format_data->param_value))
                    {
                      case 0:
                        SEND_TO_CLIENT_OR_FAIL
                         (XML_OK ("modify_report_format"));
                        break;
                      case 1:
                        if (send_find_error_to_client
                             ("modify_report_format",
                              "param",
                              modify_report_format_data->param_name,
                              write_to_client,
                              write_to_client_data))
                          {
                            error_send_to_client (error);
                            return;
                          }
                        break;
                      case 2:
                        SEND_TO_CLIENT_OR_FAIL
                         (XML_ERROR_SYNTAX ("modify_report_format",
                                            "Parameter validation failed"));
                        break;
                      default:
                        SEND_TO_CLIENT_OR_FAIL
                         (XML_INTERNAL_ERROR ("modify_report_format"));
                        break;
                    }
                }
              else
                SEND_TO_CLIENT_OR_FAIL (XML_OK ("modify_report_format"));
            }
        }
        modify_report_format_data_reset (modify_report_format_data);
        set_client_state (CLIENT_AUTHENTIC);
        break;
      CLOSE (CLIENT_MODIFY_REPORT_FORMAT, ACTIVE);
      CLOSE (CLIENT_MODIFY_REPORT_FORMAT, NAME);
      CLOSE (CLIENT_MODIFY_REPORT_FORMAT, SUMMARY);
      CLOSE (CLIENT_MODIFY_REPORT_FORMAT, PARAM);
      CLOSE (CLIENT_MODIFY_REPORT_FORMAT_PARAM, NAME);
      CLOSE (CLIENT_MODIFY_REPORT_FORMAT_PARAM, VALUE);

      case CLIENT_MODIFY_SETTING:
        {
          gchar *errdesc = NULL;

          if (((modify_setting_data->name == NULL)
               && (modify_setting_data->setting_id == NULL))
              || (modify_setting_data->value == NULL))
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("modify_setting",
                                "MODIFY_SETTING requires a NAME or setting_id"
                                " and a VALUE"));
          else switch (manage_set_setting (modify_setting_data->setting_id,
                                           modify_setting_data->name,
                                           modify_setting_data->value,
                                           &errdesc))
            {
              case 0:
                SEND_TO_CLIENT_OR_FAIL (XML_OK ("modify_setting"));
                break;
              case 1:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("modify_setting",
                                    "Failed to find setting"));
                break;
              case 2:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("modify_setting",
                                    "Value validation failed"));
                break;
              case -1:
                if (errdesc)
                  {
                    char *buf = make_xml_error_syntax ("modify_setting",
                                                       errdesc);
                    SEND_TO_CLIENT_OR_FAIL (buf);
                    g_free (buf);
                    break;
                  }
                /* Fall through.  */
              default:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_INTERNAL_ERROR ("modify_setting"));
                break;
            }
          g_free (errdesc);
        }
        modify_setting_data_reset (modify_setting_data);
        set_client_state (CLIENT_AUTHENTIC);
        break;
      CLOSE (CLIENT_MODIFY_SETTING, NAME);
      CLOSE (CLIENT_MODIFY_SETTING, VALUE);

      case CLIENT_MODIFY_TASK:
        /** @todo Update to match "create_task (config, target)". */
        if (modify_task_data->task_id)
          {
            task_t task = 0;
            if (find_task (modify_task_data->task_id, &task))
              SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("modify_task"));
            else if (task == 0)
              {
                if (send_find_error_to_client ("modify_task",
                                               "task",
                                               modify_task_data->task_id,
                                               write_to_client,
                                               write_to_client_data))
                  {
                    error_send_to_client (error);
                    return;
                  }
              }
            else if ((modify_task_data->action
                      || (modify_task_data->alerts->len > 1)
                      || modify_task_data->name
                      || modify_task_data->comment
                      || modify_task_data->rcfile)
                     == 0)
              SEND_TO_CLIENT_OR_FAIL
               (XML_ERROR_SYNTAX ("modify_task",
                                  "Too few parameters"));
            else if (modify_task_data->action
                     && (modify_task_data->comment
                         || modify_task_data->alerts->len
                         || modify_task_data->name
                         || modify_task_data->rcfile))
              SEND_TO_CLIENT_OR_FAIL
               (XML_ERROR_SYNTAX ("modify_task",
                                  "Too many parameters at once"));
            else if ((task_target (task) == 0)
                     && (modify_task_data->rcfile
                         || modify_task_data->alerts->len
                         || modify_task_data->schedule_id
                         || modify_task_data->slave_id))
              SEND_TO_CLIENT_OR_FAIL
               (XML_ERROR_SYNTAX ("modify_task",
                                  "For container tasks only name and comment"
                                  " can be modified"));
            else if (modify_task_data->action)
              {
                if (modify_task_data->file_name == NULL)
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_ERROR_SYNTAX ("modify_task",
                                      "MODIFY_TASK FILE requires a name"
                                      " attribute"));
                else if (strcmp (modify_task_data->action, "update") == 0)
                  {
                    manage_task_update_file (task,
                                             modify_task_data->file_name,
                                             modify_task_data->file
                                              ? modify_task_data->file
                                              : "");
                    g_log ("event task", G_LOG_LEVEL_MESSAGE,
                           "Task %s has been modified",
                           modify_task_data->task_id);
                    SEND_TO_CLIENT_OR_FAIL (XML_OK ("modify_task"));
                  }
                else if (strcmp (modify_task_data->action, "remove") == 0)
                  {
                    manage_task_remove_file (task, modify_task_data->file_name);
                    g_log ("event task", G_LOG_LEVEL_MESSAGE,
                           "Task %s has been modified",
                           modify_task_data->task_id);
                    SEND_TO_CLIENT_OR_FAIL (XML_OK ("modify_task"));
                  }
                else
                  {
                    SEND_TO_CLIENT_OR_FAIL
                      (XML_ERROR_SYNTAX ("modify_task",
                                         "MODIFY_TASK action must be"
                                         " \"update\" or \"remove\""));
                    g_log ("event task", G_LOG_LEVEL_MESSAGE,
                           "Task %s could not be modified",
                           modify_task_data->task_id);
                  }
              }
            else
              {
                int fail = 0;

                /** @todo It'd probably be better to allow only one
                 * modification at a time, that is, one parameter or one of
                 * file, name and comment.  Otherwise a syntax error in a
                 * later part of the command would result in an error being
                 * returned while some part of the command actually
                 * succeeded. */

                if (modify_task_data->rcfile)
                  {
                    fail = set_task_parameter (task,
                                               "RCFILE",
                                               modify_task_data->rcfile);
                    modify_task_data->rcfile = NULL;
                    if (fail)
                      {
                        SEND_TO_CLIENT_OR_FAIL
                          (XML_INTERNAL_ERROR ("modify_task"));
                        g_log ("event task", G_LOG_LEVEL_MESSAGE,
                               "Task %s could not be modified",
                               modify_task_data->task_id);
                      }
                  }

                if (fail == 0 && modify_task_data->name)
                  {
                    fail = set_task_parameter (task,
                                               "NAME",
                                               modify_task_data->name);
                    modify_task_data->name = NULL;
                    if (fail)
                      {
                        SEND_TO_CLIENT_OR_FAIL
                          (XML_INTERNAL_ERROR ("modify_task"));
                        g_log ("event task", G_LOG_LEVEL_MESSAGE,
                               "Task %s could not be modified",
                               modify_task_data->task_id);
                      }
                  }

                if (fail == 0 && modify_task_data->comment)
                  {
                    fail = set_task_parameter (task,
                                               "COMMENT",
                                               modify_task_data->comment);
                    modify_task_data->comment = NULL;
                    if (fail)
                      {
                        SEND_TO_CLIENT_OR_FAIL
                          (XML_INTERNAL_ERROR ("modify_task"));
                        g_log ("event task", G_LOG_LEVEL_MESSAGE,
                               "Task %s could not be modified",
                               modify_task_data->task_id);
                      }
                  }

                if (fail == 0 && modify_task_data->config_id)
                  {
                    config_t config = 0;

                    if (strcmp (modify_task_data->config_id, "0") == 0)
                      {
                        /* Leave it as it is. */
                      }
                    else if ((fail = (task_run_status (task)
                                      != TASK_STATUS_NEW)))
                      SEND_TO_CLIENT_OR_FAIL
                       (XML_ERROR_SYNTAX ("modify_task",
                                          "Status must be New to edit Config"));
                    else if ((fail = find_config
                                      (modify_task_data->config_id,
                                       &config)))
                      SEND_TO_CLIENT_OR_FAIL
                       (XML_INTERNAL_ERROR ("modify_task"));
                    else if (config == 0)
                      {
                        if (send_find_error_to_client
                             ("modify_task",
                              "config",
                              modify_task_data->config_id,
                              write_to_client,
                              write_to_client_data))
                          {
                            error_send_to_client (error);
                            return;
                          }
                        fail = 1;
                      }
                    else
                     set_task_config (task, config);
                  }

                if (fail == 0 && modify_task_data->observers)
                  {
                    fail = set_task_observers (task,
                                               modify_task_data->observers);
                    switch (fail)
                      {
                        case 0:
                          break;
                        case 1:
                        case 2:
                          SEND_TO_CLIENT_OR_FAIL
                            (XML_ERROR_SYNTAX ("modify_task",
                                               "User name error"));
                          g_log ("event task", G_LOG_LEVEL_MESSAGE,
                                 "Task %s could not be modified",
                                 modify_task_data->task_id);
                          break;
                        case -1:
                        default:
                          SEND_TO_CLIENT_OR_FAIL
                            (XML_INTERNAL_ERROR ("modify_task"));
                          g_log ("event task", G_LOG_LEVEL_MESSAGE,
                                 "Task %s could not be modified",
                                 modify_task_data->task_id);
                      }
                  }

                if (fail == 0 && modify_task_data->alerts->len)
                  {
                    gchar *fail_alert_id;
                    switch ((fail = set_task_alerts (task,
                                                     modify_task_data->alerts,
                                                     &fail_alert_id)))
                      {
                        case 0:
                          break;
                        case 1:
                          if (send_find_error_to_client
                               ("modify_task",
                                "alert",
                                fail_alert_id,
                                write_to_client,
                                write_to_client_data))
                            {
                              error_send_to_client (error);
                              return;
                            }
                          fail = 1;
                          g_log ("event task", G_LOG_LEVEL_MESSAGE,
                                 "Task %s could not be modified",
                                 modify_task_data->task_id);
                          break;
                        case -1:
                        default:
                          SEND_TO_CLIENT_OR_FAIL
                            (XML_INTERNAL_ERROR ("modify_task"));
                          g_log ("event task", G_LOG_LEVEL_MESSAGE,
                                 "Task %s could not be modified",
                                 modify_task_data->task_id);
                      }
                  }

                if (fail == 0 && modify_task_data->schedule_id)
                  {
                    schedule_t schedule = 0;

                    if (strcmp (modify_task_data->schedule_id, "0") == 0)
                      {
                        set_task_schedule (task, 0);
                      }
                    else if ((fail = find_schedule
                                      (modify_task_data->schedule_id,
                                       &schedule)))
                      SEND_TO_CLIENT_OR_FAIL
                       (XML_INTERNAL_ERROR ("modify_task"));
                    else if (schedule == 0)
                      {
                        if (send_find_error_to_client
                             ("modify_task",
                              "schedule",
                              modify_task_data->schedule_id,
                              write_to_client,
                              write_to_client_data))
                          {
                            error_send_to_client (error);
                            return;
                          }
                        fail = 1;
                      }
                    else if (set_task_schedule (task, schedule))
                      {
                        SEND_TO_CLIENT_OR_FAIL
                         (XML_INTERNAL_ERROR ("modify_task"));
                        fail = 1;
                      }
                  }

                if (fail == 0 && modify_task_data->slave_id)
                  {
                    slave_t slave = 0;

                    if (strcmp (modify_task_data->slave_id, "0") == 0)
                      {
                        set_task_slave (task, 0);
                      }
                    else if ((fail = find_slave
                                      (modify_task_data->slave_id,
                                       &slave)))
                      SEND_TO_CLIENT_OR_FAIL
                       (XML_INTERNAL_ERROR ("modify_task"));
                    else if (slave == 0)
                      {
                        if (send_find_error_to_client
                             ("modify_task",
                              "slave",
                              modify_task_data->slave_id,
                              write_to_client,
                              write_to_client_data))
                          {
                            error_send_to_client (error);
                            return;
                          }
                        fail = 1;
                      }
                    else
                      {
                        set_task_slave (task, slave);
                      }
                  }

                if (fail == 0 && modify_task_data->target_id)
                  {
                    target_t target = 0;

                    if (strcmp (modify_task_data->target_id, "0") == 0)
                      {
                        /* Leave it as it is. */
                      }
                    else if ((fail = (task_run_status (task)
                                      != TASK_STATUS_NEW)))
                      SEND_TO_CLIENT_OR_FAIL
                       (XML_ERROR_SYNTAX ("modify_task",
                                          "Status must be New to edit Target"));
                    else if ((fail = find_target
                                      (modify_task_data->target_id,
                                       &target)))
                      SEND_TO_CLIENT_OR_FAIL
                       (XML_INTERNAL_ERROR ("modify_task"));
                    else if (target == 0)
                      {
                        if (send_find_error_to_client
                             ("modify_task",
                              "target",
                              modify_task_data->target_id,
                              write_to_client,
                              write_to_client_data))
                          {
                            error_send_to_client (error);
                            return;
                          }
                        fail = 1;
                      }
                    else
                      set_task_target (task, target);
                  }

                if (fail == 0 && modify_task_data->preferences)
                  set_task_preferences (task,
                                        modify_task_data->preferences);

                if (fail == 0)
                  {
                    g_log ("event task", G_LOG_LEVEL_MESSAGE,
                           "Task %s has been modified",
                           modify_task_data->task_id);
                    SEND_TO_CLIENT_OR_FAIL (XML_OK ("modify_task"));
                  }
              }
          }
        else
          SEND_TO_CLIENT_OR_FAIL
           (XML_ERROR_SYNTAX ("modify_task",
                              "MODIFY_TASK requires a task_id attribute"));
        modify_task_data_reset (modify_task_data);
        set_client_state (CLIENT_AUTHENTIC);
        break;
      CLOSE (CLIENT_MODIFY_TASK, COMMENT);
      CLOSE (CLIENT_MODIFY_TASK, CONFIG);
      CLOSE (CLIENT_MODIFY_TASK, ALERT);
      CLOSE (CLIENT_MODIFY_TASK, NAME);
      CLOSE (CLIENT_MODIFY_TASK, OBSERVERS);
      CLOSE (CLIENT_MODIFY_TASK, PREFERENCES);
      CLOSE (CLIENT_MODIFY_TASK, RCFILE);
      CLOSE (CLIENT_MODIFY_TASK, SCHEDULE);
      CLOSE (CLIENT_MODIFY_TASK, SLAVE);
      CLOSE (CLIENT_MODIFY_TASK, TARGET);
      CLOSE (CLIENT_MODIFY_TASK, FILE);

      case CLIENT_MODIFY_TASK_PREFERENCES_PREFERENCE:
        assert (strcasecmp ("PREFERENCE", element_name) == 0);
        array_add (modify_task_data->preferences,
                   modify_task_data->preference);
        modify_task_data->preference = NULL;
        set_client_state (CLIENT_MODIFY_TASK_PREFERENCES);
        break;
      case CLIENT_MODIFY_TASK_PREFERENCES_PREFERENCE_NAME:
        assert (strcasecmp ("SCANNER_NAME", element_name) == 0);
        set_client_state (CLIENT_MODIFY_TASK_PREFERENCES_PREFERENCE);
        break;
      CLOSE (CLIENT_MODIFY_TASK_PREFERENCES_PREFERENCE, VALUE);

      case CLIENT_CREATE_AGENT:
        {
          agent_t agent;

          assert (strcasecmp ("CREATE_AGENT", element_name) == 0);

          if (openvas_is_user_observer (current_credentials.username))
            {
              SEND_TO_CLIENT_OR_FAIL
               (XML_ERROR_SYNTAX ("create_agent",
                                  "CREATE is forbidden for observer users"));
            }
          else if (create_agent_data->copy)
            switch (copy_agent (create_agent_data->name,
                                create_agent_data->comment,
                                create_agent_data->copy,
                                &agent))
              {
                case 0:
                  {
                    char *uuid;
                    uuid = agent_uuid (agent);
                    SENDF_TO_CLIENT_OR_FAIL (XML_OK_CREATED_ID ("create_agent"),
                                             uuid);
                    g_log ("event agent", G_LOG_LEVEL_MESSAGE,
                           "Agent %s has been created", uuid);
                    free (uuid);
                    break;
                  }
                case 1:
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_ERROR_SYNTAX ("create_agent",
                                      "Agent exists already"));
                  g_log ("event agent", G_LOG_LEVEL_MESSAGE,
                         "Agent could not be created");
                  break;
                case 2:
                  if (send_find_error_to_client ("create_agent",
                                                 "agent",
                                                 create_agent_data->copy,
                                                 write_to_client,
                                                 write_to_client_data))
                    {
                      error_send_to_client (error);
                      return;
                    }
                  g_log ("event agent", G_LOG_LEVEL_MESSAGE,
                         "Agent could not be created");
                  break;
                case -1:
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_INTERNAL_ERROR ("create_agent"));
                  g_log ("event agent", G_LOG_LEVEL_MESSAGE,
                         "Agent could not be created");
                  break;
              }
          else if (create_agent_data->name == NULL)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("create_agent",
                                "CREATE_AGENT requires a NAME"));
          else if (strlen (create_agent_data->name) == 0)
            {
              SEND_TO_CLIENT_OR_FAIL
               (XML_ERROR_SYNTAX ("create_agent",
                                  "CREATE_AGENT name must be at"
                                  " least one character long"));
            }
          else if (strlen (create_agent_data->installer) == 0)
            {
              SEND_TO_CLIENT_OR_FAIL
               (XML_ERROR_SYNTAX ("create_agent",
                                  "CREATE_AGENT installer must be at"
                                  " least one byte long"));
            }
          else switch (create_agent (create_agent_data->name,
                                     create_agent_data->comment,
                                     create_agent_data->installer,
                                     create_agent_data->installer_filename,
                                     create_agent_data->installer_signature,
                                     create_agent_data->howto_install,
                                     create_agent_data->howto_use,
                                     &agent))
            {
              case 0:
                {
                  char *uuid;
                  uuid = agent_uuid (agent);
                  SENDF_TO_CLIENT_OR_FAIL (XML_OK_CREATED_ID ("create_agent"),
                                           uuid);
                  g_log ("event agent", G_LOG_LEVEL_MESSAGE,
                         "Agent %s has been created", uuid);
                  g_free (uuid);
                  break;
                }
              case 1:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("create_agent",
                                    "Agent exists already"));
                break;
              case 2:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("create_agent",
                                    "Name may only contain alphanumeric"
                                    " characters"));
                break;
              default:
                assert (0);
              case -1:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_INTERNAL_ERROR ("create_agent"));
                break;
            }
          create_agent_data_reset (create_agent_data);
          set_client_state (CLIENT_AUTHENTIC);
          break;
        }
      CLOSE (CLIENT_CREATE_AGENT, COMMENT);
      CLOSE (CLIENT_CREATE_AGENT, COPY);
      CLOSE (CLIENT_CREATE_AGENT, HOWTO_INSTALL);
      CLOSE (CLIENT_CREATE_AGENT, HOWTO_USE);
      CLOSE (CLIENT_CREATE_AGENT, INSTALLER);
      CLOSE (CLIENT_CREATE_AGENT_INSTALLER, FILENAME);
      CLOSE (CLIENT_CREATE_AGENT_INSTALLER, SIGNATURE);
      CLOSE (CLIENT_CREATE_AGENT, NAME);

      case CLIENT_CREATE_CONFIG:
        {
          config_t config = 0, new_config;

          assert (strcasecmp ("CREATE_CONFIG", element_name) == 0);
          assert (import_config_data->import
                  || (create_config_data->name != NULL));

          /* For now the import element, GET_CONFIGS_RESPONSE, overrides
           * any other elements. */

          if (openvas_is_user_observer (current_credentials.username))
            {
              SEND_TO_CLIENT_OR_FAIL
               (XML_ERROR_SYNTAX ("create_config",
                                  "CREATE is forbidden for observer users"));
            }
          else if (import_config_data->import)
            {
              char *name;
              array_terminate (import_config_data->nvt_selectors);
              array_terminate (import_config_data->preferences);
              switch (create_config (import_config_data->name,
                                     import_config_data->comment,
                                     import_config_data->nvt_selectors,
                                     import_config_data->preferences,
                                     &new_config,
                                     &name))
                {
                  case 0:
                    {
                      gchar *uuid;
                      config_uuid (new_config, &uuid);
                      SENDF_TO_CLIENT_OR_FAIL
                       ("<create_config_response"
                        " status=\"" STATUS_OK_CREATED "\""
                        " status_text=\"" STATUS_OK_CREATED_TEXT "\""
                        " id=\"%s\">"
                        /* This is a hack for the GSA, which should really
                         * do a GET_CONFIG with the ID to get the name. */
                        "<config id=\"%s\"><name>%s</name></config>"
                        "</create_config_response>",
                        uuid,
                        uuid,
                        name);
                      g_log ("event config", G_LOG_LEVEL_MESSAGE,
                             "Scan config %s has been created", uuid);
                      g_free (uuid);
                      free (name);
                      break;
                    }
                  case 1:
                    SEND_TO_CLIENT_OR_FAIL
                     (XML_ERROR_SYNTAX ("create_config",
                                        "Config exists already"));
                      g_log ("event config", G_LOG_LEVEL_MESSAGE,
                             "Scan config could not be created");
                    break;
                  case -1:
                    SEND_TO_CLIENT_OR_FAIL
                     (XML_INTERNAL_ERROR ("create_config"));
                      g_log ("event config", G_LOG_LEVEL_MESSAGE,
                             "Scan config could not be created");
                    break;
                  case -2:
                    SEND_TO_CLIENT_OR_FAIL
                     (XML_ERROR_SYNTAX ("create_config",
                                        "CREATE_CONFIG import name must be at"
                                        " least one character long"));
                      g_log ("event config", G_LOG_LEVEL_MESSAGE,
                             "Scan config could not be created");
                    break;
                  case -3:
                    SEND_TO_CLIENT_OR_FAIL
                     (XML_ERROR_SYNTAX ("create_config",
                                        "Error in NVT_SELECTORS element."));
                      g_log ("event config", G_LOG_LEVEL_MESSAGE,
                             "Scan config could not be created");
                    break;
                  case -4:
                    SEND_TO_CLIENT_OR_FAIL
                     (XML_ERROR_SYNTAX ("create_config",
                                        "Error in PREFERENCES element."));
                      g_log ("event config", G_LOG_LEVEL_MESSAGE,
                             "Scan config could not be created");
                    break;
                }
            }
          else if (strlen (create_config_data->name) == 0
                   && (create_config_data->copy == NULL
                       || strlen (create_config_data->copy) == 0))
            {
              g_log ("event config", G_LOG_LEVEL_MESSAGE,
                     "Scan config could not be created");
              SEND_TO_CLIENT_OR_FAIL
               (XML_ERROR_SYNTAX ("create_config",
                                  "CREATE_CONFIG name and base config to copy"
                                  " must be at least one character long"));
            }
          else if ((create_config_data->rcfile
                    && create_config_data->copy)
                   || (create_config_data->rcfile == NULL
                       && create_config_data->copy == NULL))
            {
              g_log ("event config", G_LOG_LEVEL_MESSAGE,
                     "Scan config could not be created");
              SEND_TO_CLIENT_OR_FAIL
               (XML_ERROR_SYNTAX ("create_config",
                                  "CREATE_CONFIG requires either a COPY or an"
                                  " RCFILE element"));
            }
          else if (create_config_data->rcfile)
            {
              int ret;
              gsize base64_len;
              guchar *base64;

              base64 = g_base64_decode (create_config_data->rcfile,
                                        &base64_len);
              /* g_base64_decode can return NULL (Glib 2.12.4-2), at least
               * when create_config_data->rcfile is zero length. */
              if (base64 == NULL)
                {
                  base64 = (guchar*) g_strdup ("");
                  base64_len = 0;
                }

              ret = create_config_rc (create_config_data->name,
                                      create_config_data->comment,
                                      (char*) base64,
                                      &new_config);
              g_free (base64);
              switch (ret)
                {
                  case 0:
                    {
                      char *uuid;
                      config_uuid (new_config, &uuid);
                      SENDF_TO_CLIENT_OR_FAIL (XML_OK_CREATED_ID
                                                ("create_config"),
                                               uuid);
                      g_log ("event config", G_LOG_LEVEL_MESSAGE,
                             "Scan config %s has been created", uuid);
                      free (uuid);
                      break;
                    }
                  case 1:
                    SEND_TO_CLIENT_OR_FAIL
                     (XML_ERROR_SYNTAX ("create_config",
                                        "Config exists already"));
                    g_log ("event config", G_LOG_LEVEL_MESSAGE,
                           "Scan config could not be created");
                    break;
                  case -1:
                    SEND_TO_CLIENT_OR_FAIL
                     (XML_INTERNAL_ERROR ("create_config"));
                    g_log ("event config", G_LOG_LEVEL_MESSAGE,
                           "Scan config could not be created");
                    break;
                }
            }
          else if (find_config (create_config_data->copy, &config))
            SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("create_config"));
          else if (config == 0)
            {
              if (send_find_error_to_client ("create_config",
                                             "config",
                                             create_config_data->copy,
                                             write_to_client,
                                             write_to_client_data))
                {
                  error_send_to_client (error);
                  return;
                }
            }
          else switch (copy_config (create_config_data->name,
                                    create_config_data->comment,
                                    config,
                                    &new_config))
            {
              case 0:
                {
                  char *uuid;
                  config_uuid (new_config, &uuid);
                  SENDF_TO_CLIENT_OR_FAIL (XML_OK_CREATED_ID ("create_config"),
                                           uuid);
                  g_log ("event config", G_LOG_LEVEL_MESSAGE,
                         "Scan config %s has been created", uuid);
                  free (uuid);
                  break;
                }
              case 1:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("create_config",
                                    "Config exists already"));
                g_log ("event config", G_LOG_LEVEL_MESSAGE,
                       "Scan config could not be created");
                break;
              case -1:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_INTERNAL_ERROR ("create_config"));
                g_log ("event config", G_LOG_LEVEL_MESSAGE,
                       "Scan config could not be created");
                break;
            }
          create_config_data_reset (create_config_data);
          set_client_state (CLIENT_AUTHENTIC);
          break;
        }
      CLOSE (CLIENT_CREATE_CONFIG, COMMENT);
      CLOSE (CLIENT_CREATE_CONFIG, COPY);
      CLOSE (CLIENT_CREATE_CONFIG, NAME);
      CLOSE (CLIENT_CREATE_CONFIG, RCFILE);

      case CLIENT_C_C_GCR:
        assert (strcasecmp ("GET_CONFIGS_RESPONSE", element_name) == 0);
        set_client_state (CLIENT_CREATE_CONFIG);
        break;
      CLOSE (CLIENT_C_C_GCR, CONFIG);
      CLOSE (CLIENT_C_C_GCR_CONFIG, COMMENT);
      CLOSE (CLIENT_C_C_GCR_CONFIG, NAME);
      CLOSE (CLIENT_C_C_GCR_CONFIG, NVT_SELECTORS);
      case CLIENT_C_C_GCR_CONFIG_NVT_SELECTORS_NVT_SELECTOR:
        {
          int include;

          assert (strcasecmp ("NVT_SELECTOR", element_name) == 0);

          if (import_config_data->nvt_selector_include
              && strcmp (import_config_data->nvt_selector_include, "0") == 0)
            include = 0;
          else
            include = 1;

          array_add (import_config_data->nvt_selectors,
                     nvt_selector_new
                      (import_config_data->nvt_selector_name,
                       import_config_data->nvt_selector_type,
                       include,
                       import_config_data->nvt_selector_family_or_nvt));

          import_config_data->nvt_selector_name = NULL;
          import_config_data->nvt_selector_type = NULL;
          free (import_config_data->nvt_selector_include);
          import_config_data->nvt_selector_include = NULL;
          import_config_data->nvt_selector_family_or_nvt = NULL;

          set_client_state (CLIENT_C_C_GCR_CONFIG_NVT_SELECTORS);
          break;
        }
      CLOSE (CLIENT_C_C_GCR_CONFIG_NVT_SELECTORS_NVT_SELECTOR, INCLUDE);
      CLOSE (CLIENT_C_C_GCR_CONFIG_NVT_SELECTORS_NVT_SELECTOR, NAME);
      CLOSE (CLIENT_C_C_GCR_CONFIG_NVT_SELECTORS_NVT_SELECTOR, TYPE);
      CLOSE (CLIENT_C_C_GCR_CONFIG_NVT_SELECTORS_NVT_SELECTOR, FAMILY_OR_NVT);
      CLOSE (CLIENT_C_C_GCR_CONFIG, PREFERENCES);
      case CLIENT_C_C_GCR_CONFIG_PREFERENCES_PREFERENCE:
        assert (strcasecmp ("PREFERENCE", element_name) == 0);
        array_terminate (import_config_data->preference_alts);
        array_add (import_config_data->preferences,
                   preference_new (import_config_data->preference_name,
                                   import_config_data->preference_type,
                                   import_config_data->preference_value,
                                   import_config_data->preference_nvt_name,
                                   import_config_data->preference_nvt_oid,
                                   import_config_data->preference_alts));
        import_config_data->preference_name = NULL;
        import_config_data->preference_type = NULL;
        import_config_data->preference_value = NULL;
        import_config_data->preference_nvt_name = NULL;
        import_config_data->preference_nvt_oid = NULL;
        import_config_data->preference_alts = NULL;
        set_client_state (CLIENT_C_C_GCR_CONFIG_PREFERENCES);
        break;
      case CLIENT_C_C_GCR_CONFIG_PREFERENCES_PREFERENCE_ALT:
        assert (strcasecmp ("ALT", element_name) == 0);
        array_add (import_config_data->preference_alts,
                   import_config_data->preference_alt);
        import_config_data->preference_alt = NULL;
        set_client_state (CLIENT_C_C_GCR_CONFIG_PREFERENCES_PREFERENCE);
        break;
      CLOSE (CLIENT_C_C_GCR_CONFIG_PREFERENCES_PREFERENCE, NAME);
      CLOSE (CLIENT_C_C_GCR_CONFIG_PREFERENCES_PREFERENCE, NVT);
      CLOSE (CLIENT_C_C_GCR_CONFIG_PREFERENCES_PREFERENCE_NVT, NAME);
      CLOSE (CLIENT_C_C_GCR_CONFIG_PREFERENCES_PREFERENCE, TYPE);
      CLOSE (CLIENT_C_C_GCR_CONFIG_PREFERENCES_PREFERENCE, VALUE);

      case CLIENT_CREATE_ALERT:
        {
          event_t event;
          alert_condition_t condition;
          alert_method_t method;
          alert_t new_alert;

          assert (strcasecmp ("CREATE_ALERT", element_name) == 0);
          assert (create_alert_data->name != NULL);
          assert (create_alert_data->condition != NULL);
          assert (create_alert_data->method != NULL);
          assert (create_alert_data->event != NULL);

          array_terminate (create_alert_data->condition_data);
          array_terminate (create_alert_data->event_data);
          array_terminate (create_alert_data->method_data);

          if (openvas_is_user_observer (current_credentials.username))
            {
              SEND_TO_CLIENT_OR_FAIL
               (XML_ERROR_SYNTAX ("create_alert",
                                  "CREATE is forbidden for observer users"));
            }
          else if (create_alert_data->copy)
            switch (copy_alert (create_alert_data->name,
                                create_alert_data->comment,
                                create_alert_data->copy,
                                &new_alert))
              {
                case 0:
                  {
                    char *uuid;
                    uuid = alert_uuid (new_alert);
                    SENDF_TO_CLIENT_OR_FAIL (XML_OK_CREATED_ID ("create_alert"),
                                             uuid);
                    g_log ("event alert", G_LOG_LEVEL_MESSAGE,
                           "Alert %s has been created", uuid);
                    free (uuid);
                    break;
                  }
                case 1:
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_ERROR_SYNTAX ("create_alert",
                                      "Alert exists already"));
                  g_log ("event alert", G_LOG_LEVEL_MESSAGE,
                         "Alert could not be created");
                  break;
                case 2:
                  if (send_find_error_to_client ("create_alert",
                                                 "alert",
                                                 create_alert_data->copy,
                                                 write_to_client,
                                                 write_to_client_data))
                    {
                      error_send_to_client (error);
                      return;
                    }
                  g_log ("event alert", G_LOG_LEVEL_MESSAGE,
                         "Alert could not be created");
                  break;
                case -1:
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_INTERNAL_ERROR ("create_alert"));
                  g_log ("event alert", G_LOG_LEVEL_MESSAGE,
                         "Alert could not be created");
                  break;
              }
          else if (strlen (create_alert_data->name) == 0)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("create_alert",
                                "CREATE_ALERT requires NAME element which"
                                " is at least one character long"));
          else if (strlen (create_alert_data->condition) == 0)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("create_alert",
                                "CREATE_ALERT requires a value in a"
                                " CONDITION element"));
          else if (strlen (create_alert_data->event) == 0)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("create_alert",
                                "CREATE_ALERT requires a value in an"
                                " EVENT element"));
          else if (strlen (create_alert_data->method) == 0)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("create_alert",
                                "CREATE_ALERT requires a value in a"
                                " METHOD element"));
          else if ((condition = alert_condition_from_name
                                 (create_alert_data->condition))
                   == 0)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("create_alert",
                                "Failed to recognise condition name"));
          else if ((event = event_from_name (create_alert_data->event))
                   == 0)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("create_alert",
                                "Failed to recognise event name"));
          else if ((method = alert_method_from_name
                              (create_alert_data->method))
                   == 0)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("create_alert",
                                "Failed to recognise method name"));
          else
            {
              switch (create_alert (create_alert_data->name,
                                    create_alert_data->comment,
                                    create_alert_data->filter_id,
                                    event,
                                    create_alert_data->event_data,
                                    condition,
                                    create_alert_data->condition_data,
                                    method,
                                    create_alert_data->method_data,
                                    &new_alert))
                {
                  case 0:
                    {
                      char *uuid;
                      uuid = alert_uuid (new_alert);
                      SENDF_TO_CLIENT_OR_FAIL
                       (XML_OK_CREATED_ID ("create_alert"), uuid);
                      g_log ("event alert", G_LOG_LEVEL_MESSAGE,
                             "Alert %s has been created", uuid);
                      free (uuid);
                      break;
                    }
                  case 1:
                    SEND_TO_CLIENT_OR_FAIL
                     (XML_ERROR_SYNTAX ("create_alert",
                                        "Alert exists already"));
                    g_log ("event alert", G_LOG_LEVEL_MESSAGE,
                           "Alert could not be created");
                    break;
                  case 2:
                    SEND_TO_CLIENT_OR_FAIL
                     (XML_ERROR_SYNTAX ("create_alert",
                                        "Validation of email address failed"));
                    g_log ("event alert", G_LOG_LEVEL_MESSAGE,
                           "Alert could not be created");
                    break;
                  case 3:
                    if (send_find_error_to_client ("create_alert",
                                                   "filter",
                                                   create_alert_data->filter_id,
                                                   write_to_client,
                                                   write_to_client_data))
                      {
                        error_send_to_client (error);
                        return;
                      }
                    g_log ("event alert", G_LOG_LEVEL_MESSAGE,
                           "Alert could not be created");
                    break;
                  case 4:
                    SEND_TO_CLIENT_OR_FAIL
                     (XML_ERROR_SYNTAX ("create_alert",
                                        "Filter type must be report if"
                                        " specified"));
                    g_log ("event alert", G_LOG_LEVEL_MESSAGE,
                           "Alert could not be created");
                    break;
                  default:
                    assert (0);
                  case -1:
                    SEND_TO_CLIENT_OR_FAIL
                     (XML_INTERNAL_ERROR ("create_alert"));
                    g_log ("event alert", G_LOG_LEVEL_MESSAGE,
                           "Alert could not be created");
                    break;
                }
            }
          create_alert_data_reset (create_alert_data);
          set_client_state (CLIENT_AUTHENTIC);
          break;
        }
      CLOSE (CLIENT_CREATE_ALERT, COMMENT);
      CLOSE (CLIENT_CREATE_ALERT, COPY);
      CLOSE (CLIENT_CREATE_ALERT, CONDITION);
      CLOSE (CLIENT_CREATE_ALERT, EVENT);
      CLOSE (CLIENT_CREATE_ALERT, FILTER);
      CLOSE (CLIENT_CREATE_ALERT, METHOD);
      CLOSE (CLIENT_CREATE_ALERT, NAME);

      case CLIENT_CREATE_ALERT_CONDITION_DATA:
        {
          gchar *string;

          assert (strcasecmp ("DATA", element_name) == 0);
          assert (create_alert_data->condition_data);
          assert (create_alert_data->part_data);
          assert (create_alert_data->part_name);

          string = g_strconcat (create_alert_data->part_name,
                                "0",
                                create_alert_data->part_data,
                                NULL);
          string[strlen (create_alert_data->part_name)] = '\0';
          array_add (create_alert_data->condition_data, string);

          openvas_free_string_var (&create_alert_data->part_data);
          openvas_free_string_var (&create_alert_data->part_name);
          openvas_append_string (&create_alert_data->part_data, "");
          openvas_append_string (&create_alert_data->part_name, "");
          set_client_state (CLIENT_CREATE_ALERT_CONDITION);
          break;
        }
      case CLIENT_CREATE_ALERT_CONDITION_DATA_NAME:
        assert (strcasecmp ("NAME", element_name) == 0);
        set_client_state (CLIENT_CREATE_ALERT_CONDITION_DATA);
        break;

      case CLIENT_CREATE_ALERT_EVENT_DATA:
        {
          gchar *string;

          assert (strcasecmp ("DATA", element_name) == 0);
          assert (create_alert_data->event_data);
          assert (create_alert_data->part_data);
          assert (create_alert_data->part_name);

          string = g_strconcat (create_alert_data->part_name,
                                "0",
                                create_alert_data->part_data,
                                NULL);
          string[strlen (create_alert_data->part_name)] = '\0';
          array_add (create_alert_data->event_data, string);

          openvas_free_string_var (&create_alert_data->part_data);
          openvas_free_string_var (&create_alert_data->part_name);
          openvas_append_string (&create_alert_data->part_data, "");
          openvas_append_string (&create_alert_data->part_name, "");
          set_client_state (CLIENT_CREATE_ALERT_EVENT);
          break;
        }
      CLOSE (CLIENT_CREATE_ALERT_EVENT_DATA, NAME);

      case CLIENT_CREATE_ALERT_METHOD_DATA:
        {
          gchar *string;

          assert (strcasecmp ("DATA", element_name) == 0);
          assert (create_alert_data->method_data);
          assert (create_alert_data->part_data);
          assert (create_alert_data->part_name);

          string = g_strconcat (create_alert_data->part_name,
                                "0",
                                create_alert_data->part_data,
                                NULL);
          string[strlen (create_alert_data->part_name)] = '\0';
          array_add (create_alert_data->method_data, string);

          openvas_free_string_var (&create_alert_data->part_data);
          openvas_free_string_var (&create_alert_data->part_name);
          openvas_append_string (&create_alert_data->part_data, "");
          openvas_append_string (&create_alert_data->part_name, "");
          set_client_state (CLIENT_CREATE_ALERT_METHOD);
          break;
        }
      CLOSE (CLIENT_CREATE_ALERT_METHOD_DATA, NAME);

      case CLIENT_CREATE_FILTER:
        {
          filter_t new_filter;

          assert (strcasecmp ("CREATE_FILTER", element_name) == 0);
          assert (create_filter_data->term != NULL);

          if (openvas_is_user_observer (current_credentials.username))
            {
              SEND_TO_CLIENT_OR_FAIL
               (XML_ERROR_SYNTAX ("create_filter",
                                  "CREATE is forbidden for observer users"));
            }
          else if (create_filter_data->copy)
            /* TODO make_unique (same for targets). */
            switch (copy_filter (create_filter_data->name,
                                 create_filter_data->comment,
                                 create_filter_data->copy,
                                 &new_filter))
              {
                case 0:
                  {
                    char *uuid;
                    uuid = filter_uuid (new_filter);
                    SENDF_TO_CLIENT_OR_FAIL (XML_OK_CREATED_ID ("create_filter"),
                                             uuid);
                    g_log ("event filter", G_LOG_LEVEL_MESSAGE,
                           "Filter %s has been created", uuid);
                    free (uuid);
                    break;
                  }
                case 1:
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_ERROR_SYNTAX ("create_filter",
                                      "Filter exists already"));
                  g_log ("event filter", G_LOG_LEVEL_MESSAGE,
                         "Filter could not be created");
                  break;
                case 2:
                  if (send_find_error_to_client ("create_filter",
                                                 "filter",
                                                 create_filter_data->copy,
                                                 write_to_client,
                                                 write_to_client_data))
                    {
                      error_send_to_client (error);
                      return;
                    }
                  g_log ("event filter", G_LOG_LEVEL_MESSAGE,
                         "Filter could not be created");
                  break;
                case -1:
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_INTERNAL_ERROR ("create_filter"));
                  g_log ("event filter", G_LOG_LEVEL_MESSAGE,
                         "Filter could not be created");
                  break;
              }
          else if (create_filter_data->name == NULL)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("create_filter",
                                "CREATE_FILTER requires a NAME"));
          else if (strlen (create_filter_data->name) == 0)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("create_filter",
                                "CREATE_FILTER name must be at"
                                " least one character long"));
          else switch (create_filter
                        (create_filter_data->name,
                         create_filter_data->comment,
                         create_filter_data->type,
                         create_filter_data->term,
                         create_filter_data->make_name_unique
                          && strcmp (create_filter_data->make_name_unique, "0"),
                         &new_filter))
            {
              case 0:
                {
                  char *uuid = filter_uuid (new_filter);
                  SENDF_TO_CLIENT_OR_FAIL (XML_OK_CREATED_ID ("create_filter"),
                                           uuid);
                  g_log ("event filter", G_LOG_LEVEL_MESSAGE,
                         "Filter %s has been created", uuid);
                  free (uuid);
                  break;
                }
              case 1:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("create_filter",
                                    "Filter exists already"));
                g_log ("event filter", G_LOG_LEVEL_MESSAGE,
                       "Filter could not be created");
                break;
              case 2:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("create_filter",
                                    "Type must be a valid OMP type"));
                g_log ("event filter", G_LOG_LEVEL_MESSAGE,
                       "Filter could not be created");
                break;
              default:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_INTERNAL_ERROR ("create_filter"));
                g_log ("event filter", G_LOG_LEVEL_MESSAGE,
                       "Filter could not be created");
                break;
            }

          create_filter_data_reset (create_filter_data);
          set_client_state (CLIENT_AUTHENTIC);
          break;
        }
      CLOSE (CLIENT_CREATE_FILTER, COMMENT);
      CLOSE (CLIENT_CREATE_FILTER, COPY);
      CLOSE (CLIENT_CREATE_FILTER, NAME);
      CLOSE (CLIENT_CREATE_FILTER, TERM);
      CLOSE (CLIENT_CREATE_FILTER, TYPE);

      CLOSE (CLIENT_CREATE_FILTER_NAME, MAKE_UNIQUE);

      case CLIENT_CREATE_LSC_CREDENTIAL:
        {
          lsc_credential_t new_lsc_credential;

          assert (strcasecmp ("CREATE_LSC_CREDENTIAL", element_name) == 0);
          assert (create_lsc_credential_data->name != NULL);
          assert (create_lsc_credential_data->login != NULL);

          if (openvas_is_user_observer (current_credentials.username))
            {
              SEND_TO_CLIENT_OR_FAIL
               (XML_ERROR_SYNTAX ("create_lsc_credential",
                                  "CREATE is forbidden for observer users"));
            }
          else if (create_lsc_credential_data->copy)
            switch (copy_lsc_credential (create_lsc_credential_data->name,
                                         create_lsc_credential_data->comment,
                                         create_lsc_credential_data->copy,
                                         &new_lsc_credential))
              {
                case 0:
                  {
                    char *uuid;
                    uuid = lsc_credential_uuid (new_lsc_credential);
                    SENDF_TO_CLIENT_OR_FAIL (XML_OK_CREATED_ID ("create_lsc_credential"),
                                             uuid);
                    g_log ("event lsc_credential", G_LOG_LEVEL_MESSAGE,
                           "LSC Credential %s has been created", uuid);
                    free (uuid);
                    break;
                  }
                case 1:
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_ERROR_SYNTAX ("create_lsc_credential",
                                      "Credential exists already"));
                  g_log ("event lsc_credential", G_LOG_LEVEL_MESSAGE,
                         "LSC Credential could not be created");
                  break;
                case 2:
                  if (send_find_error_to_client ("create_lsc_credential",
                                                 "lsc_credential",
                                                 create_lsc_credential_data->copy,
                                                 write_to_client,
                                                 write_to_client_data))
                    {
                      error_send_to_client (error);
                      return;
                    }
                  g_log ("event lsc_credential", G_LOG_LEVEL_MESSAGE,
                         "LSC Credential could not be created");
                  break;
                case -1:
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_INTERNAL_ERROR ("create_lsc_credential"));
                  g_log ("event lsc_credential", G_LOG_LEVEL_MESSAGE,
                         "LSC Credential could not be created");
                  break;
              }
          else if (strlen (create_lsc_credential_data->name) == 0)
            {
              SEND_TO_CLIENT_OR_FAIL
               (XML_ERROR_SYNTAX ("create_lsc_credential",
                                  "CREATE_LSC_CREDENTIAL name must be at"
                                  " least one character long"));
            }
          else if (strlen (create_lsc_credential_data->login) == 0)
            {
              SEND_TO_CLIENT_OR_FAIL
               (XML_ERROR_SYNTAX ("create_lsc_credential",
                                  "CREATE_LSC_CREDENTIAL login must be at"
                                  " least one character long"));
            }
          else if (create_lsc_credential_data->key
                   && ((create_lsc_credential_data->key_public == NULL)
                       || (create_lsc_credential_data->key_private == NULL)))
            {
              SEND_TO_CLIENT_OR_FAIL
               (XML_ERROR_SYNTAX ("create_lsc_credential",
                                  "CREATE_LSC_CREDENTIAL KEY requires a PUBLIC"
                                  " and a PRIVATE"));
            }
          else switch (create_lsc_credential
                        (create_lsc_credential_data->name,
                         create_lsc_credential_data->comment,
                         create_lsc_credential_data->login,
                         create_lsc_credential_data->key_public
                          ? create_lsc_credential_data->key_phrase
                          : create_lsc_credential_data->password,
                         create_lsc_credential_data->key_private,
                         create_lsc_credential_data->key_public,
                         &new_lsc_credential))
            {
              case 0:
                {
                  char *uuid = lsc_credential_uuid (new_lsc_credential);
                  SENDF_TO_CLIENT_OR_FAIL
                   (XML_OK_CREATED_ID ("create_lsc_credential"), uuid);
                  free (uuid);
                  break;
                }
              case 1:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("create_lsc_credential",
                                    "LSC Credential exists already"));
                break;
              case 2:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("create_lsc_credential",
                                    "Login may only contain alphanumeric"
                                    " characters if autogenerating"
                                    " credential"));
                break;
              default:
                assert (0);
              case -1:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_INTERNAL_ERROR ("create_lsc_credential"));
                break;
            }
          create_lsc_credential_data_reset (create_lsc_credential_data);
          set_client_state (CLIENT_AUTHENTIC);
          break;
        }
      CLOSE (CLIENT_CREATE_LSC_CREDENTIAL, COMMENT);
      CLOSE (CLIENT_CREATE_LSC_CREDENTIAL, COPY);
      CLOSE (CLIENT_CREATE_LSC_CREDENTIAL, KEY);
      CLOSE (CLIENT_CREATE_LSC_CREDENTIAL_KEY, PHRASE);
      CLOSE (CLIENT_CREATE_LSC_CREDENTIAL_KEY, PRIVATE);
      CLOSE (CLIENT_CREATE_LSC_CREDENTIAL_KEY, PUBLIC);
      CLOSE (CLIENT_CREATE_LSC_CREDENTIAL, LOGIN);
      CLOSE (CLIENT_CREATE_LSC_CREDENTIAL, NAME);
      CLOSE (CLIENT_CREATE_LSC_CREDENTIAL, PASSWORD);

      case CLIENT_CREATE_NOTE:
        {
          task_t task = 0;
          result_t result = 0;
          note_t new_note;
          int max;

          assert (strcasecmp ("CREATE_NOTE", element_name) == 0);

          if (openvas_is_user_observer (current_credentials.username))
            {
              SEND_TO_CLIENT_OR_FAIL
               (XML_ERROR_SYNTAX ("create_note",
                                  "CREATE is forbidden for observer users"));
            }
          else if (create_note_data->copy)
            switch (copy_note (create_note_data->copy, &new_note))
              {
                case 0:
                  {
                    char *uuid;
                    note_uuid (new_note, &uuid);
                    SENDF_TO_CLIENT_OR_FAIL (XML_OK_CREATED_ID ("create_note"),
                                             uuid);
                    g_log ("event note", G_LOG_LEVEL_MESSAGE,
                           "Note %s has been created", uuid);
                    free (uuid);
                    break;
                  }
                case 1:
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_ERROR_SYNTAX ("create_note",
                                      "Note exists already"));
                  g_log ("event note", G_LOG_LEVEL_MESSAGE,
                         "Note could not be created");
                  break;
                case 2:
                  if (send_find_error_to_client ("create_note",
                                                 "note",
                                                 create_note_data->copy,
                                                 write_to_client,
                                                 write_to_client_data))
                    {
                      error_send_to_client (error);
                      return;
                    }
                  g_log ("event note", G_LOG_LEVEL_MESSAGE,
                         "Note could not be created");
                  break;
                case -1:
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_INTERNAL_ERROR ("create_note"));
                  g_log ("event note", G_LOG_LEVEL_MESSAGE,
                         "Note could not be created");
                  break;
              }
          else if (create_note_data->nvt_oid == NULL)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("create_note",
                                "CREATE_NOTE requires an NVT entity"));
          else if (create_note_data->text == NULL)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("create_note",
                                "CREATE_NOTE requires a TEXT entity"));
          else if (create_note_data->hosts
                   && ((max = manage_max_hosts (create_note_data->hosts))
                       == -1))
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("create_note",
                                "Error in host specification"));
          else if (create_note_data->hosts && (max > MANAGE_MAX_HOSTS))
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("create_note",
                                "Host specification exceeds"
                                " " G_STRINGIFY (MANAGE_MAX_HOSTS) " hosts"));
          else if (create_note_data->task_id
                   && find_task_for_actions (create_note_data->task_id,
                                             &task,
                                             "g"))
            SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("create_note"));
          else if (create_note_data->task_id && task == 0)
            {
              if (send_find_error_to_client ("create_note",
                                             "task",
                                             create_note_data->task_id,
                                             write_to_client,
                                             write_to_client_data))
                {
                  error_send_to_client (error);
                  return;
                }
            }
          else if (create_note_data->result_id
                   && find_result_for_actions (create_note_data->result_id,
                                               &result,
                                               "g"))
            SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("create_note"));
          else if (create_note_data->result_id && result == 0)
            {
              if (send_find_error_to_client ("create_note",
                                             "result",
                                             create_note_data->result_id,
                                             write_to_client,
                                             write_to_client_data))
                {
                  error_send_to_client (error);
                  return;
                }
            }
          else switch (create_note (create_note_data->active,
                                    create_note_data->nvt_oid,
                                    create_note_data->text,
                                    create_note_data->hosts,
                                    create_note_data->port,
                                    create_note_data->threat,
                                    task,
                                    result,
                                    &new_note))
            {
              case 0:
                {
                  char *uuid;
                  note_uuid (new_note, &uuid);
                  SENDF_TO_CLIENT_OR_FAIL (XML_OK_CREATED_ID ("create_note"),
                                           uuid);
                  free (uuid);
                  break;
                }
              case 1:
                if (send_find_error_to_client ("create_note",
                                               "nvt",
                                               create_note_data->nvt_oid,
                                               write_to_client,
                                               write_to_client_data))
                  {
                    error_send_to_client (error);
                    return;
                  }
              case 2:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("create_note",
                                    "Error in port specification"));
                g_log ("event note", G_LOG_LEVEL_MESSAGE,
                       "Note could not be created");
                break;
                break;
              case -1:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_INTERNAL_ERROR ("create_note"));
                break;
              default:
                assert (0);
                SEND_TO_CLIENT_OR_FAIL
                 (XML_INTERNAL_ERROR ("create_note"));
                break;
            }
          create_note_data_reset (create_note_data);
          set_client_state (CLIENT_AUTHENTIC);
          break;
        }
      CLOSE (CLIENT_CREATE_NOTE, ACTIVE);
      CLOSE (CLIENT_CREATE_NOTE, COPY);
      CLOSE (CLIENT_CREATE_NOTE, HOSTS);
      CLOSE (CLIENT_CREATE_NOTE, NVT);
      CLOSE (CLIENT_CREATE_NOTE, PORT);
      CLOSE (CLIENT_CREATE_NOTE, RESULT);
      CLOSE (CLIENT_CREATE_NOTE, TASK);
      CLOSE (CLIENT_CREATE_NOTE, TEXT);
      CLOSE (CLIENT_CREATE_NOTE, THREAT);

      case CLIENT_CREATE_OVERRIDE:
        {
          task_t task = 0;
          result_t result = 0;
          override_t new_override;
          int max;

          assert (strcasecmp ("CREATE_OVERRIDE", element_name) == 0);

          if (openvas_is_user_observer (current_credentials.username))
            {
              SEND_TO_CLIENT_OR_FAIL
               (XML_ERROR_SYNTAX ("create_override",
                                  "CREATE is forbidden for observer users"));
            }
          else if (create_override_data->copy)
            switch (copy_override (create_override_data->copy, &new_override))
              {
                case 0:
                  {
                    char *uuid;
                    override_uuid (new_override, &uuid);
                    SENDF_TO_CLIENT_OR_FAIL (XML_OK_CREATED_ID ("create_override"),
                                             uuid);
                    g_log ("event override", G_LOG_LEVEL_MESSAGE,
                           "Override %s has been created", uuid);
                    free (uuid);
                    break;
                  }
                case 1:
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_ERROR_SYNTAX ("create_override",
                                      "Override exists already"));
                  g_log ("event override", G_LOG_LEVEL_MESSAGE,
                         "Override could not be created");
                  break;
                case 2:
                  if (send_find_error_to_client ("create_override",
                                                 "override",
                                                 create_override_data->copy,
                                                 write_to_client,
                                                 write_to_client_data))
                    {
                      error_send_to_client (error);
                      return;
                    }
                  g_log ("event override", G_LOG_LEVEL_MESSAGE,
                         "Override could not be created");
                  break;
                case -1:
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_INTERNAL_ERROR ("create_override"));
                  g_log ("event override", G_LOG_LEVEL_MESSAGE,
                         "Override could not be created");
                  break;
              }
          else if (create_override_data->nvt_oid == NULL)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("create_override",
                                "CREATE_OVERRIDE requires an NVT entity"));
          else if (create_override_data->text == NULL)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("create_override",
                                "CREATE_OVERRIDE requires a TEXT entity"));
          else if (create_override_data->hosts
                   && ((max = manage_max_hosts (create_override_data->hosts))
                       == -1))
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("create_override",
                                "Error in host specification"));
          else if (create_override_data->hosts && (max > MANAGE_MAX_HOSTS))
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("create_override",
                                "Host specification exceeds"
                                " " G_STRINGIFY (MANAGE_MAX_HOSTS) " hosts"));
          else if (create_override_data->new_threat == NULL)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("create_override",
                                "CREATE_OVERRIDE requires a NEW_THREAT"
                                " entity"));
          else if (create_override_data->task_id
              && find_task_for_actions (create_override_data->task_id,
                                        &task,
                                        "g"))
            SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("create_override"));
          else if (create_override_data->task_id && task == 0)
            {
              if (send_find_error_to_client ("create_override",
                                             "task",
                                             create_override_data->task_id,
                                             write_to_client,
                                             write_to_client_data))
                {
                  error_send_to_client (error);
                  return;
                }
            }
          else if (create_override_data->result_id
                   && find_result_for_actions (create_override_data->result_id,
                                               &result,
                                               "g"))
            SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("create_override"));
          else if (create_override_data->result_id && result == 0)
            {
              if (send_find_error_to_client ("create_override",
                                             "result",
                                             create_override_data->result_id,
                                             write_to_client,
                                             write_to_client_data))
                {
                  error_send_to_client (error);
                  return;
                }
            }
          else switch (create_override (create_override_data->active,
                                        create_override_data->nvt_oid,
                                        create_override_data->text,
                                        create_override_data->hosts,
                                        create_override_data->port,
                                        create_override_data->threat,
                                        create_override_data->new_threat,
                                        task,
                                        result,
                                        &new_override))
            {
              case 0:
                {
                  char *uuid;
                  override_uuid (new_override, &uuid);
                  SENDF_TO_CLIENT_OR_FAIL
                   (XML_OK_CREATED_ID ("create_override"), uuid);
                  free (uuid);
                  break;
                }
              case 1:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("create_override",
                                    "Error in port specification"));
                g_log ("event override", G_LOG_LEVEL_MESSAGE,
                       "Override could not be created");
                break;
              case -1:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_INTERNAL_ERROR ("create_override"));
                break;
              default:
                assert (0);
                SEND_TO_CLIENT_OR_FAIL
                 (XML_INTERNAL_ERROR ("create_override"));
                break;
            }
          create_override_data_reset (create_override_data);
          set_client_state (CLIENT_AUTHENTIC);
          break;
        }
      CLOSE (CLIENT_CREATE_OVERRIDE, ACTIVE);
      CLOSE (CLIENT_CREATE_OVERRIDE, COPY);
      CLOSE (CLIENT_CREATE_OVERRIDE, HOSTS);
      CLOSE (CLIENT_CREATE_OVERRIDE, NEW_THREAT);
      CLOSE (CLIENT_CREATE_OVERRIDE, NVT);
      CLOSE (CLIENT_CREATE_OVERRIDE, PORT);
      CLOSE (CLIENT_CREATE_OVERRIDE, RESULT);
      CLOSE (CLIENT_CREATE_OVERRIDE, TASK);
      CLOSE (CLIENT_CREATE_OVERRIDE, TEXT);
      CLOSE (CLIENT_CREATE_OVERRIDE, THREAT);

      case CLIENT_CREATE_PORT_LIST:
        {
          port_list_t new_port_list;
          array_t *manage_ranges;

          assert (strcasecmp ("CREATE_PORT_LIST", element_name) == 0);

          manage_ranges = NULL;

          /* The import element, GET_PORT_LISTS_RESPONSE, overrides any other
           * elements. */

          if (openvas_is_user_observer (current_credentials.username))
            {
              SEND_TO_CLIENT_OR_FAIL
               (XML_ERROR_SYNTAX ("create_port_list",
                                  "CREATE is forbidden for observer users"));
            }
          else if (create_port_list_data->import)
            {
              array_terminate (create_port_list_data->ranges);

              if (create_port_list_data->name == NULL)
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("create_port_list",
                                    "CREATE_PORT_LIST"
                                    " GET_PORT_LISTS_RESPONSE requires a"
                                    " NAME element"));
              else if (strlen (create_port_list_data->name) == 0)
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("create_port_list",
                                    "CREATE_PORT_LIST"
                                    " GET_PORT_LISTS_RESPONSE NAME must be"
                                    " at least one character long"));
              else if (create_port_list_data->id == NULL)
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("create_port_list",
                                    "CREATE_PORT_LIST"
                                    " GET_PORT_LISTS_RESPONSE requires an"
                                    " ID attribute"));
              else if (strlen (create_port_list_data->id) == 0)
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("create_port_list",
                                    "CREATE_PORT_LIST"
                                    " GET_PORT_LISTS_RESPONSE ID must be"
                                    " at least one character long"));
              else if (!is_uuid (create_port_list_data->id))
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("create_port_list",
                                    "CREATE_PORT_LIST"
                                    " GET_PORT_LISTS_RESPONSE ID must be"
                                    " a UUID"));
              else if ((manage_ranges = convert_to_manage_ranges
                                         (create_port_list_data->ranges))
                       == NULL)
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("create_port_list",
                                    "Error in GET_PORT_LISTS_RESPONSE ranges"));
              else switch (create_port_list
                            (create_port_list_data->id,
                             create_port_list_data->name,
                             create_port_list_data->comment,
                             NULL,
                             manage_ranges,
                             &new_port_list))
                {
                  case 1:
                    SEND_TO_CLIENT_OR_FAIL
                     (XML_ERROR_SYNTAX ("create_port_list",
                                        "Port list exists already"));
                    g_log ("event port_list", G_LOG_LEVEL_MESSAGE,
                           "Port list could not be created");
                    break;
                  case 2:
                    SEND_TO_CLIENT_OR_FAIL
                     (XML_ERROR_SYNTAX ("create_port_list",
                                        "Port list exists already, in"
                                        " trashcan"));
                    g_log ("event port_list", G_LOG_LEVEL_MESSAGE,
                           "Port list could not be created");
                    break;
                  case -1:
                    SEND_TO_CLIENT_OR_FAIL
                     (XML_INTERNAL_ERROR ("create_port_list"));
                    g_log ("event port_list", G_LOG_LEVEL_MESSAGE,
                           "Port List could not be created");
                    break;
                  default:
                    {
                      char *uuid = port_list_uuid (new_port_list);
                      SENDF_TO_CLIENT_OR_FAIL
                       (XML_OK_CREATED_ID ("create_port_list"),
                        uuid);
                      g_log ("event port_list", G_LOG_LEVEL_MESSAGE,
                             "Port List %s has been created", uuid);
                      free (uuid);
                      break;
                    }
                }
              /* Range fields are freed by the reset function below. */
              array_free (manage_ranges);
            }
          else if (create_port_list_data->copy)
            switch (copy_port_list (create_port_list_data->name,
                                    create_port_list_data->comment,
                                    create_port_list_data->copy,
                                    &new_port_list))
              {
                case 0:
                  {
                    char *uuid;
                    uuid = port_list_uuid (new_port_list);
                    SENDF_TO_CLIENT_OR_FAIL (XML_OK_CREATED_ID
                                             ("create_port_list"),
                                             uuid);
                    g_log ("event port_list", G_LOG_LEVEL_MESSAGE,
                           "Port List %s has been created", uuid);
                    free (uuid);
                    break;
                  }
                case 1:
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_ERROR_SYNTAX ("create_port_list",
                                      "Port List exists already"));
                  g_log ("event port_list", G_LOG_LEVEL_MESSAGE,
                         "Port List could not be created");
                  break;
                case 2:
                  if (send_find_error_to_client ("create_port_list",
                                                 "port_list",
                                                 create_port_list_data->copy,
                                                 write_to_client,
                                                 write_to_client_data))
                    {
                      error_send_to_client (error);
                      return;
                    }
                  g_log ("event port_list", G_LOG_LEVEL_MESSAGE,
                         "Port List could not be created");
                  break;
                case -1:
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_INTERNAL_ERROR ("create_port_list"));
                  g_log ("event port_list", G_LOG_LEVEL_MESSAGE,
                         "Port List could not be created");
                  break;
              }
          else if (create_port_list_data->name == NULL)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("create_port_list",
                                "CREATE_PORT_LIST requires a NAME"));
          else if (strlen (create_port_list_data->name) == 0)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("create_port_list",
                                "CREATE_PORT_LIST name must be at"
                                " least one character long"));
          else switch (create_port_list
                        (NULL,
                         create_port_list_data->name,
                         create_port_list_data->comment,
                         create_port_list_data->port_range,
                         NULL,
                         &new_port_list))
            {
              case 1:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("create_port_list",
                                    "Port list exists already"));
                g_log ("event port_list", G_LOG_LEVEL_MESSAGE,
                       "Port list could not be created");
                break;
              case 4:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("create_port_list",
                                    "Error in port range"));
                g_log ("event port_list", G_LOG_LEVEL_MESSAGE,
                       "Port list could not be created");
                break;
              case -1:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_INTERNAL_ERROR ("create_port_list"));
                g_log ("event port_list", G_LOG_LEVEL_MESSAGE,
                       "Port list could not be created");
                break;
              default:
                {
                  char *uuid = port_list_uuid (new_port_list);
                  SENDF_TO_CLIENT_OR_FAIL
                   (XML_OK_CREATED_ID ("create_port_list"), uuid);
                  g_log ("event port_list", G_LOG_LEVEL_MESSAGE,
                         "Port list %s has been created", uuid);
                  free (uuid);
                  break;
                }
            }

          create_port_list_data_reset (create_port_list_data);
          set_client_state (CLIENT_AUTHENTIC);
          break;
        }
      CLOSE (CLIENT_CREATE_PORT_LIST, COMMENT);
      CLOSE (CLIENT_CREATE_PORT_LIST, COPY);
      case CLIENT_CPL_GPLR:
        assert (strcasecmp ("GET_PORT_LISTS_RESPONSE", element_name) == 0);
        set_client_state (CLIENT_CREATE_PORT_LIST);
        break;
      CLOSE (CLIENT_CREATE_PORT_LIST, NAME);
      CLOSE (CLIENT_CREATE_PORT_LIST, PORT_RANGE);

      CLOSE (CLIENT_CPL_GPLR, PORT_LIST);
      CLOSE (CLIENT_CPL_GPLR_PORT_LIST, COMMENT);
      CLOSE (CLIENT_CPL_GPLR_PORT_LIST, IN_USE);
      CLOSE (CLIENT_CPL_GPLR_PORT_LIST, NAME);
      CLOSE_READ_OVER (CLIENT_CPL_GPLR_PORT_LIST, TARGETS);
      CLOSE (CLIENT_CPL_GPLR_PORT_LIST, PORT_RANGE);
      CLOSE (CLIENT_CPL_GPLR_PORT_LIST, PORT_RANGES);

      case CLIENT_CPL_GPLR_PORT_LIST_PORT_RANGES_PORT_RANGE:
        {
          assert (strcasecmp ("PORT_RANGE", element_name) == 0);
          assert (create_port_list_data->ranges);

          array_add (create_port_list_data->ranges,
                     create_port_list_data->range);
          create_port_list_data->range = NULL;
          set_client_state (CLIENT_CPL_GPLR_PORT_LIST_PORT_RANGES);
          break;
        }
      CLOSE (CLIENT_CPL_GPLR_PORT_LIST_PORT_RANGES_PORT_RANGE, COMMENT);
      CLOSE (CLIENT_CPL_GPLR_PORT_LIST_PORT_RANGES_PORT_RANGE, END);
      CLOSE (CLIENT_CPL_GPLR_PORT_LIST_PORT_RANGES_PORT_RANGE, START);
      CLOSE (CLIENT_CPL_GPLR_PORT_LIST_PORT_RANGES_PORT_RANGE, TYPE);

      case CLIENT_CREATE_PORT_RANGE:
        {
          port_range_t new_port_range;

          assert (strcasecmp ("CREATE_PORT_RANGE", element_name) == 0);

          if (openvas_is_user_observer (current_credentials.username))
            {
              SEND_TO_CLIENT_OR_FAIL
               (XML_ERROR_SYNTAX ("create_port_range",
                                  "CREATE is forbidden for observer users"));
            }
          else if (create_port_range_data->start == NULL
                   || create_port_range_data->end == NULL
                   || create_port_range_data->port_list_id == NULL)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("create_port_range",
                                "CREATE_PORT_RANGE requires a START, END and"
                                " PORT_LIST ID"));
          else switch (create_port_range
                        (create_port_range_data->port_list_id,
                         create_port_range_data->type,
                         create_port_range_data->start,
                         create_port_range_data->end,
                         create_port_range_data->comment,
                         &new_port_range))
            {
              case 1:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("create_port_range",
                                    "Port range START must be a number"
                                    " 1-65535"));
                g_log ("event port_range", G_LOG_LEVEL_MESSAGE,
                       "Port range could not be created");
                break;
              case 2:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("create_port_range",
                                    "Port range END must be a number"
                                    " 1-65535"));
                g_log ("event port_range", G_LOG_LEVEL_MESSAGE,
                       "Port range could not be created");
                break;
              case 3:
                if (send_find_error_to_client
                     ("create_port_range",
                      "port_range",
                      create_port_range_data->port_list_id,
                      write_to_client,
                      write_to_client_data))
                  {
                    error_send_to_client (error);
                    return;
                  }
                g_log ("event port_range", G_LOG_LEVEL_MESSAGE,
                       "Port range could not be created");
                break;
              case 4:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("create_port_range",
                                    "Port range TYPE must be TCP or UDP"));
                g_log ("event port_range", G_LOG_LEVEL_MESSAGE,
                       "Port range could not be created");
                break;
              case 5:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("create_port_range",
                                    "Port list is in use"));
                break;
              case 6:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("create_port_range",
                                    "New range overlaps an existing"
                                    " range"));
                break;
              case -1:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_INTERNAL_ERROR ("create_port_range"));
                g_log ("event port_range", G_LOG_LEVEL_MESSAGE,
                       "Port range could not be created");
                break;
              default:
                {
                  char *uuid;
                  uuid = port_range_uuid (new_port_range);
                  SENDF_TO_CLIENT_OR_FAIL
                   (XML_OK_CREATED_ID ("create_port_range"), uuid);
                  g_log ("event port_range", G_LOG_LEVEL_MESSAGE,
                         "Port range %s has been created", uuid);
                  free (uuid);
                  break;
                }
            }

          create_port_range_data_reset (create_port_range_data);
          set_client_state (CLIENT_AUTHENTIC);
          break;
        }
      CLOSE (CLIENT_CREATE_PORT_RANGE, COMMENT);
      CLOSE (CLIENT_CREATE_PORT_RANGE, END);
      CLOSE (CLIENT_CREATE_PORT_RANGE, START);
      CLOSE (CLIENT_CREATE_PORT_RANGE, TYPE);
      CLOSE (CLIENT_CREATE_PORT_RANGE, PORT_LIST);

      case CLIENT_CREATE_REPORT:
        {
          char *uuid;

          assert (strcasecmp ("CREATE_REPORT", element_name) == 0);

          array_terminate (create_report_data->results);
          array_terminate (create_report_data->host_ends);
          array_terminate (create_report_data->host_starts);
          array_terminate (create_report_data->details);

          if (openvas_is_user_observer (current_credentials.username))
            {
              SEND_TO_CLIENT_OR_FAIL
               (XML_ERROR_SYNTAX ("create_report",
                                  "CREATE is forbidden for observer users"));
            }
          else if (create_report_data->results == NULL)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("create_report",
                                "CREATE_REPORT requires a REPORT element"));
          else if (create_report_data->type
                   && strcmp (create_report_data->type, "scan"))
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("create_report",
                                "CREATE_REPORT type must be 'scan'"));
          else switch (create_report
                        (create_report_data->results,
                         create_report_data->task_id,
                         create_report_data->task_name,
                         create_report_data->task_comment,
                         create_report_data->scan_start,
                         create_report_data->scan_end,
                         create_report_data->host_starts,
                         create_report_data->host_ends,
                         create_report_data->details,
                         &uuid))
            {
              case -1:
              case -2:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_INTERNAL_ERROR ("create_report"));
                g_log ("event report", G_LOG_LEVEL_MESSAGE,
                       "Report could not be created");
                break;
              case -3:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("create_report",
                                    "CREATE_REPORT TASK_NAME is required"));
                g_log ("event report", G_LOG_LEVEL_MESSAGE,
                       "Report could not be created");
                break;
              case -4:
                g_log ("event report", G_LOG_LEVEL_MESSAGE,
                       "Report could not be created");
                if (send_find_error_to_client
                     ("create_report",
                      "task",
                      create_report_data->task_id,
                      write_to_client,
                      write_to_client_data))
                  {
                    error_send_to_client (error);
                    return;
                  }
                break;
              case -5:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("create_report",
                                    "CREATE_REPORT TASK must be a container"));
                g_log ("event report", G_LOG_LEVEL_MESSAGE,
                       "Report could not be created");

                break;
              default:
                {
                  SENDF_TO_CLIENT_OR_FAIL
                   (XML_OK_CREATED_ID ("create_report"),
                    uuid);
                  g_log ("event report", G_LOG_LEVEL_MESSAGE,
                         "Report %s has been created", uuid);
                  free (uuid);
                  break;
                }
            }

          omp_parser->importing = 0;
          create_report_data_reset (create_report_data);
          set_client_state (CLIENT_AUTHENTIC);
          break;
        }
      CLOSE (CLIENT_CREATE_REPORT, REPORT);
      case CLIENT_CREATE_REPORT_RR:
        assert (strcasecmp ("REPORT", element_name) == 0);
        if (create_report_data->wrapper)
          set_client_state (CLIENT_CREATE_REPORT_REPORT);
        else
          set_client_state (CLIENT_CREATE_REPORT);
        break;
      CLOSE_READ_OVER (CLIENT_CREATE_REPORT_RR, FILTERS);
      CLOSE_READ_OVER (CLIENT_CREATE_REPORT_RR, HOST_COUNT);
      case CLIENT_CREATE_REPORT_RR_HOST_END:
        assert (strcasecmp ("HOST_END", element_name) == 0);

        if (create_report_data->host_end_host)
          {
            create_report_result_t *result;

            assert (create_report_data->host_ends);
            assert (create_report_data->host_end_host);

            result = g_malloc (sizeof (create_report_result_t));
            result->description = create_report_data->host_end;
            result->host = create_report_data->host_end_host;

            array_add (create_report_data->host_ends, result);

            create_report_data->host_end = NULL;
            create_report_data->host_end_host = NULL;
          }
        else
          openvas_free_string_var (&create_report_data->host_end);

        set_client_state (CLIENT_CREATE_REPORT_RR);
        break;
      case CLIENT_CREATE_REPORT_RR_HOST_START:
        assert (strcasecmp ("HOST_START", element_name) == 0);

        if (create_report_data->host_start_host)
          {
            create_report_result_t *result;

            assert (create_report_data->host_starts);
            assert (create_report_data->host_start);
            assert (create_report_data->host_start_host);

            result = g_malloc (sizeof (create_report_result_t));
            result->description = create_report_data->host_start;
            result->host = create_report_data->host_start_host;

            array_add (create_report_data->host_starts, result);

            create_report_data->host_start = NULL;
            create_report_data->host_start_host = NULL;
          }
        else
          openvas_free_string_var (&create_report_data->host_start);

        set_client_state (CLIENT_CREATE_REPORT_RR);
        break;
      CLOSE_READ_OVER (CLIENT_CREATE_REPORT_RR, HOSTS);
      CLOSE_READ_OVER (CLIENT_CREATE_REPORT_RR, PORTS);
      CLOSE_READ_OVER (CLIENT_CREATE_REPORT_RR, REPORT_FORMAT);
      CLOSE (CLIENT_CREATE_REPORT_RR, RESULTS);
      CLOSE_READ_OVER (CLIENT_CREATE_REPORT_RR, SCAN_RUN_STATUS);
      CLOSE (CLIENT_CREATE_REPORT_RR, SCAN_END);
      CLOSE (CLIENT_CREATE_REPORT_RR, SCAN_START);
      CLOSE_READ_OVER (CLIENT_CREATE_REPORT_RR, SORT);
      CLOSE_READ_OVER (CLIENT_CREATE_REPORT_RR, TASK);
      CLOSE_READ_OVER (CLIENT_CREATE_REPORT_RR, RESULT_COUNT);

      CLOSE (CLIENT_CREATE_REPORT_RR_HOST_END, HOST);
      CLOSE (CLIENT_CREATE_REPORT_RR_HOST_START, HOST);

      case CLIENT_CREATE_REPORT_RR_H:
        {
          openvas_free_string_var (&create_report_data->ip);
          set_client_state (CLIENT_CREATE_REPORT_RR);
          break;
        }

      CLOSE (CLIENT_CREATE_REPORT_RR_H, IP);
      CLOSE (CLIENT_CREATE_REPORT_RR_H, START);
      CLOSE (CLIENT_CREATE_REPORT_RR_H, END);

      case CLIENT_CREATE_REPORT_RR_H_DETAIL:
        {
          assert (strcasecmp ("DETAIL", element_name) == 0);
          assert (create_report_data->details);

          if (create_report_data->ip)
            {
              host_detail_t *detail;

              detail = g_malloc (sizeof (host_detail_t));
              detail->ip = g_strdup (create_report_data->ip);
              detail->name = create_report_data->detail_name;
              detail->source_desc = create_report_data->detail_source_desc;
              detail->source_name = create_report_data->detail_source_name;
              detail->source_type = create_report_data->detail_source_type;
              detail->value = create_report_data->detail_value;

              array_add (create_report_data->details, detail);

              create_report_data->detail_name = NULL;
              create_report_data->detail_source_desc = NULL;
              create_report_data->detail_source_name = NULL;
              create_report_data->detail_source_type = NULL;
              create_report_data->detail_value = NULL;
            }

          set_client_state (CLIENT_CREATE_REPORT_RR_H);
          break;
        }

      CLOSE (CLIENT_CREATE_REPORT_RR_H_DETAIL, NAME);
      CLOSE (CLIENT_CREATE_REPORT_RR_H_DETAIL, VALUE);
      CLOSE (CLIENT_CREATE_REPORT_RR_H_DETAIL, SOURCE);

      CLOSE (CLIENT_CREATE_REPORT_RR_H_DETAIL_SOURCE, TYPE);
      CLOSE (CLIENT_CREATE_REPORT_RR_H_DETAIL_SOURCE, NAME);
      case CLIENT_CREATE_REPORT_RR_H_DETAIL_SOURCE_DESC:
        assert (strcasecmp ("DESCRIPTION", element_name) == 0);
        set_client_state (CLIENT_CREATE_REPORT_RR_H_DETAIL_SOURCE);
        break;

      case CLIENT_CREATE_REPORT_RR_RESULTS_RESULT:
        {
          create_report_result_t *result;

          assert (strcasecmp ("RESULT", element_name) == 0);
          assert (create_report_data->results);
          assert (create_report_data->result_description);
          assert (create_report_data->result_host);
          assert (create_report_data->result_nvt_oid);
          assert (create_report_data->result_port);
          assert (create_report_data->result_subnet);
          assert (create_report_data->result_threat);

          result = g_malloc (sizeof (create_report_result_t));
          result->description = create_report_data->result_description;
          result->host = create_report_data->result_host;
          result->nvt_oid = create_report_data->result_nvt_oid;
          result->port = create_report_data->result_port;
          result->subnet = create_report_data->result_subnet;
          result->threat = create_report_data->result_threat;

          array_add (create_report_data->results, result);

          create_report_data->result_description = NULL;
          create_report_data->result_host = NULL;
          create_report_data->result_nvt_oid = NULL;
          create_report_data->result_port = NULL;
          create_report_data->result_subnet = NULL;
          create_report_data->result_threat = NULL;

          set_client_state (CLIENT_CREATE_REPORT_RR_RESULTS);
          break;
        }
      CLOSE (CLIENT_CREATE_REPORT_RR_RESULTS_RESULT, DESCRIPTION);
      CLOSE_READ_OVER (CLIENT_CREATE_REPORT_RR_RESULTS_RESULT, DETECTION);
      CLOSE (CLIENT_CREATE_REPORT_RR_RESULTS_RESULT, HOST);
      CLOSE_READ_OVER (CLIENT_CREATE_REPORT_RR_RESULTS_RESULT, NOTES);
      CLOSE (CLIENT_CREATE_REPORT_RR_RESULTS_RESULT, NVT);
      CLOSE (CLIENT_CREATE_REPORT_RR_RESULTS_RESULT, ORIGINAL_THREAT);
      CLOSE_READ_OVER (CLIENT_CREATE_REPORT_RR_RESULTS_RESULT, OVERRIDES);
      CLOSE (CLIENT_CREATE_REPORT_RR_RESULTS_RESULT, PORT);
      CLOSE (CLIENT_CREATE_REPORT_RR_RESULTS_RESULT, SUBNET);
      CLOSE (CLIENT_CREATE_REPORT_RR_RESULTS_RESULT, THREAT);

      CLOSE (CLIENT_CREATE_REPORT_RR_RESULTS_RESULT_NVT, BID);
      CLOSE (CLIENT_CREATE_REPORT_RR_RESULTS_RESULT_NVT, CVE);
      CLOSE (CLIENT_CREATE_REPORT_RR_RESULTS_RESULT_NVT, CVSS_BASE);
      CLOSE (CLIENT_CREATE_REPORT_RR_RESULTS_RESULT_NVT, FAMILY);
      CLOSE (CLIENT_CREATE_REPORT_RR_RESULTS_RESULT_NVT, NAME);
      CLOSE (CLIENT_CREATE_REPORT_RR_RESULTS_RESULT_NVT, RISK_FACTOR);
      CLOSE (CLIENT_CREATE_REPORT_RR_RESULTS_RESULT_NVT, XREF);
      CLOSE (CLIENT_CREATE_REPORT_RR_RESULTS_RESULT_NVT, CERT);

      CLOSE (CLIENT_CREATE_REPORT_RR_RESULTS_RESULT_NVT_CERT, CERT_REF);

      CLOSE (CLIENT_CREATE_REPORT, TASK);
      CLOSE (CLIENT_CREATE_REPORT_TASK, COMMENT);
      CLOSE (CLIENT_CREATE_REPORT_TASK, NAME);

      case CLIENT_CREATE_REPORT_FORMAT:
        {
          report_format_t new_report_format;

          assert (strcasecmp ("CREATE_REPORT_FORMAT", element_name) == 0);

          /* For now the import element, GET_REPORT_FORMATS_RESPONSE, overrides
           * any other elements. */

          if (openvas_is_user_observer (current_credentials.username))
            {
              SEND_TO_CLIENT_OR_FAIL
               (XML_ERROR_SYNTAX ("create_report_format",
                                  "CREATE is forbidden for observer users"));
            }
          else if (create_report_format_data->copy)
            {
              switch (copy_report_format (create_report_format_data->name,
                                          create_report_format_data->copy,
                                          &new_report_format))
              {
                case 0:
                  {
                    char *uuid;
                    uuid = report_format_uuid (new_report_format);
                    SENDF_TO_CLIENT_OR_FAIL (XML_OK_CREATED_ID
                                             ("create_report_format"),
                                             uuid);
                    g_log ("event report_format", G_LOG_LEVEL_MESSAGE,
                           "Report Format %s has been created", uuid);
                    free (uuid);
                    break;
                  }
                case 1:
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_ERROR_SYNTAX ("create_report_format",
                                      "Report Format exists already"));
                  g_log ("event report_format", G_LOG_LEVEL_MESSAGE,
                         "Report Format could not be created");
                  break;
                case 2:
                  if (send_find_error_to_client ("create_report_format",
                                                 "report_format",
                                                 create_report_format_data->copy,
                                                 write_to_client,
                                                 write_to_client_data))
                    {
                      error_send_to_client (error);
                      return;
                    }
                  g_log ("event report_format", G_LOG_LEVEL_MESSAGE,
                         "Report Format could not be created");
                  break;
                case -1:
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_INTERNAL_ERROR ("create_report_format"));
                  g_log ("event report_format", G_LOG_LEVEL_MESSAGE,
                         "Report Format could not be created");
                  break;
              }
            }
          else if (create_report_format_data->import)
            {
              array_terminate (create_report_format_data->files);
              array_terminate (create_report_format_data->params);
              array_terminate (create_report_format_data->params_options);

              if (create_report_format_data->name == NULL)
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("create_report_format",
                                    "CREATE_REPORT_FORMAT"
                                    " GET_REPORT_FORMATS_RESPONSE requires a"
                                    " NAME element"));
              else if (strlen (create_report_format_data->name) == 0)
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("create_report_format",
                                    "CREATE_REPORT_FORMAT"
                                    " GET_REPORT_FORMATS_RESPONSE NAME must be"
                                    " at least one character long"));
              else if (create_report_format_data->id == NULL)
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("create_report_format",
                                    "CREATE_REPORT_FORMAT"
                                    " GET_REPORT_FORMATS_RESPONSE requires an"
                                    " ID attribute"));
              else if (strlen (create_report_format_data->id) == 0)
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("create_report_format",
                                    "CREATE_REPORT_FORMAT"
                                    " GET_REPORT_FORMATS_RESPONSE ID must be"
                                    " at least one character long"));
              else if (!is_uuid (create_report_format_data->id))
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("create_report_format",
                                    "CREATE_REPORT_FORMAT"
                                    " GET_REPORT_FORMATS_RESPONSE ID must be"
                                    " a UUID"));
              else switch (create_report_format
                            (create_report_format_data->id,
                             create_report_format_data->name,
                             create_report_format_data->content_type,
                             create_report_format_data->extension,
                             create_report_format_data->summary,
                             create_report_format_data->description,
                             create_report_format_data->global
                               && strcmp (create_report_format_data->global,
                                          "0"),
                             create_report_format_data->files,
                             create_report_format_data->params,
                             create_report_format_data->params_options,
                             create_report_format_data->signature,
                             &new_report_format))
                {
                  case -1:
                    SEND_TO_CLIENT_OR_FAIL
                     (XML_INTERNAL_ERROR ("create_report_format"));
                    g_log ("event report_format", G_LOG_LEVEL_MESSAGE,
                           "Report format could not be created");
                    break;
                  case 1:
                    SEND_TO_CLIENT_OR_FAIL
                     (XML_ERROR_SYNTAX ("create_report_format",
                                        "Report format exists already"));
                    g_log ("event report_format", G_LOG_LEVEL_MESSAGE,
                           "Report format could not be created");
                    break;
                  case 2:
                    SEND_TO_CLIENT_OR_FAIL
                     (XML_ERROR_SYNTAX ("create_report_format",
                                        "Every FILE must have a name"
                                        " attribute"));
                    g_log ("event report_format", G_LOG_LEVEL_MESSAGE,
                           "Report format could not be created");
                    break;
                  case 3:
                    SEND_TO_CLIENT_OR_FAIL
                     (XML_ERROR_SYNTAX ("create_report_format",
                                        "Parameter value validation failed"));
                    g_log ("event report_format", G_LOG_LEVEL_MESSAGE,
                           "Report format could not be created");
                    break;
                  case 4:
                    SEND_TO_CLIENT_OR_FAIL
                     (XML_ERROR_SYNTAX ("create_report_format",
                                        "Parameter default validation failed"));
                    g_log ("event report_format", G_LOG_LEVEL_MESSAGE,
                           "Report format could not be created");
                    break;
                  case 5:
                    SEND_TO_CLIENT_OR_FAIL
                     (XML_ERROR_SYNTAX ("create_report_format",
                                        "CREATE_REPORT_FORMAT PARAM requires a"
                                        " DEFAULT element"));
                    g_log ("event report_format", G_LOG_LEVEL_MESSAGE,
                           "Report format could not be created");
                    break;
                  case 6:
                    SEND_TO_CLIENT_OR_FAIL
                     (XML_ERROR_SYNTAX ("create_report_format",
                                        "CREATE_REPORT_FORMAT PARAM MIN or MAX"
                                        " out of range"));
                    g_log ("event report_format", G_LOG_LEVEL_MESSAGE,
                           "Report format could not be created");
                    break;
                  case 7:
                    SEND_TO_CLIENT_OR_FAIL
                     (XML_ERROR_SYNTAX ("create_report_format",
                                        "CREATE_REPORT_FORMAT PARAM requires a"
                                        " TYPE element"));
                    g_log ("event report_format", G_LOG_LEVEL_MESSAGE,
                           "Report format could not be created");
                    break;
                  case 8:
                    SEND_TO_CLIENT_OR_FAIL
                     (XML_ERROR_SYNTAX ("create_report_format",
                                        "Duplicate PARAM name"));
                    g_log ("event report_format", G_LOG_LEVEL_MESSAGE,
                           "Report format could not be created");
                    break;
                  case 9:
                    SEND_TO_CLIENT_OR_FAIL
                     (XML_ERROR_SYNTAX ("create_report_format",
                                        "Bogus PARAM type"));
                    g_log ("event report_format", G_LOG_LEVEL_MESSAGE,
                           "Report format could not be created");
                    break;
                  default:
                    {
                      char *uuid = report_format_uuid (new_report_format);
                      SENDF_TO_CLIENT_OR_FAIL
                       (XML_OK_CREATED_ID ("create_report_format"),
                        uuid);
                      g_log ("event report_format", G_LOG_LEVEL_MESSAGE,
                             "Report format %s has been created", uuid);
                      free (uuid);
                      break;
                    }
                }
            }
          else
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("create_report_format",
                                "CREATE_REPORT_FORMAT requires a"
                                " GET_REPORT_FORMATS element"));

          create_report_format_data_reset (create_report_format_data);
          set_client_state (CLIENT_AUTHENTIC);
          break;
        }
      CLOSE (CLIENT_CREATE_REPORT_FORMAT, COPY);
      case CLIENT_CRF_GRFR:
        assert (strcasecmp ("GET_REPORT_FORMATS_RESPONSE", element_name) == 0);
        set_client_state (CLIENT_CREATE_REPORT_FORMAT);
        break;
      CLOSE (CLIENT_CRF_GRFR, REPORT_FORMAT);
      CLOSE (CLIENT_CRF_GRFR_REPORT_FORMAT, CONTENT_TYPE);
      CLOSE (CLIENT_CRF_GRFR_REPORT_FORMAT, DESCRIPTION);
      CLOSE (CLIENT_CRF_GRFR_REPORT_FORMAT, EXTENSION);
      case CLIENT_CRF_GRFR_REPORT_FORMAT_FILE:
        {
          gchar *string;

          assert (strcasecmp ("FILE", element_name) == 0);
          assert (create_report_format_data->files);
          assert (create_report_format_data->file);
          assert (create_report_format_data->file_name);

          string = g_strconcat (create_report_format_data->file_name,
                                "0",
                                create_report_format_data->file,
                                NULL);
          string[strlen (create_report_format_data->file_name)] = '\0';
          array_add (create_report_format_data->files, string);
          openvas_free_string_var (&create_report_format_data->file);
          openvas_free_string_var (&create_report_format_data->file_name);
          set_client_state (CLIENT_CRF_GRFR_REPORT_FORMAT);
          break;
        }
      CLOSE (CLIENT_CRF_GRFR_REPORT_FORMAT, GLOBAL);
      CLOSE (CLIENT_CRF_GRFR_REPORT_FORMAT, NAME);
      case CLIENT_CRF_GRFR_REPORT_FORMAT_PARAM:
        {
          create_report_format_param_t *param;

          assert (strcasecmp ("PARAM", element_name) == 0);
          assert (create_report_format_data->params);
          assert (create_report_format_data->param_name);
          assert (create_report_format_data->param_value);

          param = g_malloc (sizeof (*param));
          param->fallback
           = create_report_format_data->param_default
              ? g_strdup (create_report_format_data->param_default)
              : NULL;
          param->name = g_strdup (create_report_format_data->param_name);
          param->type
           = create_report_format_data->param_type
              ? g_strdup (create_report_format_data->param_type)
              : NULL;
          param->type_max
           = create_report_format_data->param_type_max
              ? g_strdup (create_report_format_data->param_type_max)
              : NULL;
          param->type_min
           = create_report_format_data->param_type_min
              ? g_strdup (create_report_format_data->param_type_min)
              : NULL;
          param->value = g_strdup (create_report_format_data->param_value);

          array_add (create_report_format_data->params, param);
          openvas_free_string_var (&create_report_format_data->param_default);
          openvas_free_string_var (&create_report_format_data->param_name);
          openvas_free_string_var (&create_report_format_data->param_type);
          openvas_free_string_var (&create_report_format_data->param_type_max);
          openvas_free_string_var (&create_report_format_data->param_type_min);
          openvas_free_string_var (&create_report_format_data->param_value);

          array_terminate (create_report_format_data->param_options);
          array_add (create_report_format_data->params_options,
                     create_report_format_data->param_options);

          set_client_state (CLIENT_CRF_GRFR_REPORT_FORMAT);
          break;
        }
      CLOSE (CLIENT_CRF_GRFR_REPORT_FORMAT_PARAM, DEFAULT);
      CLOSE (CLIENT_CRF_GRFR_REPORT_FORMAT_PARAM, NAME);
      CLOSE (CLIENT_CRF_GRFR_REPORT_FORMAT_PARAM, TYPE);
      CLOSE (CLIENT_CRF_GRFR_REPORT_FORMAT_PARAM, OPTIONS);
      CLOSE (CLIENT_CRF_GRFR_REPORT_FORMAT_PARAM, VALUE);
      CLOSE (CLIENT_CRF_GRFR_REPORT_FORMAT, PREDEFINED);
      CLOSE (CLIENT_CRF_GRFR_REPORT_FORMAT, SIGNATURE);
      CLOSE (CLIENT_CRF_GRFR_REPORT_FORMAT, SUMMARY);
      CLOSE (CLIENT_CRF_GRFR_REPORT_FORMAT, TRUST);

      case CLIENT_CRF_GRFR_REPORT_FORMAT_PARAM_OPTIONS_OPTION:
        assert (strcasecmp ("OPTION", element_name) == 0);
        array_add (create_report_format_data->param_options,
                   create_report_format_data->param_option);
        create_report_format_data->param_option = NULL;
        set_client_state (CLIENT_CRF_GRFR_REPORT_FORMAT_PARAM_OPTIONS);
        break;

      CLOSE (CLIENT_CRF_GRFR_REPORT_FORMAT_PARAM_TYPE, MAX);
      CLOSE (CLIENT_CRF_GRFR_REPORT_FORMAT_PARAM_TYPE, MIN);

      case CLIENT_CREATE_SCHEDULE:
        {
          time_t first_time, period, period_months, duration;
          schedule_t new_schedule;

          period_months = 0;

          assert (strcasecmp ("CREATE_SCHEDULE", element_name) == 0);

          if (openvas_is_user_observer (current_credentials.username))
            {
              SEND_TO_CLIENT_OR_FAIL
               (XML_ERROR_SYNTAX ("create_schedule",
                                  "CREATE is forbidden for observer users"));
            }
          else if (create_schedule_data->copy)
            switch (copy_schedule (create_schedule_data->name,
                                   create_schedule_data->comment,
                                   create_schedule_data->copy,
                                   &new_schedule))
              {
                case 0:
                  {
                    char *uuid;
                    uuid = schedule_uuid (new_schedule);
                    SENDF_TO_CLIENT_OR_FAIL (XML_OK_CREATED_ID ("create_schedule"),
                                             uuid);
                    g_log ("event schedule", G_LOG_LEVEL_MESSAGE,
                           "Schedule %s has been created", uuid);
                    free (uuid);
                    break;
                  }
                case 1:
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_ERROR_SYNTAX ("create_schedule",
                                      "Schedule exists already"));
                  g_log ("event schedule", G_LOG_LEVEL_MESSAGE,
                         "Schedule could not be created");
                  break;
                case 2:
                  if (send_find_error_to_client ("create_schedule",
                                                 "schedule",
                                                 create_schedule_data->copy,
                                                 write_to_client,
                                                 write_to_client_data))
                    {
                      error_send_to_client (error);
                      return;
                    }
                  g_log ("event schedule", G_LOG_LEVEL_MESSAGE,
                         "Schedule could not be created");
                  break;
                case -1:
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_INTERNAL_ERROR ("create_schedule"));
                  g_log ("event schedule", G_LOG_LEVEL_MESSAGE,
                         "Schedule could not be created");
                  break;
              }
          else if (create_schedule_data->name == NULL)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("create_schedule",
                                "CREATE_SCHEDULE requires a NAME entity"));
          else if ((first_time = time_from_strings
                                  (create_schedule_data->first_time_hour,
                                   create_schedule_data->first_time_minute,
                                   create_schedule_data->first_time_day_of_month,
                                   create_schedule_data->first_time_month,
                                   create_schedule_data->first_time_year,
                                   NULL))
                   == -1)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("create_schedule",
                                "Failed to create time from FIRST_TIME"
                                " elements"));
          else if ((period = interval_from_strings
                              (create_schedule_data->period,
                               create_schedule_data->period_unit,
                               &period_months))
                   == -3)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("create_schedule",
                                "PERIOD out of range"));
          else if (period < -1)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("create_schedule",
                                "Failed to create interval from PERIOD"));
          else if ((duration = interval_from_strings
                                (create_schedule_data->duration,
                                 create_schedule_data->duration_unit,
                                 NULL))
                   == -3)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("create_schedule",
                                "DURATION out of range"));
          else if (duration < -1)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("create_schedule",
                                "Failed to create interval from DURATION"));
#if 0
          /* The actual time of a period in months can vary, so it's extremely
           * hard to do this check.  The schedule will still work fine if the
           * duration is longer than the period. */
          else if (period_months
                   && (duration > (period_months * 60 * 60 * 24 * 28)))
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("create_schedule",
                                "Duration too long for number of months"));
#endif
          else if (period && (duration > period))
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("create_schedule",
                                "Duration is longer than period"));
          else switch (create_schedule (create_schedule_data->name,
                                        create_schedule_data->comment,
                                        first_time,
                                        period,
                                        period_months,
                                        duration,
                                        &new_schedule))
            {
              case 0:
                {
                  char *uuid = schedule_uuid (new_schedule);
                  SENDF_TO_CLIENT_OR_FAIL
                   (XML_OK_CREATED_ID ("create_schedule"), uuid);
                  g_log ("event schedule", G_LOG_LEVEL_MESSAGE,
                         "Schedule %s has been created", uuid);
                  free (uuid);
                  break;
                }
              case 1:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("create_schedule",
                                    "Schedule exists already"));
                g_log ("event schedule", G_LOG_LEVEL_MESSAGE,
                       "Schedule could not be created");
                break;
              case -1:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_INTERNAL_ERROR ("create_schedule"));
                g_log ("event schedule", G_LOG_LEVEL_MESSAGE,
                       "Schedule could not be created");
                break;
              default:
                assert (0);
                SEND_TO_CLIENT_OR_FAIL
                 (XML_INTERNAL_ERROR ("create_schedule"));
                g_log ("event schedule", G_LOG_LEVEL_MESSAGE,
                       "Schedule could not be created");
                break;
            }
          create_schedule_data_reset (create_schedule_data);
          set_client_state (CLIENT_AUTHENTIC);
          break;
        }
      CLOSE (CLIENT_CREATE_SCHEDULE, COMMENT);
      CLOSE (CLIENT_CREATE_SCHEDULE, COPY);
      CLOSE (CLIENT_CREATE_SCHEDULE, DURATION);
      CLOSE (CLIENT_CREATE_SCHEDULE, FIRST_TIME);
      CLOSE (CLIENT_CREATE_SCHEDULE, NAME);
      CLOSE (CLIENT_CREATE_SCHEDULE, PERIOD);

      CLOSE (CLIENT_CREATE_SCHEDULE_FIRST_TIME, DAY_OF_MONTH);
      CLOSE (CLIENT_CREATE_SCHEDULE_FIRST_TIME, HOUR);
      CLOSE (CLIENT_CREATE_SCHEDULE_FIRST_TIME, MINUTE);
      CLOSE (CLIENT_CREATE_SCHEDULE_FIRST_TIME, MONTH);
      CLOSE (CLIENT_CREATE_SCHEDULE_FIRST_TIME, YEAR);

      CLOSE (CLIENT_CREATE_SCHEDULE_DURATION, UNIT);

      CLOSE (CLIENT_CREATE_SCHEDULE_PERIOD, UNIT);

      case CLIENT_CREATE_SLAVE:
        {
          slave_t new_slave;

          assert (strcasecmp ("CREATE_SLAVE", element_name) == 0);

          if (openvas_is_user_observer (current_credentials.username))
            {
              SEND_TO_CLIENT_OR_FAIL
               (XML_ERROR_SYNTAX ("create_slave",
                                  "CREATE is forbidden for observer users"));
            }
          else if (create_slave_data->copy)
            switch (copy_slave (create_slave_data->name,
                                create_slave_data->comment,
                                create_slave_data->copy,
                                &new_slave))
              {
                case 0:
                  {
                    char *uuid;
                    uuid = slave_uuid (new_slave);
                    SENDF_TO_CLIENT_OR_FAIL (XML_OK_CREATED_ID ("create_slave"),
                                             uuid);
                    g_log ("event slave", G_LOG_LEVEL_MESSAGE,
                           "Slave %s has been created", uuid);
                    free (uuid);
                    break;
                  }
                case 1:
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_ERROR_SYNTAX ("create_slave",
                                      "Slave exists already"));
                  g_log ("event slave", G_LOG_LEVEL_MESSAGE,
                         "Slave could not be created");
                  break;
                case 2:
                  if (send_find_error_to_client ("create_slave",
                                                 "slave",
                                                 create_slave_data->copy,
                                                 write_to_client,
                                                 write_to_client_data))
                    {
                      error_send_to_client (error);
                      return;
                    }
                  g_log ("event slave", G_LOG_LEVEL_MESSAGE,
                         "Slave could not be created");
                  break;
                case -1:
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_INTERNAL_ERROR ("create_slave"));
                  g_log ("event slave", G_LOG_LEVEL_MESSAGE,
                         "Slave could not be created");
                  break;
              }
          else if (create_slave_data->host == NULL)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("create_slave",
                                "CREATE_SLAVE requires a HOST"));
          else if (strlen (create_slave_data->host) == 0)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("create_slave",
                                "CREATE_SLAVE HOST must be at"
                                " least one character long"));
          else if (create_slave_data->login == NULL)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("create_slave",
                                "CREATE_SLAVE requires a LOGIN"));
          else if (strlen (create_slave_data->login) == 0)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("create_slave",
                                "CREATE_SLAVE LOGIN must be at"
                                " least one character long"));
          else if (create_slave_data->name == NULL)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("create_slave",
                                "CREATE_SLAVE requires a NAME"));
          else if (strlen (create_slave_data->name) == 0)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("create_slave",
                                "CREATE_SLAVE NAME must be at"
                                " least one character long"));
          else if (create_slave_data->port == NULL)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("create_slave",
                                "CREATE_SLAVE requires a PORT"));
          else if (strlen (create_slave_data->port) == 0)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("create_slave",
                                "CREATE_SLAVE PORT must be at"
                                " least one character long"));
          /* Create slave from host string. */
          else switch (create_slave
                        (create_slave_data->name,
                         create_slave_data->comment,
                         create_slave_data->host,
                         create_slave_data->port,
                         create_slave_data->login,
                         create_slave_data->password,
                         &new_slave))
            {
              case 0:
                {
                  char *uuid = slave_uuid (new_slave);
                  SENDF_TO_CLIENT_OR_FAIL (XML_OK_CREATED_ID ("create_slave"),
                                           uuid);
                  g_log ("event slave", G_LOG_LEVEL_MESSAGE,
                         "Slave %s has been created", uuid);
                  free (uuid);
                  break;
                }
              case 1:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("create_slave",
                                    "Slave exists already"));
                g_log ("event slave", G_LOG_LEVEL_MESSAGE,
                       "Slave could not be created");
                break;
              default:
                assert (0);
              case -1:
                SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("create_slave"));
                g_log ("event slave", G_LOG_LEVEL_MESSAGE,
                       "Slave could not be created");
                break;
            }

          create_slave_data_reset (create_slave_data);
          set_client_state (CLIENT_AUTHENTIC);
          break;
        }
      CLOSE (CLIENT_CREATE_SLAVE, COMMENT);
      CLOSE (CLIENT_CREATE_SLAVE, COPY);
      CLOSE (CLIENT_CREATE_SLAVE, HOST);
      CLOSE (CLIENT_CREATE_SLAVE, LOGIN);
      CLOSE (CLIENT_CREATE_SLAVE, NAME);
      CLOSE (CLIENT_CREATE_SLAVE, PASSWORD);
      CLOSE (CLIENT_CREATE_SLAVE, PORT);

      case CLIENT_CREATE_TARGET:
        {
          lsc_credential_t ssh_lsc_credential = 0, smb_lsc_credential = 0;
          target_t new_target;

          assert (strcasecmp ("CREATE_TARGET", element_name) == 0);
          assert (create_target_data->target_locator
                  || create_target_data->hosts != NULL);

          if (openvas_is_user_observer (current_credentials.username))
            {
              SEND_TO_CLIENT_OR_FAIL
               (XML_ERROR_SYNTAX ("create_target",
                                  "CREATE is forbidden for observer users"));
            }
          else if (create_target_data->copy)
            switch (copy_target (create_target_data->name,
                                 create_target_data->comment,
                                 create_target_data->copy,
                                 &new_target))
              {
                case 0:
                  {
                    char *uuid;
                    uuid = target_uuid (new_target);
                    SENDF_TO_CLIENT_OR_FAIL (XML_OK_CREATED_ID ("create_target"),
                                             uuid);
                    g_log ("event target", G_LOG_LEVEL_MESSAGE,
                           "Target %s has been created", uuid);
                    free (uuid);
                    break;
                  }
                case 1:
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_ERROR_SYNTAX ("create_target",
                                      "Target exists already"));
                  g_log ("event target", G_LOG_LEVEL_MESSAGE,
                         "Target could not be created");
                  break;
                case 2:
                  if (send_find_error_to_client ("create_target",
                                                 "target",
                                                 create_target_data->copy,
                                                 write_to_client,
                                                 write_to_client_data))
                    {
                      error_send_to_client (error);
                      return;
                    }
                  g_log ("event target", G_LOG_LEVEL_MESSAGE,
                         "Target could not be created");
                  break;
                case -1:
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_INTERNAL_ERROR ("create_target"));
                  g_log ("event target", G_LOG_LEVEL_MESSAGE,
                         "Target could not be created");
                  break;
              }
          else if (create_target_data->name == NULL)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("create_target",
                                "CREATE_TARGET requires a NAME"));
          else if (strlen (create_target_data->name) == 0)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("create_target",
                                "CREATE_TARGET name must be at"
                                " least one character long"));
          else if (strlen (create_target_data->hosts) == 0
                   && create_target_data->target_locator == NULL)
            /** @todo Legitimate to pass an empty hosts element? */
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("create_target",
                                "CREATE_TARGET hosts must both be at least one"
                                " character long, or TARGET_LOCATOR must"
                                " be set"));
          else if (strlen (create_target_data->hosts) != 0
                   && create_target_data->target_locator != NULL)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("create_target",
                                " CREATE_TARGET requires either a"
                                " TARGET_LOCATOR or a host"));
          else if (create_target_data->ssh_lsc_credential_id
                   && find_lsc_credential
                       (create_target_data->ssh_lsc_credential_id,
                        &ssh_lsc_credential))
            SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("create_target"));
          else if (create_target_data->ssh_lsc_credential_id
                   && ssh_lsc_credential == 0)
            {
              if (send_find_error_to_client
                   ("create_target",
                    "LSC credential",
                    create_target_data->ssh_lsc_credential_id,
                    write_to_client,
                    write_to_client_data))
                {
                  error_send_to_client (error);
                  return;
                }
            }
          else if (create_target_data->smb_lsc_credential_id
                   && find_lsc_credential
                       (create_target_data->smb_lsc_credential_id,
                        &smb_lsc_credential))
            SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("create_target"));
          else if (create_target_data->smb_lsc_credential_id
                   && smb_lsc_credential == 0)
            {
              if (send_find_error_to_client
                   ("create_target",
                    "LSC credential",
                    create_target_data->smb_lsc_credential_id,
                    write_to_client,
                    write_to_client_data))
                {
                  error_send_to_client (error);
                  return;
                }
            }
          /* Create target from host string. */
          else switch (create_target
                        (create_target_data->name,
                         create_target_data->hosts,
                         create_target_data->comment,
                         create_target_data->port_list_id,
                         create_target_data->port_range,
                         ssh_lsc_credential,
                         create_target_data->ssh_port,
                         smb_lsc_credential,
                         create_target_data->target_locator,
                         create_target_data->target_locator_username,
                         create_target_data->target_locator_password,
                         (create_target_data->make_name_unique
                          && strcmp (create_target_data->make_name_unique, "0"))
                           ? 1 : 0,
                         &new_target))
            {
              case 1:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("create_target",
                                    "Target exists already"));
                g_log ("event target", G_LOG_LEVEL_MESSAGE,
                       "Target could not be created");
                break;
              case 2:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("create_target",
                                    "Error in host specification"));
                g_log ("event target", G_LOG_LEVEL_MESSAGE,
                       "Target could not be created");
                break;
              case 3:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("create_target",
                                    "Host specification exceeds"
                                    " " G_STRINGIFY (MANAGE_MAX_HOSTS)
                                    " hosts"));
                g_log ("event target", G_LOG_LEVEL_MESSAGE,
                       "Target could not be created");
                break;
              case 4:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("create_target",
                                    "Error in port range"));
                g_log ("event target", G_LOG_LEVEL_MESSAGE,
                       "Target could not be created");
                break;
              case 5:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("create_target",
                                    "Error in SSH port"));
                g_log ("event target", G_LOG_LEVEL_MESSAGE,
                       "Target could not be created");
                break;
              case 6:
                g_log ("event target", G_LOG_LEVEL_MESSAGE,
                       "Target could not be created");
                if (send_find_error_to_client
                     ("create_target",
                      "port_list",
                      create_target_data->port_list_id,
                      write_to_client,
                      write_to_client_data))
                  {
                    error_send_to_client (error);
                    return;
                  }
                break;
              case -1:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("create_target",
                                    "Import from target_locator failed"));
                g_log ("event target", G_LOG_LEVEL_MESSAGE,
                       "Target could not be created");
                break;
              default:
                {
                  char *uuid = target_uuid (new_target);
                  SENDF_TO_CLIENT_OR_FAIL (XML_OK_CREATED_ID ("create_target"),
                                           uuid);
                  g_log ("event target", G_LOG_LEVEL_MESSAGE,
                         "Target %s has been created", uuid);
                  free (uuid);
                  break;
                }
            }

          create_target_data_reset (create_target_data);
          set_client_state (CLIENT_AUTHENTIC);
          break;
        }
      CLOSE (CLIENT_CREATE_TARGET, COMMENT);
      CLOSE (CLIENT_CREATE_TARGET, COPY);
      CLOSE (CLIENT_CREATE_TARGET, HOSTS);
      CLOSE (CLIENT_CREATE_TARGET, NAME);
      CLOSE (CLIENT_CREATE_TARGET, PORT_LIST);
      CLOSE (CLIENT_CREATE_TARGET, PORT_RANGE);
      CLOSE (CLIENT_CREATE_TARGET, SSH_LSC_CREDENTIAL);
      CLOSE (CLIENT_CREATE_TARGET, SMB_LSC_CREDENTIAL);
      CLOSE (CLIENT_CREATE_TARGET_TARGET_LOCATOR, PASSWORD);
      CLOSE (CLIENT_CREATE_TARGET, TARGET_LOCATOR);
      CLOSE (CLIENT_CREATE_TARGET_TARGET_LOCATOR, USERNAME);

      CLOSE (CLIENT_CREATE_TARGET_NAME, MAKE_UNIQUE);

      CLOSE (CLIENT_CREATE_TARGET_SSH_LSC_CREDENTIAL, PORT);

      case CLIENT_CREATE_TASK:
        {
          config_t config = 0;
          target_t target = 0;
          slave_t slave = 0;
          char *tsk_uuid, *name, *description;
          guint index;
          int fail;

          /* @todo Buffer the entire task creation and pass everything to a
           *       libmanage function, so that libmanage can do the locking
           *       properly instead of exposing the task_t.  Probably easier
           *       after removing the option to create a task from an RC
           *       file. */

          assert (strcasecmp ("CREATE_TASK", element_name) == 0);
          assert (create_task_data->task != (task_t) 0);

          /* The task already exists in the database at this point,
           * including the RC file (in the description column), so on
           * failure be sure to call request_delete_task to remove the
           * task. */
          /** @todo Any fail cases of the CLIENT_CREATE_TASK_* states must do
           *        so too. */

          if (openvas_is_user_observer (current_credentials.username))
            {
              request_delete_task (&create_task_data->task);
              SEND_TO_CLIENT_OR_FAIL
               (XML_ERROR_SYNTAX ("create_task",
                                  "CREATE is forbidden for observer users"));
              create_task_data_reset (create_task_data);
              set_client_state (CLIENT_AUTHENTIC);
              break;
            }

          if (create_task_data->copy)
            {
              int ret;
              gchar *name, *comment;
              task_t new_task;

              name = task_name (create_task_data->task);
              comment = task_comment (create_task_data->task);
              ret = copy_task (name,
                               comment,
                               create_task_data->copy,
                               &new_task);
              g_free (name);
              g_free (comment);
              request_delete_task (&create_task_data->task);
              switch (ret)
                {
                  case 0:
                    {
                      char *uuid;
                      task_uuid (new_task, &uuid);
                      SENDF_TO_CLIENT_OR_FAIL (XML_OK_CREATED_ID ("create_task"),
                                               uuid);
                      g_log ("event task", G_LOG_LEVEL_MESSAGE,
                             "Task %s has been created", uuid);
                      free (uuid);
                      break;
                    }
                  case 1:
                    SEND_TO_CLIENT_OR_FAIL
                     (XML_ERROR_SYNTAX ("create_task",
                                        "Task exists already"));
                    g_log ("event task", G_LOG_LEVEL_MESSAGE,
                           "Task could not be created");
                    break;
                  case 2:
                    if (send_find_error_to_client ("create_task",
                                                   "task",
                                                   create_task_data->copy,
                                                   write_to_client,
                                                   write_to_client_data))
                      {
                        error_send_to_client (error);
                        return;
                      }
                    g_log ("event task", G_LOG_LEVEL_MESSAGE,
                           "Task could not be created");
                    break;
                  case -1:
                    SEND_TO_CLIENT_OR_FAIL
                     (XML_INTERNAL_ERROR ("create_task"));
                    g_log ("event task", G_LOG_LEVEL_MESSAGE,
                           "Task could not be created");
                    break;
                }
              create_task_data_reset (create_task_data);
              set_client_state (CLIENT_AUTHENTIC);
              break;
            }

          /* Get the task ID. */

          if (task_uuid (create_task_data->task, &tsk_uuid))
            {
              request_delete_task (&create_task_data->task);
              SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("create_task"));
              create_task_data_reset (create_task_data);
              set_client_state (CLIENT_AUTHENTIC);
              break;
            }

          /* Check for the right combination of rcfile, target and config. */

          description = task_description (create_task_data->task);
          if ((description
               && (create_task_data->config_id || create_task_data->target_id))
              || (description == NULL
                  && (create_task_data->config_id == NULL
                      || create_task_data->target_id == NULL)))
            {
              request_delete_task (&create_task_data->task);
              free (tsk_uuid);
              free (description);
              SEND_TO_CLIENT_OR_FAIL
               (XML_ERROR_SYNTAX ("create_task",
                                  "CREATE_TASK requires either an rcfile"
                                  " or both a config and a target"));
              create_task_data_reset (create_task_data);
              set_client_state (CLIENT_AUTHENTIC);
              break;
            }

          assert (description
                  || (create_task_data->config_id
                      && create_task_data->target_id));

          /* Set any alert. */

          assert (create_task_data->alerts);
          index = create_task_data->alerts->len;
          fail = 0;
          while (index--)
            {
              alert_t alert;
              gchar *alert_id;

              alert_id = (gchar*) g_ptr_array_index (create_task_data->alerts,
                                                     index);
              if (find_alert (alert_id, &alert))
                {
                  request_delete_task (&create_task_data->task);
                  free (tsk_uuid);
                  free (description);
                  SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("create_task"));
                  create_task_data_reset (create_task_data);
                  set_client_state (CLIENT_AUTHENTIC);
                  fail = 1;
                  break;
                }
              if (alert == 0)
                {
                  request_delete_task (&create_task_data->task);
                  free (tsk_uuid);
                  free (description);
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_ERROR_SYNTAX ("create_task",
                                      "CREATE_TASK alert must exist"));
                  create_task_data_reset (create_task_data);
                  set_client_state (CLIENT_AUTHENTIC);
                  fail = 1;
                  break;
                }
              add_task_alert (create_task_data->task, alert);
            }
          if (fail)
            break;

          /* Set any schedule. */

          if (create_task_data->schedule_id)
            {
              schedule_t schedule;
              if (find_schedule (create_task_data->schedule_id, &schedule))
                {
                  request_delete_task (&create_task_data->task);
                  free (tsk_uuid);
                  free (description);
                  SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("create_task"));
                  create_task_data_reset (create_task_data);
                  set_client_state (CLIENT_AUTHENTIC);
                  break;
                }
              if (schedule == 0)
                {
                  request_delete_task (&create_task_data->task);
                  free (tsk_uuid);
                  free (description);
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_ERROR_SYNTAX ("create_task",
                                      "CREATE_TASK schedule must exist"));
                  create_task_data_reset (create_task_data);
                  set_client_state (CLIENT_AUTHENTIC);
                  break;
                }
              /** @todo
               *
               * This is a contention hole.  Some other process could remove
               * the schedule at this point.  The variable "schedule" would
               * still refer to the removed schedule.
               *
               * This happens all over the place.  Anywhere that a libmanage
               * client gets a reference to a resource, in fact.
               *
               * Possibly libmanage should lock the db whenever it hands out a
               * reference, and the client should call something to release
               * the lock when it's done.
               *
               * In many cases, like this one, the client could pass the UUID
               * directly to libmanage, instead of getting the reference.  In
               * this case the client would then need something like
               * set_task_schedule_uuid.
               */
              set_task_schedule (create_task_data->task, schedule);
            }

          /* Set any observers. */

          if (create_task_data->observers)
            {
              int fail;
              fail = set_task_observers (create_task_data->task,
                                         create_task_data->observers);
              switch (fail)
                {
                  case 0:
                    break;
                  case 1:
                  case 2:
                    SEND_TO_CLIENT_OR_FAIL
                      (XML_ERROR_SYNTAX ("create_task",
                                         "User name error in observers"));
                    break;
                  case -1:
                  default:
                    SEND_TO_CLIENT_OR_FAIL
                      (XML_INTERNAL_ERROR ("create_task"));
                }
              if (fail)
                {
                  request_delete_task (&create_task_data->task);
                  free (tsk_uuid);
                  free (description);
                  create_task_data_reset (create_task_data);
                  set_client_state (CLIENT_AUTHENTIC);
                  break;
                }
            }

          /* Check for name. */

          name = task_name (create_task_data->task);
          if (name == NULL)
            {
              request_delete_task (&create_task_data->task);
              free (tsk_uuid);
              free (description);
              SEND_TO_CLIENT_OR_FAIL
               (XML_ERROR_SYNTAX ("create_task",
                                  "CREATE_TASK requires a name attribute"));
              create_task_data_reset (create_task_data);
              set_client_state (CLIENT_AUTHENTIC);
              break;
            }

          /* If there's an rc file, setup the target and config, otherwise
           * check that the target and config exist. */

          if (description)
            {
              int ret;
              char *hosts;
              gchar *target_name, *config_name;

              /* Create the config. */

              config_name = g_strdup_printf ("Imported config for task %s",
                                             tsk_uuid);
              ret = create_config_rc (config_name, NULL, (char*) description,
                                      &config);
              set_task_config (create_task_data->task, config);
              g_free (config_name);
              if (ret)
                {
                  request_delete_task (&create_task_data->task);
                  free (description);
                  SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("create_task"));
                  create_task_data_reset (create_task_data);
                  set_client_state (CLIENT_AUTHENTIC);
                  break;
                }

              /* Create the target. */

              hosts = rc_preference (description, "targets");
              if (hosts == NULL)
                {
                  request_delete_task (&create_task_data->task);
                  free (description);
                  free (tsk_uuid);
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_ERROR_SYNTAX
                     ("create_task",
                      "CREATE_TASK rcfile must have targets"));
                  create_task_data_reset (create_task_data);
                  set_client_state (CLIENT_AUTHENTIC);
                  break;
                }
              free (description);

              target_name = g_strdup_printf ("Imported target for task %s",
                                             tsk_uuid);
              if (create_target (target_name, hosts, NULL, NULL, NULL, 0, NULL,
                                 0, NULL, NULL, NULL, 0, &target))
                {
                  request_delete_task (&create_task_data->task);
                  g_free (target_name);
                  free (tsk_uuid);
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_INTERNAL_ERROR ("create_task"));
                  create_task_data_reset (create_task_data);
                  set_client_state (CLIENT_AUTHENTIC);
                  break;
                }
              set_task_target (create_task_data->task, target);
              g_free (target_name);
            }
          else if (find_config (create_task_data->config_id, &config))
            {
              request_delete_task (&create_task_data->task);
              free (tsk_uuid);
              SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("create_task"));
              create_task_data_reset (create_task_data);
              set_client_state (CLIENT_AUTHENTIC);
              break;
            }
          else if (config == 0)
            {
              request_delete_task (&create_task_data->task);
              free (tsk_uuid);
              if (send_find_error_to_client ("create_task",
                                             "config",
                                             create_task_data->config_id,
                                             write_to_client,
                                             write_to_client_data))
                {
                  error_send_to_client (error);
                  return;
                }
              create_task_data_reset (create_task_data);
              set_client_state (CLIENT_AUTHENTIC);
              break;
            }
          else if (find_target (create_task_data->target_id, &target))
            {
              request_delete_task (&create_task_data->task);
              free (tsk_uuid);
              SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("create_task"));
              create_task_data_reset (create_task_data);
              set_client_state (CLIENT_AUTHENTIC);
              break;
            }
          else if (target == 0)
            {
              request_delete_task (&create_task_data->task);
              free (tsk_uuid);
              if (send_find_error_to_client ("create_task",
                                             "target",
                                             create_task_data->target_id,
                                             write_to_client,
                                             write_to_client_data))
                {
                  /* Out of space. */
                  error_send_to_client (error);
                  return;
                }
              create_task_data_reset (create_task_data);
              set_client_state (CLIENT_AUTHENTIC);
              break;
            }
          else if (create_task_data->slave_id
                   && find_slave (create_task_data->slave_id, &slave))
            {
              request_delete_task (&create_task_data->task);
              free (tsk_uuid);
              SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("create_task"));
              create_task_data_reset (create_task_data);
              set_client_state (CLIENT_AUTHENTIC);
              break;
            }
          else if (create_task_data->slave_id && slave == 0)
            {
              request_delete_task (&create_task_data->task);
              free (tsk_uuid);
              if (send_find_error_to_client ("create_task",
                                             "target",
                                             create_task_data->slave_id,
                                             write_to_client,
                                             write_to_client_data))
                {
                  /* Out of space. */
                  error_send_to_client (error);
                  return;
                }
              create_task_data_reset (create_task_data);
              set_client_state (CLIENT_AUTHENTIC);
              break;
            }
          else
            {
              set_task_config (create_task_data->task, config);
              set_task_slave (create_task_data->task, slave);
              set_task_target (create_task_data->task, target);
              if (create_task_data->preferences)
                set_task_preferences (create_task_data->task,
                                      create_task_data->preferences);

              /* Generate the rcfile in the task. */

              if (make_task_rcfile (create_task_data->task))
                {
                  request_delete_task (&create_task_data->task);
                  free (tsk_uuid);
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_ERROR_SYNTAX ("create_task",
                                      "Failed to generate task rcfile"));
                  create_task_data_reset (create_task_data);
                  set_client_state (CLIENT_AUTHENTIC);
                  break;
                }
            }

          /* Send success response. */

          SENDF_TO_CLIENT_OR_FAIL (XML_OK_CREATED_ID ("create_task"),
                                   tsk_uuid);
          make_task_complete (tsk_uuid);
          g_log ("event task", G_LOG_LEVEL_MESSAGE,
                 "Task %s has been created", tsk_uuid);
          free (tsk_uuid);
          create_task_data_reset (create_task_data);
          set_client_state (CLIENT_AUTHENTIC);
          break;
        }
      CLOSE (CLIENT_CREATE_TASK, COMMENT);
      CLOSE (CLIENT_CREATE_TASK, CONFIG);
      CLOSE (CLIENT_CREATE_TASK, COPY);
      CLOSE (CLIENT_CREATE_TASK, ALERT);
      CLOSE (CLIENT_CREATE_TASK, NAME);
      CLOSE (CLIENT_CREATE_TASK, OBSERVERS);
      CLOSE (CLIENT_CREATE_TASK, PREFERENCES);
      case CLIENT_CREATE_TASK_RCFILE:
        assert (strcasecmp ("RCFILE", element_name) == 0);
        if (create_task_data->task)
          {
            gsize out_len;
            guchar* out;
            char* description = task_description (create_task_data->task);
            if (description)
              {
                out = g_base64_decode (description, &out_len);
                /* g_base64_decode can return NULL (Glib 2.12.4-2), at least
                 * when description is zero length. */
                if (out == NULL)
                  {
                    out = (guchar*) g_strdup ("");
                    out_len = 0;
                  }
              }
            else
              {
                out = (guchar*) g_strdup ("");
                out_len = 0;
              }
            free (description);
            set_task_description (create_task_data->task, (char*) out, out_len);
            set_client_state (CLIENT_CREATE_TASK);
          }
        break;
      CLOSE (CLIENT_CREATE_TASK, TARGET);
      CLOSE (CLIENT_CREATE_TASK, SCHEDULE);
      CLOSE (CLIENT_CREATE_TASK, SLAVE);

      case CLIENT_CREATE_TASK_PREFERENCES_PREFERENCE:
        assert (strcasecmp ("PREFERENCE", element_name) == 0);
        array_add (create_task_data->preferences,
                   create_task_data->preference);
        create_task_data->preference = NULL;
        set_client_state (CLIENT_CREATE_TASK_PREFERENCES);
        break;
      case CLIENT_CREATE_TASK_PREFERENCES_PREFERENCE_NAME:
        assert (strcasecmp ("SCANNER_NAME", element_name) == 0);
        set_client_state (CLIENT_CREATE_TASK_PREFERENCES_PREFERENCE);
        break;
      CLOSE (CLIENT_CREATE_TASK_PREFERENCES_PREFERENCE, VALUE);

      case CLIENT_EMPTY_TRASHCAN:
        switch (manage_empty_trashcan ())
          {
            case 0:
              SEND_TO_CLIENT_OR_FAIL (XML_OK ("empty_trashcan"));
              g_log ("event task", G_LOG_LEVEL_MESSAGE,
                     "Trashcan has been emptied");
              break;
            default:  /* Programming error. */
              assert (0);
            case -1:
              SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("empty_trashcan"));
              break;
          }
        set_client_state (CLIENT_AUTHENTIC);
        break;

      case CLIENT_MODIFY_AGENT:
        {
          assert (strcasecmp ("MODIFY_AGENT", element_name) == 0);

          if (openvas_is_user_observer (current_credentials.username))
            {
              SEND_TO_CLIENT_OR_FAIL
               (XML_ERROR_SYNTAX ("modify_agent",
                                  "MODIFY is forbidden for observer users"));
            }
          else switch (modify_agent
                        (modify_agent_data->agent_id,
                         modify_agent_data->name,
                         modify_agent_data->comment))
            {
              case 0:
                SENDF_TO_CLIENT_OR_FAIL (XML_OK ("modify_agent"));
                g_log ("event agent", G_LOG_LEVEL_MESSAGE,
                       "Agent %s has been modified",
                       modify_agent_data->agent_id);
                break;
              case 1:
                if (send_find_error_to_client ("modify_agent",
                                               "agent",
                                               modify_agent_data->agent_id,
                                               write_to_client,
                                               write_to_client_data))
                  {
                    error_send_to_client (error);
                    return;
                  }
                g_log ("event agent", G_LOG_LEVEL_MESSAGE,
                       "Agent could not be modified");
                break;
              case 2:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("modify_agent",
                                    "agent with new name exists already"));
                g_log ("event agent", G_LOG_LEVEL_MESSAGE,
                       "agent could not be modified");
                break;
              case 3:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("modify_agent",
                                    "MODIFY_agent requires a agent_id"));
                g_log ("event agent", G_LOG_LEVEL_MESSAGE,
                       "agent could not be modified");
                break;
              default:
              case -1:
                SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("modify_agent"));
                g_log ("event agent", G_LOG_LEVEL_MESSAGE,
                       "agent could not be modified");
                break;
            }

          modify_agent_data_reset (modify_agent_data);
          set_client_state (CLIENT_AUTHENTIC);
          break;
        }
      CLOSE (CLIENT_MODIFY_AGENT, COMMENT);
      CLOSE (CLIENT_MODIFY_AGENT, NAME);

      case CLIENT_MODIFY_ALERT:
        {
          event_t event;
          alert_condition_t condition;
          alert_method_t method;

          assert (strcasecmp ("MODIFY_ALERT", element_name) == 0);

          event = EVENT_ERROR;
          condition = ALERT_CONDITION_ERROR;
          method  = ALERT_METHOD_ERROR;

          array_terminate (modify_alert_data->event_data);
          array_terminate (modify_alert_data->condition_data);
          array_terminate (modify_alert_data->method_data);

          if (openvas_is_user_observer (current_credentials.username))
            {
              SEND_TO_CLIENT_OR_FAIL
               (XML_ERROR_SYNTAX ("modify_alert",
                                  "MODIFY is forbidden for observer users"));
            }
          else if (strlen (modify_alert_data->event) &&
                   (event = event_from_name (modify_alert_data->event))
                   == 0)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("modify_alert",
                                "Failed to recognise event name"));
          else if (strlen (modify_alert_data->condition) &&
                   (condition = alert_condition_from_name
                                 (modify_alert_data->condition))
                   == 0)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("modify_alert",
                                "Failed to recognise condition name"));
          else if (strlen (modify_alert_data->method) &&
                   (method = alert_method_from_name
                                 (modify_alert_data->method))
                   == 0)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("modify_alert",
                                "Failed to recognise method name"));
          else switch (modify_alert
                        (modify_alert_data->alert_id,
                         modify_alert_data->name,
                         modify_alert_data->comment,
                         modify_alert_data->filter_id,
                         event,
                         modify_alert_data->event_data,
                         condition,
                         modify_alert_data->condition_data,
                         method,
                         modify_alert_data->method_data))
            {
              case 0:
                SENDF_TO_CLIENT_OR_FAIL (XML_OK ("modify_alert"));
                g_log ("event alert", G_LOG_LEVEL_MESSAGE,
                       "Alert %s has been modified",
                       modify_alert_data->alert_id);
                break;
              case 1:
                if (send_find_error_to_client ("modify_alert",
                                               "alert",
                                               modify_alert_data->alert_id,
                                               write_to_client,
                                               write_to_client_data))
                  {
                    error_send_to_client (error);
                    return;
                  }
                g_log ("event alert", G_LOG_LEVEL_MESSAGE,
                       "Alert could not be modified");
                break;
              case 2:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("modify_alert",
                                    "alert with new name exists already"));
                g_log ("event alert", G_LOG_LEVEL_MESSAGE,
                       "Alert could not be modified");
                break;
              case 3:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("modify_alert",
                                    "MODIFY_alert requires an alert_id"));
                g_log ("event alert", G_LOG_LEVEL_MESSAGE,
                       "Alert could not be modified");
                break;
              case 4:
                if (send_find_error_to_client ("modify_alert",
                                               "filter",
                                               modify_alert_data->filter_id,
                                               write_to_client,
                                               write_to_client_data))
                  {
                    error_send_to_client (error);
                    return;
                  }
                g_log ("event alert", G_LOG_LEVEL_MESSAGE,
                       "Alert could not be created");
                break;
              case 5:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("modify_alert",
                                    "Filter type must be report if"
                                    " specified"));
                g_log ("event alert", G_LOG_LEVEL_MESSAGE,
                       "Alert could not be created");
                break;
              case 6:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("modify_alert",
                                    "Validation of email address failed"));
                g_log ("event alert", G_LOG_LEVEL_MESSAGE,
                       "Alert could not be created");
                break;
              default:
              case -1:
                SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("modify_alert"));
                g_log ("event alert", G_LOG_LEVEL_MESSAGE,
                       "Alert could not be modified");
                break;
            }

          modify_alert_data_reset (modify_alert_data);
          set_client_state (CLIENT_AUTHENTIC);
          break;
        }
      CLOSE (CLIENT_MODIFY_ALERT, COMMENT);
      CLOSE (CLIENT_MODIFY_ALERT, NAME);
      CLOSE (CLIENT_MODIFY_ALERT, FILTER);
      CLOSE (CLIENT_MODIFY_ALERT, EVENT);
      CLOSE (CLIENT_MODIFY_ALERT, CONDITION);
      CLOSE (CLIENT_MODIFY_ALERT, METHOD);

      case CLIENT_MODIFY_ALERT_EVENT_DATA:
        {
          gchar *string;

          assert (strcasecmp ("DATA", element_name) == 0);
          assert (modify_alert_data->event_data);
          assert (modify_alert_data->part_data);
          assert (modify_alert_data->part_name);

          string = g_strconcat (modify_alert_data->part_name,
                                "0",
                                modify_alert_data->part_data,
                                NULL);
          string[strlen (modify_alert_data->part_name)] = '\0';
          array_add (modify_alert_data->event_data, string);

          openvas_free_string_var (&modify_alert_data->part_data);
          openvas_free_string_var (&modify_alert_data->part_name);
          openvas_append_string (&modify_alert_data->part_data, "");
          openvas_append_string (&modify_alert_data->part_name, "");
          set_client_state (CLIENT_MODIFY_ALERT_EVENT);
          break;
        }
      CLOSE (CLIENT_MODIFY_ALERT_EVENT_DATA, NAME);

      case CLIENT_MODIFY_ALERT_CONDITION_DATA:
        {
          gchar *string;

          assert (strcasecmp ("DATA", element_name) == 0);
          assert (modify_alert_data->condition_data);
          assert (modify_alert_data->part_data);
          assert (modify_alert_data->part_name);

          string = g_strconcat (modify_alert_data->part_name,
                                "0",
                                modify_alert_data->part_data,
                                NULL);
          string[strlen (modify_alert_data->part_name)] = '\0';
          array_add (modify_alert_data->condition_data, string);

          openvas_free_string_var (&modify_alert_data->part_data);
          openvas_free_string_var (&modify_alert_data->part_name);
          openvas_append_string (&modify_alert_data->part_data, "");
          openvas_append_string (&modify_alert_data->part_name, "");
          set_client_state (CLIENT_MODIFY_ALERT_CONDITION);
          break;
        }
      CLOSE (CLIENT_MODIFY_ALERT_CONDITION_DATA, NAME);

      case CLIENT_MODIFY_ALERT_METHOD_DATA:
        {
          gchar *string;

          assert (strcasecmp ("DATA", element_name) == 0);
          assert (modify_alert_data->method_data);
          assert (modify_alert_data->part_data);
          assert (modify_alert_data->part_name);

          string = g_strconcat (modify_alert_data->part_name,
                                "0",
                                modify_alert_data->part_data,
                                NULL);
          string[strlen (modify_alert_data->part_name)] = '\0';
          array_add (modify_alert_data->method_data, string);

          openvas_free_string_var (&modify_alert_data->part_data);
          openvas_free_string_var (&modify_alert_data->part_name);
          openvas_append_string (&modify_alert_data->part_data, "");
          openvas_append_string (&modify_alert_data->part_name, "");
          set_client_state (CLIENT_MODIFY_ALERT_METHOD);
          break;
        }
      CLOSE (CLIENT_MODIFY_ALERT_METHOD_DATA, NAME);

      case CLIENT_MODIFY_FILTER:
        {
          assert (strcasecmp ("MODIFY_FILTER", element_name) == 0);

          if (openvas_is_user_observer (current_credentials.username))
            {
              SEND_TO_CLIENT_OR_FAIL
               (XML_ERROR_SYNTAX ("modify_filter",
                                  "MODIFY is forbidden for observer users"));
            }
          else switch (modify_filter
                        (modify_filter_data->filter_id,
                         modify_filter_data->name,
                         modify_filter_data->comment,
                         modify_filter_data->term,
                         modify_filter_data->type))
            {
              case 0:
                SENDF_TO_CLIENT_OR_FAIL (XML_OK ("modify_filter"));
                g_log ("event filter", G_LOG_LEVEL_MESSAGE,
                       "Filter %s has been modified",
                       modify_filter_data->filter_id);
                break;
              case 1:
                if (send_find_error_to_client ("modify_filter",
                                               "filter",
                                               modify_filter_data->filter_id,
                                               write_to_client,
                                               write_to_client_data))
                  {
                    error_send_to_client (error);
                    return;
                  }
                g_log ("event filter", G_LOG_LEVEL_MESSAGE,
                       "Filter could not be modified");
                break;
              case 2:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("modify_filter",
                                    "Filter with new name exists already"));
                g_log ("event filter", G_LOG_LEVEL_MESSAGE,
                       "Filter could not be modified");
                break;
              case 3:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("modify_filter",
                                    "Error in type name"));
                g_log ("event filter", G_LOG_LEVEL_MESSAGE,
                       "Filter could not be modified");
                break;
              case 4:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("modify_filter",
                                    "MODIFY_FILTER requires a filter_id"));
                g_log ("event filter", G_LOG_LEVEL_MESSAGE,
                       "Filter could not be modified");
                break;
              case 5:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("modify_filter",
                                    "Filter is used by an alert so type must be"
                                    " 'report' if specified"));
                g_log ("event filter", G_LOG_LEVEL_MESSAGE,
                       "Filter could not be modified");
                break;
              default:
              case -1:
                SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("modify_filter"));
                g_log ("event filter", G_LOG_LEVEL_MESSAGE,
                       "Filter could not be modified");
                break;
            }

          modify_filter_data_reset (modify_filter_data);
          set_client_state (CLIENT_AUTHENTIC);
          break;
        }
      CLOSE (CLIENT_MODIFY_FILTER, COMMENT);
      CLOSE (CLIENT_MODIFY_FILTER, NAME);
      CLOSE (CLIENT_MODIFY_FILTER, TYPE);
      CLOSE (CLIENT_MODIFY_FILTER, TERM);

      case CLIENT_MODIFY_PORT_LIST:
        {
          assert (strcasecmp ("MODIFY_PORT_LIST", element_name) == 0);

          if (openvas_is_user_observer (current_credentials.username))
            {
              SEND_TO_CLIENT_OR_FAIL
               (XML_ERROR_SYNTAX ("modify_port_list",
                                  "MODIFY is forbidden for observer users"));
            }
          else switch (modify_port_list
                        (modify_port_list_data->port_list_id,
                         modify_port_list_data->name,
                         modify_port_list_data->comment))
            {
              case 0:
                SENDF_TO_CLIENT_OR_FAIL (XML_OK ("modify_port_list"));
                g_log ("event port_list", G_LOG_LEVEL_MESSAGE,
                       "Port List %s has been modified",
                       modify_port_list_data->port_list_id);
                break;
              case 1:
                if (send_find_error_to_client ("modify_port_list",
                                               "port_list",
                                               modify_port_list_data->port_list_id,
                                               write_to_client,
                                               write_to_client_data))
                  {
                    error_send_to_client (error);
                    return;
                  }
                g_log ("event port_list", G_LOG_LEVEL_MESSAGE,
                       "Port List could not be modified");
                break;
              case 2:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("modify_port_list",
                                    "Port List with new name exists already"));
                g_log ("event port_list", G_LOG_LEVEL_MESSAGE,
                       "Port List could not be modified");
                break;
              case 3:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("modify_port_list",
                                    "modify_port_list requires a port_list_id"));
                g_log ("event port_list", G_LOG_LEVEL_MESSAGE,
                       "Port List could not be modified");
                break;
              default:
              case -1:
                SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("modify_port_list"));
                g_log ("event port_list", G_LOG_LEVEL_MESSAGE,
                       "Port List could not be modified");
                break;
            }

          modify_port_list_data_reset (modify_port_list_data);
          set_client_state (CLIENT_AUTHENTIC);
          break;
        }
      CLOSE (CLIENT_MODIFY_PORT_LIST, COMMENT);
      CLOSE (CLIENT_MODIFY_PORT_LIST, NAME);

      case CLIENT_MODIFY_NOTE:
        {
          task_t task = 0;
          result_t result = 0;
          note_t note = 0;

          assert (strcasecmp ("MODIFY_NOTE", element_name) == 0);

          if (modify_note_data->note_id == NULL)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("modify_note",
                                "MODIFY_NOTE requires a note_id attribute"));
          else if (modify_note_data->text == NULL)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("modify_note",
                                "MODIFY_NOTE requires a TEXT entity"));
          else if (find_note (modify_note_data->note_id, &note))
            SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("modify_note"));
          else if (note == 0)
            {
              if (send_find_error_to_client ("modify_note",
                                             "note",
                                             modify_note_data->note_id,
                                             write_to_client,
                                             write_to_client_data))
                {
                  error_send_to_client (error);
                  return;
                }
            }
          else if (modify_note_data->task_id
                   && find_task_for_actions (modify_note_data->task_id,
                                             &task,
                                             "g"))
            SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("modify_note"));
          else if (modify_note_data->task_id && task == 0)
            {
              if (send_find_error_to_client ("modify_note",
                                             "task",
                                             modify_note_data->task_id,
                                             write_to_client,
                                             write_to_client_data))
                {
                  error_send_to_client (error);
                  return;
                }
            }
          else if (modify_note_data->result_id
                   && find_result_for_actions (modify_note_data->result_id,
                                               &result,
                                               "g"))
            SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("modify_note"));
          else if (modify_note_data->result_id && result == 0)
            {
              if (send_find_error_to_client ("modify_note",
                                             "result",
                                             modify_note_data->result_id,
                                             write_to_client,
                                             write_to_client_data))
                {
                  error_send_to_client (error);
                  return;
                }
            }
          else switch (modify_note (note,
                                    modify_note_data->active,
                                    modify_note_data->text,
                                    modify_note_data->hosts,
                                    modify_note_data->port,
                                    modify_note_data->threat,
                                    task,
                                    result))
            {
              case 0:
                SENDF_TO_CLIENT_OR_FAIL (XML_OK ("modify_note"));
                break;
              case -1:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_INTERNAL_ERROR ("modify_note"));
                break;
              case 2:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("modify_note",
                                    "Error in port specification"));
                g_log ("event note", G_LOG_LEVEL_MESSAGE,
                       "Note could not be created");
                break;
              default:
                assert (0);
                SEND_TO_CLIENT_OR_FAIL
                 (XML_INTERNAL_ERROR ("modify_note"));
                break;
            }
          modify_note_data_reset (modify_note_data);
          set_client_state (CLIENT_AUTHENTIC);
          break;
        }
      CLOSE (CLIENT_MODIFY_NOTE, ACTIVE);
      CLOSE (CLIENT_MODIFY_NOTE, HOSTS);
      CLOSE (CLIENT_MODIFY_NOTE, PORT);
      CLOSE (CLIENT_MODIFY_NOTE, RESULT);
      CLOSE (CLIENT_MODIFY_NOTE, TASK);
      CLOSE (CLIENT_MODIFY_NOTE, TEXT);
      CLOSE (CLIENT_MODIFY_NOTE, THREAT);

      case CLIENT_MODIFY_OVERRIDE:
        {
          task_t task = 0;
          result_t result = 0;
          override_t override = 0;

          assert (strcasecmp ("MODIFY_OVERRIDE", element_name) == 0);

          if (modify_override_data->override_id == NULL)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("modify_override",
                                "MODIFY_OVERRIDE requires a override_id attribute"));
          else if (modify_override_data->text == NULL)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("modify_override",
                                "MODIFY_OVERRIDE requires a TEXT entity"));
          else if (find_override (modify_override_data->override_id, &override))
            SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("modify_override"));
          else if (override == 0)
            {
              if (send_find_error_to_client ("modify_override",
                                             "override",
                                             modify_override_data->override_id,
                                             write_to_client,
                                             write_to_client_data))
                {
                  error_send_to_client (error);
                  return;
                }
            }
          else if (modify_override_data->task_id
                   && find_task_for_actions (modify_override_data->task_id,
                                             &task,
                                             "g"))
            SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("modify_override"));
          else if (modify_override_data->task_id && task == 0)
            {
              if (send_find_error_to_client ("modify_override",
                                             "task",
                                             modify_override_data->task_id,
                                             write_to_client,
                                             write_to_client_data))
                {
                  error_send_to_client (error);
                  return;
                }
            }
          else if (modify_override_data->result_id
                   && find_result_for_actions (modify_override_data->result_id,
                                               &result,
                                               "g"))
            SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("modify_override"));
          else if (modify_override_data->result_id && result == 0)
            {
              if (send_find_error_to_client ("modify_override",
                                             "result",
                                             modify_override_data->result_id,
                                             write_to_client,
                                             write_to_client_data))
                {
                  error_send_to_client (error);
                  return;
                }
            }
          else switch (modify_override (override,
                                        modify_override_data->active,
                                        modify_override_data->text,
                                        modify_override_data->hosts,
                                        modify_override_data->port,
                                        modify_override_data->threat,
                                        modify_override_data->new_threat,
                                        task,
                                        result))
            {
              case 0:
                SENDF_TO_CLIENT_OR_FAIL (XML_OK ("modify_override"));
                break;
              case 1:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("modify_override",
                                    "ACTIVE must be an integer >= -2"));
                break;
              case 2:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("modify_override",
                                    "Error in port specification"));
                g_log ("event override", G_LOG_LEVEL_MESSAGE,
                       "Override could not be modified");
                break;
              case -1:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_INTERNAL_ERROR ("modify_override"));
                break;
              default:
                assert (0);
                SEND_TO_CLIENT_OR_FAIL
                 (XML_INTERNAL_ERROR ("modify_override"));
                break;
            }
          modify_override_data_reset (modify_override_data);
          set_client_state (CLIENT_AUTHENTIC);
          break;
        }
      CLOSE (CLIENT_MODIFY_OVERRIDE, ACTIVE);
      CLOSE (CLIENT_MODIFY_OVERRIDE, HOSTS);
      CLOSE (CLIENT_MODIFY_OVERRIDE, NEW_THREAT);
      CLOSE (CLIENT_MODIFY_OVERRIDE, PORT);
      CLOSE (CLIENT_MODIFY_OVERRIDE, RESULT);
      CLOSE (CLIENT_MODIFY_OVERRIDE, TASK);
      CLOSE (CLIENT_MODIFY_OVERRIDE, TEXT);
      CLOSE (CLIENT_MODIFY_OVERRIDE, THREAT);

      case CLIENT_MODIFY_SCHEDULE:
        {
          time_t first_time, period, period_months, duration;

          assert (strcasecmp ("MODIFY_SCHEDULE", element_name) == 0);

          period_months = 0;

          /* Only change schedule "first time" if given. */
          first_time = modify_schedule_data->first_time_hour
                        || modify_schedule_data->first_time_minute
                        || modify_schedule_data->first_time_day_of_month
                        || modify_schedule_data->first_time_month
                        || modify_schedule_data->first_time_year;

          if (openvas_is_user_observer (current_credentials.username))
            {
              SEND_TO_CLIENT_OR_FAIL
               (XML_ERROR_SYNTAX ("modify_schedule",
                                  "MODIFY is forbidden for observer users"));
            }
          else if (first_time
                   && ((first_time
                         = time_from_strings
                            (modify_schedule_data->first_time_hour,
                             modify_schedule_data->first_time_minute,
                             modify_schedule_data->first_time_day_of_month,
                             modify_schedule_data->first_time_month,
                             modify_schedule_data->first_time_year,
                             modify_schedule_data->timezone))
                       == -1))
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("modify_schedule",
                                "Failed to create time from FIRST_TIME"
                                " elements"));
          else if ((period = interval_from_strings
                              (modify_schedule_data->period,
                               modify_schedule_data->period_unit,
                               &period_months))
                   == -3)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("modify_schedule",
                                "PERIOD out of range"));
          else if (period < -1)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("modify_schedule",
                                "Failed to create interval from PERIOD"));
          else if ((duration = interval_from_strings
                                (modify_schedule_data->duration,
                                 modify_schedule_data->duration_unit,
                                 NULL))
                   == -3)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("modify_schedule",
                                "DURATION out of range"));
          else if (duration < -1)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("modify_schedule",
                                "Failed to create interval from DURATION"));
#if 0
          /* The actual time of a period in months can vary, so it's extremely
           * hard to do this check.  The schedule will still work fine if the
           * duration is longer than the period. */
          else if (period_months
                   && (duration > (period_months * 60 * 60 * 24 * 28)))
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("modify_schedule",
                                "Duration too long for number of months"));
#endif
          else if (period && (duration > period))
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("modify_schedule",
                                "Duration is longer than period"));
          else switch (modify_schedule
                        (modify_schedule_data->schedule_id,
                         modify_schedule_data->name,
                         modify_schedule_data->comment,
                         first_time,
                         period,
                         period_months,
                         duration,
                         modify_schedule_data->timezone))
            {
              case 0:
                SENDF_TO_CLIENT_OR_FAIL (XML_OK ("modify_schedule"));
                g_log ("event schedule", G_LOG_LEVEL_MESSAGE,
                       "Schedule %s has been modified",
                       modify_schedule_data->schedule_id);
                break;
              case 1:
                if (send_find_error_to_client ("modify_schedule",
                                               "schedule",
                                               modify_schedule_data->schedule_id,
                                               write_to_client,
                                               write_to_client_data))
                  {
                    error_send_to_client (error);
                    return;
                  }
                g_log ("event schedule", G_LOG_LEVEL_MESSAGE,
                       "Schedule could not be modified");
                break;
              case 2:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("modify_schedule",
                                    "Schedule with new name exists already"));
                g_log ("event schedule", G_LOG_LEVEL_MESSAGE,
                       "Schedule could not be modified");
                break;
              case 3:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("modify_schedule",
                                    "Error in type name"));
                g_log ("event schedule", G_LOG_LEVEL_MESSAGE,
                       "Schedule could not be modified");
                break;
              case 4:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("modify_schedule",
                                    "MODIFY_SCHEDULE requires a schedule_id"));
                g_log ("event schedule", G_LOG_LEVEL_MESSAGE,
                       "Schedule could not be modified");
                break;
              default:
              case -1:
                SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("modify_schedule"));
                g_log ("event schedule", G_LOG_LEVEL_MESSAGE,
                       "Schedule could not be modified");
                break;
            }

          modify_schedule_data_reset (modify_schedule_data);
          set_client_state (CLIENT_AUTHENTIC);
          break;
        }
      CLOSE (CLIENT_MODIFY_SCHEDULE, COMMENT);
      CLOSE (CLIENT_MODIFY_SCHEDULE, DURATION);
      CLOSE (CLIENT_MODIFY_SCHEDULE, FIRST_TIME);
      CLOSE (CLIENT_MODIFY_SCHEDULE, NAME);
      CLOSE (CLIENT_MODIFY_SCHEDULE, PERIOD);
      CLOSE (CLIENT_MODIFY_SCHEDULE, TIMEZONE);

      CLOSE (CLIENT_MODIFY_SCHEDULE_FIRST_TIME, DAY_OF_MONTH);
      CLOSE (CLIENT_MODIFY_SCHEDULE_FIRST_TIME, HOUR);
      CLOSE (CLIENT_MODIFY_SCHEDULE_FIRST_TIME, MINUTE);
      CLOSE (CLIENT_MODIFY_SCHEDULE_FIRST_TIME, MONTH);
      CLOSE (CLIENT_MODIFY_SCHEDULE_FIRST_TIME, YEAR);

      CLOSE (CLIENT_MODIFY_SCHEDULE_DURATION, UNIT);

      CLOSE (CLIENT_MODIFY_SCHEDULE_PERIOD, UNIT);

      case CLIENT_MODIFY_SLAVE:
        {
          assert (strcasecmp ("MODIFY_SLAVE", element_name) == 0);

          if (openvas_is_user_observer (current_credentials.username))
            {
              SEND_TO_CLIENT_OR_FAIL
               (XML_ERROR_SYNTAX ("modify_slave",
                                  "MODIFY is forbidden for observer users"));
            }
          else switch (modify_slave
                        (modify_slave_data->slave_id,
                         modify_slave_data->name,
                         modify_slave_data->comment,
                         modify_slave_data->host,
                         modify_slave_data->port,
                         modify_slave_data->login,
                         modify_slave_data->password))
            {
              case 0:
                SENDF_TO_CLIENT_OR_FAIL (XML_OK ("modify_slave"));
                g_log ("event slave", G_LOG_LEVEL_MESSAGE,
                       "Slave %s has been modified",
                       modify_slave_data->slave_id);
                break;
              case 1:
                if (send_find_error_to_client ("modify_slave",
                                               "slave",
                                               modify_slave_data->slave_id,
                                               write_to_client,
                                               write_to_client_data))
                  {
                    error_send_to_client (error);
                    return;
                  }
                g_log ("event slave", G_LOG_LEVEL_MESSAGE,
                       "Slave could not be modified");
                break;
              case 2:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("modify_slave",
                                    "Slave with new name exists already"));
                g_log ("event slave", G_LOG_LEVEL_MESSAGE,
                       "Slave could not be modified");
                break;
              case 3:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("modify_slave",
                                    "MODIFY_SLAVE requires a slave_id"));
                g_log ("event slave", G_LOG_LEVEL_MESSAGE,
                       "Slave could not be modified");
                break;
              default:
              case -1:
                SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("modify_slave"));
                g_log ("event slave", G_LOG_LEVEL_MESSAGE,
                       "Slave could not be modified");
                break;
            }

          modify_slave_data_reset (modify_slave_data);
          set_client_state (CLIENT_AUTHENTIC);
          break;
        }
      CLOSE (CLIENT_MODIFY_SLAVE, COMMENT);
      CLOSE (CLIENT_MODIFY_SLAVE, NAME);
      CLOSE (CLIENT_MODIFY_SLAVE, HOST);
      CLOSE (CLIENT_MODIFY_SLAVE, PORT);
      CLOSE (CLIENT_MODIFY_SLAVE, LOGIN);
      CLOSE (CLIENT_MODIFY_SLAVE, PASSWORD);

      case CLIENT_MODIFY_TARGET:
        {
          assert (strcasecmp ("MODIFY_TARGET", element_name) == 0);

          if (openvas_is_user_observer (current_credentials.username))
            {
              SEND_TO_CLIENT_OR_FAIL
               (XML_ERROR_SYNTAX ("modify_target",
                                  "MODIFY is forbidden for observer users"));
            }
          else if (modify_target_data->target_id == NULL)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("modify_target",
                                "MODIFY_TARGET requires a target_id attribute"));
          else if (modify_target_data->port_list_id == NULL)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("modify_target",
                                "MODIFY_TARGET requires a PORT_LIST"));
          else if (modify_target_data->name == NULL)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("modify_target",
                                "MODIFY_TARGET requires a NAME entity"));
          else if (strlen (modify_target_data->name) == 0)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("modify_target",
                                "MODIFY_TARGET name must be at"
                                " least one character long"));
          else if (((modify_target_data->hosts == NULL)
                    || (strlen (modify_target_data->hosts) == 0))
                   && modify_target_data->target_locator == NULL)
            /** @todo Legitimate to pass an empty hosts element? */
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("modify_target",
                                "MODIFY_TARGET hosts must both be at least one"
                                " character long, or TARGET_LOCATOR must"
                                " be set"));
          else if (strlen (modify_target_data->hosts) != 0
                   && modify_target_data->target_locator != NULL)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("modify_target",
                                " MODIFY_TARGET requires either a"
                                " TARGET_LOCATOR or a host"));
          /* Modify target from host string. */
          else switch (modify_target
                        (modify_target_data->target_id,
                         modify_target_data->name,
                         modify_target_data->hosts,
                         modify_target_data->comment,
                         modify_target_data->port_list_id,
                         modify_target_data->ssh_lsc_credential_id,
                         modify_target_data->ssh_port,
                         modify_target_data->smb_lsc_credential_id,
                         modify_target_data->target_locator,
                         modify_target_data->target_locator_username,
                         modify_target_data->target_locator_password))
            {
              case 1:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("modify_target",
                                    "Target exists already"));
                g_log ("event target", G_LOG_LEVEL_MESSAGE,
                       "Target could not be modified");
                break;
              case 2:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("modify_target",
                                    "Error in host specification"));
                g_log ("event target", G_LOG_LEVEL_MESSAGE,
                       "Target could not be modified");
                break;
              case 3:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("modify_target",
                                    "Host specification exceeds"
                                    " " G_STRINGIFY (MANAGE_MAX_HOSTS)
                                    " hosts"));
                g_log ("event target", G_LOG_LEVEL_MESSAGE,
                       "Target could not be modified");
                break;
              case 4:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("modify_target",
                                    "Error in port range"));
                g_log ("event target", G_LOG_LEVEL_MESSAGE,
                       "Target could not be modified");
                break;
              case 5:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_ERROR_SYNTAX ("modify_target",
                                    "Error in SSH port"));
                g_log ("event target", G_LOG_LEVEL_MESSAGE,
                       "Target could not be modified");
                break;
              case 6:
                g_log ("event target", G_LOG_LEVEL_MESSAGE,
                       "Target could not be modified");
                if (send_find_error_to_client
                     ("modify_target",
                      "port_list",
                      modify_target_data->port_list_id,
                      write_to_client,
                      write_to_client_data))
                  {
                    error_send_to_client (error);
                    return;
                  }
                break;
              case 7:
                g_log ("event target", G_LOG_LEVEL_MESSAGE,
                       "Target could not be modified");
                if (send_find_error_to_client
                     ("modify_target",
                      "LSC credential",
                      modify_target_data->ssh_lsc_credential_id,
                      write_to_client,
                      write_to_client_data))
                  {
                    error_send_to_client (error);
                    return;
                  }
                break;
              case 8:
                g_log ("event target", G_LOG_LEVEL_MESSAGE,
                       "Target could not be modified");
                if (send_find_error_to_client
                     ("modify_target",
                      "LSC credential",
                      modify_target_data->smb_lsc_credential_id,
                      write_to_client,
                      write_to_client_data))
                  {
                    error_send_to_client (error);
                    return;
                  }
                break;
              case 9:
                g_log ("event target", G_LOG_LEVEL_MESSAGE,
                       "Target could not be modified");
                if (send_find_error_to_client
                     ("modify_target",
                      "target",
                      modify_target_data->target_id,
                      write_to_client,
                      write_to_client_data))
                  {
                    error_send_to_client (error);
                    return;
                  }
                break;
              case -1:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_INTERNAL_ERROR ("modify_target"));
                g_log ("event target", G_LOG_LEVEL_MESSAGE,
                       "Target could not be modified");
                break;
              default:
                {
                  SENDF_TO_CLIENT_OR_FAIL (XML_OK ("modify_target"));
                  g_log ("event target", G_LOG_LEVEL_MESSAGE,
                         "Target %s has been modified",
                         modify_target_data->target_id);
                  break;
                }
            }

          modify_target_data_reset (modify_target_data);
          set_client_state (CLIENT_AUTHENTIC);
          break;
        }
      CLOSE (CLIENT_MODIFY_TARGET, COMMENT);
      CLOSE (CLIENT_MODIFY_TARGET, HOSTS);
      CLOSE (CLIENT_MODIFY_TARGET, NAME);
      CLOSE (CLIENT_MODIFY_TARGET, PORT_LIST);
      CLOSE (CLIENT_MODIFY_TARGET, SSH_LSC_CREDENTIAL);
      CLOSE (CLIENT_MODIFY_TARGET, SMB_LSC_CREDENTIAL);
      CLOSE (CLIENT_MODIFY_TARGET_TARGET_LOCATOR, PASSWORD);
      CLOSE (CLIENT_MODIFY_TARGET, TARGET_LOCATOR);
      CLOSE (CLIENT_MODIFY_TARGET_TARGET_LOCATOR, USERNAME);

      CLOSE (CLIENT_MODIFY_TARGET_SSH_LSC_CREDENTIAL, PORT);

      case CLIENT_TEST_ALERT:
        if (test_alert_data->alert_id)
          {
            alert_t alert;
            task_t task;

            if (find_alert (test_alert_data->alert_id, &alert))
              SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("test_alert"));
            else if (alert == 0)
              {
                if (send_find_error_to_client
                     ("test_alert",
                      "alert",
                      test_alert_data->alert_id,
                      write_to_client,
                      write_to_client_data))
                  {
                    error_send_to_client (error);
                    return;
                  }
              }
            else if (find_task (MANAGE_EXAMPLE_TASK_UUID, &task))
              SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("test_alert"));
            else if (task == 0)
              SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("test_alert"));
            else switch (manage_alert (alert,
                                       task,
                                       EVENT_TASK_RUN_STATUS_CHANGED,
                                       (void*) TASK_STATUS_DONE))
              {
                case 0:
                  SEND_TO_CLIENT_OR_FAIL (XML_OK ("test_alert"));
                  break;
                case -1:
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_INTERNAL_ERROR ("test_alert"));
                  break;
                default: /* Programming error. */
                  assert (0);
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_INTERNAL_ERROR ("test_alert"));
                  break;
              }
          }
        else
          SEND_TO_CLIENT_OR_FAIL
           (XML_ERROR_SYNTAX ("test_alert",
                              "TEST_ALERT requires an alert_id"
                              " attribute"));
        test_alert_data_reset (test_alert_data);
        set_client_state (CLIENT_AUTHENTIC);
        break;

      case CLIENT_PAUSE_TASK:
        if (pause_task_data->task_id)
          {
            task_t task;
            if (find_task (pause_task_data->task_id, &task))
              SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("pause_task"));
            else if (task == 0)
              {
                if (send_find_error_to_client ("pause_task",
                                               "task",
                                               pause_task_data->task_id,
                                               write_to_client,
                                               write_to_client_data))
                  {
                    error_send_to_client (error);
                    return;
                  }
              }
            else switch (pause_task (task))
              {
                case 0:   /* Paused. */
                  SEND_TO_CLIENT_OR_FAIL (XML_OK ("pause_task"));
                  g_log ("event task", G_LOG_LEVEL_MESSAGE,
                         "Task %s has been paused",
                         pause_task_data->task_id);
                  break;
                case 1:   /* Pause requested. */
                  SEND_TO_CLIENT_OR_FAIL (XML_OK_REQUESTED ("pause_task"));
                  g_log ("event task", G_LOG_LEVEL_MESSAGE,
                         "Task %s has been requested to pause",
                         pause_task_data->task_id);
                  break;
                case -5:
                  SEND_TO_CLIENT_OR_FAIL (XML_SERVICE_DOWN ("pause_task"));
                  g_log ("event task", G_LOG_LEVEL_MESSAGE,
                         "Task %s has failed to pause",
                         pause_task_data->task_id);
                  break;
                default:  /* Programming error. */
                  assert (0);
                case -1:
                  /* to_scanner is full. */
                  /** @todo Consider reverting parsing for retry. */
                  /** @todo process_omp_client_input must return -2. */
                  abort ();
              }
          }
        else
          SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("pause_task"));
        pause_task_data_reset (pause_task_data);
        set_client_state (CLIENT_AUTHENTIC);
        break;

      case CLIENT_RESTORE:
        if (restore_data->id)
          {
            switch (manage_restore (restore_data->id))
              {
                case 0:
                  SEND_TO_CLIENT_OR_FAIL (XML_OK ("restore"));
                  g_log ("event task", G_LOG_LEVEL_MESSAGE,
                         "Resource %s has been restored",
                         restore_data->id);
                  break;
                case 1:
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_ERROR_SYNTAX ("restore",
                                      "Resource refers into trashcan"));
                  break;
                case 2:
                  if (send_find_error_to_client ("restore",
                                                 "resource",
                                                 restore_data->id,
                                                 write_to_client,
                                                 write_to_client_data))
                    {
                      error_send_to_client (error);
                      return;
                    }
                  break;
                case 3:
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_ERROR_SYNTAX ("restore",
                                      "A resource with this name exists"
                                      " already"));
                  break;
                default:  /* Programming error. */
                  assert (0);
                case -1:
                  SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("restore"));
                  break;
              }
          }
        else
          SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("restore"));
        restore_data_reset (restore_data);
        set_client_state (CLIENT_AUTHENTIC);
        break;

      case CLIENT_RESUME_OR_START_TASK:
        if (resume_or_start_task_data->task_id)
          {
            task_t task;
            if (find_task (resume_or_start_task_data->task_id, &task))
              SEND_TO_CLIENT_OR_FAIL
               (XML_INTERNAL_ERROR ("resume_or_start_task"));
            else if (task == 0)
              {
                if (send_find_error_to_client
                     ("resume_or_start_task",
                      "task",
                      resume_or_start_task_data->task_id,
                      write_to_client,
                      write_to_client_data))
                  {
                    error_send_to_client (error);
                    return;
                  }
              }
            else if (forked == 2)
              /* Prevent the forked child from forking again, as then both
               * forked children would be using the same server session. */
              abort (); /** @todo Respond with error or something. */
            else
              {
                char *report_id;
                switch (resume_or_start_task (task, &report_id))
                  {
                    case 0:
                      {
                        gchar *msg;
                        msg = g_strdup_printf
                               ("<resume_or_start_task_response"
                                " status=\"" STATUS_OK_REQUESTED "\""
                                " status_text=\""
                                STATUS_OK_REQUESTED_TEXT
                                "\">"
                                "<report_id>%s</report_id>"
                                "</resume_or_start_task_response>",
                                report_id);
                        free (report_id);
                        if (send_to_client (msg,
                                            write_to_client,
                                            write_to_client_data))
                          {
                            g_free (msg);
                            error_send_to_client (error);
                            return;
                          }
                        g_free (msg);
                        g_log ("event task", G_LOG_LEVEL_MESSAGE,
                               "Task %s has been requested to start",
                               resume_or_start_task_data->task_id);
                      }
                      forked = 1;
                      break;
                    case 1:
                      SEND_TO_CLIENT_OR_FAIL
                       (XML_ERROR_SYNTAX ("resume_or_start_task",
                                          "Task is active already"));
                      g_log ("event task", G_LOG_LEVEL_MESSAGE,
                             "Task %s has failed to start",
                             resume_or_start_task_data->task_id);
                      break;
                    case 22:
                      SEND_TO_CLIENT_OR_FAIL
                       (XML_ERROR_SYNTAX ("resume_or_start_task",
                                          "Task must be in Stopped state"));
                      g_log ("event task", G_LOG_LEVEL_MESSAGE,
                             "Task %s has failed to start",
                             resume_or_start_task_data->task_id);
                      break;
                    case 2:
                      /* Forked task process: success. */
                      current_error = 2;
                      g_set_error (error,
                                   G_MARKUP_ERROR,
                                   G_MARKUP_ERROR_INVALID_CONTENT,
                                   "Dummy error for current_error");
                      break;
                    case -10:
                      /* Forked task process: error. */
                      current_error = -10;
                      g_set_error (error,
                                   G_MARKUP_ERROR,
                                   G_MARKUP_ERROR_INVALID_CONTENT,
                                   "Dummy error for current_error");
                      break;
                    case -6:
                      SEND_TO_CLIENT_OR_FAIL
                       (XML_ERROR_SYNTAX ("resume_or_start_task",
                                          "There is already a task running in"
                                          " this process"));
                      g_log ("event task", G_LOG_LEVEL_MESSAGE,
                             "Task %s has failed to start",
                             resume_or_start_task_data->task_id);
                      break;
                    case -2:
                      /* Task target lacks hosts.  This is checked when the
                       * target is created. */
                      assert (0);
                      /*@fallthrough@*/
                    case -4:
                      /* Task lacks target.  This is checked when the task is
                       * created anyway. */
                      assert (0);
                      /*@fallthrough@*/
                    case -1:
                    case -3: /* Failed to create report. */
                      SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("resume_or_start_task"));
                      g_log ("event task", G_LOG_LEVEL_MESSAGE,
                             "Task %s has failed to start",
                             resume_or_start_task_data->task_id);
                      break;
                    case -5:
                      SEND_TO_CLIENT_OR_FAIL
                       (XML_SERVICE_DOWN ("resume_or_start_task"));
                      g_log ("event task", G_LOG_LEVEL_MESSAGE,
                             "Task %s has failed to start",
                             resume_or_start_task_data->task_id);
                      break;
                    default: /* Programming error. */
                      assert (0);
                      SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("resume_or_start_task"));
                      g_log ("event task", G_LOG_LEVEL_MESSAGE,
                             "Task %s has failed to start",
                             resume_or_start_task_data->task_id);
                      break;
                  }
              }
          }
        else
          SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("resume_or_start_task"));
        resume_or_start_task_data_reset (resume_or_start_task_data);
        set_client_state (CLIENT_AUTHENTIC);
        break;

      case CLIENT_RESUME_PAUSED_TASK:
        if (resume_paused_task_data->task_id)
          {
            task_t task;
            if (find_task (resume_paused_task_data->task_id, &task))
              SEND_TO_CLIENT_OR_FAIL
               (XML_INTERNAL_ERROR ("resume_paused_task"));
            else if (task == 0)
              {
                if (send_find_error_to_client
                     ("resume_paused_task",
                      "task",
                      resume_paused_task_data->task_id,
                      write_to_client,
                      write_to_client_data))
                  {
                    error_send_to_client (error);
                    return;
                  }
              }
            else switch (resume_paused_task (task))
              {
                case 0:   /* Resumed. */
                  SEND_TO_CLIENT_OR_FAIL (XML_OK ("resume_paused_task"));
                  g_log ("event task", G_LOG_LEVEL_MESSAGE,
                         "Task %s has been resumed",
                         resume_paused_task_data->task_id);
                  break;
                case 1:   /* Resume requested. */
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_OK_REQUESTED ("resume_paused_task"));
                  g_log ("event task", G_LOG_LEVEL_MESSAGE,
                         "Task %s has been requested to resume",
                         resume_paused_task_data->task_id);
                  break;
                case -5:
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_SERVICE_DOWN ("resume_paused_task"));
                  g_log ("event task", G_LOG_LEVEL_MESSAGE,
                         "Task %s has failed to resume",
                         resume_paused_task_data->task_id);
                  break;
                default:  /* Programming error. */
                  assert (0);
                case -1:
                  /* to_scanner is full. */
                  /** @todo Consider reverting parsing for retry. */
                  /** @todo process_omp_client_input must return -2. */
                  abort ();
              }
          }
        else
          SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("resume_paused_task"));
        resume_paused_task_data_reset (resume_paused_task_data);
        set_client_state (CLIENT_AUTHENTIC);
        break;

      case CLIENT_RESUME_STOPPED_TASK:
        if (resume_stopped_task_data->task_id)
          {
            task_t task;
            if (find_task (resume_stopped_task_data->task_id, &task))
              SEND_TO_CLIENT_OR_FAIL
               (XML_INTERNAL_ERROR ("resume_stopped_task"));
            else if (task == 0)
              {
                if (send_find_error_to_client ("resume_stopped_task",
                                               "task",
                                               resume_stopped_task_data->task_id,
                                               write_to_client,
                                               write_to_client_data))
                  {
                    error_send_to_client (error);
                    return;
                  }
              }
            else if (forked == 2)
              /* Prevent the forked child from forking again, as then both
               * forked children would be using the same server session. */
              abort (); /** @todo Respond with error or something. */
            else
              {
                char *report_id;
                switch (resume_stopped_task (task, &report_id))
                  {
                    case 0:
                      {
                        gchar *msg;
                        msg = g_strdup_printf
                               ("<resume_stopped_task_response"
                                " status=\"" STATUS_OK_REQUESTED "\""
                                " status_text=\""
                                STATUS_OK_REQUESTED_TEXT
                                "\">"
                                "<report_id>%s</report_id>"
                                "</resume_stopped_task_response>",
                                report_id);
                        free (report_id);
                        if (send_to_client (msg,
                                            write_to_client,
                                            write_to_client_data))
                          {
                            g_free (msg);
                            error_send_to_client (error);
                            return;
                          }
                        g_free (msg);
                      }
                      forked = 1;
                      g_log ("event task", G_LOG_LEVEL_MESSAGE,
                             "Task %s has been resumed",
                             resume_stopped_task_data->task_id);
                      break;
                    case 1:
                      SEND_TO_CLIENT_OR_FAIL
                       (XML_ERROR_SYNTAX ("resume_stopped_task",
                                          "Task is active already"));
                      g_log ("event task", G_LOG_LEVEL_MESSAGE,
                             "Task %s has failed to resume",
                             resume_stopped_task_data->task_id);
                      break;
                    case 22:
                      SEND_TO_CLIENT_OR_FAIL
                       (XML_ERROR_SYNTAX ("resume_stopped_task",
                                          "Task must be in Stopped state"));
                      g_log ("event task", G_LOG_LEVEL_MESSAGE,
                             "Task %s has failed to resume",
                             resume_stopped_task_data->task_id);
                      break;
                    case 2:
                      /* Forked task process: success. */
                      current_error = 2;
                      g_set_error (error,
                                   G_MARKUP_ERROR,
                                   G_MARKUP_ERROR_INVALID_CONTENT,
                                   "Dummy error for current_error");
                      break;
                    case -10:
                      /* Forked task process: error. */
                      current_error = -10;
                      g_set_error (error,
                                   G_MARKUP_ERROR,
                                   G_MARKUP_ERROR_INVALID_CONTENT,
                                   "Dummy error for current_error");
                      break;
                    case -6:
                      SEND_TO_CLIENT_OR_FAIL
                       (XML_ERROR_SYNTAX ("resume_stopped_task",
                                          "There is already a task running in"
                                          " this process"));
                      g_log ("event task", G_LOG_LEVEL_MESSAGE,
                             "Task %s has failed to resume",
                             resume_stopped_task_data->task_id);
                      break;
                    case -2:
                      /* Task target lacks hosts.  This is checked when the
                       * target is created. */
                      assert (0);
                      /*@fallthrough@*/
                    case -4:
                      /* Task lacks target.  This is checked when the task is
                       * created anyway. */
                      assert (0);
                      /*@fallthrough@*/
                    case -1:
                    case -3: /* Failed to create report. */
                      SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("resume_stopped_task"));
                      g_log ("event task", G_LOG_LEVEL_MESSAGE,
                             "Task %s has failed to resume",
                             resume_stopped_task_data->task_id);
                      break;
                    case -5:
                      SEND_TO_CLIENT_OR_FAIL
                       (XML_SERVICE_DOWN ("resume_stopped_task"));
                      g_log ("event task", G_LOG_LEVEL_MESSAGE,
                             "Task %s has failed to resume",
                             resume_stopped_task_data->task_id);
                      break;
                    default: /* Programming error. */
                      assert (0);
                      SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("resume_stopped_task"));
                      g_log ("event task", G_LOG_LEVEL_MESSAGE,
                             "Task %s has failed to resume",
                             resume_stopped_task_data->task_id);
                      break;
                  }
              }
          }
        else
          SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("resume_stopped_task"));
        resume_stopped_task_data_reset (resume_stopped_task_data);
        set_client_state (CLIENT_AUTHENTIC);
        break;

      case CLIENT_RUN_WIZARD:
        if (run_wizard_data->name)
          {
            gchar *command_error;
            switch (manage_run_wizard (run_wizard_data->name,
                                       (int (*) (void *, gchar *, gchar **))
                                         process_omp,
                                       omp_parser,
                                       run_wizard_data->params,
                                       &command_error))
              {
                case 3:
                  /* Parent after a start_task fork. */
                  forked = 1;
                case 0:
                  {
                    gchar *msg;
                    msg = g_strdup_printf
                           ("<run_wizard_response"
                            " status=\"" STATUS_OK_REQUESTED "\""
                            " status_text=\"" STATUS_OK_REQUESTED_TEXT "\">"
                            "</run_wizard_response>");
                    if (send_to_client (msg,
                                        write_to_client,
                                        write_to_client_data))
                      {
                        g_free (msg);
                        error_send_to_client (error);
                        return;
                      }
                    g_free (msg);
                    g_log ("event task", G_LOG_LEVEL_MESSAGE,
                           "Wizard ran");
                    break;
                  }

                case 1:
                  {
                    SEND_TO_CLIENT_OR_FAIL
                     (XML_ERROR_SYNTAX ("run_wizard",
                                        "NAME characters must be alphanumeric"
                                        " or underscore"));
                    run_wizard_data_reset (run_wizard_data);
                    set_client_state (CLIENT_AUTHENTIC);
                    break;
                  }

                case 2:
                  {
                    /* Process forked to run a task. */
                    current_error = 2;
                    g_set_error (error,
                                 G_MARKUP_ERROR,
                                 G_MARKUP_ERROR_INVALID_CONTENT,
                                 "Dummy error for current_error");
                    break;
                  }

                case 4:
                  {
                    gchar *msg;
                    msg = g_strdup_printf
                           ("<run_wizard_response"
                            " status=\"" STATUS_ERROR_SYNTAX "\""
                            " status_text=\"%s\"/>",
                            command_error ? command_error : "Internal Error");
                    if (command_error)
                      g_free (command_error);
                    if (send_to_client (msg,
                                        write_to_client,
                                        write_to_client_data))
                      {
                        g_free (msg);
                        error_send_to_client (error);
                        return;
                      }
                    g_free (msg);
                    g_log ("event wizard", G_LOG_LEVEL_MESSAGE,
                           "Wizard failed to run");
                    break;
                  }

                case -1:
                  {
                    /* Internal error. */
                    SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("run_wizard"));
                    g_log ("event wizard", G_LOG_LEVEL_MESSAGE,
                           "Wizard failed to run");
                    break;
                  }

                case -2:
                  {
                    gchar *msg;
                    /* to_scanner buffer full. */
                    msg = g_strdup_printf
                           ("<run_wizard_response"
                            " status=\"" STATUS_INTERNAL_ERROR "\""
                            " status_text=\""
                            STATUS_INTERNAL_ERROR_TEXT
                            ": Wizard filled up to_scanner buffer\">"
                            "</run_wizard_response>");
                    if (send_to_client (msg,
                                        write_to_client,
                                        write_to_client_data))
                      {
                        g_free (msg);
                        error_send_to_client (error);
                        return;
                      }
                    g_free (msg);
                    g_log ("event task", G_LOG_LEVEL_MESSAGE,
                           "Wizard failed to run: to_scanner buffer full");
                    break;
                  }

                case -10:
                  {
                    /* Process forked to run a task.  Task start failed. */
                    current_error = -10;
                    g_set_error (error,
                                 G_MARKUP_ERROR,
                                 G_MARKUP_ERROR_INVALID_CONTENT,
                                 "Dummy error for current_error");
                    break;
                  }
              }
          }
        else
          SEND_TO_CLIENT_OR_FAIL (XML_ERROR_SYNTAX ("run_wizard",
                                                    "RUN_WIZARD requires a NAME"
                                                    " element"));
        run_wizard_data_reset (run_wizard_data);
        set_client_state (CLIENT_AUTHENTIC);
        break;

      CLOSE (CLIENT_RUN_WIZARD, NAME);
      CLOSE (CLIENT_RUN_WIZARD, PARAMS);
      CLOSE (CLIENT_RUN_WIZARD_PARAMS_PARAM, NAME);
      CLOSE (CLIENT_RUN_WIZARD_PARAMS_PARAM, VALUE);

      case CLIENT_RUN_WIZARD_PARAMS_PARAM:
        assert (strcasecmp ("PARAM", element_name) == 0);
        array_add (run_wizard_data->params, run_wizard_data->param);
        run_wizard_data->param = NULL;
        set_client_state (CLIENT_RUN_WIZARD_PARAMS);
        break;

      case CLIENT_START_TASK:
        if (start_task_data->task_id)
          {
            task_t task;
            if (find_task (start_task_data->task_id, &task))
              SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("start_task"));
            else if (task == 0)
              {
                if (send_find_error_to_client ("start_task",
                                               "task",
                                               start_task_data->task_id,
                                               write_to_client,
                                               write_to_client_data))
                  {
                    error_send_to_client (error);
                    return;
                  }
              }
            else if (forked == 2)
              /* Prevent the forked child from forking again, as then both
               * forked children would be using the same server session. */
              abort (); /** @todo Respond with error or something. */
            else
              {
                char *report_id;
                switch (start_task (task, &report_id))
                  {
                    case 0:
                      {
                        gchar *msg;
                        msg = g_strdup_printf
                               ("<start_task_response"
                                " status=\"" STATUS_OK_REQUESTED "\""
                                " status_text=\""
                                STATUS_OK_REQUESTED_TEXT
                                "\">"
                                "<report_id>%s</report_id>"
                                "</start_task_response>",
                                report_id);
                        free (report_id);
                        if (send_to_client (msg,
                                            write_to_client,
                                            write_to_client_data))
                          {
                            g_free (msg);
                            error_send_to_client (error);
                            return;
                          }
                        g_free (msg);
                        g_log ("event task", G_LOG_LEVEL_MESSAGE,
                               "Task %s has been requested to start",
                               start_task_data->task_id);
                      }
                      forked = 1;
                      break;
                    case 1:
                      SEND_TO_CLIENT_OR_FAIL
                       (XML_ERROR_SYNTAX ("start_task",
                                          "Task is active already"));
                      g_log ("event task", G_LOG_LEVEL_MESSAGE,
                             "Task %s has failed to start",
                             start_task_data->task_id);
                      break;
                    case 2:
                      /* Forked task process: success. */
                      current_error = 2;
                      g_set_error (error,
                                   G_MARKUP_ERROR,
                                   G_MARKUP_ERROR_INVALID_CONTENT,
                                   "Dummy error for current_error");
                      break;
                    case -10:
                      /* Forked task process: error. */
                      current_error = -10;
                      g_set_error (error,
                                   G_MARKUP_ERROR,
                                   G_MARKUP_ERROR_INVALID_CONTENT,
                                   "Dummy error for current_error");
                      break;
                    case -6:
                      SEND_TO_CLIENT_OR_FAIL
                       (XML_ERROR_SYNTAX ("start_task",
                                          "There is already a task running in"
                                          " this process"));
                      g_log ("event task", G_LOG_LEVEL_MESSAGE,
                             "Task %s has failed to start",
                             start_task_data->task_id);
                      break;
                    case -2:
                      /* Task lacks target.  This is true for container
                       * tasks. */
                      SEND_TO_CLIENT_OR_FAIL
                       (XML_ERROR_SYNTAX ("start_task",
                                          "Task must have a target"));
                      g_log ("event task", G_LOG_LEVEL_MESSAGE,
                             "Task %s has failed to start",
                             start_task_data->task_id);
                      break;
                    case -4:
                      /* Task target lacks hosts.  This is checked when the
                       * target is created. */
                      assert (0);
                      /*@fallthrough@*/
                    case -3: /* Failed to create report. */
                      SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("start_task"));
                      g_log ("event task", G_LOG_LEVEL_MESSAGE,
                             "Task %s has failed to start",
                             start_task_data->task_id);
                      break;
                    case -5:
                      SEND_TO_CLIENT_OR_FAIL (XML_SERVICE_DOWN ("start_task"));
                      g_log ("event task", G_LOG_LEVEL_MESSAGE,
                             "Task %s has failed to start",
                             start_task_data->task_id);
                      break;
                    default: /* Programming error. */
                      assert (0);
                      SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("start_task"));
                      g_log ("event task", G_LOG_LEVEL_MESSAGE,
                             "Task %s has failed to start",
                             start_task_data->task_id);
                      break;
                  }
              }
          }
        else
          SEND_TO_CLIENT_OR_FAIL (XML_ERROR_SYNTAX ("start_task",
                                                    "START_TASK task_id"
                                                    " attribute must be set"));
        start_task_data_reset (start_task_data);
        set_client_state (CLIENT_AUTHENTIC);
        break;

      case CLIENT_STOP_TASK:
        if (stop_task_data->task_id)
          {
            task_t task;

            if (find_task (stop_task_data->task_id, &task))
              SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("stop_task"));
            else if (task == 0)
              {
                if (send_find_error_to_client ("stop_task",
                                               "task",
                                               stop_task_data->task_id,
                                               write_to_client,
                                               write_to_client_data))
                  {
                    error_send_to_client (error);
                    return;
                  }
              }
            else switch (stop_task (task))
              {
                case 0:   /* Stopped. */
                  SEND_TO_CLIENT_OR_FAIL (XML_OK ("stop_task"));
                  g_log ("event task", G_LOG_LEVEL_MESSAGE,
                         "Task %s has been stopped",
                         stop_task_data->task_id);
                  break;
                case 1:   /* Stop requested. */
                  SEND_TO_CLIENT_OR_FAIL (XML_OK_REQUESTED ("stop_task"));
                  g_log ("event task", G_LOG_LEVEL_MESSAGE,
                         "Task %s has been requested to stop",
                         stop_task_data->task_id);
                  break;
                case -5:
                  SEND_TO_CLIENT_OR_FAIL (XML_SERVICE_DOWN ("stop_task"));
                  g_log ("event task", G_LOG_LEVEL_MESSAGE,
                         "Task %s has failed to stop",
                         stop_task_data->task_id);
                  break;
                default:  /* Programming error. */
                  assert (0);
                case -1:
                  /* to_scanner is full. */
                  /** @todo Consider reverting parsing for retry. */
                  /** @todo process_omp_client_input must return -2. */
                  abort ();
              }
          }
        else
          SEND_TO_CLIENT_OR_FAIL
           (XML_ERROR_SYNTAX ("stop_task",
                              "STOP_TASK requires a task_id attribute"));
        stop_task_data_reset (stop_task_data);
        set_client_state (CLIENT_AUTHENTIC);
        break;

      case CLIENT_GET_AGENTS:
        {
          int format;

          assert (strcasecmp ("GET_AGENTS", element_name) == 0);

          if (get_agents_data->format)
            {
              if (strlen (get_agents_data->format))
                {
                  if (strcasecmp (get_agents_data->format, "installer") == 0)
                    format = 1;
                  else if (strcasecmp (get_agents_data->format,
                                       "howto_install")
                           == 0)
                    format = 2;
                  else if (strcasecmp (get_agents_data->format, "howto_use")
                           == 0)
                    format = 3;
                  else
                    format = -1;
                }
              else
                format = 0;
            }
          else if (get_agents_data->get.details == 1) /* For exporting */
            format = 1;
          else
            format = 0;
          if (format == -1)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("get_agents",
                                "GET_AGENTS format attribute should"
                                " be 'installer', 'howto_install' or 'howto_use'."));
          else
            {
              iterator_t agents;
              int ret, count, filtered, first;
              get_data_t * get;

              get = &get_agents_data->get;
              if ((!get->filter && !get->filt_id)
                  || (get->filt_id && strcmp (get->filt_id, "-2") == 0))
                {
                  char *user_filter = setting_filter ("Agents");

                  if (user_filter && strlen (user_filter))
                    {
                      get->filt_id = user_filter;
                      get->filter = filter_term (user_filter);
                    }
                  else
                    get->filt_id = g_strdup("0");
                }

              ret = init_agent_iterator (&agents,
                                         &get_agents_data->get);
              if (ret)
                {
                  switch (ret)
                    {
                      case 1:
                        if (send_find_error_to_client ("get_agents",
                                                       "agents",
                                                       get_agents_data->get.id,
                                                       write_to_client,
                                                       write_to_client_data))
                          {
                            error_send_to_client (error);
                            return;
                          }
                        break;
                      case 2:
                        if (send_find_error_to_client
                             ("get_agents",
                              "filter",
                              get_filters_data->get.filt_id,
                              write_to_client,
                              write_to_client_data))
                          {
                            error_send_to_client (error);
                            return;
                          }
                        break;
                      case -1:
                        SEND_TO_CLIENT_OR_FAIL
                         (XML_INTERNAL_ERROR ("get_agents"));
                        break;
                    }
                  get_agents_data_reset (get_agents_data);
                  set_client_state (CLIENT_AUTHENTIC);
                  break;
                }

              count = 0;
              manage_filter_controls (get->filter, &first, NULL, NULL, NULL);
              SEND_GET_START ("agent", &get_agents_data->get);

              while (1)
                {
                  ret = get_next (&agents, &get_agents_data->get, &first,
                                  &count, init_agent_iterator);
                  if (ret == 1)
                    break;
                  if (ret == -1)
                    {
                      internal_error_send_to_client (error);
                      return;
                    }

                  SEND_GET_COMMON (agent, &get_agents_data->get,
                                   &agents);
                  switch (format)
                    {
                      case 1: /* installer */
                        {
                          time_t trust_time;
                          trust_time = agent_iterator_trust_time (&agents);

                          SENDF_TO_CLIENT_OR_FAIL
                           ("<package format=\"installer\">"
                            "<filename>%s</filename>"
                            "%s"
                            "</package>"
                            "<installer>"
                            "<trust>%s<time>%s</time></trust>"
                            "</installer>"
                            "</agent>",
                            agent_iterator_installer_filename (&agents),
                            agent_iterator_installer_64 (&agents),
                            agent_iterator_trust (&agents),
                            iso_time (&trust_time));
                        }
                        break;
                      case 2: /* howto_install */
                        SENDF_TO_CLIENT_OR_FAIL
                         ("<package format=\"howto_install\">%s</package>"
                          "</agent>",
                          agent_iterator_howto_install (&agents));
                        break;
                      case 3: /* howto_use */
                        SENDF_TO_CLIENT_OR_FAIL
                         ("<package format=\"howto_use\">%s</package>"
                          "</agent>",
                          agent_iterator_howto_use (&agents));
                        break;
                      default:
                        {
                          time_t trust_time;

                          trust_time = agent_iterator_trust_time (&agents);

                          SENDF_TO_CLIENT_OR_FAIL
                           ("<installer>"
                            "<trust>%s<time>%s</time></trust>"
                            "</installer>"
                            "</agent>",
                            agent_iterator_trust (&agents),
                            iso_time (&trust_time));
                        }
                        break;
                    }
                  count++;
                }
              cleanup_iterator (&agents);
              filtered = get_agents_data->get.id
                          ? 1
                          : agent_count (&get_agents_data->get);
              SEND_GET_END ("agent", &get_agents_data->get, count, filtered);
            }
          get_agents_data_reset (get_agents_data);
          set_client_state (CLIENT_AUTHENTIC);
          break;
        }

      case CLIENT_GET_CONFIGS:
        {
          iterator_t configs;
          int ret, filtered, first, count;
          get_data_t *get;

          assert (strcasecmp ("GET_CONFIGS", element_name) == 0);

          get = &get_configs_data->get;
          if ((!get->filter && !get->filt_id)
              || (get->filt_id && strcmp (get->filt_id, "-2") == 0))
            {
              char *user_filter = setting_filter ("Configs");

              if (user_filter && strlen (user_filter))
                {
                  get->filt_id = user_filter;
                  get->filter = filter_term (user_filter);
                }
              else
                get->filt_id = g_strdup("0");
            }

          ret = init_config_iterator (&configs, get);
          if (ret)
            {
              switch (ret)
                {
                  case 1:
                    if (send_find_error_to_client ("get_configs",
                                                   "config",
                                                   get_configs_data->get.id,
                                                   write_to_client,
                                                   write_to_client_data))
                      {
                        error_send_to_client (error);
                        return;
                      }
                    break;
                  case 2:
                    if (send_find_error_to_client
                         ("get_configs",
                          "config",
                          get_configs_data->get.filt_id,
                          write_to_client,
                          write_to_client_data))
                      {
                        error_send_to_client (error);
                        return;
                      }
                    break;
                  case -1:
                    SEND_TO_CLIENT_OR_FAIL
                     (XML_INTERNAL_ERROR ("get_configs"));
                    break;
                }
              get_configs_data_reset (get_configs_data);
              set_client_state (CLIENT_AUTHENTIC);
              break;
            }

          count = 0;
          manage_filter_controls (get->filter, &first, NULL, NULL, NULL);
          SEND_GET_START ("config", &get_configs_data->get);
          while (1)
            {
              int config_nvts_growing, config_families_growing;
              const char *selector;
              config_t config;

              ret = get_next (&configs, &get_configs_data->get, &first,
                              &count, init_config_iterator);
              if (ret == 1)
                break;
              if (ret == -1)
                {
                  internal_error_send_to_client (error);
                  return;
                }
              SEND_GET_COMMON (config, &get_configs_data->get,
                               &configs);

              /** @todo This should really be an nvt_selector_t. */
              selector = config_iterator_nvt_selector (&configs);
              config = config_iterator_config (&configs);
              config_nvts_growing = config_iterator_nvts_growing (&configs);
              config_families_growing = config_iterator_families_growing
                                         (&configs);

              SENDF_TO_CLIENT_OR_FAIL ("<family_count>"
                                       "%i<growing>%i</growing>"
                                       "</family_count>"
                                       /* The number of NVT's selected
                                        * by the selector. */
                                       "<nvt_count>"
                                       "%i<growing>%i</growing>"
                                       "</nvt_count>",
                                       config_iterator_family_count (&configs),
                                       config_families_growing,
                                       config_iterator_nvt_count (&configs),
                                       config_nvts_growing);

              if (get_configs_data->families
                  || get_configs_data->get.details)
                {
                  iterator_t families;
                  int max_nvt_count = 0, known_nvt_count = 0;

                  SENDF_TO_CLIENT_OR_FAIL ("<families>");
                  init_family_iterator (&families,
                                        config_families_growing,
                                        selector,
                                        1);
                  while (next (&families))
                    {
                      int family_growing, family_max;
                      int family_selected_count;
                      const char *family;

                      family = family_iterator_name (&families);
                      if (family)
                        {
                          family_growing = nvt_selector_family_growing
                                            (selector,
                                             family,
                                             config_families_growing);
                          family_max = family_nvt_count (family);
                          family_selected_count
                            = nvt_selector_nvt_count (selector,
                                                      family,
                                                      family_growing);
                          known_nvt_count += family_selected_count;
                        }
                      else
                        {
                          /* The family can be NULL if an RC adds an
                           * NVT to a config and the NVT is missing
                           * from the NVT cache. */
                          family_growing = 0;
                          family_max = -1;
                          family_selected_count = nvt_selector_nvt_count
                                                   (selector, NULL, 0);
                        }

                      SENDF_TO_CLIENT_OR_FAIL
                       ("<family>"
                        "<name>%s</name>"
                        /* The number of selected NVT's. */
                        "<nvt_count>%i</nvt_count>"
                        /* The total number of NVT's in the family. */
                        "<max_nvt_count>%i</max_nvt_count>"
                        "<growing>%i</growing>"
                        "</family>",
                        family ? family : "",
                        family_selected_count,
                        family_max,
                        family_growing);
                      if (family_max > 0)
                        max_nvt_count += family_max;
                    }
                  cleanup_iterator (&families);
                  SENDF_TO_CLIENT_OR_FAIL
                   ("</families>"
                    /* The total number of NVT's in all the
                     * families for selector selects at least one
                     * NVT. */
                    "<max_nvt_count>%i</max_nvt_count>"
                    /* Total number of selected known NVT's. */
                    "<known_nvt_count>"
                    "%i"
                    "</known_nvt_count>",
                    max_nvt_count,
                    known_nvt_count);
                }

              if (get_configs_data->preferences
                  || get_configs_data->get.details)
                {
                  iterator_t prefs;
                  config_t config = config_iterator_config (&configs);

                  assert (config);

                  SEND_TO_CLIENT_OR_FAIL ("<preferences>");

                  init_nvt_preference_iterator (&prefs, NULL);
                  while (next (&prefs))
                    {
                      GString *buffer = g_string_new ("");
                      buffer_config_preference_xml (buffer, &prefs, config);
                      SEND_TO_CLIENT_OR_FAIL (buffer->str);
                      g_string_free (buffer, TRUE);
                    }
                  cleanup_iterator (&prefs);

                  SEND_TO_CLIENT_OR_FAIL ("</preferences>");
                }

              if (get_configs_data->get.details)
                {
                  iterator_t selectors;

                  SEND_TO_CLIENT_OR_FAIL ("<nvt_selectors>");

                  init_nvt_selector_iterator (&selectors,
                                              NULL,
                                              config,
                                              NVT_SELECTOR_TYPE_ANY);
                  while (next (&selectors))
                    {
                      int type = nvt_selector_iterator_type (&selectors);
                      SENDF_TO_CLIENT_OR_FAIL
                       ("<nvt_selector>"
                        "<name>%s</name>"
                        "<include>%i</include>"
                        "<type>%i</type>"
                        "<family_or_nvt>%s</family_or_nvt>"
                        "</nvt_selector>",
                        nvt_selector_iterator_name (&selectors),
                        nvt_selector_iterator_include (&selectors),
                        type,
                        (type == NVT_SELECTOR_TYPE_ALL
                          ? ""
                          : nvt_selector_iterator_nvt (&selectors)));
                    }
                  cleanup_iterator (&selectors);

                  SEND_TO_CLIENT_OR_FAIL ("</nvt_selectors>");
                }

              if (get_configs_data->tasks)
                {
                  iterator_t tasks;

                  SEND_TO_CLIENT_OR_FAIL ("<tasks>");
                  init_config_task_iterator
                   (&tasks,
                    config_iterator_config (&configs),
                    0);
                  while (next (&tasks))
                    SENDF_TO_CLIENT_OR_FAIL
                     ("<task id=\"%s\">"
                      "<name>%s</name>"
                      "</task>",
                      config_task_iterator_uuid (&tasks),
                      config_task_iterator_name (&tasks));
                  cleanup_iterator (&tasks);
                  SEND_TO_CLIENT_OR_FAIL ("</tasks>");
                }

              SEND_TO_CLIENT_OR_FAIL ("</config>");
              count++;
            }
          cleanup_iterator (&configs);
          filtered = get_configs_data->get.id
                      ? 1
                      : config_count (&get_configs_data->get);
          SEND_GET_END ("config", &get_configs_data->get, count, filtered);

          get_configs_data_reset (get_configs_data);
          set_client_state (CLIENT_AUTHENTIC);
          break;
        }

      case CLIENT_GET_ALERTS:
        {
          iterator_t alerts;
          int count, filtered, ret, first;
          get_data_t * get;

          assert (strcasecmp ("GET_ALERTS", element_name) == 0);

          get = &get_alerts_data->get;
          if ((!get->filter && !get->filt_id)
              || (get->filt_id && strcmp (get->filt_id, "-2") == 0))
            {
              char *user_filter = setting_filter ("Alerts");

              if (user_filter && strlen (user_filter))
                {
                  get->filt_id = user_filter;
                  get->filter = filter_term (user_filter);
                }
              else
                get->filt_id = g_strdup("0");
            }

          ret = init_alert_iterator (&alerts, &get_alerts_data->get);
          if (ret)
            {
              switch (ret)
                {
                  case 1:
                    if (send_find_error_to_client ("get_alerts",
                                                   "alert",
                                                   get_alerts_data->get.id,
                                                   write_to_client,
                                                   write_to_client_data))
                      {
                        error_send_to_client (error);
                        return;
                      }
                    break;
                  case 2:
                    if (send_find_error_to_client
                         ("get_alerts",
                          "alert",
                          get_alerts_data->get.filt_id,
                          write_to_client,
                          write_to_client_data))
                      {
                        error_send_to_client (error);
                        return;
                      }
                    break;
                  case -1:
                    SEND_TO_CLIENT_OR_FAIL
                     (XML_INTERNAL_ERROR ("get_alerts"));
                    break;
                }
              get_alerts_data_reset (get_alerts_data);
              set_client_state (CLIENT_AUTHENTIC);
              break;
            }

          count = 0;
          manage_filter_controls (get->filter, &first, NULL, NULL, NULL);
          SEND_GET_START ("alert", &get_alerts_data->get);
          while (1)
            {
              iterator_t data;
              char *filter_uuid;

              ret = get_next (&alerts, &get_alerts_data->get, &first,
                              &count, init_alert_iterator);
              if (ret == 1)
                break;
              if (ret == -1)
                {
                  internal_error_send_to_client (error);
                  return;
                }
              SEND_GET_COMMON (alert, &get_alerts_data->get,
                               &alerts);

              /* Filter. */

              filter_uuid = alert_iterator_filter_uuid (&alerts);
              if (filter_uuid)
                SENDF_TO_CLIENT_OR_FAIL ("<filter id=\"%s\">"
                                         "<name>%s</name>"
                                         "<trash>%i</trash>"
                                         "</filter>",
                                         filter_uuid,
                                         alert_iterator_filter_name (&alerts),
                                         alert_iterator_filter_trash (&alerts));

              /* Condition. */

              SENDF_TO_CLIENT_OR_FAIL ("<condition>%s",
                                       alert_condition_name
                                        (alert_iterator_condition
                                          (&alerts)));
              init_alert_data_iterator (&data,
                                        alert_iterator_alert
                                         (&alerts),
                                        get_alerts_data->get.trash,
                                        "condition");
              while (next (&data))
                SENDF_TO_CLIENT_OR_FAIL ("<data>"
                                         "<name>%s</name>"
                                         "%s"
                                         "</data>",
                                         alert_data_iterator_name (&data),
                                         alert_data_iterator_data (&data));
              cleanup_iterator (&data);

              SEND_TO_CLIENT_OR_FAIL ("</condition>");

              /* Event. */

              SENDF_TO_CLIENT_OR_FAIL ("<event>%s",
                                       event_name (alert_iterator_event
                                        (&alerts)));
              init_alert_data_iterator (&data,
                                        alert_iterator_alert
                                         (&alerts),
                                        get_alerts_data->get.trash,
                                        "event");
              while (next (&data))
                SENDF_TO_CLIENT_OR_FAIL ("<data>"
                                         "<name>%s</name>"
                                         "%s"
                                         "</data>",
                                         alert_data_iterator_name (&data),
                                         alert_data_iterator_data (&data));
              cleanup_iterator (&data);
              SEND_TO_CLIENT_OR_FAIL ("</event>");

              /* Method. */

              SENDF_TO_CLIENT_OR_FAIL ("<method>%s",
                                       alert_method_name
                                        (alert_iterator_method
                                          (&alerts)));
              init_alert_data_iterator (&data,
                                        alert_iterator_alert
                                         (&alerts),
                                        get_alerts_data->get.trash,
                                        "method");
              while (next (&data))
                SENDF_TO_CLIENT_OR_FAIL ("<data>"
                                         "<name>%s</name>"
                                         "%s"
                                         "</data>",
                                         alert_data_iterator_name (&data),
                                         alert_data_iterator_data (&data));
              cleanup_iterator (&data);
              SEND_TO_CLIENT_OR_FAIL ("</method>");

              if (get_alerts_data->tasks)
                {
                  iterator_t tasks;

                  SEND_TO_CLIENT_OR_FAIL ("<tasks>");
                  init_alert_task_iterator (&tasks,
                                            alert_iterator_alert
                                             (&alerts),
                                            0);
                  while (next (&tasks))
                    SENDF_TO_CLIENT_OR_FAIL
                     ("<task id=\"%s\">"
                      "<name>%s</name>"
                      "</task>",
                      alert_task_iterator_uuid (&tasks),
                      alert_task_iterator_name (&tasks));
                  cleanup_iterator (&tasks);
                  SEND_TO_CLIENT_OR_FAIL ("</tasks>");
                }

              SEND_TO_CLIENT_OR_FAIL ("</alert>");
              count++;
            }
          cleanup_iterator (&alerts);
          filtered = get_alerts_data->get.id
                      ? 1
                      : alert_count (&get_alerts_data->get);
          SEND_GET_END ("alert", &get_alerts_data->get, count, filtered);

          get_alerts_data_reset (get_alerts_data);
          set_client_state (CLIENT_AUTHENTIC);
          break;
        }

      case CLIENT_GET_FILTERS:
        {
          iterator_t filters;
          int count, filtered, ret, first;
          get_data_t * get;

          assert (strcasecmp ("GET_FILTERS", element_name) == 0);

          get = &get_filters_data->get;
          if ((!get->filter && !get->filt_id)
              || (get->filt_id && strcmp (get->filt_id, "-2") == 0))
            {
              char *user_filter = setting_filter ("Filters");

              if (user_filter && strlen (user_filter))
                {
                  get->filt_id = user_filter;
                  get->filter = filter_term (user_filter);
                }
              else
                get->filt_id = g_strdup("0");
            }

          ret = init_filter_iterator (&filters, &get_filters_data->get);
          if (ret)
            {
              switch (ret)
                {
                  case 1:
                    if (send_find_error_to_client ("get_filters",
                                                   "filter",
                                                   get_filters_data->get.id,
                                                   write_to_client,
                                                   write_to_client_data))
                      {
                        error_send_to_client (error);
                        return;
                      }
                    break;
                  case 2:
                    if (send_find_error_to_client
                         ("get_filters",
                          "filter",
                          get_filters_data->get.filt_id,
                          write_to_client,
                          write_to_client_data))
                      {
                        error_send_to_client (error);
                        return;
                      }
                    break;
                  case -1:
                    SEND_TO_CLIENT_OR_FAIL
                     (XML_INTERNAL_ERROR ("get_filters"));
                    break;
                }
              get_filters_data_reset (get_filters_data);
              set_client_state (CLIENT_AUTHENTIC);
              break;
            }

          count = 0;
          manage_filter_controls (get->filter, &first, NULL, NULL, NULL);
          SEND_GET_START ("filter", &get_filters_data->get);
          while (1)
            {
              ret = get_next (&filters, get, &first, &count,
                              init_filter_iterator);
              if (ret == 1)
                break;
              if (ret == -1)
                {
                  internal_error_send_to_client (error);
                  return;
                }

              SEND_GET_COMMON (filter, &get_filters_data->get, &filters);

              SENDF_TO_CLIENT_OR_FAIL ("<type>%s</type>"
                                       "<term>%s</term>",
                                       filter_iterator_type (&filters),
                                       filter_iterator_term (&filters));

              if (get_filters_data->alerts)
                {
                  iterator_t alerts;

                  SEND_TO_CLIENT_OR_FAIL ("<alerts>");
                  init_filter_alert_iterator (&alerts,
                                              get_iterator_resource
                                               (&filters));
                  while (next (&alerts))
                    SENDF_TO_CLIENT_OR_FAIL
                     ("<alert id=\"%s\">"
                      "<name>%s</name>"
                      "</alert>",
                      filter_alert_iterator_uuid (&alerts),
                      filter_alert_iterator_name (&alerts));
                  cleanup_iterator (&alerts);
                  SEND_TO_CLIENT_OR_FAIL ("</alerts>");
                }

              SEND_TO_CLIENT_OR_FAIL ("</filter>");

              count++;
            }
          cleanup_iterator (&filters);
          filtered = get_filters_data->get.id
                      ? 1
                      : filter_count (&get_filters_data->get);
          SEND_GET_END ("filter", &get_filters_data->get, count, filtered);

          get_filters_data_reset (get_filters_data);
          set_client_state (CLIENT_AUTHENTIC);
          break;
        }

      case CLIENT_GET_INFO:
        {
          iterator_t info;
          int count, first, filtered, ret;
          int (*init_info_iterator) (iterator_t*, get_data_t *, const char *);
          int (*info_count) (const get_data_t *get);
          get_data_t *get;

          if (manage_scap_loaded () == 0)
            {
              SEND_TO_CLIENT_OR_FAIL
               (XML_ERROR_SYNTAX ("get_info",
                                  "GET_INFO requires the SCAP database."));
              get_info_data_reset (get_info_data);
              set_client_state (CLIENT_AUTHENTIC);
              break;
            }
          if (manage_cert_loaded () == 0)
            {
              SEND_TO_CLIENT_OR_FAIL
               (XML_ERROR_SYNTAX ("get_info",
                                  "GET_INFO requires the CERT database."));
              get_info_data_reset (get_info_data);
              set_client_state (CLIENT_AUTHENTIC);
              break;
            }

          if (get_info_data->name && get_info_data->get.id)
            {
              SEND_TO_CLIENT_OR_FAIL
                (XML_ERROR_SYNTAX ("get_info",
                                   "Only one of name and the id attribute"
                                   " may be given."));
              get_info_data_reset (get_info_data);
              set_client_state (CLIENT_AUTHENTIC);
              break;
            }
          if (get_info_data->type == NULL)
            {
              SEND_TO_CLIENT_OR_FAIL
                (XML_ERROR_SYNTAX ("get_info",
                                   "No type specified."));
              get_info_data_reset (get_info_data);
              set_client_state (CLIENT_AUTHENTIC);
              break;
            }

          /* Set type specific functions */
          if (g_strcmp0 ("cpe", get_info_data->type) == 0)
            {
              init_info_iterator = init_cpe_info_iterator;
              info_count = cpe_info_count;
              get_info_data->get.subtype = g_strdup ("cpe");
            }
          else if (g_strcmp0 ("cve", get_info_data->type) == 0)
            {
              init_info_iterator = init_cve_info_iterator;
              info_count = cve_info_count;
              get_info_data->get.subtype = g_strdup ("cve");
            }
          else if ((g_strcmp0 ("nvt", get_info_data->type) == 0)
                   && (get_info_data->name == NULL))
            {
              init_info_iterator = init_nvt_info_iterator;
              info_count = nvt_info_count;
              get_info_data->get.subtype = g_strdup ("nvt");
            }
          else if (g_strcmp0 ("nvt", get_info_data->type) == 0)
            {
              gchar *result;

              get_info_data->get.subtype = g_strdup ("nvt");

              manage_read_info (get_info_data->type, get_info_data->name, &result);
              if (result)
                {
                  SEND_GET_START ("info", &get_info_data->get);
                  SEND_TO_CLIENT_OR_FAIL ("<info>");
                  SEND_TO_CLIENT_OR_FAIL (result);
                  SEND_TO_CLIENT_OR_FAIL ("</info>");
                  SEND_TO_CLIENT_OR_FAIL ("<details>1</details>");
                  SEND_GET_END ("info", &get_info_data->get, 1, 1);
                  g_free (result);
                  get_info_data_reset (get_info_data);
                  set_client_state (CLIENT_AUTHENTIC);
                  break;
                }
              else
                {
                  if (send_find_error_to_client ("get_info", "name",
                                                 get_info_data->name,
                                                 write_to_client,
                                                 write_to_client_data))
                    {
                      error_send_to_client (error);
                    }
                  return;
                }
            }
          else if (g_strcmp0 ("ovaldef", get_info_data->type) == 0)
            {
              init_info_iterator = init_ovaldef_info_iterator;
              info_count = ovaldef_info_count;
              get_info_data->get.subtype = g_strdup ("ovaldef");
            }
          else if (g_strcmp0 ("dfn_cert_adv", get_info_data->type) == 0)
            {
              init_info_iterator = init_dfn_cert_adv_info_iterator;
              info_count = dfn_cert_adv_info_count;
              get_info_data->get.subtype = g_strdup ("dfn_cert_adv");
            }
          else
            {
              if (send_find_error_to_client ("get_info",
                                             "type",
                                             get_info_data->type,
                                             write_to_client,
                                             write_to_client_data))
                {
                  error_send_to_client (error);
                }
              return;
            }

          get = &get_info_data->get;
          if ((!get->filter && !get->filt_id)
              || (get->filt_id && strcmp (get->filt_id, "-2") == 0))
            {
              char *user_filter;
              gchar *name;

              if (strcmp (get_info_data->type, "cpe") == 0)
                name = g_strdup ("CPE");
              else if (strcmp (get_info_data->type, "cve") == 0)
                name = g_strdup ("CVE");
              else if (strcmp (get_info_data->type, "ovaldef") == 0)
                name = g_strdup ("OVAL");
              else if (strcmp (get_info_data->type, "dfn_cert_adv") == 0)
                name = g_strdup ("DFN-CERT");
              else if (strcmp (get_info_data->type, "nvt") == 0)
                name = g_strdup ("NVT");
              else
                {
                  if (send_find_error_to_client ("get_info",
                                                 "type",
                                                 get_info_data->type,
                                                 write_to_client,
                                                 write_to_client_data))
                    {
                      error_send_to_client (error);
                    }
                  return;
                }

              user_filter = setting_filter (name);
              g_free (name);

              if (user_filter && strlen (user_filter))
                {
                  get->filt_id = user_filter;
                  get->filter = filter_term (user_filter);
                }
              else
                get->filt_id = g_strdup("0");
            }

          ret = init_info_iterator (&info, &get_info_data->get, get_info_data->name);
          if (ret)
            {
              switch (ret)
                {
                case 1:
                  if (send_find_error_to_client ("get_info",
                                                 "type",
                                                 get_info_data->type,
                                                 write_to_client,
                                                 write_to_client_data))
                    {
                      error_send_to_client (error);
                      return;
                    }
                  break;
                case 2:
                  if (send_find_error_to_client
                      ("get_info",
                       "filter",
                       get_info_data->get.filt_id,
                       write_to_client,
                       write_to_client_data))
                    {
                      error_send_to_client (error);
                      return;
                    }
                  break;
                case -1:
                  SEND_TO_CLIENT_OR_FAIL
                    (XML_INTERNAL_ERROR ("get_info"));
                  break;
                }
              get_info_data_reset (get_info_data);
              set_client_state (CLIENT_AUTHENTIC);
              return;
            }

          count = 0;
          manage_filter_controls (get_info_data->get.filter, &first, NULL, NULL, NULL);
          SEND_GET_START ("info", &get_info_data->get);
          while (next (&info))
            {
              GString *result;

              /* Info's are currently always read only */
              if (send_get_common ("info", &get_info_data->get, &info,
                               write_to_client, write_to_client_data, 0, 0))
                {
                  error_send_to_client (error);
                  return;
                }

              SENDF_TO_CLIENT_OR_FAIL ("<update_time>%s</update_time>",
                                       manage_scap_update_time ());

              result = g_string_new ("");

              /* Information depending on type */

              if (g_strcmp0 ("cpe", get_info_data->type) == 0)
                {
                  const char *title;

                  xml_string_append (result, "<cpe>");
                  title = cpe_info_iterator_title (&info);
                  if (title)
                    xml_string_append (result,
                                       "<title>%s</title>",
                                       cpe_info_iterator_title (&info));
                  xml_string_append (result,
                                     "<max_cvss>%s</max_cvss>"
                                     "<cve_refs>%s</cve_refs>"
                                     "<status>%s</status>",
                                     cpe_info_iterator_max_cvss (&info),
                                     cpe_info_iterator_cve_refs (&info),
                                     cpe_info_iterator_status (&info) ?
                                     cpe_info_iterator_status (&info) : "");

                  if (get_info_data->details == 1)
                    {
                      iterator_t cves;
                      g_string_append (result, "<cves>");
                      init_cpe_cve_iterator (&cves, get_iterator_name (&info), 0, NULL);
                      while (next (&cves))
                        xml_string_append (result,
                                           "<cve>"
                                           "<entry"
                                           " xmlns:cpe-lang=\"http://cpe.mitre.org/language/2.0\""
                                           " xmlns:vuln=\"http://scap.nist.gov/schema/vulnerability/0.4\""
                                           " xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\""
                                           " xmlns:patch=\"http://scap.nist.gov/schema/patch/0.1\""
                                           " xmlns:scap-core=\"http://scap.nist.gov/schema/scap-core/0.1\""
                                           " xmlns:cvss=\"http://scap.nist.gov/schema/cvss-v2/0.2\""
                                           " xmlns=\"http://scap.nist.gov/schema/feed/vulnerability/2.0\""
                                           " id=\"%s\">"
                                           "<vuln:cvss>"
                                           "<cvss:base_metrics>"
                                           "<cvss:score>%s</cvss:score>"
                                           "</cvss:base_metrics>"
                                           "</vuln:cvss>"
                                           "</entry>"
                                           "</cve>",
                                           cve_iterator_name (&cves),
                                           cve_iterator_cvss (&cves));
                      cleanup_iterator (&cves);
                      g_string_append (result, "</cves>");
                    }
                }
              else if (g_strcmp0 ("cve", get_info_data->type) == 0)
                {
                  xml_string_append (result,
                                     "<cve>"
                                     "<cvss>%s</cvss>"
                                     "<vector>%s</vector>"
                                     "<complexity>%s</complexity>"
                                     "<authentication>%s</authentication>"
                                     "<confidentiality_impact>%s</confidentiality_impact>"
                                     "<integrity_impact>%s</integrity_impact>"
                                     "<availability_impact>%s</availability_impact>"
                                     "<description>%s</description>"
                                     "<products>%s</products>",
                                     cve_info_iterator_cvss (&info),
                                     cve_info_iterator_vector (&info),
                                     cve_info_iterator_complexity (&info),
                                     cve_info_iterator_authentication (&info),
                                     cve_info_iterator_confidentiality_impact (&info),
                                     cve_info_iterator_integrity_impact (&info),
                                     cve_info_iterator_availability_impact (&info),
                                     cve_info_iterator_description (&info),
                                     cve_info_iterator_products (&info));
                  if (get_info_data->details == 1)
                    {
                      iterator_t nvts;
                      iterator_t cert_advs;
                      init_cve_nvt_iterator (&nvts,  get_iterator_name (&info), 1, NULL);
                      g_string_append (result, "<nvts>");
                      while (next (&nvts))
                        xml_string_append (result,
                                           "<nvt oid=\"%s\">"
                                           "<name>%s</name>"
                                           "</nvt>",
                                           nvt_iterator_oid (&nvts),
                                           nvt_iterator_name (&nvts));
                      g_string_append (result, "</nvts>");
                      cleanup_iterator (&nvts);

                      g_string_append (result, "<cert>");
                      if (manage_cert_loaded())
                        {
                          init_cve_dfn_cert_adv_iterator (&cert_advs,
                                                          get_iterator_name
                                                            (&info),
                                                          1, NULL);
                          while (next (&cert_advs))
                            {
                              xml_string_append (result,
                                                "<cert_ref type=\"DFN-CERT\">"
                                                "<name>%s</name>"
                                                "<title>%s</title>"
                                                "</cert_ref>",
                                                get_iterator_name (&cert_advs),
                                                dfn_cert_adv_info_iterator_title
                                                  (&cert_advs));
                          };
                          cleanup_iterator (&cert_advs);
                        }
                      else
                        {
                          g_string_append(result, "<warning>"
                                                  "database not available"
                                                  "</warning>");
                        }
                      g_string_append (result, "</cert>");
                    }
                }
              else if (g_strcmp0 ("ovaldef", get_info_data->type) == 0)
                {
                  const char *description;
                  xml_string_append (result,
                                     "<ovaldef>"
                                     "<version>%s</version>"
                                     "<deprecated>%s</deprecated>"
                                     "<status>%s</status>"
                                     "<def_class>%s</def_class>"
                                     "<title>%s</title>",
                                     ovaldef_info_iterator_version (&info),
                                     ovaldef_info_iterator_deprecated (&info),
                                     ovaldef_info_iterator_status (&info),
                                     ovaldef_info_iterator_def_class (&info),
                                     ovaldef_info_iterator_title (&info));
                  description = ovaldef_info_iterator_description (&info);
                  if (get_info_data->details == 1)
                    xml_string_append (result,
                                       "<description>%s</description>"
                                       "<xml_file>%s</xml_file>",
                                       description,
                                       ovaldef_info_iterator_xml_file (&info));
                }
              else if (g_strcmp0 ("dfn_cert_adv", get_info_data->type) == 0)
                {
                  xml_string_append (result,
                                     "<dfn_cert_adv>"
                                     "<title>%s</title>"
                                     "<summary>%s</summary>"
                                     "<cve_refs>%s</cve_refs>",
                                     dfn_cert_adv_info_iterator_title (&info),
                                     dfn_cert_adv_info_iterator_summary (&info),
                                     dfn_cert_adv_info_iterator_cve_refs (&info)
                                    );
                }
              else if (g_strcmp0 ("nvt", get_info_data->type) == 0)
                {
                  if (send_nvt (&info, 1, -1, NULL, write_to_client,
                                write_to_client_data))
                    {
                      cleanup_iterator (&info);
                      error_send_to_client (error);
                      return;
                    }
                }

              /* Append raw data if full details are requested */

              if (get_info_data->details == 1)
                {
                  gchar *raw_data = NULL;
                  gchar *nonconst_name = g_strdup(get_iterator_name (&info));
                  manage_read_info (get_info_data->type, nonconst_name, &raw_data);
                  g_string_append_printf (result, "<raw_data>%s</raw_data>",
                                          raw_data);
                  g_free(nonconst_name);
                  g_free(raw_data);
                }

              g_string_append_printf (result, "</%s></info>", get_info_data->type);
              SEND_TO_CLIENT_OR_FAIL (result->str);
              count++;
              g_string_free (result, TRUE);
            }
          cleanup_iterator (&info);

          if (get_info_data->details == 1)
            SEND_TO_CLIENT_OR_FAIL ("<details>1</details>");

          filtered = get_info_data->get.id || get_info_data->name
                     ? 1
                     : info_count (&get_info_data->get);
          SEND_GET_END ("info", &get_info_data->get, count, filtered);

          get_info_data_reset (get_info_data);
          set_client_state (CLIENT_AUTHENTIC);
          break;
        }

      case CLIENT_GET_LSC_CREDENTIALS:
        {
          iterator_t credentials;
          int count, filtered, ret, first;
          int format;
          get_data_t* get;
          char *data_format;

          assert (strcasecmp ("GET_LSC_CREDENTIALS", element_name) == 0);

          data_format = get_lsc_credentials_data->format;
          if (data_format)
            {
              if (strlen (data_format))
                {
                  if (strcasecmp (data_format, "key") == 0)
                    format = 1;
                  else if (strcasecmp (data_format, "rpm") == 0)
                    format = 2;
                  else if (strcasecmp (data_format, "deb") == 0)
                    format = 3;
                  else if (strcasecmp (data_format, "exe") == 0)
                    format = 4;
                  else
                    format = -1;
                }
              else
                format = 0;
            }
          else
            format = 0;

          if (format == -1)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("get_lsc_credentials",
                                "GET_LSC_CREDENTIALS format attribute should"
                                " be 'key', 'rpm', 'deb' or 'exe'."));

          get = &get_lsc_credentials_data->get;
          if ((!get->filter && !get->filt_id)
              || (get->filt_id && strcmp (get->filt_id, "-2") == 0))
            {
              char *user_filter = setting_filter ("Credentials");

              if (user_filter && strlen (user_filter))
                {
                  get->filt_id = user_filter;
                  get->filter = filter_term (user_filter);
                }
              else
                get->filt_id = g_strdup("0");
            }

          ret = init_lsc_credential_iterator (&credentials, get);
          if (ret)
            {
              switch (ret)
                {
                  case 1:
                    if (send_find_error_to_client ("get_lsc_credentials",
                                                   "lsc_credential",
                                                   get_lsc_credentials_data->get.id,
                                                   write_to_client,
                                                   write_to_client_data))
                      {
                        error_send_to_client (error);
                        return;
                      }
                    break;
                  case 2:
                    if (send_find_error_to_client
                         ("get_lsc_credentials",
                          "lsc_credential",
                          get_lsc_credentials_data->get.filt_id,
                          write_to_client,
                          write_to_client_data))
                      {
                        error_send_to_client (error);
                        return;
                      }
                    break;
                  case -1:
                    SEND_TO_CLIENT_OR_FAIL
                     (XML_INTERNAL_ERROR ("get_lsc_credentials"));
                    break;
                }
              get_lsc_credentials_data_reset (get_lsc_credentials_data);
              set_client_state (CLIENT_AUTHENTIC);
              break;
            }
          count = 0;
          get = &get_lsc_credentials_data->get;
          manage_filter_controls (get->filter, &first, NULL, NULL, NULL);
          SEND_GET_START("lsc_credential", &get_lsc_credentials_data->get);

          while (1)
            {
              const char* public_key;

              ret = get_next (&credentials, &get_lsc_credentials_data->get,
                              &first, &count, init_lsc_credential_iterator);
              if (ret == 1)
                break;
              if (ret == -1)
                {
                  internal_error_send_to_client (error);
                  return;
                }

              SEND_GET_COMMON (lsc_credential, &get_lsc_credentials_data->get,
                               &credentials);
              public_key = lsc_credential_iterator_public_key (&credentials);
              SENDF_TO_CLIENT_OR_FAIL
               ("<login>%s</login>"
                "<type>%s</type>",
                lsc_credential_iterator_login (&credentials),
                public_key ? "gen" : "pass");

              switch (format)
                {
                  case 1: /* key */
                    SENDF_TO_CLIENT_OR_FAIL
                     ("<public_key>%s</public_key>",
                      public_key ? public_key
                                 : "");
                    break;
                  case 2: /* rpm */
                    SENDF_TO_CLIENT_OR_FAIL
                     ("<package format=\"rpm\">%s</package>",
                      lsc_credential_iterator_rpm (&credentials)
                        ? lsc_credential_iterator_rpm (&credentials)
                        : "");
                    break;
                  case 3: /* deb */
                    SENDF_TO_CLIENT_OR_FAIL
                     ("<package format=\"deb\">%s</package>",
                      lsc_credential_iterator_deb (&credentials)
                        ? lsc_credential_iterator_deb (&credentials)
                        : "");
                    break;
                  case 4: /* exe */
                    SENDF_TO_CLIENT_OR_FAIL
                     ("<package format=\"exe\">%s</package>",
                      lsc_credential_iterator_exe (&credentials)
                        ? lsc_credential_iterator_exe (&credentials)
                        : "");
                    break;
                }

              if (get_lsc_credentials_data->targets)
                {
                  iterator_t targets;

                  SENDF_TO_CLIENT_OR_FAIL ("<targets>");
                  init_lsc_credential_target_iterator
                   (&targets,
                    lsc_credential_iterator_lsc_credential
                     (&credentials),
                    0);
                  while (next (&targets))
                    SENDF_TO_CLIENT_OR_FAIL
                     ("<target id=\"%s\">"
                      "<name>%s</name>"
                      "</target>",
                      lsc_credential_target_iterator_uuid (&targets),
                      lsc_credential_target_iterator_name (&targets));
                  cleanup_iterator (&targets);

                  SEND_TO_CLIENT_OR_FAIL ("</targets>");
                }

              SEND_TO_CLIENT_OR_FAIL ("</lsc_credential>");
              count++;
            }

          cleanup_iterator (&credentials);
          filtered = get_lsc_credentials_data->get.id
                      ? 1
                      : lsc_credential_count (&get_lsc_credentials_data->get);
          SEND_GET_END ("lsc_credential", &get_lsc_credentials_data->get,
                        count, filtered);
          get_lsc_credentials_data_reset (get_lsc_credentials_data);
          set_client_state (CLIENT_AUTHENTIC);
          break;
        }

      case CLIENT_GET_SETTINGS:
        {
          setting_t setting = 0;
          iterator_t settings;
          int count, filtered;

          assert (strcasecmp ("GET_SETTINGS", element_name) == 0);

          init_setting_iterator (&settings,
                                 get_settings_data->setting_id,
                                 get_settings_data->filter,
                                 get_settings_data->first,
                                 get_settings_data->max,
                                 get_settings_data->sort_order,
                                 get_settings_data->sort_field);

          SEND_TO_CLIENT_OR_FAIL ("<get_settings_response"
                                  " status=\"" STATUS_OK "\""
                                  " status_text=\"" STATUS_OK_TEXT "\">");
          SENDF_TO_CLIENT_OR_FAIL ("<filters>"
                                   "<term>%s</term>"
                                   "</filters>"
                                   "<settings start=\"%i\" max=\"%i\"/>",
                                   get_settings_data->filter
                                    ? get_settings_data->filter
                                    : "",
                                   /* Add 1 for 1 indexing. */
                                   get_settings_data->first + 1,
                                   get_settings_data->max);
          count = 0;
          while (next (&settings))
            {
              SENDF_TO_CLIENT_OR_FAIL ("<setting id=\"%s\">"
                                       "<name>%s</name>"
                                       "<comment>%s</comment>"
                                       "<value>%s</value>"
                                       "</setting>",
                                       setting_iterator_uuid (&settings),
                                       setting_iterator_name (&settings),
                                       setting_iterator_comment (&settings),
                                       setting_iterator_value (&settings));
              count++;
            }
          filtered = setting
                      ? 1
                      : setting_count (get_settings_data->filter);
          SENDF_TO_CLIENT_OR_FAIL ("<setting_count>"
                                   "<filtered>%i</filtered>"
                                   "<page>%i</page>"
                                   "</setting_count>",
                                   filtered,
                                   count);
          cleanup_iterator (&settings);
          SEND_TO_CLIENT_OR_FAIL ("</get_settings_response>");

          get_settings_data_reset (get_settings_data);
          set_client_state (CLIENT_AUTHENTIC);
          break;
        }

      case CLIENT_GET_SLAVES:
        {

          assert (strcasecmp ("GET_SLAVES", element_name) == 0);

          if (get_slaves_data->tasks && get_slaves_data->get.trash)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("get_slaves",
                                "GET_SLAVES tasks given with trash"));
          else
            {
              iterator_t slaves;
              int count, filtered, ret, first;
              get_data_t * get;

              get = &get_slaves_data->get;
              if ((!get->filter && !get->filt_id)
                  || (get->filt_id && strcmp (get->filt_id, "-2") == 0))
                {
                  char *user_filter = setting_filter ("Slaves");

                  if (user_filter && strlen (user_filter))
                    {
                      get->filt_id = user_filter;
                      get->filter = filter_term (user_filter);
                    }
                  else
                    get->filt_id = g_strdup("0");
                }

              ret = init_slave_iterator (&slaves, &get_slaves_data->get);
              if (ret)
                {
                  switch (ret)
                    {
                      case 1:
                        if (send_find_error_to_client ("get_slaves",
                                                       "slave",
                                                       get_slaves_data->get.id,
                                                       write_to_client,
                                                       write_to_client_data))
                          {
                            error_send_to_client (error);
                            return;
                          }
                        break;
                      case 2:
                        if (send_find_error_to_client
                             ("get_slaves",
                              "filter",
                              get_slaves_data->get.filt_id,
                              write_to_client,
                              write_to_client_data))
                          {
                            error_send_to_client (error);
                            return;
                          }
                        break;
                      case -1:
                        SEND_TO_CLIENT_OR_FAIL
                         (XML_INTERNAL_ERROR ("get_slaves"));
                        break;
                    }
                  get_slaves_data_reset (get_slaves_data);
                  set_client_state (CLIENT_AUTHENTIC);
                  break;
                }

              count = 0;
              manage_filter_controls (get->filter, &first, NULL, NULL, NULL);
              SEND_GET_START ("slave", &get_slaves_data->get);
              while (1)
                {

                  ret = get_next (&slaves, get, &first, &count,
                                  init_slave_iterator);
                  if (ret == 1)
                    break;
                  if (ret == -1)
                    {
                      internal_error_send_to_client (error);
                      return;
                    }

                  SEND_GET_COMMON (slave, &get_slaves_data->get, &slaves);

                  SENDF_TO_CLIENT_OR_FAIL ("<host>%s</host>"
                                           "<port>%s</port>"
                                           "<login>%s</login>",
                                           slave_iterator_host (&slaves),
                                           slave_iterator_port (&slaves),
                                           slave_iterator_login (&slaves));

                  if (get_slaves_data->tasks)
                    {
                      iterator_t tasks;

                      SEND_TO_CLIENT_OR_FAIL ("<tasks>");
                      init_slave_task_iterator (&tasks,
                                                slave_iterator_slave
                                                 (&slaves));
                      while (next (&tasks))
                        SENDF_TO_CLIENT_OR_FAIL ("<task id=\"%s\">"
                                                 "<name>%s</name>"
                                                 "</task>",
                                                 slave_task_iterator_uuid (&tasks),
                                                 slave_task_iterator_name (&tasks));
                      cleanup_iterator (&tasks);
                      SEND_TO_CLIENT_OR_FAIL ("</tasks>");
                    }

                  SEND_TO_CLIENT_OR_FAIL ("</slave>");
                  count++;
                }
              cleanup_iterator (&slaves);
              filtered = get_slaves_data->get.id
                          ? 1
                          : slave_count (&get_slaves_data->get);
              SEND_GET_END ("slave", &get_slaves_data->get, count, filtered);
            }
          get_slaves_data_reset (get_slaves_data);
          set_client_state (CLIENT_AUTHENTIC);
          break;
        }

      case CLIENT_GET_SYSTEM_REPORTS:
        {
          int ret;
          report_type_iterator_t types;

          assert (strcasecmp ("GET_SYSTEM_REPORTS", element_name) == 0);

          ret = init_system_report_type_iterator
                 (&types,
                  get_system_reports_data->name,
                  get_system_reports_data->slave_id);
          switch (ret)
            {
              case 1:
                if (send_find_error_to_client ("get_system_reports",
                                               "system report",
                                               get_system_reports_data->name,
                                               write_to_client,
                                               write_to_client_data))
                  {
                    error_send_to_client (error);
                    return;
                  }
                break;
              case 2:
                if (send_find_error_to_client
                     ("get_system_reports",
                      "slave",
                      get_system_reports_data->slave_id,
                      write_to_client,
                      write_to_client_data))
                  {
                    error_send_to_client (error);
                    return;
                  }
                break;
              default:
                assert (0);
                /*@fallthrough@*/
              case -1:
                SEND_TO_CLIENT_OR_FAIL
                 (XML_INTERNAL_ERROR ("get_system_reports"));
                break;
              case 0:
              case 3:
                {
                  int report_ret;
                  char *report;
                  SEND_TO_CLIENT_OR_FAIL ("<get_system_reports_response"
                                          " status=\"" STATUS_OK "\""
                                          " status_text=\""
                                          STATUS_OK_TEXT
                                          "\">");
                  while (next_report_type (&types))
                    if (get_system_reports_data->brief
                        && (ret != 3))
                      SENDF_TO_CLIENT_OR_FAIL
                       ("<system_report>"
                        "<name>%s</name>"
                        "<title>%s</title>"
                        "</system_report>",
                        report_type_iterator_name (&types),
                        report_type_iterator_title (&types));
                    else if ((report_ret = manage_system_report
                                            (report_type_iterator_name (&types),
                                             get_system_reports_data->duration,
                                             get_system_reports_data->slave_id,
                                             &report))
                             && (report_ret != 3))
                      {
                        cleanup_report_type_iterator (&types);
                        internal_error_send_to_client (error);
                        return;
                      }
                    else if (report)
                      {
                        SENDF_TO_CLIENT_OR_FAIL
                         ("<system_report>"
                          "<name>%s</name>"
                          "<title>%s</title>"
                          "<report format=\"%s\" duration=\"%s\">"
                          "%s"
                          "</report>"
                          "</system_report>",
                          report_type_iterator_name (&types),
                          report_type_iterator_title (&types),
                          (ret == 3 ? "txt" : "png"),
                          get_system_reports_data->duration
                           ? get_system_reports_data->duration
                           : "86400",
                          report);
                        free (report);
                      }
                  cleanup_report_type_iterator (&types);
                  SEND_TO_CLIENT_OR_FAIL ("</get_system_reports_response>");
                }
            }

          get_system_reports_data_reset (get_system_reports_data);
          set_client_state (CLIENT_AUTHENTIC);
          break;
        }

      case CLIENT_GET_TARGETS:
        {
          assert (strcasecmp ("GET_TARGETS", element_name) == 0);

          if (get_targets_data->tasks && get_targets_data->get.trash)
            SEND_TO_CLIENT_OR_FAIL
             (XML_ERROR_SYNTAX ("get_target",
                                "GET_TARGETS tasks given with trash"));
          else
            {
              iterator_t targets;
              int count, filtered, ret, first;
              get_data_t * get;

              get = &get_targets_data->get;

              /* If no filter applied by the user , set the default one from
               * settings. */
              if ((!get->filter && !get->filt_id)
                  || (get->filt_id && strcmp (get->filt_id, "-2") == 0))
                {
                  char *user_filter = setting_filter ("Targets");

                  if (user_filter && strlen (user_filter))
                    {
                      get->filt_id = user_filter;
                      get->filter = filter_term (user_filter);
                    }
                  else
                    get->filt_id = g_strdup("0");
                }

              ret = init_target_iterator (&targets, &get_targets_data->get);
              if (ret)
                {
                  switch (ret)
                    {
                      case 1:
                        if (send_find_error_to_client ("get_targets",
                                                       "target",
                                                       get_targets_data->get.id,
                                                       write_to_client,
                                                       write_to_client_data))
                          {
                            error_send_to_client (error);
                            return;
                          }
                        break;
                      case 2:
                        if (send_find_error_to_client
                             ("get_targets",
                              "filter",
                              get_targets_data->get.filt_id,
                              write_to_client,
                              write_to_client_data))
                          {
                            error_send_to_client (error);
                            return;
                          }
                        break;
                      case -1:
                        SEND_TO_CLIENT_OR_FAIL
                         (XML_INTERNAL_ERROR ("get_targets"));
                        break;
                    }
                  get_targets_data_reset (get_targets_data);
                  set_client_state (CLIENT_AUTHENTIC);
                  break;
                }

              count = 0;
              manage_filter_controls (get->filter, &first, NULL, NULL, NULL);
              SEND_GET_START ("target", &get_targets_data->get);
              while (1)
                {
                  char *ssh_lsc_name, *ssh_lsc_uuid, *smb_lsc_name, *smb_lsc_uuid;
                  const char *port_list_uuid, *port_list_name, *ssh_port;
                  lsc_credential_t ssh_credential, smb_credential;
                  int port_list_trash;
                  char *port_range;

                  ret = get_next (&targets, get, &first, &count,
                                  init_target_iterator);
                  if (ret == 1)
                    break;
                  if (ret == -1)
                    {
                      internal_error_send_to_client (error);
                      return;
                    }

                  ssh_credential = target_iterator_ssh_credential (&targets);
                  smb_credential = target_iterator_smb_credential (&targets);
                  if (get_targets_data->get.trash
                      && target_iterator_ssh_trash (&targets))
                    {
                      ssh_lsc_name = trash_lsc_credential_name (ssh_credential);
                      ssh_lsc_uuid = trash_lsc_credential_uuid (ssh_credential);
                    }
                  else
                    {
                      ssh_lsc_name = lsc_credential_name (ssh_credential);
                      ssh_lsc_uuid = lsc_credential_uuid (ssh_credential);
                    }
                  if (get_targets_data->get.trash
                      && target_iterator_smb_trash (&targets))
                    {
                      smb_lsc_name = trash_lsc_credential_name (smb_credential);
                      smb_lsc_uuid = trash_lsc_credential_uuid (smb_credential);
                    }
                  else
                    {
                      smb_lsc_name = lsc_credential_name (smb_credential);
                      smb_lsc_uuid = lsc_credential_uuid (smb_credential);
                    }
                  port_list_uuid = target_iterator_port_list_uuid (&targets);
                  port_list_name = target_iterator_port_list_name (&targets);
                  port_list_trash = target_iterator_port_list_trash (&targets);
                  ssh_port = target_iterator_ssh_port (&targets);
                  port_range = target_port_range (target_iterator_target
                                                    (&targets));

                  SEND_GET_COMMON (target, &get_targets_data->get, &targets);

                  SENDF_TO_CLIENT_OR_FAIL ("<hosts>%s</hosts>"
                                           "<max_hosts>%i</max_hosts>"
                                           "<port_range>%s</port_range>"
                                           "<port_list id=\"%s\">"
                                           "<name>%s</name>"
                                           "<trash>%i</trash>"
                                           "</port_list>"
                                           "<ssh_lsc_credential id=\"%s\">"
                                           "<name>%s</name>"
                                           "<port>%s</port>"
                                           "<trash>%i</trash>"
                                           "</ssh_lsc_credential>"
                                           "<smb_lsc_credential id=\"%s\">"
                                           "<name>%s</name>"
                                           "<trash>%i</trash>"
                                           "</smb_lsc_credential>",
                                           target_iterator_hosts (&targets),
                                           manage_max_hosts
                                            (target_iterator_hosts (&targets)),
                                           port_range,
                                           port_list_uuid ? port_list_uuid : "",
                                           port_list_name ? port_list_name : "",
                                           port_list_trash,
                                           ssh_lsc_uuid ? ssh_lsc_uuid : "",
                                           ssh_lsc_name ? ssh_lsc_name : "",
                                           ssh_port ? ssh_port : "",
                                           (get_targets_data->get.trash
                                             && target_iterator_ssh_trash
                                                 (&targets)),
                                           smb_lsc_uuid ? smb_lsc_uuid : "",
                                           smb_lsc_name ? smb_lsc_name : "",
                                           (get_targets_data->get.trash
                                             && target_iterator_smb_trash
                                                 (&targets)));

                  if (get_targets_data->tasks)
                    {
                      iterator_t tasks;

                      SEND_TO_CLIENT_OR_FAIL ("<tasks>");
                      init_target_task_iterator (&tasks,
                                                 target_iterator_target
                                                  (&targets));
                      while (next (&tasks))
                        SENDF_TO_CLIENT_OR_FAIL ("<task id=\"%s\">"
                                                 "<name>%s</name>"
                                                 "</task>",
                                                 target_task_iterator_uuid (&tasks),
                                                 target_task_iterator_name (&tasks));
                      cleanup_iterator (&tasks);
                      SEND_TO_CLIENT_OR_FAIL ("</tasks>");
                    }

                  SEND_TO_CLIENT_OR_FAIL ("</target>");
                  count++;
                  free (ssh_lsc_name);
                  free (ssh_lsc_uuid);
                  free (smb_lsc_name);
                  free (smb_lsc_uuid);
                  free (port_range);
                }
              cleanup_iterator (&targets);
              filtered = get_targets_data->get.id
                          ? 1
                          : target_count (&get_targets_data->get);
              SEND_GET_END ("target", &get_targets_data->get, count, filtered);
            }
          get_targets_data_reset (get_targets_data);
          set_client_state (CLIENT_AUTHENTIC);
          break;
        }

      case CLIENT_GET_TASKS:
        {
          iterator_t tasks;
          int count, filtered, ret, first, apply_overrides;
          get_data_t * get;
          gchar *overrides, *filter, *clean_filter;

          assert (strcasecmp ("GET_TASKS", element_name) == 0);

          if (get_tasks_data->get.details && get_tasks_data->get.trash)
            {
              SEND_TO_CLIENT_OR_FAIL
               (XML_ERROR_SYNTAX ("get_task",
                                  "GET_TASKS details given with trash"));
              get_tasks_data_reset (get_tasks_data);
              set_client_state (CLIENT_AUTHENTIC);
              break;
            }
          get = &get_tasks_data->get;
          if ((!get->filter && !get->filt_id)
              || (get->filt_id && strcmp (get->filt_id, "-2") == 0))
            {
              char *user_filter = setting_filter ("Tasks");

              if (user_filter && strlen (user_filter))
                {
                  get->filt_id = user_filter;
                  get->filter = filter_term (user_filter);
                }
              else
                get->filt_id = g_strdup("0");
            }

          ret = init_task_iterator (&tasks, &get_tasks_data->get);
          if (ret)
            {
              switch (ret)
                {
                  case 1:
                    if (send_find_error_to_client ("get_tasks",
                                                   "task",
                                                   get_tasks_data->get.id,
                                                   write_to_client,
                                                   write_to_client_data))
                      {
                        error_send_to_client (error);
                        return;
                      }
                    break;
                  case 2:
                    if (send_find_error_to_client
                         ("get_tasks",
                          "task",
                          get_tasks_data->get.filt_id,
                          write_to_client,
                          write_to_client_data))
                      {
                        error_send_to_client (error);
                        return;
                      }
                    break;
                  case -1:
                    SEND_TO_CLIENT_OR_FAIL
                     (XML_INTERNAL_ERROR ("get_tasks"));
                    break;
                }
              get_tasks_data_reset (get_tasks_data);
              set_client_state (CLIENT_AUTHENTIC);
              break;
            }

          count = 0;

          if (get->filt_id && strcmp (get->filt_id, "0"))
            {
              filter = filter_term (get->filt_id);
              if (filter == NULL)
                {
                  error_send_to_client (error);
                  return;
                }
            }
          else
            filter = NULL;

          clean_filter = manage_clean_filter (filter ? filter : get->filter);

          // FIX what about filt_id?
          manage_filter_controls (get->filter, &first, NULL, NULL, NULL);
          SEND_GET_START ("task", &get_tasks_data->get);

          overrides = filter_term_value (clean_filter, "apply_overrides");
          g_free (clean_filter);
          apply_overrides = overrides ? strcmp (overrides, "0") : 0;
          g_free (overrides);
          SENDF_TO_CLIENT_OR_FAIL ("<apply_overrides>%i</apply_overrides>",
                                   apply_overrides);

          while (next (&tasks))
            {
              int maximum_hosts;
              task_t index;
              gchar *progress_xml;
              target_t target;
              slave_t slave;
              char *config, *config_uuid;
              char *task_target_uuid, *task_target_name, *hosts;
              char *task_slave_uuid, *task_slave_name;
              char *task_schedule_uuid, *task_schedule_name;
              gchar *first_report_id, *first_report;
              char *description;
              gchar *description64, *last_report_id, *last_report;
              gchar *second_last_report_id, *second_last_report;
              report_t running_report;
              schedule_t schedule;
              time_t next_time;
              char *owner, *observers;
              int target_in_trash, schedule_in_trash;
              int debugs, holes, infos, logs, warnings;
              int holes_2, infos_2, warnings_2;
              int false_positives;
              gchar *response;
              iterator_t alerts;
              gchar *in_assets, *max_checks, *max_hosts;

              index = get_iterator_resource (&tasks);
              target = task_target (index);
              slave = task_slave (index);

              target_in_trash = task_target_in_trash (index);
              if (target_in_trash)
                hosts = target ? trash_target_hosts (target) : NULL;
              else
                hosts = target ? target_hosts (target) : NULL;
              maximum_hosts = hosts ? manage_max_hosts (hosts) : 0;

              running_report = task_current_report (index);
              if ((target == 0)
                  && (task_run_status (index) == TASK_STATUS_RUNNING))
                progress_xml = g_strdup_printf
                                ("%i",
                                 task_upload_progress (index));
              else if (running_report
                       && report_slave_task_uuid (running_report))
                progress_xml = g_strdup_printf ("%i",
                                                report_slave_progress
                                                 (running_report));
              else if (running_report)
                {
                  long total = 0;
                  int num_hosts = 0, total_progress;
                  iterator_t hosts;
                  GString *string = g_string_new ("");

                  init_host_iterator (&hosts, running_report, NULL, 0);
                  while (next (&hosts))
                    {
                      unsigned int max_port, current_port;
                      long progress;

                      max_port = host_iterator_max_port (&hosts);
                      current_port = host_iterator_current_port (&hosts);
                      if (max_port)
                        {
                          progress = (current_port * 100) / max_port;
                          if (progress < 0) progress = 0;
                          else if (progress > 100) progress = 100;
                        }
                      else
                        progress = current_port ? 100 : 0;

#if 0
                      tracef ("   attack_state: %s\n", host_iterator_attack_state (&hosts));
                      tracef ("   current_port: %u\n", current_port);
                      tracef ("   max_port: %u\n", max_port);
                      tracef ("   progress for %s: %li\n", host_iterator_host (&hosts), progress);
                      tracef ("   total now: %li\n", total);
#endif
                      total += progress;
                      num_hosts++;

                      g_string_append_printf (string,
                                              "<host_progress>"
                                              "<host>%s</host>"
                                              "%li"
                                              "</host_progress>",
                                              host_iterator_host (&hosts),
                                              progress);
                    }
                  cleanup_iterator (&hosts);

                  total_progress = maximum_hosts
                                   ? (total / maximum_hosts) : 0;

#if 1
                  tracef ("   total: %li\n", total);
                  tracef ("   num_hosts: %i\n", num_hosts);
                  tracef ("   maximum_hosts: %i\n", maximum_hosts);
                  tracef ("   total_progress: %i\n", total_progress);
#endif

                  if (total_progress == 0) total_progress = 1;
                  else if (total_progress == 100) total_progress = 99;

                  g_string_append_printf (string,
                                          "%i",
                                          total_progress);
                  progress_xml = g_string_free (string, FALSE);
                }
              else
                progress_xml = g_strdup ("-1");

              if (get_tasks_data->rcfile)
                {
                  description = task_description (index);
                  if (description && strlen (description))
                    {
                      gchar *d64;
                      d64 = g_base64_encode ((guchar*) description,
                                             strlen (description));
                      description64 = g_strdup_printf ("<rcfile>"
                                                       "%s"
                                                       "</rcfile>",
                                                       d64);
                      g_free (d64);
                    }
                  else
                    description64 = g_strdup ("<rcfile></rcfile>");
                  free (description);
                }
              else
                description64 = g_strdup ("");

              first_report_id = task_first_report_id (index);
              if (first_report_id)
                {
                  gchar *timestamp;

                  // TODO Could skip this count for tasks page.
                  if (report_counts (first_report_id,
                                     &debugs, &holes_2, &infos_2, &logs,
                                     &warnings_2, &false_positives,
                                     apply_overrides,
                                     0))
                    /** @todo Either fail better or abort at SQL level. */
                    abort ();

                  if (report_timestamp (first_report_id, &timestamp))
                    /** @todo Either fail better or abort at SQL level. */
                    abort ();

                  first_report = g_strdup_printf ("<first_report>"
                                                  "<report id=\"%s\">"
                                                  "<timestamp>"
                                                  "%s"
                                                  "</timestamp>"
                                                  "<result_count>"
                                                  "<debug>%i</debug>"
                                                  "<hole>%i</hole>"
                                                  "<info>%i</info>"
                                                  "<log>%i</log>"
                                                  "<warning>%i</warning>"
                                                  "<false_positive>"
                                                  "%i"
                                                  "</false_positive>"
                                                  "</result_count>"
                                                  "</report>"
                                                  "</first_report>",
                                                  first_report_id,
                                                  timestamp,
                                                  debugs,
                                                  holes_2,
                                                  infos_2,
                                                  logs,
                                                  warnings_2,
                                                  false_positives);
                  g_free (timestamp);
                }
              else
                first_report = g_strdup ("");

              second_last_report_id = task_second_last_report_id (index);
              if (second_last_report_id)
                {
                  gchar *timestamp;

                  /* If the first report is the second last report then skip
                   * doing the count again. */
                  if (((first_report_id == NULL)
                       || (strcmp (second_last_report_id, first_report_id)))
                      && report_counts (second_last_report_id,
                                        &debugs, &holes_2, &infos_2,
                                        &logs, &warnings_2,
                                        &false_positives,
                                        apply_overrides,
                                        0))
                    /** @todo Either fail better or abort at SQL level. */
                    abort ();

                  if (report_timestamp (second_last_report_id, &timestamp))
                    abort ();

                  second_last_report = g_strdup_printf
                                        ("<second_last_report>"
                                         "<report id=\"%s\">"
                                         "<timestamp>%s</timestamp>"
                                         "<result_count>"
                                         "<debug>%i</debug>"
                                         "<hole>%i</hole>"
                                         "<info>%i</info>"
                                         "<log>%i</log>"
                                         "<warning>%i</warning>"
                                         "<false_positive>"
                                         "%i"
                                         "</false_positive>"
                                         "</result_count>"
                                         "</report>"
                                         "</second_last_report>",
                                         second_last_report_id,
                                         timestamp,
                                         debugs,
                                         holes_2,
                                         infos_2,
                                         logs,
                                         warnings_2,
                                         false_positives);
                  g_free (timestamp);
                }
              else
                second_last_report = g_strdup ("");

              last_report_id = task_last_report_id (index);
              if (last_report_id)
                {
                  gchar *timestamp;

                  /* If the last report is the first report or the second
                   * last report, then reuse the counts from before. */
                  if ((first_report_id == NULL)
                      || (second_last_report_id == NULL)
                      || (strcmp (last_report_id, first_report_id)
                          && strcmp (last_report_id,
                                     second_last_report_id)))
                    {
                      if (report_counts
                           (last_report_id,
                            &debugs, &holes, &infos, &logs,
                            &warnings, &false_positives,
                            apply_overrides,
                            0))
                        /** @todo Either fail better or abort at SQL level. */
                        abort ();
                    }
                  else
                    {
                      holes = holes_2;
                      infos = infos_2;
                      warnings = warnings_2;
                    }

                  if (report_timestamp (last_report_id, &timestamp))
                    abort ();

                  last_report = g_strdup_printf ("<last_report>"
                                                 "<report id=\"%s\">"
                                                 "<timestamp>%s</timestamp>"
                                                 "<result_count>"
                                                 "<debug>%i</debug>"
                                                 "<hole>%i</hole>"
                                                 "<info>%i</info>"
                                                 "<log>%i</log>"
                                                 "<warning>%i</warning>"
                                                 "<false_positive>"
                                                 "%i"
                                                 "</false_positive>"
                                                 "</result_count>"
                                                 "</report>"
                                                 "</last_report>",
                                                 last_report_id,
                                                 timestamp,
                                                 debugs,
                                                 holes,
                                                 infos,
                                                 logs,
                                                 warnings,
                                                 false_positives);
                  g_free (timestamp);
                  g_free (last_report_id);
                }
              else
                last_report = g_strdup ("");

              g_free (first_report_id);
              g_free (second_last_report_id);

              SEND_GET_COMMON (task, &get_tasks_data->get, &tasks);

              owner = task_owner_name (index);
              observers = task_observers (index);
              config = task_config_name (index);
              config_uuid = task_config_uuid (index);
              if (target_in_trash)
                {
                  task_target_uuid = trash_target_uuid (target);
                  task_target_name = trash_target_name (target);
                }
              else
                {
                  task_target_uuid = target_uuid (target);
                  task_target_name = target_name (target);
                }
              if (task_slave_in_trash (index))
                {
                  task_slave_uuid = trash_slave_uuid (slave);
                  task_slave_name = trash_slave_name (slave);
                }
              else
                {
                  task_slave_uuid = slave_uuid (slave);
                  task_slave_name = slave_name (slave);
                }
              schedule = task_schedule (index);
              if (schedule)
                {
                  task_schedule_uuid = schedule_uuid (schedule);
                  task_schedule_name = schedule_name (schedule);
                  schedule_in_trash = task_schedule_in_trash (index);
                }
              else
                {
                  task_schedule_uuid = (char*) g_strdup ("");
                  task_schedule_name = (char*) g_strdup ("");
                  schedule_in_trash = 0;
                }
              next_time = task_schedule_next_time_tz (index);

              response = g_strdup_printf
                          ("<owner><name>%s</name></owner>"
                           "<observers>%s</observers>"
                           "<config id=\"%s\">"
                           "<name>%s</name>"
                           "<trash>%i</trash>"
                           "</config>"
                           "<target id=\"%s\">"
                           "<name>%s</name>"
                           "<trash>%i</trash>"
                           "</target>"
                           "<slave id=\"%s\">"
                           "<name>%s</name>"
                           "<trash>%i</trash>"
                           "</slave>"
                           "<status>%s</status>"
                           "<progress>%s</progress>"
                           "%s"
                           "<report_count>"
                           "%u<finished>%u</finished>"
                           "</report_count>"
                           "<trend>%s</trend>"
                           "<schedule id=\"%s\">"
                           "<name>%s</name>"
                           "<next_time>%s</next_time>"
                           "<trash>%i</trash>"
                           "</schedule>"
                           "%s%s%s",
                           owner ? owner : "",
                           ((owner == NULL)
                            || (strcmp (owner,
                                        current_credentials.username)))
                             ? ""
                             : observers,
                           config_uuid ? config_uuid : "",
                           config ? config : "",
                           task_config_in_trash (index),
                           task_target_uuid ? task_target_uuid : "",
                           task_target_name ? task_target_name : "",
                           target_in_trash,
                           task_slave_uuid ? task_slave_uuid : "",
                           task_slave_name ? task_slave_name : "",
                           task_slave_in_trash (index),
                           task_run_status_name (index),
                           progress_xml,
                           description64,
                           /* TODO These can come from iterator now. */
                           task_report_count (index),
                           task_finished_report_count (index),
                           task_trend_counts
                            (index, holes, warnings, infos,
                             holes_2, warnings_2, infos_2),
                           task_schedule_uuid,
                           task_schedule_name,
                           (next_time == 0
                             ? "over"
                             : iso_time (&next_time)),
                           schedule_in_trash,
                           first_report,
                           last_report,
                           second_last_report);
              free (config);
              free (task_target_name);
              free (task_target_uuid);
              g_free (progress_xml);
              g_free (first_report);
              g_free (last_report);
              g_free (second_last_report);
              free (owner);
              free (observers);
              g_free (description64);
              free (task_schedule_uuid);
              free (task_schedule_name);
              free (task_slave_uuid);
              free (task_slave_name);
              if (send_to_client (response,
                                  write_to_client,
                                  write_to_client_data))
                {
                  g_free (response);
                  cleanup_iterator (&tasks);
                  error_send_to_client (error);
                  cleanup_iterator (&tasks);
                  return;
                }
              g_free (response);

              init_task_alert_iterator (&alerts, index, 0);
              while (next (&alerts))
                SENDF_TO_CLIENT_OR_FAIL
                 ("<alert id=\"%s\">"
                  "<name>%s</name>"
                  "</alert>",
                  task_alert_iterator_uuid (&alerts),
                  task_alert_iterator_name (&alerts));
              cleanup_iterator (&alerts);

              if (get_tasks_data->get.details)
                {
                  /* The detailed version. */

                  /** @todo Handle error cases.
                   *
                   * The errors are either SQL errors or out of space in
                   * buffer errors.  Both should probably just lead to aborts
                   * at the SQL or buffer output level.
                   */
                  (void) send_reports (index,
                                       apply_overrides,
                                       write_to_client,
                                       write_to_client_data);
                }

              in_assets = task_preference_value (index, "in_assets");
              max_checks = task_preference_value (index, "max_checks");
              max_hosts = task_preference_value (index, "max_hosts");

              SENDF_TO_CLIENT_OR_FAIL
               ("<preferences>"
                "<preference>"
                "<name>"
                "Maximum concurrently executed NVTs per host"
                "</name>"
                "<scanner_name>max_checks</scanner_name>"
                "<value>%s</value>"
                "</preference>"
                "<preference>"
                "<name>"
                "Maximum concurrently scanned hosts"
                "</name>"
                "<scanner_name>max_hosts</scanner_name>"
                "<value>%s</value>"
                "</preference>"
                "<preference>"
                "<name>"
                "Add results to Asset Management"
                "</name>"
                "<scanner_name>in_assets</scanner_name>"
                "<value>%s</value>"
                "</preference>"
                "</preferences>"
                "</task>",
                max_checks ? max_checks : "4",
                max_hosts ? max_hosts : "20",
                in_assets ? in_assets : "yes");

              g_free (in_assets);
              g_free (max_checks);
              g_free (max_hosts);

              count++;
            }
          cleanup_iterator (&tasks);
          filtered = get_tasks_data->get.id
                      ? 1
                      : task_count (&get_tasks_data->get);
          SEND_GET_END ("task", &get_tasks_data->get, count, filtered);
        }

        get_tasks_data_reset (get_tasks_data);
        set_client_state (CLIENT_AUTHENTIC);
        break;

      case CLIENT_VERIFY_AGENT:
        assert (strcasecmp ("VERIFY_AGENT", element_name) == 0);
        if (verify_agent_data->agent_id)
          {
            agent_t agent;

            if (find_agent (verify_agent_data->agent_id, &agent))
              SEND_TO_CLIENT_OR_FAIL (XML_INTERNAL_ERROR ("verify_agent"));
            else if (agent == 0)
              {
                if (send_find_error_to_client
                     ("verify_agent",
                      "report format",
                      verify_agent_data->agent_id,
                      write_to_client,
                      write_to_client_data))
                  {
                    error_send_to_client (error);
                    return;
                  }
              }
            else switch (verify_agent (agent))
              {
                case 0:
                  SEND_TO_CLIENT_OR_FAIL (XML_OK ("verify_agent"));
                  break;
                case 1:
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_ERROR_SYNTAX ("verify_agent",
                                      "Attempt to verify a hidden report"
                                      " format"));
                  break;
                default:
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_INTERNAL_ERROR ("verify_agent"));
                  break;
              }
          }
        else
          SEND_TO_CLIENT_OR_FAIL
           (XML_ERROR_SYNTAX ("verify_agent",
                              "VERIFY_AGENT requires a agent_id"
                              " attribute"));
        verify_agent_data_reset (verify_agent_data);
        set_client_state (CLIENT_AUTHENTIC);
        break;

      case CLIENT_VERIFY_REPORT_FORMAT:
        assert (strcasecmp ("VERIFY_REPORT_FORMAT", element_name) == 0);
        if (verify_report_format_data->report_format_id)
          {
            report_format_t report_format;

            if (find_report_format (verify_report_format_data->report_format_id,
                                    &report_format))
              SEND_TO_CLIENT_OR_FAIL
               (XML_INTERNAL_ERROR ("verify_report_format"));
            else if (report_format == 0)
              {
                if (send_find_error_to_client
                     ("verify_report_format",
                      "report format",
                      verify_report_format_data->report_format_id,
                      write_to_client,
                      write_to_client_data))
                  {
                    error_send_to_client (error);
                    return;
                  }
              }
            else switch (verify_report_format (report_format))
              {
                case 0:
                  SEND_TO_CLIENT_OR_FAIL (XML_OK ("verify_report_format"));
                  break;
                case 1:
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_ERROR_SYNTAX ("verify_report_format",
                                      "Attempt to verify a hidden report"
                                      " format"));
                  break;
                default:
                  SEND_TO_CLIENT_OR_FAIL
                   (XML_INTERNAL_ERROR ("verify_report_format"));
                  break;
              }
          }
        else
          SEND_TO_CLIENT_OR_FAIL
           (XML_ERROR_SYNTAX ("verify_report_format",
                              "VERIFY_REPORT_FORMAT requires a report_format_id"
                              " attribute"));
        verify_report_format_data_reset (verify_report_format_data);
        set_client_state (CLIENT_AUTHENTIC);
        break;

      default:
        assert (0);
        break;
    }
}

/**
 * @brief Append text to a var for a case in omp_xml_hand_text.
 *
 * @param[in]  state  Parser state.
 * @param[in]  dest   Append destination.
 */
#define APPEND(state, dest)                      \
  case state:                                    \
    openvas_append_text (dest, text, text_len);  \
    break;

/**
 * @brief Handle the addition of text to an OMP XML element.
 *
 * React to the addition of text to the value of an XML element.
 * React according to the current value of \ref client_state,
 * usually appending the text to some part of the current task
 * with functions like openvas_append_text,
 * \ref add_task_description_line and \ref append_to_task_comment.
 *
 * @param[in]  context           Parser context.
 * @param[in]  text              The text.
 * @param[in]  text_len          Length of the text.
 * @param[in]  user_data         Dummy parameter.
 * @param[in]  error             Error parameter.
 */
static void
omp_xml_handle_text (/*@unused@*/ GMarkupParseContext* context,
                     const gchar *text,
                     gsize text_len,
                     /*@unused@*/ gpointer user_data,
                     /*@unused@*/ GError **error)
{
  if (text_len == 0) return;
  tracef ("   XML   text: %s\n", text);
  switch (client_state)
    {
      case CLIENT_AUTHENTICATE_CREDENTIALS_USERNAME:
        append_to_credentials_username (&current_credentials, text, text_len);
        break;

      case CLIENT_AUTHENTICATE_CREDENTIALS_PASSWORD:
        append_to_credentials_password (&current_credentials, text, text_len);
        break;

      APPEND (CLIENT_MODIFY_CONFIG_NVT_SELECTION_FAMILY,
              &modify_config_data->nvt_selection_family);

      APPEND (CLIENT_MODIFY_CONFIG_FAMILY_SELECTION_FAMILY_ALL,
              &modify_config_data->family_selection_family_all_text);

      APPEND (CLIENT_MODIFY_CONFIG_FAMILY_SELECTION_FAMILY_GROWING,
              &modify_config_data->family_selection_family_growing_text);

      APPEND (CLIENT_MODIFY_CONFIG_FAMILY_SELECTION_FAMILY_NAME,
              &modify_config_data->family_selection_family_name);

      APPEND (CLIENT_MODIFY_CONFIG_FAMILY_SELECTION_GROWING,
              &modify_config_data->family_selection_growing_text);


      APPEND (CLIENT_MODIFY_LSC_CREDENTIAL_NAME,
              &modify_lsc_credential_data->name);

      APPEND (CLIENT_MODIFY_LSC_CREDENTIAL_COMMENT,
              &modify_lsc_credential_data->comment);

      APPEND (CLIENT_MODIFY_LSC_CREDENTIAL_LOGIN,
              &modify_lsc_credential_data->login);

      APPEND (CLIENT_MODIFY_LSC_CREDENTIAL_PASSWORD,
              &modify_lsc_credential_data->password);


      APPEND (CLIENT_MODIFY_CONFIG_COMMENT,
              &modify_config_data->comment);

      APPEND (CLIENT_MODIFY_CONFIG_NAME,
              &modify_config_data->name);

      APPEND (CLIENT_MODIFY_CONFIG_PREFERENCE_NAME,
              &modify_config_data->preference_name);

      APPEND (CLIENT_MODIFY_CONFIG_PREFERENCE_VALUE,
              &modify_config_data->preference_value);


      APPEND (CLIENT_MODIFY_REPORT_COMMENT,
              &modify_report_data->comment);


      APPEND (CLIENT_MODIFY_REPORT_FORMAT_ACTIVE,
              &modify_report_format_data->active);

      APPEND (CLIENT_MODIFY_REPORT_FORMAT_NAME,
              &modify_report_format_data->name);

      APPEND (CLIENT_MODIFY_REPORT_FORMAT_SUMMARY,
              &modify_report_format_data->summary);

      APPEND (CLIENT_MODIFY_REPORT_FORMAT_PARAM_NAME,
              &modify_report_format_data->param_name);

      APPEND (CLIENT_MODIFY_REPORT_FORMAT_PARAM_VALUE,
              &modify_report_format_data->param_value);


      APPEND (CLIENT_MODIFY_SETTING_NAME,
              &modify_setting_data->name);

      APPEND (CLIENT_MODIFY_SETTING_VALUE,
              &modify_setting_data->value);


      APPEND (CLIENT_MODIFY_TASK_COMMENT,
              &modify_task_data->comment);

      APPEND (CLIENT_MODIFY_TASK_NAME,
              &modify_task_data->name);

      APPEND (CLIENT_MODIFY_TASK_OBSERVERS,
              &modify_task_data->observers);

      APPEND (CLIENT_MODIFY_TASK_RCFILE,
              &modify_task_data->rcfile);

      APPEND (CLIENT_MODIFY_TASK_FILE,
              &modify_task_data->file);


      APPEND (CLIENT_MODIFY_TASK_PREFERENCES_PREFERENCE_NAME,
              &modify_task_data->preference->name);

      APPEND (CLIENT_MODIFY_TASK_PREFERENCES_PREFERENCE_VALUE,
              &modify_task_data->preference->value);


      APPEND (CLIENT_CREATE_AGENT_COMMENT,
              &create_agent_data->comment);

      APPEND (CLIENT_CREATE_AGENT_COPY,
              &create_agent_data->copy);

      APPEND (CLIENT_CREATE_AGENT_HOWTO_INSTALL,
              &create_agent_data->howto_install);

      APPEND (CLIENT_CREATE_AGENT_HOWTO_USE,
              &create_agent_data->howto_use);

      APPEND (CLIENT_CREATE_AGENT_INSTALLER,
              &create_agent_data->installer);

      APPEND (CLIENT_CREATE_AGENT_INSTALLER_FILENAME,
              &create_agent_data->installer_filename);

      APPEND (CLIENT_CREATE_AGENT_INSTALLER_SIGNATURE,
              &create_agent_data->installer_signature);

      APPEND (CLIENT_CREATE_AGENT_NAME,
              &create_agent_data->name);


      APPEND (CLIENT_CREATE_CONFIG_COMMENT,
              &create_config_data->comment);

      APPEND (CLIENT_CREATE_CONFIG_COPY,
              &create_config_data->copy);

      APPEND (CLIENT_CREATE_CONFIG_NAME,
              &create_config_data->name);

      APPEND (CLIENT_CREATE_CONFIG_RCFILE,
              &create_config_data->rcfile);

      APPEND (CLIENT_C_C_GCR_CONFIG_COMMENT,
              &import_config_data->comment);

      APPEND (CLIENT_C_C_GCR_CONFIG_NAME,
              &import_config_data->name);

      APPEND (CLIENT_C_C_GCR_CONFIG_NVT_SELECTORS_NVT_SELECTOR_INCLUDE,
              &import_config_data->nvt_selector_include);

      APPEND (CLIENT_C_C_GCR_CONFIG_NVT_SELECTORS_NVT_SELECTOR_NAME,
              &import_config_data->nvt_selector_name);

      APPEND (CLIENT_C_C_GCR_CONFIG_NVT_SELECTORS_NVT_SELECTOR_TYPE,
              &import_config_data->nvt_selector_type);

      APPEND (CLIENT_C_C_GCR_CONFIG_NVT_SELECTORS_NVT_SELECTOR_FAMILY_OR_NVT,
              &import_config_data->nvt_selector_family_or_nvt);

      APPEND (CLIENT_C_C_GCR_CONFIG_PREFERENCES_PREFERENCE_ALT,
              &import_config_data->preference_alt);

      APPEND (CLIENT_C_C_GCR_CONFIG_PREFERENCES_PREFERENCE_NAME,
              &import_config_data->preference_name);

      APPEND (CLIENT_C_C_GCR_CONFIG_PREFERENCES_PREFERENCE_NVT_NAME,
              &import_config_data->preference_nvt_name);

      APPEND (CLIENT_C_C_GCR_CONFIG_PREFERENCES_PREFERENCE_TYPE,
              &import_config_data->preference_type);

      APPEND (CLIENT_C_C_GCR_CONFIG_PREFERENCES_PREFERENCE_VALUE,
              &import_config_data->preference_value);


      APPEND (CLIENT_CREATE_LSC_CREDENTIAL_COMMENT,
              &create_lsc_credential_data->comment);

      APPEND (CLIENT_CREATE_LSC_CREDENTIAL_COPY,
              &create_lsc_credential_data->copy);

      APPEND (CLIENT_CREATE_LSC_CREDENTIAL_KEY_PHRASE,
              &create_lsc_credential_data->key_phrase);

      APPEND (CLIENT_CREATE_LSC_CREDENTIAL_KEY_PRIVATE,
              &create_lsc_credential_data->key_private);

      APPEND (CLIENT_CREATE_LSC_CREDENTIAL_KEY_PUBLIC,
              &create_lsc_credential_data->key_public);

      APPEND (CLIENT_CREATE_LSC_CREDENTIAL_LOGIN,
              &create_lsc_credential_data->login);

      APPEND (CLIENT_CREATE_LSC_CREDENTIAL_NAME,
              &create_lsc_credential_data->name);

      APPEND (CLIENT_CREATE_LSC_CREDENTIAL_PASSWORD,
              &create_lsc_credential_data->password);


      APPEND (CLIENT_CREATE_ALERT_COMMENT,
              &create_alert_data->comment);

      APPEND (CLIENT_CREATE_ALERT_COPY,
              &create_alert_data->copy);

      APPEND (CLIENT_CREATE_ALERT_CONDITION,
              &create_alert_data->condition);

      APPEND (CLIENT_CREATE_ALERT_EVENT,
              &create_alert_data->event);

      APPEND (CLIENT_CREATE_ALERT_METHOD,
              &create_alert_data->method);

      APPEND (CLIENT_CREATE_ALERT_NAME,
              &create_alert_data->name);


      APPEND (CLIENT_CREATE_ALERT_CONDITION_DATA,
              &create_alert_data->part_data);

      APPEND (CLIENT_CREATE_ALERT_EVENT_DATA,
              &create_alert_data->part_data);

      APPEND (CLIENT_CREATE_ALERT_METHOD_DATA,
              &create_alert_data->part_data);


      APPEND (CLIENT_CREATE_ALERT_CONDITION_DATA_NAME,
              &create_alert_data->part_name);

      APPEND (CLIENT_CREATE_ALERT_EVENT_DATA_NAME,
              &create_alert_data->part_name);

      APPEND (CLIENT_CREATE_ALERT_METHOD_DATA_NAME,
              &create_alert_data->part_name);


      APPEND (CLIENT_CREATE_FILTER_COMMENT,
              &create_filter_data->comment);

      APPEND (CLIENT_CREATE_FILTER_COPY,
              &create_filter_data->copy);

      APPEND (CLIENT_CREATE_FILTER_NAME,
              &create_filter_data->name);

      APPEND (CLIENT_CREATE_FILTER_NAME_MAKE_UNIQUE,
              &create_filter_data->make_name_unique);

      APPEND (CLIENT_CREATE_FILTER_TERM,
              &create_filter_data->term);

      APPEND (CLIENT_CREATE_FILTER_TYPE,
              &create_filter_data->type);


      APPEND (CLIENT_CREATE_NOTE_ACTIVE,
              &create_note_data->active);

      APPEND (CLIENT_CREATE_NOTE_COPY,
              &create_note_data->copy);

      APPEND (CLIENT_CREATE_NOTE_HOSTS,
              &create_note_data->hosts);

      APPEND (CLIENT_CREATE_NOTE_PORT,
              &create_note_data->port);

      APPEND (CLIENT_CREATE_NOTE_TEXT,
              &create_note_data->text);

      APPEND (CLIENT_CREATE_NOTE_THREAT,
              &create_note_data->threat);


      APPEND (CLIENT_CREATE_OVERRIDE_ACTIVE,
              &create_override_data->active);

      APPEND (CLIENT_CREATE_OVERRIDE_COPY,
              &create_override_data->copy);

      APPEND (CLIENT_CREATE_OVERRIDE_HOSTS,
              &create_override_data->hosts);

      APPEND (CLIENT_CREATE_OVERRIDE_NEW_THREAT,
              &create_override_data->new_threat);

      APPEND (CLIENT_CREATE_OVERRIDE_PORT,
              &create_override_data->port);

      APPEND (CLIENT_CREATE_OVERRIDE_TEXT,
              &create_override_data->text);

      APPEND (CLIENT_CREATE_OVERRIDE_THREAT,
              &create_override_data->threat);


      APPEND (CLIENT_CREATE_PORT_LIST_COMMENT,
              &create_port_list_data->comment);

      APPEND (CLIENT_CREATE_PORT_LIST_COPY,
              &create_port_list_data->copy);

      APPEND (CLIENT_CREATE_PORT_LIST_NAME,
              &create_port_list_data->name);

      APPEND (CLIENT_CREATE_PORT_LIST_PORT_RANGE,
              &create_port_list_data->port_range);


      APPEND (CLIENT_CPL_GPLR_PORT_LIST_COMMENT,
              &create_port_list_data->comment);

      APPEND (CLIENT_CPL_GPLR_PORT_LIST_NAME,
              &create_port_list_data->name);

      APPEND (CLIENT_CPL_GPLR_PORT_LIST_PORT_RANGES_PORT_RANGE_COMMENT,
              &create_port_list_data->range->comment);

      APPEND (CLIENT_CPL_GPLR_PORT_LIST_PORT_RANGES_PORT_RANGE_END,
              &create_port_list_data->range->end);

      APPEND (CLIENT_CPL_GPLR_PORT_LIST_PORT_RANGES_PORT_RANGE_START,
              &create_port_list_data->range->start);

      APPEND (CLIENT_CPL_GPLR_PORT_LIST_PORT_RANGES_PORT_RANGE_TYPE,
              &create_port_list_data->range->type);


      APPEND (CLIENT_CREATE_PORT_RANGE_COMMENT,
              &create_port_range_data->comment);

      APPEND (CLIENT_CREATE_PORT_RANGE_END,
              &create_port_range_data->end);

      APPEND (CLIENT_CREATE_PORT_RANGE_START,
              &create_port_range_data->start);

      APPEND (CLIENT_CREATE_PORT_RANGE_TYPE,
              &create_port_range_data->type);


      APPEND (CLIENT_CREATE_REPORT_RR_HOST_END,
              &create_report_data->host_end);

      APPEND (CLIENT_CREATE_REPORT_RR_HOST_END_HOST,
              &create_report_data->host_end_host);

      APPEND (CLIENT_CREATE_REPORT_RR_HOST_START,
              &create_report_data->host_start);

      APPEND (CLIENT_CREATE_REPORT_RR_HOST_START_HOST,
              &create_report_data->host_start_host);


      APPEND (CLIENT_CREATE_REPORT_RR_SCAN_END,
              &create_report_data->scan_end);

      APPEND (CLIENT_CREATE_REPORT_RR_SCAN_START,
              &create_report_data->scan_start);


      APPEND (CLIENT_CREATE_REPORT_RR_RESULTS_RESULT_DESCRIPTION,
              &create_report_data->result_description);

      APPEND (CLIENT_CREATE_REPORT_RR_RESULTS_RESULT_HOST,
              &create_report_data->result_host);

      APPEND (CLIENT_CREATE_REPORT_RR_RESULTS_RESULT_PORT,
              &create_report_data->result_port);

      APPEND (CLIENT_CREATE_REPORT_RR_RESULTS_RESULT_SUBNET,
              &create_report_data->result_subnet);

      APPEND (CLIENT_CREATE_REPORT_RR_RESULTS_RESULT_THREAT,
              &create_report_data->result_threat);


      APPEND (CLIENT_CREATE_REPORT_RR_H_DETAIL_NAME,
              &create_report_data->detail_name);

      APPEND (CLIENT_CREATE_REPORT_RR_H_DETAIL_VALUE,
              &create_report_data->detail_value);

      APPEND (CLIENT_CREATE_REPORT_RR_H_DETAIL_SOURCE_DESC,
              &create_report_data->detail_source_desc);

      APPEND (CLIENT_CREATE_REPORT_RR_H_DETAIL_SOURCE_NAME,
              &create_report_data->detail_source_name);

      APPEND (CLIENT_CREATE_REPORT_RR_H_DETAIL_SOURCE_TYPE,
              &create_report_data->detail_source_type);

      APPEND (CLIENT_CREATE_REPORT_RR_H_IP,
              &create_report_data->ip);


      APPEND (CLIENT_CREATE_REPORT_TASK_NAME,
              &create_report_data->task_name);

      APPEND (CLIENT_CREATE_REPORT_TASK_COMMENT,
              &create_report_data->task_comment);


      APPEND (CLIENT_CRF_GRFR_REPORT_FORMAT_CONTENT_TYPE,
              &create_report_format_data->content_type);

      APPEND (CLIENT_CRF_GRFR_REPORT_FORMAT_DESCRIPTION,
              &create_report_format_data->description);

      APPEND (CLIENT_CREATE_REPORT_FORMAT_COPY,
              &create_report_format_data->copy);

      APPEND (CLIENT_CRF_GRFR_REPORT_FORMAT_EXTENSION,
              &create_report_format_data->extension);

      APPEND (CLIENT_CRF_GRFR_REPORT_FORMAT_FILE,
              &create_report_format_data->file);

      APPEND (CLIENT_CRF_GRFR_REPORT_FORMAT_GLOBAL,
              &create_report_format_data->global);

      APPEND (CLIENT_CRF_GRFR_REPORT_FORMAT_NAME,
              &create_report_format_data->name);

      APPEND (CLIENT_CRF_GRFR_REPORT_FORMAT_PARAM_DEFAULT,
              &create_report_format_data->param_default);

      APPEND (CLIENT_CRF_GRFR_REPORT_FORMAT_PARAM_NAME,
              &create_report_format_data->param_name);

      APPEND (CLIENT_CRF_GRFR_REPORT_FORMAT_PARAM_OPTIONS_OPTION,
              &create_report_format_data->param_option);

      APPEND (CLIENT_CRF_GRFR_REPORT_FORMAT_PARAM_TYPE,
              &create_report_format_data->param_type);

      APPEND (CLIENT_CRF_GRFR_REPORT_FORMAT_PARAM_TYPE_MAX,
              &create_report_format_data->param_type_max);

      APPEND (CLIENT_CRF_GRFR_REPORT_FORMAT_PARAM_TYPE_MIN,
              &create_report_format_data->param_type_min);

      APPEND (CLIENT_CRF_GRFR_REPORT_FORMAT_PARAM_VALUE,
              &create_report_format_data->param_value);

      APPEND (CLIENT_CRF_GRFR_REPORT_FORMAT_SIGNATURE,
              &create_report_format_data->signature);

      APPEND (CLIENT_CRF_GRFR_REPORT_FORMAT_SUMMARY,
              &create_report_format_data->summary);

      case CLIENT_CRF_GRFR_REPORT_FORMAT_TRUST:
        break;


      APPEND (CLIENT_CREATE_SCHEDULE_COMMENT,
              &create_schedule_data->comment);

      APPEND (CLIENT_CREATE_SCHEDULE_COPY,
              &create_schedule_data->copy);

      APPEND (CLIENT_CREATE_SCHEDULE_DURATION,
              &create_schedule_data->duration);

      APPEND (CLIENT_CREATE_SCHEDULE_DURATION_UNIT,
              &create_schedule_data->duration_unit);

      APPEND (CLIENT_CREATE_SCHEDULE_FIRST_TIME_DAY_OF_MONTH,
              &create_schedule_data->first_time_day_of_month);

      APPEND (CLIENT_CREATE_SCHEDULE_FIRST_TIME_HOUR,
              &create_schedule_data->first_time_hour);

      APPEND (CLIENT_CREATE_SCHEDULE_FIRST_TIME_MINUTE,
              &create_schedule_data->first_time_minute);

      APPEND (CLIENT_CREATE_SCHEDULE_FIRST_TIME_MONTH,
              &create_schedule_data->first_time_month);

      APPEND (CLIENT_CREATE_SCHEDULE_FIRST_TIME_YEAR,
              &create_schedule_data->first_time_year);

      APPEND (CLIENT_CREATE_SCHEDULE_NAME,
              &create_schedule_data->name);

      APPEND (CLIENT_CREATE_SCHEDULE_PERIOD,
              &create_schedule_data->period);

      APPEND (CLIENT_CREATE_SCHEDULE_PERIOD_UNIT,
              &create_schedule_data->period_unit);


      APPEND (CLIENT_CREATE_SLAVE_COMMENT,
              &create_slave_data->comment);

      APPEND (CLIENT_CREATE_SLAVE_HOST,
              &create_slave_data->host);

      APPEND (CLIENT_CREATE_SLAVE_COPY,
              &create_slave_data->copy);

      APPEND (CLIENT_CREATE_SLAVE_LOGIN,
              &create_slave_data->login);

      APPEND (CLIENT_CREATE_SLAVE_NAME,
              &create_slave_data->name);

      APPEND (CLIENT_CREATE_SLAVE_PASSWORD,
              &create_slave_data->password);

      APPEND (CLIENT_CREATE_SLAVE_PORT,
              &create_slave_data->port);


      APPEND (CLIENT_CREATE_TARGET_COMMENT,
              &create_target_data->comment);

      APPEND (CLIENT_CREATE_TARGET_COPY,
              &create_target_data->copy);

      APPEND (CLIENT_CREATE_TARGET_HOSTS,
              &create_target_data->hosts);

      APPEND (CLIENT_CREATE_TARGET_NAME,
              &create_target_data->name);

      APPEND (CLIENT_CREATE_TARGET_NAME_MAKE_UNIQUE,
              &create_target_data->make_name_unique);

      APPEND (CLIENT_CREATE_TARGET_PORT_RANGE,
              &create_target_data->port_range);

      APPEND (CLIENT_CREATE_TARGET_TARGET_LOCATOR,
              &create_target_data->target_locator);

      APPEND (CLIENT_CREATE_TARGET_TARGET_LOCATOR_PASSWORD,
              &create_target_data->target_locator_password);

      APPEND (CLIENT_CREATE_TARGET_TARGET_LOCATOR_USERNAME,
              &create_target_data->target_locator_username);

      APPEND (CLIENT_CREATE_TARGET_SSH_LSC_CREDENTIAL_PORT,
              &create_target_data->ssh_port);


      case CLIENT_CREATE_TASK_COMMENT:
        append_to_task_comment (create_task_data->task, text, text_len);
        break;

      APPEND (CLIENT_CREATE_TASK_COPY,
              &create_task_data->copy);

      case CLIENT_CREATE_TASK_NAME:
        append_to_task_name (create_task_data->task, text, text_len);
        break;

      APPEND (CLIENT_CREATE_TASK_OBSERVERS,
              &create_task_data->observers);

      case CLIENT_CREATE_TASK_RCFILE:
        /* Append the text to the task description. */
        add_task_description_line (create_task_data->task,
                                   text,
                                   text_len);
        break;

      APPEND (CLIENT_CREATE_TASK_PREFERENCES_PREFERENCE_NAME,
              &create_task_data->preference->name);

      APPEND (CLIENT_CREATE_TASK_PREFERENCES_PREFERENCE_VALUE,
              &create_task_data->preference->value);


      APPEND (CLIENT_MODIFY_AGENT_COMMENT,
              &modify_agent_data->comment);

      APPEND (CLIENT_MODIFY_AGENT_NAME,
              &modify_agent_data->name);


      APPEND (CLIENT_MODIFY_ALERT_NAME,
              &modify_alert_data->name);

      APPEND (CLIENT_MODIFY_ALERT_COMMENT,
              &modify_alert_data->comment);

      APPEND (CLIENT_MODIFY_ALERT_EVENT,
              &modify_alert_data->event);

      APPEND (CLIENT_MODIFY_ALERT_CONDITION,
              &modify_alert_data->condition);

      APPEND (CLIENT_MODIFY_ALERT_METHOD,
              &modify_alert_data->method);


      APPEND (CLIENT_MODIFY_ALERT_EVENT_DATA,
              &modify_alert_data->part_data);

      APPEND (CLIENT_MODIFY_ALERT_CONDITION_DATA,
              &modify_alert_data->part_data);

      APPEND (CLIENT_MODIFY_ALERT_METHOD_DATA,
              &modify_alert_data->part_data);


      APPEND (CLIENT_MODIFY_ALERT_EVENT_DATA_NAME,
              &modify_alert_data->part_name);

      APPEND (CLIENT_MODIFY_ALERT_CONDITION_DATA_NAME,
              &modify_alert_data->part_name);

      APPEND (CLIENT_MODIFY_ALERT_METHOD_DATA_NAME,
              &modify_alert_data->part_name);


      APPEND (CLIENT_MODIFY_FILTER_COMMENT,
              &modify_filter_data->comment);

      APPEND (CLIENT_MODIFY_FILTER_NAME,
              &modify_filter_data->name);

      APPEND (CLIENT_MODIFY_FILTER_TERM,
              &modify_filter_data->term);

      APPEND (CLIENT_MODIFY_FILTER_TYPE,
              &modify_filter_data->type);


      APPEND (CLIENT_MODIFY_NOTE_ACTIVE,
              &modify_note_data->active);

      APPEND (CLIENT_MODIFY_NOTE_HOSTS,
              &modify_note_data->hosts);

      APPEND (CLIENT_MODIFY_NOTE_PORT,
              &modify_note_data->port);

      APPEND (CLIENT_MODIFY_NOTE_TEXT,
              &modify_note_data->text);

      APPEND (CLIENT_MODIFY_NOTE_THREAT,
              &modify_note_data->threat);


      APPEND (CLIENT_MODIFY_OVERRIDE_ACTIVE,
              &modify_override_data->active);

      APPEND (CLIENT_MODIFY_OVERRIDE_HOSTS,
              &modify_override_data->hosts);

      APPEND (CLIENT_MODIFY_OVERRIDE_NEW_THREAT,
              &modify_override_data->new_threat);

      APPEND (CLIENT_MODIFY_OVERRIDE_PORT,
              &modify_override_data->port);

      APPEND (CLIENT_MODIFY_OVERRIDE_TEXT,
              &modify_override_data->text);

      APPEND (CLIENT_MODIFY_OVERRIDE_THREAT,
              &modify_override_data->threat);


      APPEND (CLIENT_MODIFY_PORT_LIST_COMMENT,
              &modify_port_list_data->comment);

      APPEND (CLIENT_MODIFY_PORT_LIST_NAME,
              &modify_port_list_data->name);


      APPEND (CLIENT_MODIFY_SCHEDULE_COMMENT,
              &modify_schedule_data->comment);

      APPEND (CLIENT_MODIFY_SCHEDULE_DURATION,
              &modify_schedule_data->duration);

      APPEND (CLIENT_MODIFY_SCHEDULE_DURATION_UNIT,
              &modify_schedule_data->duration_unit);

      APPEND (CLIENT_MODIFY_SCHEDULE_FIRST_TIME_DAY_OF_MONTH,
              &modify_schedule_data->first_time_day_of_month);

      APPEND (CLIENT_MODIFY_SCHEDULE_FIRST_TIME_HOUR,
              &modify_schedule_data->first_time_hour);

      APPEND (CLIENT_MODIFY_SCHEDULE_FIRST_TIME_MINUTE,
              &modify_schedule_data->first_time_minute);

      APPEND (CLIENT_MODIFY_SCHEDULE_FIRST_TIME_MONTH,
              &modify_schedule_data->first_time_month);

      APPEND (CLIENT_MODIFY_SCHEDULE_FIRST_TIME_YEAR,
              &modify_schedule_data->first_time_year);

      APPEND (CLIENT_MODIFY_SCHEDULE_NAME,
              &modify_schedule_data->name);

      APPEND (CLIENT_MODIFY_SCHEDULE_PERIOD,
              &modify_schedule_data->period);

      APPEND (CLIENT_MODIFY_SCHEDULE_PERIOD_UNIT,
              &modify_schedule_data->period_unit);

      APPEND (CLIENT_MODIFY_SCHEDULE_TIMEZONE,
              &modify_schedule_data->timezone);


      APPEND (CLIENT_MODIFY_SLAVE_COMMENT,
              &modify_slave_data->comment);

      APPEND (CLIENT_MODIFY_SLAVE_NAME,
              &modify_slave_data->name);

      APPEND (CLIENT_MODIFY_SLAVE_HOST,
              &modify_slave_data->host);

      APPEND (CLIENT_MODIFY_SLAVE_PORT,
              &modify_slave_data->port);

      APPEND (CLIENT_MODIFY_SLAVE_LOGIN,
              &modify_slave_data->login);

      APPEND (CLIENT_MODIFY_SLAVE_PASSWORD,
              &modify_slave_data->password);

      APPEND (CLIENT_MODIFY_TARGET_COMMENT,
              &modify_target_data->comment);

      APPEND (CLIENT_MODIFY_TARGET_HOSTS,
              &modify_target_data->hosts);

      APPEND (CLIENT_MODIFY_TARGET_NAME,
              &modify_target_data->name);

      APPEND (CLIENT_MODIFY_TARGET_TARGET_LOCATOR,
              &modify_target_data->target_locator);

      APPEND (CLIENT_MODIFY_TARGET_TARGET_LOCATOR_PASSWORD,
              &modify_target_data->target_locator_password);

      APPEND (CLIENT_MODIFY_TARGET_TARGET_LOCATOR_USERNAME,
              &modify_target_data->target_locator_username);

      APPEND (CLIENT_MODIFY_TARGET_SSH_LSC_CREDENTIAL_PORT,
              &modify_target_data->ssh_port);


      APPEND (CLIENT_RUN_WIZARD_NAME,
              &run_wizard_data->name);

      APPEND (CLIENT_RUN_WIZARD_PARAMS_PARAM_NAME,
              &run_wizard_data->param->name);

      APPEND (CLIENT_RUN_WIZARD_PARAMS_PARAM_VALUE,
              &run_wizard_data->param->value);


      default:
        /* Just pass over the text. */
        break;
    }
}

/**
 * @brief Handle an OMP XML parsing error.
 *
 * Simply leave the error for the caller of the parser to handle.
 *
 * @param[in]  context           Parser context.
 * @param[in]  error             The error.
 * @param[in]  user_data         Dummy parameter.
 */
static void
omp_xml_handle_error (/*@unused@*/ GMarkupParseContext* context,
                      GError *error,
                      /*@unused@*/ gpointer user_data)
{
  tracef ("   XML ERROR %s\n", error->message);
}


/* OMP input processor. */

/** @todo Most likely the client should get these from init_omp_process
 *        inside an omp_parser_t and should pass the omp_parser_t to
 *        process_omp_client_input.  process_omp_client_input can pass then
 *        pass them on to the other Manager "libraries". */
extern char from_client[];
extern buffer_size_t from_client_start;
extern buffer_size_t from_client_end;

/**
 * @brief Initialise OMP library.
 *
 * @param[in]  log_config      Logging configuration list.
 * @param[in]  nvt_cache_mode  True when running in NVT caching mode.
 * @param[in]  database        Location of manage database.
 *
 * @return 0 success, -1 error, -2 database is wrong version, -3 database
 *         needs to be initialized from server.
 */
int
init_omp (GSList *log_config, int nvt_cache_mode, const gchar *database)
{
  g_log_set_handler (G_LOG_DOMAIN,
                     ALL_LOG_LEVELS,
                     (GLogFunc) openvas_log_func,
                     log_config);
  command_data_init (&command_data);
  return init_manage (log_config, nvt_cache_mode, database);
}

/**
 * @brief Initialise OMP library data for a process.
 *
 * @param[in]  update_nvt_cache  0 operate normally, -1 just update NVT cache,
 *                               -2 just rebuild NVT cache.
 * @param[in]  database          Location of manage database.
 * @param[in]  write_to_client       Function to write to client.
 * @param[in]  write_to_client_data  Argument to \p write_to_client.
 * @param[in]  disable               Commands to disable.
 *
 * This should run once per process, before the first call to \ref
 * process_omp_client_input.
 */
void
init_omp_process (int update_nvt_cache, const gchar *database,
                  int (*write_to_client) (const char*, void*),
                  void* write_to_client_data, gchar **disable)
{
  forked = 0;
  init_manage_process (update_nvt_cache, database);
  /* Create the XML parser. */
  xml_parser.start_element = omp_xml_handle_start_element;
  xml_parser.end_element = omp_xml_handle_end_element;
  xml_parser.text = omp_xml_handle_text;
  xml_parser.passthrough = NULL;
  xml_parser.error = omp_xml_handle_error;
  if (xml_context) g_free (xml_context);
  xml_context = g_markup_parse_context_new
                 (&xml_parser,
                  0,
                  omp_parser_new (write_to_client, write_to_client_data,
                                  disable),
                  (GDestroyNotify) omp_parser_free);
}

/**
 * @brief Process any XML available in \ref from_client.
 *
 * \if STATIC
 *
 * Call the XML parser and let the callback functions do the work
 * (\ref omp_xml_handle_start_element, \ref omp_xml_handle_end_element,
 * \ref omp_xml_handle_text and \ref omp_xml_handle_error).
 *
 * The callback functions will queue any resulting scanner commands in
 * \ref to_scanner (using \ref send_to_server) and any replies for
 * the client in \ref to_client (using \ref send_to_client).
 *
 * \endif
 *
 * @todo The -2 return has been replaced by send_to_client trying to write
 *       the to_client buffer to the client when it is full.  This is
 *       necessary, as the to_client buffer may fill up halfway through the
 *       processing of an OMP element.
 *
 * @return 0 success, -1 error, -2 or -3 too little space in \ref to_client
 *         or the scanner output buffer (respectively), -4 XML syntax error.
 */
int
process_omp_client_input ()
{
  gboolean success;
  GError* error = NULL;

  /* Terminate any pending transaction. (force close = TRUE). */
  manage_transaction_stop (TRUE);

  /* In the XML parser handlers all writes to the to_scanner buffer must be
   * complete OTP commands, because the caller may also write into to_scanner
   * between calls to this function (via manage_check_current_task). */

  if (xml_context == NULL) return -1;

  current_error = 0;
  success = g_markup_parse_context_parse (xml_context,
                                          from_client + from_client_start,
                                          from_client_end - from_client_start,
                                          &error);
  if (success == FALSE)
    {
      int err;
      if (error)
        {
          err = -4;
          if (g_error_matches (error,
                               G_MARKUP_ERROR,
                               G_MARKUP_ERROR_UNKNOWN_ELEMENT))
            tracef ("   client error: G_MARKUP_ERROR_UNKNOWN_ELEMENT\n");
          else if (g_error_matches (error,
                                    G_MARKUP_ERROR,
                                    G_MARKUP_ERROR_INVALID_CONTENT))
            {
              if (current_error)
                {
                  /* This is the return status for a forked child. */
                  forked = 2; /* Prevent further forking. */
                  g_error_free (error);
                  return current_error;
                }
              tracef ("   client error: G_MARKUP_ERROR_INVALID_CONTENT\n");
            }
          else if (g_error_matches (error,
                                    G_MARKUP_ERROR,
                                    G_MARKUP_ERROR_UNKNOWN_ATTRIBUTE))
            tracef ("   client error: G_MARKUP_ERROR_UNKNOWN_ATTRIBUTE\n");
          else
            err = -1;
          infof ("   Failed to parse client XML: %s\n", error->message);
          g_error_free (error);
        }
      else
        err = -1;
      /* In all error cases the caller must cease to call this function as it
       * would be too hard, if possible at all, to figure out the position of
       * start of the next command. */
      return err;
    }
  from_client_end = from_client_start = 0;
  if (forked)
    return 3;
  return 0;
}

/**
 * @brief Buffer the response for process_omp.
 *
 * @param[in]  msg     OMP response.
 * @param[in]  buffer  Buffer.
 *
 * @return TRUE if failed, else FALSE.
 */
int
process_omp_write (const char* msg, void* buffer)
{
  tracef ("-> client internal: %s\n", msg);
  g_string_append ((GString*) buffer, msg);
  return FALSE;
}

/**
 * @brief Process an XML string.
 *
 * \if STATIC
 *
 * Call the XML parser and let the callback functions do the work
 * (\ref omp_xml_handle_start_element, \ref omp_xml_handle_end_element,
 * \ref omp_xml_handle_text and \ref omp_xml_handle_error).
 *
 * The callback functions will queue any resulting scanner commands in
 * \ref to_scanner (using \ref send_to_server) and any replies for
 * the client in \ref to_client (using \ref send_to_client).
 *
 * \endif
 *
 * @todo The -2 return has been replaced by send_to_client trying to write
 *       the to_client buffer to the client when it is full.  This is
 *       necessary, as the to_client buffer may fill up halfway through the
 *       processing of an OMP element.
 *
 * @return 0 success, -1 error, -2 or -3 too little space in \ref to_client
 *         or the scanner output buffer (respectively), -4 XML syntax error.
 */
static int
process_omp (omp_parser_t *parser, const gchar *command, gchar **response)
{
  gboolean success;
  GError* error = NULL;
  GString *buffer;
  int (*client_writer) (const char*, void*);
  void* client_writer_data;
  GMarkupParseContext *old_xml_context;
  client_state_t old_client_state;
  command_data_t old_command_data;

  /* Terminate any pending transaction. (force close = TRUE). */
  manage_transaction_stop (TRUE);

  if (response) *response = NULL;

  old_xml_context = xml_context;
  xml_context = g_markup_parse_context_new (&xml_parser, 0, parser, NULL);
  if (xml_context == NULL)
    {
      xml_context = old_xml_context;
      return -1;
    }

  old_command_data = command_data;
  command_data_init (&command_data);
  old_client_state = client_state;
  client_state = CLIENT_AUTHENTIC;
  buffer = g_string_new ("");
  client_writer = parser->client_writer;
  client_writer_data = parser->client_writer_data;
  parser->client_writer = process_omp_write;
  parser->client_writer_data = buffer;
  current_error = 0;
  success = g_markup_parse_context_parse (xml_context,
                                          command,
                                          strlen (command),
                                          &error);
  parser->client_writer = client_writer;
  parser->client_writer_data = client_writer_data;
  xml_context = old_xml_context;
  client_state = old_client_state;
  command_data = old_command_data;
  if (success == FALSE)
    {
      int err;
      if (error)
        {
          err = -4;
          if (g_error_matches (error,
                               G_MARKUP_ERROR,
                               G_MARKUP_ERROR_UNKNOWN_ELEMENT))
            tracef ("   client error: G_MARKUP_ERROR_UNKNOWN_ELEMENT\n");
          else if (g_error_matches (error,
                                    G_MARKUP_ERROR,
                                    G_MARKUP_ERROR_INVALID_CONTENT))
            {
              if (current_error)
                {
                  /* This is the return status for a forked child. */
                  forked = 2; /* Prevent further forking. */
                  g_error_free (error);
                  return current_error;
                }
              tracef ("   client error: G_MARKUP_ERROR_INVALID_CONTENT\n");
            }
          else if (g_error_matches (error,
                                    G_MARKUP_ERROR,
                                    G_MARKUP_ERROR_UNKNOWN_ATTRIBUTE))
            tracef ("   client error: G_MARKUP_ERROR_UNKNOWN_ATTRIBUTE\n");
          else
            err = -1;
          infof ("   Failed to parse client XML: %s\n", error->message);
          g_error_free (error);
        }
      else
        err = -1;
      return err;
    }

  if (response)
    *response = g_string_free (buffer, FALSE);
  else
    g_string_free (buffer, TRUE);

  if (forked)
    return 3;
  return 0;
}

/**
 * @brief Return whether the scanner is up.
 *
 * @return 1 if the scanner is available, else 0.
 */
short
scanner_is_up ()
{
  return scanner_up;
}

/**
 * @brief Return whether the scanner is active.
 *
 * @return 1 if the scanner is doing something that the manager
 *         must wait for, else 0.
 */
short
scanner_is_active ()
{
  return scanner_active;
}


/* OMP change processor. */

/**
 * @brief Deal with any changes caused by other processes.
 *
 * @return 0 success, 1 did something, -1 too little space in the scanner
 *         output buffer.
 */
int
process_omp_change ()
{
  return manage_check_current_task ();
}
