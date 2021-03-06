/* Copyright (c) 2015, 2017, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA */

#include <mysql/components/service_implementation.h>
#include <mysql/plugin.h>
#include <mysql_version.h>
#include <stddef.h>

#include "my_dbug.h"

/**
  @file test_services_plugin_registry.cc

  Test daemon plugin for the registry plugin service.

  @sa test_plugin_registry()
*/

/** Declare the interface to our own service. Usually comes from a svc header */
BEGIN_SERVICE_DEFINITION(test_services_plugin_registry_service)
  DECLARE_BOOL_METHOD(test1, (int a1, int a2, int *outres));
END_SERVICE_DEFINITION(test_services_plugin_registry_service);

/**
  Implementation of test1 method

  @param a1 first argument to add
  @param a2 second argument to add
  @param[out] outres the result of adding a1 to a2

  @retval 0         success
  @retval non-zero  failure
*/
static DEFINE_BOOL_METHOD(test1, (int a1, int a2, int *outres))
{
  *outres= a1 + a2;
  return 0;
}

/** Our own service definition: a struct of function pointers */
static SERVICE_TYPE(test_services_plugin_registry_service) svc_def= {
  test1
};

/** @ref svc_def converted to a @ref my_h_service */
static my_h_service h_my_svc= (my_h_service) &svc_def;

/**
  Tests the plugin registry service

  The test consists of:
    - acquire the registry_registration service default implememtnation
    - register a new service implementation for the mysql_server component
    - acquire the newly registered service's default implementation
    - compare the service handles for equality
    - release the service handle for our service
    - unregister our service
    - release the handle to the registry registration service

  Error messages are written to the server's error log.
  In case of success writes a single information message to the server's log.

  @retval false  success
  @retval true   failure
*/
static bool test_plugin_registry(MYSQL_PLUGIN p)
{
  bool result= false;
  SERVICE_TYPE(registry) *r= mysql_plugin_registry_acquire();
  my_h_service h_reg= NULL;
  my_h_service h_ret_svc= NULL;
  int int_result= -1;
  SERVICE_TYPE(registry_registration) *reg= NULL;
  SERVICE_TYPE(test_services_plugin_registry_service) *ret;

  enum { IDLE, REG_ACQUIRED, MY_SVC_REGISTERED,
         MY_SVC_ACQUIRED } state= IDLE;

  if (!r)
  {
    my_plugin_log_message(&p, MY_ERROR_LEVEL, "mysql_plugin_registry_acquire() returns empty");
    return true;
  }

  if (r->acquire("registry_registration", &h_reg))
  {
    my_plugin_log_message(&p, MY_ERROR_LEVEL, "finding registry_register failed");
    result= true;
    goto done;
  }

  if (!h_reg)
  {
    my_plugin_log_message(&p, MY_ERROR_LEVEL, "empty registry_query returned");
    result= true;
    goto done;
  }

  reg= reinterpret_cast<SERVICE_TYPE(registry_registration) *>(h_reg);

  state= REG_ACQUIRED;

  if (reg->register_service(
    "test_services_plugin_registry_service.mysql_server",
    h_my_svc))
  {
    my_plugin_log_message(&p, MY_ERROR_LEVEL, "can't register my new service");
    result= true;
    goto done;
  }

/* Register an already restistered service: Fail */
  if (reg->register_service(
    "test_services_plugin_registry_service.mysql_server",
    h_my_svc))
  {
    my_plugin_log_message(&p, MY_INFORMATION_LEVEL, "new service already registered");
  }

  state= MY_SVC_REGISTERED;

  if (r->acquire("test_services_plugin_registry_service", &h_ret_svc))
  {
    my_plugin_log_message(&p, MY_ERROR_LEVEL,
                          "can't find the newly registered service");
    result= true;
    goto done;
  }

  state= MY_SVC_ACQUIRED;

/* Aquire an already aquired service: Succeed (ignored) */
  if (r->acquire("test_services_plugin_registry_service", &h_ret_svc))
  {
    my_plugin_log_message(&p,  MY_INFORMATION_LEVEL,
                          "newly registered service already aquired");
  }

  state= MY_SVC_ACQUIRED;

  if (h_ret_svc != h_my_svc)
  {
    my_plugin_log_message(&p, MY_ERROR_LEVEL,
                          "Different service handle returned");
    result= true;
    goto done;
  }

  ret= reinterpret_cast<
    SERVICE_TYPE(test_services_plugin_registry_service) *>(h_ret_svc);

  if (ret->test1(1, 2, &int_result))
  {
    my_plugin_log_message(&p, MY_ERROR_LEVEL,
                          "results don't match: received %d", int_result);
    result= true;
    goto done;
  }

  if (r->release(h_ret_svc))
  {
    my_plugin_log_message(&p, MY_ERROR_LEVEL, "can't release my service");
    result= true;
    goto done;
  }

/* Release an already released service: Succeed (ignored) */
  if (r->release(h_ret_svc))
  {
    my_plugin_log_message(&p,  MY_INFORMATION_LEVEL, "my service already released");
  }

  state= MY_SVC_REGISTERED;

  if (reg->unregister("test_services_plugin_registry_service.mysql_server"))
  {
    my_plugin_log_message(&p, MY_ERROR_LEVEL, "can't unregister my service");
    result= true;
    goto done;
  }

/* Unregister an already unregistered service: Fail */
  if (reg->unregister("test_services_plugin_registry_service.mysql_server"))
  {
    my_plugin_log_message(&p,  MY_INFORMATION_LEVEL, "my service aleady unregistered");
  }

  state= REG_ACQUIRED;

  if (r->release(h_reg))
  {
    my_plugin_log_message(&p, MY_ERROR_LEVEL, "can't release registry_registration");
    result= true;
    goto done;
  }

  state= IDLE;

  my_plugin_log_message(&p, MY_INFORMATION_LEVEL, "test_plugin_registry succeeded");

done:
  switch (state)
  {
  case MY_SVC_ACQUIRED:
    r->release(h_ret_svc);
    /* fall through */
  case MY_SVC_REGISTERED:
    reg->unregister("test_services_plugin_registry_service.mysql_server");
    /* fall through */
  case REG_ACQUIRED:
    r->release(h_reg);
    /* fall through */
  case IDLE:
  default:
    mysql_plugin_registry_release(r);
  }
  return result;
}


