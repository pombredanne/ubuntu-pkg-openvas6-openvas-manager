/* OpenVAS Manager
 * $Id$
 * Description: Manager Manage library: SQL helpers.
 *
 * Authors:
 * Matthew Mundell <matthew.mundell@greenbone.net>
 *
 * Copyright:
 * Copyright (C) 2009,2010,2012 Greenbone Networks GmbH
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

#define _XOPEN_SOURCE /* Glibc2 needs this for strptime. */

#include "sql.h"
#include "tracef.h"

#include <assert.h>
#include <string.h>
#include <time.h>

#include <openvas/misc/openvas_uuid.h>


/* Headers of manage_sql.c function also used here. */

gchar*
clean_hosts (const char *, int *);

char *
iso_time (time_t *);

int
manage_max_hosts (const char *);

long
current_offset (const char *);


/* Variables */

/**
 * @brief Handle on the database.
 */
sqlite3* task_db;


/* Helpers. */

/**
 * @brief Quotes a string of a known length to be passed to sql statements.
 *
 * @param[in]  string  String to quote.
 * @param[in]  length  Size of \p string.
 *
 * @return Freshly allocated, quoted string. Free with g_free.
 */
gchar*
sql_nquote (const char* string, size_t length)
{
  gchar *new, *new_start;
  const gchar *start, *end;
  int count = 0;

  assert (string);

  /* Count number of apostrophes. */

  start = string;
  while ((start = strchr (start, '\''))) start++, count++;

  /* Allocate new string. */

  new = new_start = g_malloc0 (length + count + 1);

  /* Copy string, replacing apostrophes with double apostrophes. */

  start = string;
  end = string + length;
  for (; start < end; start++, new++)
    {
      char ch = *start;
      if (ch == '\'')
        {
          *new = '\'';
          new++;
          *new = '\'';
        }
      else
        *new = ch;
    }

  return new_start;
}

/**
 * @brief Quotes a string to be passed to sql statements.
 *
 * @param[in]  string  String to quote, has to be \\0 terminated.
 *
 * @return Freshly allocated, quoted string. Free with g_free.
 */
gchar*
sql_quote (const char* string)
{
  assert (string);
  return sql_nquote (string, strlen (string));
}

/**
 * @brief Get the SQL insert expression for a string.
 *
 * @param[in]  string  The string, which may be NULL.
 *
 * @return Freshly allocated expression suitable for an INSERT statement,
 *         including SQL quotation marks.
 */
gchar *
sql_insert (const char *string)
{
  if (string)
    {
      gchar *quoted_value = sql_quote (string);
      gchar *insert = g_strdup_printf ("'%s'", quoted_value);
      g_free (quoted_value);
      return insert;
    }
  return g_strdup ("NULL");
}

/**
 * @brief Perform an SQL statement.
 *
 * @param[in]  retry  Whether to keep retrying while database is busy or locked.
 * @param[in]  sql    Format string for SQL statement.
 * @param[in]  args   Arguments for format string.
 */
static void
sqlv (int retry, char* sql, va_list args)
{
  const char* tail;
  int ret, retries;
  sqlite3_stmt* stmt;
  gchar* formatted;

  formatted = g_strdup_vprintf (sql, args);

  tracef ("   sql: %s\n", formatted);

  /* Prepare statement. */

  retries = 10;
  while (1)
    {
      ret = sqlite3_prepare (task_db, (char*) formatted, -1, &stmt, &tail);
      if (ret == SQLITE_BUSY || ret == SQLITE_LOCKED)
        {
          if (retry)
            continue;
          if (retries--)
            continue;
          return;
        }
      g_free (formatted);
      if (ret == SQLITE_OK)
        {
          if (stmt == NULL)
            {
              g_warning ("%s: sqlite3_prepare failed with NULL stmt: %s\n",
                         __FUNCTION__,
                         sqlite3_errmsg (task_db));
              abort ();
            }
          break;
        }
      g_warning ("%s: sqlite3_prepare failed: %s\n",
                 __FUNCTION__,
                 sqlite3_errmsg (task_db));
      abort ();
    }

  /* Run statement. */

  retries = 10;
  while (1)
    {
      ret = sqlite3_step (stmt);
      if (ret == SQLITE_BUSY)
        {
          if (retry)
            continue;
          if (retries--)
            continue;
          sqlite3_finalize (stmt);
          return;
        }
      if (ret == SQLITE_DONE) break;
      if (ret == SQLITE_ERROR || ret == SQLITE_MISUSE)
        {
          if (ret == SQLITE_ERROR)
            {
              ret = sqlite3_reset (stmt);
              if (ret == SQLITE_BUSY || ret == SQLITE_LOCKED)
                {
                  if (retry)
                    continue;
                  sqlite3_finalize (stmt);
                  return;
                }
            }
          g_warning ("%s: sqlite3_step failed: %s\n",
                     __FUNCTION__,
                     sqlite3_errmsg (task_db));
          abort ();
        }
    }

  sqlite3_finalize (stmt);
}

