// ------------------------------------------------------------------------
// ecasound.cpp: Console mode user interface to ecasound.
// Copyright (C) 2002-2008 Kai Vehmanen
//
// Attributes:
//     eca-style-version: 3 (see Ecasound Programmer's Guide)
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
// 
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
// 
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307  USA
// ------------------------------------------------------------------------

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include <signal.h>    /* POSIX: various signal functions */
#include <unistd.h>    /* POSIX: sleep() */

#include <kvu_dbc.h>
#include <kvu_com_line.h>
#include <kvu_utils.h>

#include <eca-control.h>
#include <eca-error.h>
#include <eca-logger.h>
#include <eca-session.h>
#include <eca-version.h>

#include "eca-comhelp.h"
#include "eca-console.h"
#include "eca-curses.h"
#include "eca-neteci-server.h"
#include "eca-plaintext.h"
#include "textdebug.h"
#include "ecasound.h"

/**
 * Static/private function definitions
 */

static void ecasound_create_eca_objects(struct ecasound_state* state, COMMAND_LINE& cline);
static void ecasound_launch_daemon(struct ecasound_state* state);
static int ecasound_pass_at_launch_commands(struct ecasound_state* state);
static void ecasound_main_loop(struct ecasound_state* state);
void ecasound_parse_command_line(struct ecasound_state* state, 
				 const COMMAND_LINE& clinein,
				 COMMAND_LINE* clineout);
static void ecasound_print_usage(void);
static void ecasound_print_version_banner(void);
static void ecasound_setup_signals(struct ecasound_state* state);

extern "C" {
  static void ecasound_exit_cleanup(void);
  static void* ecasound_signal_watchdog_thread(void* arg); 
  static void ecasound_signal_handler(int signal);
}

static struct ecasound_state ecasound_state_global = 
  { 0,        /* ECA_CONSOLE* */
    0,        /* ECA_CONTROL */
    0,        /* ECA_LOGGER_INTERFACE */
    0,        /* ECA_NETECI_SERVER */
    0,        /* ECA_SESSION */
    0,        /* launchcmds, std::vector<std::string> */
    0,        /* pthread_t - daemon_thread */
    0,        /* pthread_mutex_t - lock */
    0,        /* sig_wait_t - exit_request */
    0,        /* sigset_t */
    ECASOUND_RETVAL_SUCCESS, /* int - return value */
    2868,     /* int - default daemon mode TCP-port */
    false,    /* daemon mode */
    false,    /* keep_running mode */
    false,    /* cerr-output-only mode */
    false,    /* interactive mode */
    false     /* quiet mode */
  };

static sig_atomic_t ecasound_cleanup_done = 0;
static sig_atomic_t ecasound_normal_exit = 0;
static sig_atomic_t ecasound_watchdog_active = 0;

/**
 * Namespace imports
 */
using namespace std;

/**
 * Function definitions
 */
