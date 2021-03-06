/**
 *       @file  serval-dna.c
 *      @brief  re-implementation of the serval-dna daemon as a
 *                commotiond plugin
 *
 *     @author  Dan Staples (dismantl), danstaples@opentechinstitute.org
 *
 *   @internal
 *     Created  12/18/2013
 *    Compiler  gcc/g++
 *     Company  The Open Technology Institute
 *   Copyright  Copyright (c) 2013, Dan Staples
 *
 * This file is part of Commotion, Copyright (c) 2013, Josh King 
 * 
 * Commotion is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published 
 * by the Free Software Foundation, either version 3 of the License, 
 * or (at your option) any later version.
 * 
 * Commotion is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with Commotion.  If not, see <http://www.gnu.org/licenses/>.
 *
 * =====================================================================================
 */

#include <stdbool.h>
#include <sys/types.h>
#include <dirent.h>

#include "serval-config.h"
#include <serval.h>
#include <serval/conf.h>
#include <serval/overlay_interface.h>

#include "debug.h"
#include "plugin.h"
#include "socket.h"
#include "loop.h"
#include "util.h"
#include "list.h"
#include "tree.h"
#include "profile.h"
#include "cmd.h"

#include "serval-dna.h"
#include "crypto.h"
#include "keyring.h"
#include "commands.h"

// Types & constructors

/* Extension type */
#define _alarm 42

#define IS_ALARM(J) (IS_EXT(J) && ((co_alarm_t *)J)->_exttype == _alarm)

typedef struct {
  co_obj_t _header;
  uint8_t _exttype;
  uint8_t _len;
  struct sched_ent *alarm;
} co_alarm_t;

static co_obj_t *co_alarm_create(struct sched_ent *alarm) {
  co_alarm_t *output = h_calloc(1,sizeof(co_alarm_t));
  CHECK_MEM(output);
  output->_header._type = _ext8;
  output->_exttype = _alarm;
  output->_len = (sizeof(co_alarm_t));
  output->alarm = alarm;
  return (co_obj_t*)output;
error:
  return NULL;
}

// Globals

extern keyring_file *keyring;  // Serval global
extern char *serval_path;

co_socket_t co_socket_proto = {};

static co_obj_t *sock_alarms = NULL;
static co_obj_t *timer_alarms = NULL;
bool serval_registered = false;
bool daemon_started = false;
svl_crypto_ctx *serval_dna_ctx = NULL;

// Private functions

/** Compares co_socket fd to serval alarm fd */
static co_obj_t *_alarm_fd_match_i(co_obj_t *alarms, co_obj_t *alarm, void *fd) {
  if(!IS_ALARM(alarm)) return NULL;
  const struct sched_ent *this_alarm = ((co_alarm_t*)alarm)->alarm;
  const int *this_fd = fd;
  if (this_alarm->poll.fd == *this_fd) return alarm;
  return NULL;
}

static co_obj_t *_alarm_ptr_match_i(co_obj_t *alarms, co_obj_t *alarm, void *ptr) {
  if(!IS_ALARM(alarm)) return NULL;
  const struct sched_ent *this_alarm = ((co_alarm_t*)alarm)->alarm;
  const void *this_ptr = ptr;
//   DEBUG("ALARM_PTR_MATCH: %p %p",this_alarm,this_ptr);
  if (this_alarm == this_ptr) return alarm;
  return NULL;
}

// Public functions

/** Callback function for when Serval socket has data to read */
int serval_socket_cb(co_obj_t *self, co_obj_t *context) {
//   DEBUG("SERVAL_SOCKET_CB");
  co_socket_t *sock = (co_socket_t*)self;
  co_obj_t *node = NULL;
  struct sched_ent *alarm = NULL;
  
  // find alarm associated w/ sock, call alarm->function(alarm)
  if ((node = co_list_parse(sock_alarms, _alarm_fd_match_i, &sock->fd->fd))) {
    alarm = ((co_alarm_t*)node)->alarm;
    alarm->poll.revents = sock->events;
    
    alarm->function(alarm); // Serval callback function associated with alarm/socket
    alarm->poll.revents = 0;
    
    return 1;
  }
  
  return 0;
}