/**
 * @brief Perform an SQL statement, retrying if database is busy or locked.
 *
 * @param[in]  sql    Format string for SQL statement.
 * @param[in]  ...    Arguments for format string.
 */
void
sql (char* sql, ...)
{
  va_list args;

  va_start (args, sql);
  sqlv (1, sql, args);
  va_end (args);
}

/**
 * @brief Perform an SQL statement, giving up if database is busy or locked.
 *
 * @param[in]  sql    Format string for SQL statement.
 * @param[in]  ...    Arguments for format string.
 */
void
sql_giveup (char* sql, ...)
{
  va_list args;

  va_start (args, sql);
  sqlv (0, sql, args);
  va_end (args);
}

/**
 * @brief Perform an SQL statement, without logging.
 *
 * @param[in]  sql    Format string for SQL statement.
 * @param[in]  ...    Arguments for format string.
 */
void
sql_quiet (char* sql, ...)
{
  const char* tail;
  int ret;
  sqlite3_stmt* stmt;
  va_list args;
  gchar* formatted;

  va_start (args, sql);
  formatted = g_strdup_vprintf (sql, args);
  va_end (args);

  /* Prepare statement. */

  while (1)
    {
      ret = sqlite3_prepare (task_db, (char*) formatted, -1, &stmt, &tail);
      if (ret == SQLITE_BUSY) continue;
      g_free (formatted);
      if (ret == SQLITE_OK)
        {
          if (stmt == NULL)
            {
              g_warning ("%s: sqlite3_prepare failed with NULL stmt: %s\n",
                         __FUNCTION__,
                         sqlite3_errmsg (task_db));
              abort ();
            }
          break;
        }
      g_warning ("%s: sqlite3_prepare failed: %s\n",
                 __FUNCTION__,
                 sqlite3_errmsg (task_db));
      abort ();
    }

  /* Run statement. */

  while (1)
    {
      ret = sqlite3_step (stmt);
      if (ret == SQLITE_BUSY) continue;
      if (ret == SQLITE_DONE) break;
      if (ret == SQLITE_ERROR || ret == SQLITE_MISUSE)
        {
          if (ret == SQLITE_ERROR) ret = sqlite3_reset (stmt);
          g_warning ("%s: sqlite3_step failed: %s\n",
                     __FUNCTION__,
                     sqlite3_errmsg (task_db));
          abort ();
        }
    }

  sqlite3_finalize (stmt);
}

/**
 * @brief Get a particular cell from a SQL query.
 *
 * @param[in]   col          Column.
 * @param[in]   row          Row.
 * @param[in]   sql          Format string for SQL query.
 * @param[in]   args         Arguments for format string.
 * @param[out]  stmt_return  Return from statement.
 *
 * @return 0 success, 1 too few rows, -1 error.
 */
int
sql_x (/*@unused@*/ unsigned int col, unsigned int row, char* sql,
       va_list args, sqlite3_stmt** stmt_return)
{
  const char* tail;
  int ret;
  sqlite3_stmt* stmt;
  gchar* formatted;

  //va_start (args, sql);
  formatted = g_strdup_vprintf (sql, args);
  //va_end (args);

  tracef ("   sql_x: %s\n", formatted);

  /* Prepare statement. */

  while (1)
    {
      ret = sqlite3_prepare (task_db, (char*) formatted, -1, &stmt, &tail);
      if (ret == SQLITE_BUSY) continue;
      g_free (formatted);
      *stmt_return = stmt;
      if (ret == SQLITE_OK)
        {
          if (stmt == NULL)
            {
              g_warning ("%s: sqlite3_prepare failed with NULL stmt: %s",
                         __FUNCTION__,
                         sqlite3_errmsg (task_db));
              return -1;
            }
          break;
        }
      g_warning ("%s: sqlite3_prepare failed: %s",
                 __FUNCTION__,
                 sqlite3_errmsg (task_db));
      return -1;
    }

  /* Run statement. */

  while (1)
    {
      ret = sqlite3_step (stmt);
      if (ret == SQLITE_BUSY) continue;
      if (ret == SQLITE_DONE)
        {
          return 1;
        }
      if (ret == SQLITE_ERROR || ret == SQLITE_MISUSE)
        {
          if (ret == SQLITE_ERROR) ret = sqlite3_reset (stmt);
          g_warning ("%s: sqlite3_step failed: %s",
                     __FUNCTION__,
                     sqlite3_errmsg (task_db));
          return -1;
        }
      if (row == 0) break;
      row--;
      tracef ("   sql_x row %i\n", row);
    }

  tracef ("   sql_x end\n");
  return 0;
}