int main(int argc, char *argv[])
{
  struct ecasound_state* state = &ecasound_state_global;

  /* 1. setup signals and the signal watchdog thread */
  ecasound_setup_signals(state);

  /* 2. parse command-line args */
  COMMAND_LINE* cline = new COMMAND_LINE(argc, argv);
  COMMAND_LINE* clineout = new COMMAND_LINE();
  ecasound_parse_command_line(state, *cline, clineout); 
  delete cline; cline = 0;

  /* 3. create console interface */
  if (state->retval == ECASOUND_RETVAL_SUCCESS) {

#if defined(ECA_PLATFORM_CURSES) 
    if (state->quiet_mode != true &&
	state->cerr_output_only_mode != true) {
      state->console = new ECA_CURSES();
      state->logger = new TEXTDEBUG();
      ECA_LOGGER::attach_logger(state->logger);
    }
    else 
#endif
      {
	ostream* ostr = (state->cerr_output_only_mode == true) ? &cerr : &cout;
	state->console = new ECA_PLAIN_TEXT(ostr);
      }
    
    if (state->quiet_mode != true) {
      /* 4. print banner */
      state->console->print_banner();
    }

    /* 5. set default debug levels */
    ECA_LOGGER::instance().set_log_level(ECA_LOGGER::errors, true);
    ECA_LOGGER::instance().set_log_level(ECA_LOGGER::info, true);
    ECA_LOGGER::instance().set_log_level(ECA_LOGGER::subsystems, true);
    ECA_LOGGER::instance().set_log_level(ECA_LOGGER::eiam_return_values, true);
    ECA_LOGGER::instance().set_log_level(ECA_LOGGER::module_names, true);
    
    /* 6. create eca objects */
    ecasound_create_eca_objects(state, *clineout);
    delete clineout; clineout = 0;

    /* 7. start ecasound daemon */
    if (state->retval == ECASOUND_RETVAL_SUCCESS) {
      if (state->daemon_mode == true) {
	ecasound_launch_daemon(state);
      }
    }

    /* 8. pass launch commands */
    ecasound_pass_at_launch_commands(state);

    /* 9. start processing */
    if (state->retval == ECASOUND_RETVAL_SUCCESS) {
      ecasound_main_loop(state);
    }
  }

  if (state->daemon_mode == true) {
    /* wait until daemon thread has exited */
    if (state->interactive_mode == true) {
      state->exit_request = 1;
    }
    pthread_join(*state->daemon_thread, NULL);
  }

  /* note: if we exist due to a signal, we never reach 
   *       the end of main() */

  /* note: we prefer to run the cleanup routines before 
   *       returning from main; there have been problems with
   *       dynamic libraries and libecasound cleanup routines
   *       when run from an atexit() handler
   */
  ecasound_normal_exit = 1;
  ecasound_exit_cleanup();

  // cerr << endl << "ecasound: main() exiting..." << endl << endl;

  DBC_CHECK(state->retval == ECASOUND_RETVAL_SUCCESS ||
	    state->retval == ECASOUND_RETVAL_INIT_FAILURE ||
	    state->retval == ECASOUND_RETVAL_START_ERROR ||
	    state->retval == ECASOUND_RETVAL_RUNTIME_ERROR);

  return state->retval;
}

/**
 * Cleanup routine that is run after either exit()
 * is called or ecasound returns from its main().
 */
void ecasound_exit_cleanup(void)
{
  struct ecasound_state* state = &ecasound_state_global;

  // cerr << endl << "ecasound: atexit cleanup" << endl;

  if (ecasound_cleanup_done == 0) {
    ecasound_cleanup_done  = 1;  
    if (state->control != 0) {
      if (state->control->is_running() == true) {
	state->control->stop_on_condition();
      }
      if (state->control->is_connected() == true) {
	state->control->disconnect_chainsetup();
      }
    }

    DBC_CHECK(ecasound_normal_exit == 1);

    if (state->control != 0) { delete state->control; state->control = 0; }
    if (state->session != 0) { delete state->session; state->session = 0; }

    if (state->launchcmds != 0) { delete state->launchcmds; state->launchcmds = 0; }
    if (state->eciserver != 0) { delete state->eciserver; state->eciserver = 0; }
    if (state->console != 0) { delete state->console; state->console = 0; }
    if (state->daemon_thread != 0) { delete state->daemon_thread; state->daemon_thread = 0; }
    if (state->lock != 0) { delete state->lock; state->lock = 0; }
    if (state->signalset != 0) { delete state->signalset; state->signalset = 0; }
  }

  // cerr << "ecasound: exit cleanup done." << endl << endl;
}

/**
 * Enters the main processing loop.
 */
void ecasound_create_eca_objects(struct ecasound_state* state, 
				 COMMAND_LINE& cline)
{
  DBC_REQUIRE(state != 0);
  DBC_REQUIRE(state->console != 0);

  try {
    state->session = new ECA_SESSION(cline);
    state->control = new ECA_CONTROL(state->session);

    DBC_ENSURE(state->session != 0);
    DBC_ENSURE(state->control != 0);
  }
  catch(ECA_ERROR& e) {
    state->console->print("---\necasound: ERROR: [" + e.error_section() + "] : \"" + e.error_message() + "\"\n");
    state->retval = ECASOUND_RETVAL_INIT_FAILURE;
  }
}

/**
 * Launches a background daemon that allows NetECI 
 * clients to connect to the current ecasound
 * session.
 */