int serval_timer_cb(co_obj_t *self, co_obj_t **output, co_obj_t *context) {
//   DEBUG("SERVAL_TIMER_CB");
  co_obj_t *timer = self;
  struct sched_ent *alarm = NULL;
  co_obj_t *alarm_node = NULL;
  
  // find alarm associated w/ timer, call alarm->function(alarm)
  CHECK((alarm_node = co_list_parse(timer_alarms, _alarm_ptr_match_i, ((co_timer_t*)timer)->ptr)),"Failed to find alarm for callback");

  alarm = ((co_alarm_t*)alarm_node)->alarm;
  co_list_delete(timer_alarms,alarm_node);
  co_obj_free(alarm_node);
  
  co_obj_free(timer);
  
  alarm->function(alarm); // Serval callback function associated with alarm/socket

  return 0;
error:
  return 1;
}

/** Overridden Serval function to schedule timed events */
int _schedule(struct __sourceloc __whence, struct sched_ent *alarm) {
//   DEBUG("###### SCHEDULE ######");
  co_obj_t *timer = NULL, *node = NULL;
  
  CHECK(alarm->function,"No callback function associated with timer");
  CHECK(!(node = co_list_parse(timer_alarms, _alarm_ptr_match_i, alarm)),"Trying to schedule duplicate alarm %p",alarm);
  
  time_ms_t now = gettime_ms();
  
  if (alarm->alarm == TIME_NEVER_WILL)
    return 0;
  
  if (alarm->deadline < alarm->alarm)
    alarm->deadline = alarm->alarm;
  
  CHECK(now - alarm->alarm <= 1000,"Alarm tried to schedule a deadline %ldms ago, from %s() %s:%d",
	 (now - alarm->deadline),
	 __whence.function,__whence.file,__whence.line);
  
  /** this messes up Serval's overlay interface setup */
  // if the alarm has already expired, execute callback function
//   if (alarm->deadline <= now) {
//     DEBUG("ALREADY EXPIRED, DEADLINE %lld %lld",alarm->alarm, gettime_ms());
//     alarm->function(alarm);
//     return 0;
//   }
  
//   DEBUG("NEW TIMER: ALARM %lld - %lld = %lld %p",alarm->alarm,now,alarm->alarm - now,alarm);
  CHECK(alarm->alarm - now < 86400000,"Timer deadline is more than 24 hrs from now, ignoring");

  time_ms_t deadline_time;
  struct timeval deadline;
//   if (alarm->alarm - now > 1)
    deadline_time = alarm->alarm;
//   else
//     deadline_time = alarm->deadline;

  deadline.tv_sec = deadline_time / 1000;
  deadline.tv_usec = (deadline_time % 1000) * 1000;
  if (deadline.tv_usec > 1000000) {
    deadline.tv_sec++;
    deadline.tv_usec %= 1000000;
  }

  CHECK_MEM((timer = co_timer_create(deadline, serval_timer_cb, alarm)));
  CHECK(co_loop_add_timer(timer,NULL),"Failed to add timer %ld.%06ld %p",deadline.tv_sec,deadline.tv_usec,alarm);
  co_obj_t *alarm_obj = co_alarm_create(alarm);
  CHECK_MEM(alarm_obj);
  CHECK(co_list_append(timer_alarms,alarm_obj),"Failed to add to timer_alarms");
  
  return 0;
  
error:
  if (timer) free(timer);
  return -1;
}

/** Overridden Serval function to unschedule timed events */
int _unschedule(struct __sourceloc __whence, struct sched_ent *alarm) {
//   DEBUG("###### UNSCHEDULE ######");
  co_obj_t *alarm_node = NULL, *timer = NULL;
  
//   CHECK((alarm_node = co_list_parse(timer_alarms, _alarm_ptr_match_i, alarm)),"Attempting to unschedule timer that is not scheduled");
  if (!(alarm_node = co_list_parse(timer_alarms, _alarm_ptr_match_i, alarm))) goto error;
  co_list_delete(timer_alarms,alarm_node);
  co_obj_free(alarm_node);
  
  // Get the timer associated with the alarm
  CHECK((timer = co_loop_get_timer(alarm,NULL)),"Could not find timer to remove");
  CHECK(co_loop_remove_timer(timer,NULL),"Failed to remove timer from event loop");
  co_obj_free(timer);
  
  return 0;
  
error:
  return -1;
}