/**
 * @brief Get a particular cell from a SQL query, as a double.
 *
 * @warning Aborts on invalid queries.
 *
 * @warning Aborts when the query returns fewer rows than \p row.  The
 *          caller must ensure that the query will return sufficient rows.
 *
 * @param[in]  col    Column.
 * @param[in]  row    Row.
 * @param[in]  sql    Format string for SQL query.
 * @param[in]  ...    Arguments for format string.
 *
 * @return Result of the query as an integer.
 */
double
sql_double (unsigned int col, unsigned int row, char* sql, ...)
{
  sqlite3_stmt* stmt;
  va_list args;
  double ret;

  int sql_x_ret;
  va_start (args, sql);
  sql_x_ret = sql_x (col, row, sql, args, &stmt);
  va_end (args);
  if (sql_x_ret)
    {
      sqlite3_finalize (stmt);
      abort ();
    }
  ret = sqlite3_column_double (stmt, col);
  sqlite3_finalize (stmt);
  return ret;
}

/**
 * @brief Get a particular cell from a SQL query, as an int.
 *
 * @warning Aborts on invalid queries.
 *
 * @warning Aborts when the query returns fewer rows than \p row.  The
 *          caller must ensure that the query will return sufficient rows.
 *
 * @param[in]  col    Column.
 * @param[in]  row    Row.
 * @param[in]  sql    Format string for SQL query.
 * @param[in]  ...    Arguments for format string.
 *
 * @return Result of the query as an integer.
 */
int
sql_int (unsigned int col, unsigned int row, char* sql, ...)
{
  sqlite3_stmt* stmt;
  va_list args;
  int ret;

  int sql_x_ret;
  va_start (args, sql);
  sql_x_ret = sql_x (col, row, sql, args, &stmt);
  va_end (args);
  if (sql_x_ret)
    {
      sqlite3_finalize (stmt);
      abort ();
    }
  ret = sqlite3_column_int (stmt, col);
  sqlite3_finalize (stmt);
  return ret;
}

/**
 * @brief Get a particular cell from a SQL query, as an string.
 *
 * @param[in]  col    Column.
 * @param[in]  row    Row.
 * @param[in]  sql    Format string for SQL query.
 * @param[in]  ...    Arguments for format string.
 *
 * @return Freshly allocated string containing the result, NULL otherwise.
 *         NULL means that either the selected value was NULL or there were
 *         fewer rows in the result than \p row.
 */
char*
sql_string (unsigned int col, unsigned int row, char* sql, ...)
{
  sqlite3_stmt* stmt;
  const unsigned char* ret2;
  char* ret;
  int sql_x_ret;

  va_list args;
  va_start (args, sql);
  sql_x_ret = sql_x (col, row, sql, args, &stmt);
  va_end (args);
  if (sql_x_ret)
    {
      sqlite3_finalize (stmt);
      return NULL;
    }
  ret2 = sqlite3_column_text (stmt, col);
  ret = g_strdup ((char*) ret2);
  sqlite3_finalize (stmt);
  return ret;
}

/**
 * @brief Get a particular cell from a SQL query, as an int64.
 *
 * @param[in]  ret    Return value.
 * @param[in]  col    Column.
 * @param[in]  row    Row.
 * @param[in]  sql    Format string for SQL query.
 * @param[in]  ...    Arguments for format string.
 *
 * @return 0 success, 1 too few rows, -1 error.
 */
int
sql_int64 (long long int* ret, unsigned int col, unsigned int row, char* sql, ...)
{
  sqlite3_stmt* stmt;
  int sql_x_ret;
  va_list args;

  va_start (args, sql);
  sql_x_ret = sql_x (col, row, sql, args, &stmt);
  va_end (args);
  switch (sql_x_ret)
    {
      case  0:
        break;
      case  1:
        sqlite3_finalize (stmt);
        return 1;
        break;
      default:
        assert (0);
        /* Fall through. */
      case -1:
        sqlite3_finalize (stmt);
        return -1;
        break;
    }
  *ret = sqlite3_column_int64 (stmt, col);
  sqlite3_finalize (stmt);
  return 0;
}

/**
 * @brief Make a UUID.
 *
 * This is a callback for a scalar SQL function of zero arguments.
 *
 * @param[in]  context  SQL context.
 * @param[in]  argc     Number of arguments.
 * @param[in]  argv     Argument array.
 */
void
sql_make_uuid (sqlite3_context *context, int argc, sqlite3_value** argv)
{
  char *uuid;

  assert (argc == 0);

  uuid = openvas_uuid_make ();
  if (uuid == NULL)
    {
      sqlite3_result_error (context, "Failed to create UUID", -1);
      return;
    }

  sqlite3_result_text (context, uuid, -1, free);
}