void ecasound_launch_daemon(struct ecasound_state* state)
{
  DBC_REQUIRE(state != 0);
  // DBC_REQUIRE(state->console != 0);

  // state->console->print("ecasound: starting the NetECI server.");

  state->daemon_thread = new pthread_t;
  state->lock = new pthread_mutex_t;
  pthread_mutex_init(state->lock, NULL);
  state->eciserver = new ECA_NETECI_SERVER(state);

  int res = pthread_create(state->daemon_thread, 
			   NULL,
			   ECA_NETECI_SERVER::launch_server_thread, 
			   reinterpret_cast<void*>(state->eciserver));
  if (res != 0) {
    cerr << "ecasound: Warning! Unable to create daemon thread." << endl;
    delete state->daemon_thread;  state->daemon_thread = 0;
    delete state->lock;  state->lock = 0;
    delete state->eciserver; state->eciserver = 0;
  }

  // state->console->print("ecasound: NetECI server started");
}

static int ecasound_pass_at_launch_commands(struct ecasound_state* state)
{
  if (state->launchcmds) {
    std::vector<std::string>::const_iterator p = state->launchcmds->begin();

    while(p != state->launchcmds->end()) {
      state->control->command(*p);
      state->control->print_last_value();
      ++p;
    }
  }

  return 0;
}

/**
 * The main processing loop.
 */
void ecasound_main_loop(struct ecasound_state* state)
{
  DBC_REQUIRE(state != 0);
  DBC_REQUIRE(state->console != 0);

  ECA_CONTROL* ctrl = state->control;

  if (state->interactive_mode == true) {

    while(state->exit_request == 0) {
      state->console->read_command("ecasound ('h' for help)> ");
      const string& cmd = state->console->last_command();
      if (cmd.size() > 0 && state->exit_request == 0) {

	if (state->daemon_mode == true) {
	  int res = pthread_mutex_lock(state->lock);
	  DBC_CHECK(res == 0);
	}

	ctrl->command(cmd);
	ctrl->print_last_value();

	if (state->daemon_mode == true) {
	  int res = pthread_mutex_unlock(state->lock);
	  DBC_CHECK(res == 0);
	}

	if (cmd == "quit" || cmd == "q") {
	  state->console->print("---\necasound: Exiting...");
	  state->exit_request = 1;
	  ECA_LOGGER::instance().flush();
	}
      }
    }
  }
  else {
    /* non-interactive mode */

    if (state->daemon_mode == true) {
      int res = pthread_mutex_lock(state->lock);
      DBC_CHECK(res == 0);
    }

    if (ctrl->is_selected() == true && 
	ctrl->is_valid() == true) {
      ctrl->connect_chainsetup();
    }


    if (ctrl->is_connected() == true) {
      if (!state->exit_request) {
	int res = ctrl->run(!state->keep_running_mode);
	if (res < 0) {
	  state->retval = ECASOUND_RETVAL_RUNTIME_ERROR;
	  cerr << "ecasound: Warning! Errors detected during processing." << endl;
	}
      }
    }
    else {
      ctrl->print_last_value();
      state->retval = ECASOUND_RETVAL_START_ERROR;
    }

    if (state->daemon_mode == true) {
      int res = pthread_mutex_unlock(state->lock);
      DBC_CHECK(res == 0);
    }
  }
  // cerr << endl << "ecasound: mainloop exiting..." << endl;
}

/**
 * Parses the command lines options in 'cline'.
 */