/** Overridden Serval function to register sockets with event loop */
int _watch(struct __sourceloc __whence, struct sched_ent *alarm) {
//   DEBUG("OVERRIDDEN WATCH FUNCTION!");
  co_socket_t *sock = NULL;
  
  /** need to set:
   * 	sock->fd
   * 	sock->rfd
   * 	sock->listen
   * 	sock->uri
   * 	sock->poll_cb
   * 	sock->fd_registered
   * 	sock->rfd_registered
   */
  
  if ((alarm->_poll_index == 1) || co_list_parse(sock_alarms, _alarm_ptr_match_i, alarm)) {
    WARN("Socket %d already registered: %d",alarm->poll.fd,alarm->_poll_index);
  } else {
    sock = (co_socket_t*)NEW(co_socket, co_socket);
    CHECK_MEM(sock);
    
    sock->fd = (co_fd_t*)co_fd_create((co_obj_t*)sock,alarm->poll.fd);
    CHECK_MEM(sock->fd);
    hattach(sock->fd, sock);
    sock->rfd_lst = co_list16_create();
    CHECK_MEM(sock->rfd_lst);
    hattach(sock->rfd_lst, sock);
    sock->listen = true;
    // NOTE: Aren't able to get the Serval socket uris, so instead use string representation of fd
    sock->uri = h_calloc(1,6);
    CHECK_MEM(sock->uri);
    hattach(sock->uri,sock);
    sprintf(sock->uri,"%d",sock->fd->fd);
    sock->fd_registered = false;
    sock->local = NULL;
    sock->remote = NULL;
  
    sock->poll_cb = serval_socket_cb;
  
    // register sock
    CHECK(co_loop_add_socket((co_obj_t*)sock, (co_obj_t*)sock->fd) == 1,"Failed to add socket %d",sock->fd->fd);
    DEBUG("Successfully added socket %d",sock->fd->fd);
  
    sock->fd_registered = true;
    // NOTE: it would be better to get the actual poll index from the event loop instead of this:
    alarm->_poll_index = 1;
    alarm->poll.revents = 0;
    
    co_obj_t *alarm_obj = co_alarm_create(alarm);
    CHECK_MEM(alarm_obj);
    CHECK(co_list_append(sock_alarms, alarm_obj),"Failed to add to sock_alarms");
    DEBUG("Successfully added to sock_alarms %p",alarm);
  }
  
  return 0;
error:
  if (sock) co_socket_destroy((co_obj_t*)sock);
  return -1;
}

int _unwatch(struct __sourceloc __whence, struct sched_ent *alarm) {
//   DEBUG("OVERRIDDEN UNWATCH FUNCTION!");
  co_obj_t *node = NULL;
  char fd_str[6] = {0};
  
  CHECK(alarm->_poll_index == 1 && (node = co_list_parse(sock_alarms, _alarm_ptr_match_i, alarm)),"Attempting to unwatch socket that is not registered");
  co_list_delete(sock_alarms,node);
  
  // Get the socket associated with the alarm (alarm's fd is equivalent to the socket's uri)
  sprintf(fd_str,"%d",alarm->poll.fd);
  CHECK((node = co_loop_get_socket(fd_str,NULL)),"Could not find socket to remove");
  CHECK(co_loop_remove_socket(node,NULL),"Failed to remove socket");
  CHECK(co_socket_destroy(node),"Failed to destroy socket");
  
  alarm->_poll_index = -1;
  
  return 0;
  
error:
  return -1;
}