/**
  Initialize the test services at server start or plugin installation.

  Call the test service.

  @retval 0 success
  @retval 1 failure
*/

static int test_services_plugin_init(void *p)
{
  DBUG_ENTER("test_services_plugin_init");
  int rc;

  rc= test_plugin_registry(reinterpret_cast<MYSQL_PLUGIN>(p)) ? 1 : 0;

  DBUG_RETURN(rc);
}


/**
  Terminate the test services at server shutdown or plugin deinstallation.

  Does nothing

  @retval 0 success
  @retval 1 failure
*/

static int test_services_plugin_deinit(void*)
{
  DBUG_ENTER("test_services_plugin_deinit");
  DBUG_RETURN(0);
}


static struct st_mysql_daemon test_services_plugin_registry=
{ MYSQL_DAEMON_INTERFACE_VERSION  };

/**
  test_services_plugin_registry descriptor
*/

mysql_declare_plugin(test_services_plugin_registry)
{
  MYSQL_DAEMON_PLUGIN,
  &test_services_plugin_registry,
  "test_services_plugin_registry",
  "Oracle",
  "test the plugin registry services",
  PLUGIN_LICENSE_GPL,
  test_services_plugin_init, /* Plugin Init */
  test_services_plugin_deinit, /* Plugin Deinit */
  0x0100 /* 1.0 */,
  NULL,                       /* status variables                */
  NULL,                       /* system variables                */
  NULL,                       /* config options                  */
  0,                          /* flags                           */
}
mysql_declare_plugin_end;