/**
 * @brief Check if a host list contains a host
 *
 * This is a callback for a scalar SQL function of two arguments.
 *
 * @param[in]  context  SQL context.
 * @param[in]  argc     Number of arguments.
 * @param[in]  argv     Argument array.
 */
void
sql_hosts_contains (sqlite3_context *context, int argc, sqlite3_value** argv)
{
  gchar **split, **point, *stripped_host;
  const unsigned char *hosts, *host;

  assert (argc == 2);

  hosts = sqlite3_value_text (argv[0]);
  if (hosts == NULL)
    {
      sqlite3_result_error (context, "Failed to get hosts argument", -1);
      return;
    }

  host = sqlite3_value_text (argv[1]);
  if (host == NULL)
    {
      sqlite3_result_error (context, "Failed to get host argument", -1);
      return;
    }

  stripped_host = g_strstrip (g_strdup ((gchar*) host));
  split = g_strsplit ((gchar*) hosts, ",", 0);
  point = split;
  while (*point)
    {
      if (strcmp (g_strstrip (*point), stripped_host) == 0)
        {
          g_strfreev (split);
          g_free (stripped_host);
          sqlite3_result_int (context, 1);
          return;
        }
      point++;
    }
  g_strfreev (split);
  g_free (stripped_host);

  sqlite3_result_int (context, 0);
}

/**
 * @brief Clean a host list.
 *
 * This is a callback for a scalar SQL function of one argument.
 *
 * @param[in]  context  SQL context.
 * @param[in]  argc     Number of arguments.
 * @param[in]  argv     Argument array.
 */
void
sql_clean_hosts (sqlite3_context *context, int argc, sqlite3_value** argv)
{
  const unsigned char *hosts;
  gchar *clean;

  assert (argc == 1);

  hosts = sqlite3_value_text (argv[0]);
  if (hosts == NULL)
    {
      sqlite3_result_error (context, "Failed to get hosts argument", -1);
      return;
    }

  clean = clean_hosts ((gchar*) hosts, NULL);
  sqlite3_result_text (context, clean, -1, SQLITE_TRANSIENT);
  g_free (clean);
}

/**
 * @brief Make a name unique.
 *
 * This is a callback for a scalar SQL function of three argument.
 *
 * It's up to the caller to ensure there is a read-only transaction.
 *
 * @param[in]  context  SQL context.
 * @param[in]  argc     Number of arguments.
 * @param[in]  argv     Argument array.
 */
void
sql_uniquify (sqlite3_context *context, int argc, sqlite3_value** argv)
{
  const unsigned char *proposed_name, *type, *suffix;
  gchar *candidate_name, *quoted_candidate_name;
  unsigned int number;
  sqlite3_int64 owner;

  assert (argc == 4);

  type = sqlite3_value_text (argv[0]);
  if (type == NULL)
    {
      sqlite3_result_error (context, "Failed to get type argument", -1);
      return;
    }

  proposed_name = sqlite3_value_text (argv[1]);
  if (proposed_name == NULL)
    {
      sqlite3_result_error (context,
                            "Failed to get proposed name argument",
                            -1);
      return;
    }

  owner = sqlite3_value_int64 (argv[2]);

  suffix = sqlite3_value_text (argv[3]);
  if (suffix == NULL)
    {
      sqlite3_result_error (context,
                            "Failed to get suffix argument",
                            -1);
      return;
    }

  number = 0;
  candidate_name = g_strdup_printf ("%s%s %i", proposed_name, suffix,
                                    ++number);
  quoted_candidate_name = sql_quote (candidate_name);

  while (sql_int (0, 0,
                  "SELECT COUNT (*) FROM %ss WHERE name = '%s'"
                   " AND ((owner IS NULL) OR (owner = %llu));",
                  type,
                  quoted_candidate_name,
                  owner))
    {
      g_free (candidate_name);
      g_free (quoted_candidate_name);
      candidate_name = g_strdup_printf ("%s%s %u", proposed_name, suffix,
                                        ++number);
      quoted_candidate_name = sql_quote (candidate_name);
    }

  g_free (quoted_candidate_name);

  sqlite3_result_text (context, candidate_name, -1, SQLITE_TRANSIENT);
  g_free (candidate_name);
}

/**
 * @brief Convert an epoch time into a string in ISO format.
 *
 * This is a callback for a scalar SQL function of one argument.
 *
 * @param[in]  context  SQL context.
 * @param[in]  argc     Number of arguments.
 * @param[in]  argv     Argument array.
 */