static void setup_sockets(void) {
  /* Setup up MDP & monitor interface unix domain sockets */
  DEBUG("Setup MDP sockets");
  overlay_mdp_setup_sockets();
  
  DEBUG("Setup MONITOR sockets");
  monitor_setup_sockets();
  
  // start the HTTP server if enabled
  DEBUG("HTTP server start");
  httpd_server_start(HTTPD_PORT, HTTPD_PORT_MAX);  
  
  DEBUG("Bind internal services");
  overlay_mdp_bind_internal_services();
  
  DEBUG("Setup OLSR sockets");
  olsr_init_socket();
  
  DEBUG("Setup RHIZOME");
  /* Get rhizome server started BEFORE populating fd list so that
   *  the server's listen so*cket is in the list for poll() */
  if (is_rhizome_enabled())
    rhizome_opendb();
  
  /* Get rhizome server started BEFORE populating fd list so that
   *  the server's listen socket is in the list for poll() */
  if (is_rhizome_enabled()){
    rhizome_opendb();
    if (config.rhizome.clean_on_start && !config.rhizome.clean_on_open)
      rhizome_cleanup(NULL);
  }
  
  DEBUG("Setup DNA HELPER");
  // start the dna helper if configured
  dna_helper_start();
  
  DEBUG("Setup DIRECTORY SERVICE");
  // preload directory service information
  directory_service_init();
  
#define SCHEDULE(X, Y, D) { \
static struct profile_total _stats_##X={.name="" #X "",}; \
static struct sched_ent _sched_##X={\
.stats = &_stats_##X, \
.function=X,\
}; \
_sched_##X.alarm=(gettime_ms()+Y);\
_sched_##X.deadline=(gettime_ms()+Y+D);\
schedule(&_sched_##X); }
  
  /* Periodically check for new interfaces */
  SCHEDULE(overlay_interface_discover, 1, 100);
    
  /* Periodically advertise bundles */
  SCHEDULE(overlay_rhizome_advertise, 1000, 10000);
  
  DEBUG("finished setup_sockets");
  
#undef SCHEDULE

}

int co_plugin_name(co_obj_t *self, co_obj_t **output, co_obj_t *params) {
  const char name[] = "serval-dna";
  CHECK((*output = co_str8_create(name,strlen(name)+1,0)),"Failed to create plugin name");
  return 1;
error:
  return 0;
}

SCHEMA(serval) {
  SCHEMA_ADD("servald","enabled");
  SCHEMA_ADD("serval_path",DEFAULT_SERVAL_PATH);
  return 1;
}

SCHEMA(mdp) {
  SCHEMA_ADD("mdp_sid",DEFAULT_SID);
  SCHEMA_ADD("mdp_keyring",DEFAULT_MDP_PATH);
  return 1;
}

int co_plugin_register(co_obj_t *self, co_obj_t **output, co_obj_t *params) {
  DEBUG("Loading serval schema.");
  SCHEMA_GLOBAL(serval);
  SCHEMA_REGISTER(mdp);
  return 1;
}

static int serval_load_config(void) {
  CHECK(co_profile_get_str(co_profile_global(),&serval_path,"serval_path",sizeof("serval_path")) < PATH_MAX - 16,"serval_path config parameter too long");
  if (serval_path[strlen(serval_path) - 1] == '/')
    serval_path[strlen(serval_path) - 1] = '\0'; // remove trailing slash
  CHECK(setenv("SERVALINSTANCE_PATH",serval_path,1) == 0,"Failed to set SERVALINSTANCE_PATH env variable");
  DEBUG("serval_path: %s",serval_path);
  return 1;
error:
  return 0;
}

static int
serval_reload_keyring_server(svl_crypto_ctx *ctx)
{
  CHECK(serval_reload_keyring(ctx), "Failed to reload server keyring");
  keyring = serval_get_keyring_file(ctx);
  return 1;
error:
  return 0;
}