void ecasound_parse_command_line(struct ecasound_state* state, 
				 const COMMAND_LINE& cline,
				 COMMAND_LINE* clineout)
{
  if (cline.size() < 2) {
    ecasound_print_usage();
    state->retval = ECASOUND_RETVAL_INIT_FAILURE;
  }
  else {
   cline.begin();
    while(cline.end() != true) {
      if (cline.current() == "-o:stdout" ||
	  cline.current() == "stdout" ||
	  cline.current() == "-d:0" ||
	  cline.current() == "-q") {

	state->quiet_mode = true;
	/* pass option to libecasound */
	clineout->push_back(cline.current());
      } 

      else if (cline.current() == "-c") {
	state->interactive_mode = true;
      }

      else if (cline.current() == "-C") {
	state->interactive_mode = false;
      }

      else if (cline.current() == "-D") {
	state->cerr_output_only_mode = true;
      }
      
      else if (cline.current() == "--daemon") {
	state->daemon_mode = true;
      }

      else if (cline.current().find("-E") != string::npos) {
	cline.next();
	if (cline.end() != true) {
	  state->launchcmds = 
	    new std::vector<std::string>
 	      (kvu_string_to_vector(cline.current(), ';'));
	}
      }

      else if (cline.current().find("--daemon-port") != string::npos) {
	std::vector<std::string> argpair = 
	  kvu_string_to_vector(cline.current(), '=');
	if (argpair.size() > 1) {
	  /* --daemon-port=XXXX */
	  state->daemon_port = atoi(argpair[1].c_str());
	}
      }


      else if (cline.current() == "--nodaemon") {
	state->daemon_mode = false;
      }

      else if (cline.current() == "-h" ||
	       cline.current() == "--help") {
	ecasound_print_usage();
	state->retval = ECASOUND_RETVAL_INIT_FAILURE;
	break;
      }

      else if (cline.current() == "-K" ||
	       cline.current() == "--keep-running") {
	state->keep_running_mode = true;
      }

      else if (cline.current() == "--version") {
	ecasound_print_version_banner();
	state->retval = ECASOUND_RETVAL_INIT_FAILURE;
	break;
      }
      
      else {
	/* pass rest of the options to libecasound */
	clineout->push_back(cline.current());
      }

      cline.next();
    }
  }
}

void ecasound_print_usage(void)
{
  cout << ecasound_parameter_help();
}

void ecasound_print_version_banner(void)
{
  cout << "ecasound v" << ecasound_library_version << endl;
  cout << "Copyright (C) 1997-2008 Kai Vehmanen and others." << endl;
  cout << "Ecasound comes with ABSOLUTELY NO WARRANTY." << endl;
  cout << "You may redistribute copies of ecasound under the terms of the GNU" << endl;
  cout << "General Public License. For more information about these matters, see" << endl; 
  cout << "the file named COPYING." << endl;
}

static void ecasound_signal_handler(int signal)
{
#if defined(HAVE_SIGPROCMASK) || defined(HAVE_PTHREAD_SIGMASK)
  if (ecasound_watchdog_active &&
      ecasound_state_global.exit_request == 0) {
    cerr << "(ecasound-watchdog) WARNING: ecasound_signal_handler entered, this should _NOT_ happen!";
    cerr << " pid=" << getpid() << endl;
  }
#endif

  if (ecasound_watchdog_active &&
      ecasound_state_global.exit_request) {
    cerr << "(ecasound-watchdog) WARNING: Signal received during cleanup, exiting immediately.\n";
    exit(ECASOUND_RETVAL_RUNTIME_ERROR);
  }
}

/**
 * Sets up a signal mask with sigaction() that blocks 
 * all common signals, and then launces a signal watchdog
 * thread that waits on the blocked signals using
 * sigwait(). 
 * 
 * This design causes all non-fatal termination signals
 * to be routed through a single thread. This signal watchdog
 * in turn performs a clean exit upon receiving a signal.
 * Without this setup, interactions between threads when handling 
 * would be harder to control (especially considering that
 * ecasound needs to work on various different platforms).
 */
void ecasound_setup_signals(struct ecasound_state* state)
{
  pthread_t watchdog;
  sigset_t* signalset;

  /* man pthread_sigmask:
   *  "...signal actions and signal handlers, as set with
   *   sigaction(2), are shared between all threads"
   */

  /* handle the following signals explicitly */
  signalset = new sigset_t;
  state->signalset = signalset;
  sigemptyset(signalset);
  sigaddset(signalset, SIGTERM);
  sigaddset(signalset, SIGINT);
  sigaddset(signalset, SIGHUP);
  sigaddset(signalset, SIGPIPE);
  sigaddset(signalset, SIGQUIT);

  /* create a dummy signal handler */
  struct sigaction blockaction;
  blockaction.sa_handler = ecasound_signal_handler;
  sigemptyset(&blockaction.sa_mask);
  blockaction.sa_flags = 0;

  /* attach the dummy handler to the following signals */
  sigaction(SIGTERM, &blockaction, 0);
  sigaction(SIGINT, &blockaction, 0);
  sigaction(SIGHUP, &blockaction, 0);
  sigaction(SIGPIPE, &blockaction, 0);
  sigaction(SIGQUIT, &blockaction, 0);

#ifdef __FreeBSD__
  /* ignore signals instead of passing them to our handler */
  blockaction.sa_handler = SIG_IGN;
  sigaction(SIGFPE, &blockaction, 0);
#endif

  int res = pthread_create(&watchdog, 
			   NULL, 
			   ecasound_signal_watchdog_thread, 
			   reinterpret_cast<void*>(state));
  if (res != 0) {
    cerr << "ecasound: Warning! Unable to create watchdog thread." << endl;
  }

  /* block all signals in 'signalset' (see above) */
#if defined(HAVE_PTHREAD_SIGMASK)
  pthread_sigmask(SIG_BLOCK, signalset, NULL);
#elif defined(HAVE_SIGPROCMASK)
  sigprocmask(SIG_BLOCK, signalset, NULL);
#endif
}