void
sql_iso_time (sqlite3_context *context, int argc, sqlite3_value** argv)
{
  time_t epoch_time;

  assert (argc == 1);

  epoch_time = sqlite3_value_int (argv[0]);
  if (epoch_time == 0)
    sqlite3_result_text (context, "", -1, SQLITE_TRANSIENT);
  else
    {
      const char *iso;

      iso = iso_time (&epoch_time);
      if (iso)
        sqlite3_result_text (context, iso, -1, SQLITE_TRANSIENT);
      else
        sqlite3_result_error (context, "Failed to format time", -1);
    }
}

/**
 * @brief Try convert an OTP NVT tag time string into epoch time.
 *
 * This is a callback for a scalar SQL function of one argument.
 *
 * @param[in]  context  SQL context.
 * @param[in]  argc     Number of arguments.
 * @param[in]  argv     Argument array.
 */
void
sql_parse_time (sqlite3_context *context, int argc, sqlite3_value** argv)
{
  const unsigned char *string;
  int epoch_time, offset;
  struct tm tm;

  assert (argc == 1);

  string = sqlite3_value_text (argv[0]);

  if ((strcmp ((char*) string, "") == 0)
      || (strcmp ((char*) string, "$Date: $") == 0)
      || (strcmp ((char*) string, "$Date$") == 0)
      || (strcmp ((char*) string, "$Date:$") == 0)
      || (strcmp ((char*) string, "$Date") == 0)
      || (strcmp ((char*) string, "$$") == 0))
    {
      sqlite3_result_int (context, 0);
      return;
    }

  /* Parse the time. */

  /* 2011-08-09 08:20:34 +0200 (Tue, 09 Aug 2011) */
  /* $Date: 2012-02-17 16:05:26 +0100 (Fr, 17. Feb 2012) $ */
  /* $Date: Fri, 11 Nov 2011 14:42:28 +0100 $ */
  if ((strptime ((char*) string, "%F %T %z", &tm) == NULL)
      && (strptime ((char*) string, "$Date: %F %T %z", &tm) == NULL)
      && (strptime ((char*) string, "%a %b %d %T %Y %z", &tm) == NULL)
      && (strptime ((char*) string, "$Date: %a, %d %b %Y %T %z", &tm) == NULL)
      && (strptime ((char*) string, "$Date: %a %b %d %T %Y %z", &tm) == NULL))
    {
      g_warning ("%s: Failed to parse time: %s", __FUNCTION__, string);
      sqlite3_result_int (context, 0);
      return;
    }
  epoch_time = mktime (&tm);
  if (epoch_time == -1)
    {
      g_warning ("%s: Failed to make time: %s", __FUNCTION__, string);
      sqlite3_result_int (context, 0);
      return;
    }

  /* Get the timezone offset from the string. */

  if ((sscanf ((char*) string, "%*u-%*u-%*u %*u:%*u:%*u %d%*[^]]", &offset)
               != 1)
      && (sscanf ((char*) string, "$Date: %*u-%*u-%*u %*u:%*u:%*u %d%*[^]]",
                  &offset)
          != 1)
      && (sscanf ((char*) string, "%*s %*s %*s %*u:%*u:%*u %*u %d%*[^]]",
                  &offset)
          != 1)
      && (sscanf ((char*) string,
                  "$Date: %*s %*s %*s %*u %*u:%*u:%*u %d%*[^]]",
                  &offset)
          != 1)
      && (sscanf ((char*) string, "$Date: %*s %*s %*s %*u:%*u:%*u %*u %d%*[^]]",
                  &offset)
          != 1))
    {
      g_warning ("%s: Failed to parse timezone offset: %s", __FUNCTION__,
                 string);
      sqlite3_result_int (context, 0);
      return;
    }

  /* Use the offset to convert to UTC. */

  if (offset < 0)
    {
      epoch_time += ((-offset) / 100) * 60 * 60;
      epoch_time += ((-offset) % 100) * 60;
    }
  else if (offset > 0)
    {
      epoch_time -= (offset / 100) * 60 * 60;
      epoch_time -= (offset % 100) * 60;
    }

  sqlite3_result_int (context, epoch_time);
  return;
}

/**
 * @brief Get the current time as an epoch integer.
 *
 * This is a callback for a scalar SQL function of zero arguments.
 *
 * @param[in]  context  SQL context.
 * @param[in]  argc     Number of arguments.
 * @param[in]  argv     Argument array.
 */
void
sql_now (sqlite3_context *context, int argc, sqlite3_value** argv)
{
  assert (argc == 0);
  sqlite3_result_int (context, time (NULL));
}

/**
 * @brief Extract a tag from an OTP tag list.
 *
 * This is a callback for a scalar SQL function of two arguments.
 *
 * @param[in]  context  SQL context.
 * @param[in]  argc     Number of arguments.
 * @param[in]  argv     Argument array.
 */