int co_plugin_init(co_obj_t *self, co_obj_t **output, co_obj_t *params) {
  char *enabled = NULL;
  
  co_profile_get_str(co_profile_global(),&enabled,"servald",sizeof("servald"));
  if (strcmp(enabled,"disabled") == 0) return 1;

  DEBUG("Initializing Serval plugin");
  
  srandomdev();
  
  CHECK(serval_load_config(),"Failed to load Serval config parameters");
  serval_dna_ctx = svl_crypto_ctx_new();
  CHECK(serval_open_keyring(serval_dna_ctx, serval_reload_keyring_server),"Failed to open keyring");
  keyring = serval_get_keyring_file(serval_dna_ctx);
  
  if (!serval_registered) {
//     CHECK(serval_register(),"Failed to register Serval commands");
    CHECK(serval_daemon_register(),"Failed to register Serval daemon commands");
    CHECK(serval_crypto_register(),"Failed to register Serval-crypto commands");
    CHECK(olsrd_mdp_register(),"Failed to register OLSRd-mdp commands");
  }
  
  CHECK(cf_init() == 0, "Failed to initialize config");
  CHECK(cf_load_strict() == 1, "Failed to load config");
  CHECK(create_serval_instance_dir() != -1,"Unable to get/create Serval instance dir");
  CHECK(config.interfaces.ac != 0,"No network interfaces configured (empty 'interfaces' config option)");
  
  serverMode = 1;
  
  overlay_queue_init();
  
  // Initialize our list of Serval alarms/sockets
  sock_alarms = co_list16_create();
  CHECK_MEM(sock_alarms);
  timer_alarms = co_list16_create();
  CHECK_MEM(timer_alarms);
  
  setup_sockets();
  
  serval_registered = true;
  daemon_started = true;
  
  return 1;
error:
  return 0;
}

static co_obj_t *destroy_alarms(co_obj_t *alarms, co_obj_t *alarm, void *context) {
  struct sched_ent *this_alarm = ((co_alarm_t*)alarm)->alarm;
  struct __sourceloc nil;
  if (this_alarm->alarm)
    _unschedule(nil,this_alarm);
  if (this_alarm->_poll_index)
    _unwatch(nil,this_alarm);
  return NULL;
}

/** From serval-dna/server.c */
static void clean_proc()
{
  char path_buf[400];
  strbuf sbname = strbuf_local(path_buf, sizeof path_buf);
  strbuf_path_join(sbname, serval_instancepath(), "proc", NULL);
  
  DIR *dir;
  struct dirent *dp;
  if ((dir = opendir(path_buf)) == NULL) {
    WARNF_perror("opendir(%s)", alloca_str_toprint(path_buf));
    return;
  }
  while ((dp = readdir(dir)) != NULL) {
    strbuf_reset(sbname);
    strbuf_path_join(sbname, serval_instancepath(), "proc", dp->d_name, NULL);
    
    struct stat st;
    if (lstat(path_buf, &st)) {
      WARNF_perror("stat(%s)", path_buf);
      continue;
    }
    
    if (S_ISREG(st.st_mode))
      unlink(path_buf);
  }
  closedir(dir);
}

int co_plugin_shutdown(co_obj_t *self, co_obj_t **output, co_obj_t *params) {
  if (daemon_started == false) return 1;

  DEBUG("Serval shutdown");
  
  servalShutdown = 1;
  
  // Clean up serval-dna functions (commotiond will take care of closing sockets)
  rhizome_close_db();
  dna_helper_shutdown();
  overlay_interface_close_all();
  overlay_mdp_clean_socket_files();
  clean_proc();
  server_remove_stopfile();
  
  svl_crypto_ctx_free(serval_dna_ctx);
  
  daemon_started = false;
  
  co_obj_free(sock_alarms);
  co_obj_free(timer_alarms);
  
  return 1;
}

int serval_daemon_handler(co_obj_t *self, co_obj_t **output, co_obj_t *params) {
  CLEAR_ERR();
  
  CHECK(IS_LIST(params) && co_list_length(params) == 1,"Invalid parameters");
  
  /*if (co_str_cmp_str(co_list_element(params,0),"start") == 0) {
    CHECK(daemon_started == false,"Daemon is already started");
    CHECK(co_plugin_init(NULL,NULL,NULL),"Failed to start daemon");
  } else if (co_str_cmp_str(co_list_element(params,0),"stop") == 0) {
    CHECK(daemon_started == true,"Daemon is already stopped");
    CHECK(co_plugin_shutdown(NULL,NULL,NULL),"Failed to stop daemon");
  } else*/ if (co_str_cmp_str(co_list_element(params,0),"reload") == 0) {
    CHECK(serval_reload_keyring(serval_dna_ctx), "Failed to reload keyring");
    keyring = serval_get_keyring_file(serval_dna_ctx);
  }
  
  co_obj_t *out = co_str8_create("success",sizeof("success"),0);
  CHECK_MEM(out);
  CMD_OUTPUT("result", out);
  
error:
  INS_ERROR();
  return 1;
}