/**
 * Runs a watchdog thread that centrally catches signals that
 * will cause ecasound to exit.
 */
void* ecasound_signal_watchdog_thread(void* arg)
{
  int signalno = 0;
  struct ecasound_state* state = reinterpret_cast<struct ecasound_state*>(arg);

  // cerr << "Watchdog-thread created, pid=" << getpid() << "." << endl;

#ifdef HAVE_SIGWAIT

  /* step: block execution until a signal received 
   **********************************************/

  ecasound_watchdog_active = 1;

# if defined(HAVE_PTHREAD_SIGMASK)
  pthread_sigmask(SIG_BLOCK, state->signalset, NULL);
# elif defined(HAVE_SIGPROCMASK)
  /* the set of signals must be blocked before entering sigwait() */
  sigprocmask(SIG_BLOCK, state->signalset, NULL);
# endif

  sigwait(state->signalset, &signalno);

  cerr << endl << "(ecasound-watchdog) Received signal " << signalno << ". Cleaning up and exiting..." << endl;

  /* step: use pause() instead (alternative to sigwait()) 
  ********************************************************/

#elif HAVE_PAUSE /* !HAVE_SIGWAIT */

  /* note: with pause() we don't set 'ecasound_watchdog_active' as
   * it is normal to get signals when watchdog is running */
  pause();

  cerr << endl << "(ecasound-watchdog) Received signal and returned from pause(). Cleaning up and Exiting..." << Endl;

#else /* !HAVE_PAUSE */

  /* 2. If proper signal handling is not possible, stop compilation.
   *****************************************************************/

# error "Neither sigwait() or pause() Is available, unable to continue."

#endif

  /* step: signal the mainloop that process should terminate */
  state->exit_request = 1;

  /* step: unblock signals after process termination has 
   *       been started */

#ifdef HAVE_SIGWAIT
# if defined(HAVE_PTHREAD_SIGMASK)
  pthread_sigmask(SIG_UNBLOCK, state->signalset, NULL);
# elif defined(HAVE_SIGPROCMASK)
  /* the set of signals must be blocked before entering sigwait() */
  sigprocmask(SIG_UNBLOCK, state->signalset, NULL);
# endif
#endif

  /* step: in case mainloop is blocked running a batch job, we signal
   *       the engine thread directly and force it to terminate */
  if (state->interactive_mode != true &&
      state->control)
    state->control->quit_async();

  while(ecasound_normal_exit != 1) {
    /* sleep for one 200ms */
    kvu_sleep(0, 200000000);

    /* note: A race condition exists between ECA_CONTROL_BASE
     *       quit_async() and run(): if quit_async() is called
     *       after run() has been entered, but before run()
     *       has managed to start the engine, it is possible engine
     *       may still be started. 
     * 
     *       Thus we will keep checking the engine status until 
     *       shutdown is really completed. 
     *
     *       For robustness, this check is also done when in
     *       interactive mode (in case the mainloop does not for
     *       some reason react to our exit request).
     */
    if (state->control) {
      if (state->control->is_engine_started() == true) {
	state->control->quit_async();
      }
    }

  }
    
  ecasound_watchdog_active = 0;

  // cerr << endl << "ecasound: watchdog thread exiting..." << endl;

  return 0;
}