void
sql_tag (sqlite3_context *context, int argc, sqlite3_value** argv)
{
  const char *tags, *tag;
  gchar **split, **point;

  assert (argc == 2);

  tags = (char*) sqlite3_value_text (argv[0]);
  if (tags == NULL)
    {
      sqlite3_result_error (context, "Failed to get tags argument", -1);
      return;
    }

  tag = (char*) sqlite3_value_text (argv[1]);
  if (tag == NULL)
    {
      sqlite3_result_error (context, "Failed to get tag argument", -1);
      return;
    }

  /* creation_date=2009-04-09 14:18:58 +0200 (Thu, 09 Apr 2009)|... */

  split = g_strsplit (tags, "|", 0);
  point = split;

  while (*point)
    {
      if ((strlen (*point) > strlen (tag))
          && (strncmp (*point, tag, strlen (tag)) == 0)
          && ((*point)[strlen (tag)] == '='))
        {
          sqlite3_result_text (context, *point + strlen (tag) + 1, -1,
                               SQLITE_TRANSIENT);
          g_strfreev (split);
          return;
        }
      point++;
    }
  g_strfreev (split);

  sqlite3_result_text (context, "", -1, SQLITE_TRANSIENT);
  return;
}

/**
 * @brief Return number of hosts.
 *
 * This is a callback for a scalar SQL function of one argument.
 *
 * @param[in]  context  SQL context.
 * @param[in]  argc     Number of arguments.
 * @param[in]  argv     Argument array.
 */
void
sql_max_hosts (sqlite3_context *context, int argc, sqlite3_value** argv)
{
  const unsigned char *hosts;
  gchar *max;

  assert (argc == 1);

  hosts = sqlite3_value_text (argv[0]);
  if (hosts == NULL)
    {
      /* Seems this happens when the query result is empty. */
      sqlite3_result_text (context, "0", -1, SQLITE_TRANSIENT);
      return;
    }

  max = g_strdup_printf ("%i", manage_max_hosts ((gchar*) hosts));
  sqlite3_result_text (context, max, -1, SQLITE_TRANSIENT);
  g_free (max);
}

/**
 * @brief Move data from a table to a new table, heeding column rename.
 *
 * @param[in]  old_table  Existing table.
 * @param[in]  new_table  New empty table with renamed column.
 * @param[in]  old_name   Name of column in old table.
 * @param[in]  new_name   Name of column in new table.
 */
void
sql_rename_column (const char *old_table, const char *new_table,
                   const char *old_name, const char *new_name)
{
  iterator_t rows;

  /* Get a row with all columns. */

  init_iterator (&rows, "SELECT * FROM %s LIMIT 1;", old_table);
  if (next (&rows))
    {
      GString *one, *two;
      int end, column, first;

      /* Build the INSERT query from the column names in the row. */

      one = g_string_new ("");
      g_string_append_printf (one, "INSERT INTO %s (", new_table);

      two = g_string_new (") SELECT ");

      end = iterator_column_count (&rows);
      first = 1;
      for (column = 0; column < end; column++)
        {
          const char *name;
          name = iterator_column_name (&rows, column);
          g_string_append_printf (one, "%s%s",
                                  (first ? "" : ", "),
                                  (strcmp (name, old_name) == 0
                                    ? new_name
                                    : name));
          if (first)
            first = 0;
          else
            g_string_append (two, ", ");
          g_string_append (two, name);
        }
      cleanup_iterator (&rows);

      g_string_append_printf (one, "%s FROM %s;", two->str, old_table);

      /* Run the INSERT query. */

      sql (one->str);

      g_string_free (one, TRUE);
      g_string_free (two, TRUE);
    }
  else
    cleanup_iterator (&rows);
}

/**
 * @brief Check if a host list contains a host
 *
 * This is a callback for a scalar SQL function of two arguments.
 *
 * @param[in]  context  SQL context.
 * @param[in]  argc     Number of arguments.
 * @param[in]  argv     Argument array.
 */
void
sql_common_cve (sqlite3_context *context, int argc, sqlite3_value** argv)
{
  gchar **split_1, **split_2, **point_1, **point_2;
  const unsigned char *cve1, *cve2;

  assert (argc == 2);

  tracef ("   %s: top\n", __FUNCTION__);

  cve1 = sqlite3_value_text (argv[0]);
  if (cve1 == NULL)
    {
      sqlite3_result_error (context, "Failed to get first CVE argument", -1);
      return;
    }

  cve2 = sqlite3_value_text (argv[1]);
  if (cve2 == NULL)
    {
      sqlite3_result_error (context, "Failed to get second CVE argument", -1);
      return;
    }

  split_1 = g_strsplit ((gchar*) cve1, ",", 0);
  split_2 = g_strsplit ((gchar*) cve2, ",", 0);
  point_1 = split_1;
  point_2 = split_2;
  while (*point_1)
    {
      while (*point_2)
        {
          tracef ("   %s: %s vs %s\n", __FUNCTION__, g_strstrip (*point_1), g_strstrip (*point_2));
          if (strcmp (g_strstrip (*point_1), g_strstrip (*point_2)) == 0)
            {
              g_strfreev (split_1);
              g_strfreev (split_2);
              sqlite3_result_int (context, 1);
              return;
            }
          point_2++;
        }
      point_1++;
    }
  g_strfreev (split_1);
  g_strfreev (split_2);

  sqlite3_result_int (context, 0);
}

/**
 * @brief Get the offset from UTC of the current time for a timezone.
 *
 * This is a callback for a scalar SQL function of one argument.
 *
 * @param[in]  context  SQL context.
 * @param[in]  argc     Number of arguments.
 * @param[in]  argv     Argument array.
 */
void
sql_current_offset (sqlite3_context *context, int argc, sqlite3_value** argv)
{
  assert (argc == 1);
  sqlite3_result_int
   (context,
    (int) current_offset ((const char *) sqlite3_value_text (argv[0])));
}

/**
 * @brief Calculate the trend of a task.
 *
 * This is a callback for a scalar SQL function of two argument.
 *
 * @param[in]  context  SQL context.
 * @param[in]  argc     Number of arguments.
 * @param[in]  argv     Argument array.
 */
void
sql_task_trend (sqlite3_context *context, int argc, sqlite3_value** argv)
{
  unsigned int overrides;
  task_t task;

  assert (argc == 2);

  task = sqlite3_value_int64 (argv[0]);
  if (task == 0)
    {
      sqlite3_result_text (context, "", -1, SQLITE_TRANSIENT);
      return;
    }

  overrides = sqlite3_value_int (argv[1]);

  sqlite3_result_text (context, task_trend (task, overrides), -1,
                       SQLITE_TRANSIENT);
}

/**
 * @brief Calculate the threat level of a task.
 *
 * This is a callback for a scalar SQL function of one argument.
 *
 * @param[in]  context  SQL context.
 * @param[in]  argc     Number of arguments.
 * @param[in]  argv     Argument array.
 */
void
sql_threat_level (sqlite3_context *context, int argc, sqlite3_value** argv)
{
  task_t task;
  report_t last_report;
  const char *threat;
  unsigned int overrides;

  assert (argc == 2);

  task = sqlite3_value_int64 (argv[0]);
  if (task == 0)
    {
      sqlite3_result_text (context, "", -1, SQLITE_TRANSIENT);
      return;
    }

  overrides = sqlite3_value_int (argv[1]);

  threat = task_threat_level (task, overrides);
  tracef ("   %s: %llu: %s\n", __FUNCTION__, task, threat);
  if (threat)
    {
      sqlite3_result_text (context, threat, -1, SQLITE_TRANSIENT);
      return;
    }

  task_last_report (task, &last_report);
  if (last_report == 0)
    {
      sqlite3_result_text (context, "", -1, SQLITE_TRANSIENT);
      return;
    }

  sqlite3_result_text (context, "None", -1, SQLITE_TRANSIENT);
  return;
}

/**
 * @brief Get the name of a task run status.
 *
 * This is a callback for a scalar SQL function of one argument.
 *
 * @param[in]  context  SQL context.
 * @param[in]  argc     Number of arguments.
 * @param[in]  argv     Argument array.
 */
void
sql_run_status_name (sqlite3_context *context, int argc, sqlite3_value** argv)
{
  const char *name;
  int status;

  assert (argc == 1);

  status = sqlite3_value_int (argv[0]);

  name = run_status_name (status);
  sqlite3_result_text (context, name ? name : "", -1, SQLITE_TRANSIENT);
  return;
}


/* Iterators. */

/**
 * @brief Prepare a statement.
 *
 * @param[in]  sql       Format string for SQL.
 */
sqlite3_stmt *
sql_prepare (const char* sql, ...)
{
  int ret;
  const char* tail;
  sqlite3_stmt* stmt;
  va_list args;
  gchar* formatted;

  va_start (args, sql);
  formatted = g_strdup_vprintf (sql, args);
  va_end (args);

  tracef ("   sql: %s\n", formatted);

  stmt = NULL;
  while (1)
    {
      ret = sqlite3_prepare (task_db, formatted, -1, &stmt, &tail);
      if (ret == SQLITE_BUSY) continue;
      g_free (formatted);
      if (ret == SQLITE_OK)
        {
          if (stmt == NULL)
            {
              g_warning ("%s: sqlite3_prepare failed with NULL stmt: %s\n",
                         __FUNCTION__,
                         sqlite3_errmsg (task_db));
              abort ();
            }
          break;
        }
      g_warning ("%s: sqlite3_prepare failed: %s\n",
                 __FUNCTION__,
                 sqlite3_errmsg (task_db));
      abort ();
    }

  tracef ("   prepared as: %p\n", stmt);

  return stmt;
}

/**
 * @brief Initialise an iterator.
 *
 * @param[in]  iterator  Iterator.
 * @param[in]  stmt      Statement.
 */
void
init_prepared_iterator (iterator_t* iterator, sqlite3_stmt* stmt)
{
  iterator->done = FALSE;
  iterator->stmt = stmt;
  iterator->prepared = 1;
  iterator->crypt_ctx = NULL;
  tracef ("   sql: init prepared %p\n", stmt);
}

/**
 * @brief Initialise an iterator.
 *
 * @param[in]  iterator  Iterator.
 * @param[in]  sql       Format string for SQL.
 */
void
init_iterator (iterator_t* iterator, const char* sql, ...)
{
  int ret;
  const char* tail;
  sqlite3_stmt* stmt;
  va_list args;
  gchar* formatted;

  va_start (args, sql);
  formatted = g_strdup_vprintf (sql, args);
  va_end (args);

  tracef ("   sql: %s\n", formatted);

  iterator->done = FALSE;
  iterator->prepared = 0;
  iterator->crypt_ctx = NULL;
  while (1)
    {
      ret = sqlite3_prepare (task_db, formatted, -1, &stmt, &tail);
      if (ret == SQLITE_BUSY) continue;
      g_free (formatted);
      iterator->stmt = stmt;
      if (ret == SQLITE_OK)
        {
          if (stmt == NULL)
            {
              g_warning ("%s: sqlite3_prepare failed with NULL stmt: %s\n",
                         __FUNCTION__,
                         sqlite3_errmsg (task_db));
              abort ();
            }
          break;
        }
      g_warning ("%s: sqlite3_prepare failed: %s\n",
                 __FUNCTION__,
                 sqlite3_errmsg (task_db));
      abort ();
    }
}

/**
 * @brief Get an integer column from an iterator.
 *
 * @param[in]  iterator  Iterator.
 * @param[in]  col       Column offset.
 *
 * @return Value of given column.
 */
long long int
iterator_int64 (iterator_t* iterator, int col)
{
  if (iterator->done) abort ();
  return (long long int) sqlite3_column_int64 (iterator->stmt, col);
}

/**
 * @brief Get a string column from an iterator.
 *
 * @param[in]  iterator  Iterator.
 * @param[in]  col       Column offset.
 *
 * @return Value of given column.
 */
const char*
iterator_string (iterator_t* iterator, int col)
{
  if (iterator->done) abort ();
  return (const char*) sqlite3_column_text (iterator->stmt, col);
}

/**
 * @brief Get a column name from an iterator.
 *
 * @param[in]  iterator  Iterator.
 * @param[in]  col       Column offset.
 *
 * @return Name of given column.
 */
const char*
iterator_column_name (iterator_t* iterator, int col)
{
  if (iterator->done) abort ();
  return (const char*) sqlite3_column_name (iterator->stmt, col);
}

/**
 * @brief Get number of columns from an iterator.
 *
 * @param[in]  iterator  Iterator.
 *
 * @return Number of columns.
 */
int
iterator_column_count (iterator_t* iterator)
{
  if (iterator->done) abort ();
  return sqlite3_column_count (iterator->stmt);
}

/**
 * @brief Cleanup an iterator.
 *
 * @param[in]  iterator  Iterator.
 */
void
cleanup_iterator (iterator_t* iterator)
{
  if (iterator == NULL)
    {
      g_warning ("%s: null iterator pointer.\n", __FUNCTION__);
      return;
    }

  if (iterator->prepared == 0)
    sqlite3_finalize (iterator->stmt);
  if (iterator->crypt_ctx)
    {
      lsc_crypt_release (iterator->crypt_ctx);
      iterator->crypt_ctx = NULL;
    }
}

/**
 * @brief Increment an iterator.
 *
 * @param[in]  iterator  Iterator.
 *
 * @return TRUE if there was a next item, else FALSE.
 */
gboolean
next (iterator_t* iterator)
{
  int ret;

  if (iterator->done) return FALSE;

  lsc_crypt_flush (iterator->crypt_ctx);
  while ((ret = sqlite3_step (iterator->stmt)) == SQLITE_BUSY);
  if (ret == SQLITE_DONE)
    {
      iterator->done = TRUE;
      return FALSE;
    }
  if (ret == SQLITE_ERROR || ret == SQLITE_MISUSE)
    {
      if (ret == SQLITE_ERROR) ret = sqlite3_reset (iterator->stmt);
      g_warning ("%s: sqlite3_step failed: %s\n",
                 __FUNCTION__,
                 sqlite3_errmsg (task_db));
      abort ();
    }
  return TRUE;
}
