// ------------------------------------------------------------------------
// audioio_jack_manager.cpp: Manager for JACK client objects
// Copyright (C) 2001-2003 Kai Vehmanen (kai.vehmanen@wakkanet.fi)
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

#include <algorithm> /* std::count() */
#include <iostream>
#include <string>

#include <sys/time.h> /* gettimeofday() */
#include <errno.h> /* ETIMEDOUT */
#include <jack/jack.h>

#include <kvu_dbc.h>
#include <kvu_numtostr.h>
#include <kvu_procedure_timer.h>
#include <kvu_threads.h>

#include "eca-engine.h"
#include "eca-chainsetup.h"
#include "eca-logger.h"

/**
 * Enable and disable features
 */

/* Debug control flow */ 
// #define DEBUG_CFLOW

/* Profile callback execution */
// #define PROFILE_CALLBACK_EXECUTION

/**
 * Local macro definitions
 */

#ifdef DEBUG_CFLOW
#define DEBUG_CFLOW_STATEMENT(x) (x)
#else
#define DEBUG_CFLOW_STATEMENT(x) ((void)0)
#endif

#ifdef PROFILE_CALLBACK_EXECUTION
#define PROFILE_CE_STATEMENT(x) (x)
static PROCEDURE_TIMER profile_callback_timer;
#else
#define PROFILE_CE_STATEMENT(x) ((void)0)
#endif

/**
 * Prototypes for static functions
 */

static int eca_jack_process(jack_nframes_t nframes, void *arg);
static void eca_jack_process_engine_iteration(jack_nframes_t nframes, void *arg);
static void eca_jack_process_mute(jack_nframes_t nframes, void* arg);
static void eca_jack_process_timebase_master(jack_nframes_t nframes, void *arg);
static void eca_jack_process_timebase_slave(jack_nframes_t nframes, void *arg);
#ifdef PROFILE_CALLBACK_EXECUTION
static void eca_jack_process_profile_pre(void);
static void eca_jack_process_profile_post(void);
#endif
static int eca_jack_srate (jack_nframes_t nframes, void *arg);
static void eca_jack_shutdown (void *arg);

static std::string eca_get_jack_port_item(const char **ports, int item);

#include "audioio_jack_manager.h"

using std::cerr;
using std::endl;
using std::list;
using std::string;
using std::vector;

/**
 * Implementations of static functions
 */

/**
 * How many ecasound JACK manager instances 
 * can run at the same time (affects connection
 * setup time in some situations).
 */
const int AUDIO_IO_JACK_MANAGER::instance_limit = 8;

/**
 * Processes all registered JACK input and output ports.
 * This is the main callback function registered to 
 * the JACK framework.
 */
static int eca_jack_process(jack_nframes_t nframes, void *arg)
{
  AUDIO_IO_JACK_MANAGER* current = static_cast<AUDIO_IO_JACK_MANAGER*>(arg);

  PROFILE_CE_STATEMENT(eca_jack_process_profile_pre());

  /* try to get the driver lock; if it fails or connection 
   * is not fully establish, skip this processing cycle */
  int ret = pthread_mutex_trylock(&current->engine_mod_lock_rep);
  if (ret == 0) {
    // DEBUG_CFLOW_STATEMENT(cerr << "eca_jack_process(): got lock" << endl);

    /* 1. transport control processing in "streaming" mode */
    if (current->mode_rep == AUDIO_IO_JACK_MANAGER::Streaming) {
      /* execute one engine iteration */
      if (current->is_running() == true) {
	eca_jack_process_engine_iteration(nframes, current);
      }
      else {
	eca_jack_process_mute(nframes, current);
      }
    }
#if ECA_JACK_TRANSPORT_API >= 2
    else {
      /* 2. transport control processing in "slave" mode */
      if (current->mode_rep == AUDIO_IO_JACK_MANAGER::Slave) {
	eca_jack_process_timebase_slave(nframes, arg);
      }
      /* 3. transport control processing in "master" mode */
      else if (current->mode_rep == AUDIO_IO_JACK_MANAGER::Master) {
	eca_jack_process_timebase_master(nframes, arg);
      }
      else { /* never reached */ }
    }
#endif

    pthread_mutex_unlock(&current->engine_mod_lock_rep);
  }
  else {
    // DEBUG_CFLOW_STATEMENT(cerr << "eca_jack_process(): couldn't get lock; muting" << endl);
    eca_jack_process_mute(nframes, current);
  }
  
  PROFILE_CE_STATEMENT(eca_jack_process_profile_post());
  
  return(0);
}

/**
 * Helper routine. Only called by eca_jack_process*() functions.
 */
static void eca_jack_process_engine_iteration(jack_nframes_t nframes, void* arg)
{
  AUDIO_IO_JACK_MANAGER* current = static_cast<AUDIO_IO_JACK_MANAGER*>(arg);

  /* this is guaranteed by libjack */
  DBC_CHECK(current->buffersize_rep == static_cast<long int>(nframes));

  /* FIXME: remove me! */
  if (current->buffersize_rep != static_cast<long int>(nframes)) {
    std::cerr << "JACK_MANAGER: invalid nframes! buffersize=" << current->buffersize_rep << ", nframes=" << nframes << std::endl;
  }

  if (current->engine_repp->status() != ECA_ENGINE::engine_status_finished) {

    /* 1. copy audio data from port input buffers to ecasound buffers */
    for(size_t n = 0; n < current->inports_rep.size(); n++) {
      if (current->inports_rep[n]->cb_buffer != 0) {
	jack_default_audio_sample_t* in_cb_buffer = 
	  static_cast<jack_default_audio_sample_t*>
	  (jack_port_get_buffer(current->inports_rep[n]->jackport, nframes));
	
	memcpy(current->inports_rep[n]->cb_buffer, 
	       in_cb_buffer, 
	       current->buffersize_rep * sizeof(jack_default_audio_sample_t));
      }
    }
    
    // DEBUG_CFLOW_STATEMENT(cerr << endl << "eca_jack_process(): engine_iter_in");
  
    /* 2. execute one engine iteration */
    current->engine_repp->engine_iteration(); 
    
    // DEBUG_CFLOW_STATEMENT(cerr << endl << "eca_jack_process(): engine_iter_out");
    
    /* 3. copy data from ecasound buffers to port output buffers */
    for(size_t n = 0; n < current->outports_rep.size(); n++) {
      if (current->outports_rep[n]->cb_buffer != 0) {
	jack_default_audio_sample_t* out_cb_buffer = 
	  static_cast<jack_default_audio_sample_t*>
	  (jack_port_get_buffer(current->outports_rep[n]->jackport, nframes));
	
	memcpy(out_cb_buffer, 
	       current->outports_rep[n]->cb_buffer, 
	       current->buffersize_rep * sizeof(jack_default_audio_sample_t));
      }
    }
  }
  else {
    /* 4. chainsetup finished, mute */
    DEBUG_CFLOW_STATEMENT(cerr << "eca_jack_process(): chainsetup finished, muting\n");
    eca_jack_process_mute(nframes, current);
  }
  
  /* 5. update engine status based on the last iteration */
  current->engine_repp->update_engine_state();
}

/**
 * Helper routine. Only called by eca_jack_process*() functions.
 */
static void eca_jack_process_mute(jack_nframes_t nframes, void* arg)
{
  AUDIO_IO_JACK_MANAGER* current = static_cast<AUDIO_IO_JACK_MANAGER*>(arg);

  for(size_t n = 0; n < current->outports_rep.size(); n++) {
    if (current->outports_rep[n]->cb_buffer != 0) {
      jack_default_audio_sample_t* out_cb_buffer = 
	static_cast<jack_default_audio_sample_t*>
	(jack_port_get_buffer(current->outports_rep[n]->jackport, nframes));

      memset(out_cb_buffer, 
	     0, 
	     current->buffersize_rep * sizeof(jack_default_audio_sample_t));
    }
  }
}

/**
 * Helper routine. Only called by eca_jack_process() function.
 */
static void eca_jack_process_timebase_master(jack_nframes_t nframes, void *arg)
{
#if ECA_JACK_TRANSPORT_API >= 2
  AUDIO_IO_JACK_MANAGER* current = static_cast<AUDIO_IO_JACK_MANAGER*>(arg);

  /* 1. engine is running; update transport and run engine */
  if (current->is_running() == true) {
    /* 1.1 first iteration after starting; do not change both position and 
     *     transport state at the same (see jackit-devel archives) */
    if (current->transport_info_rep.transport_state == JackTransportStopped) {
      DBC_CHECK(current->transport_info_rep.frame == current->engine_repp->current_position_in_samples());
    }
    /* 1.2 normal running operation */
    else {
      current->transport_info_rep.frame = 
	current->engine_repp->current_position_in_samples() + nframes;

      /* execute one engine iteration */
      eca_jack_process_engine_iteration(nframes, current);
    }

    if (current->transport_info_rep.frame !=
	current->engine_repp->current_position_in_samples()) {
      ECA_LOG_MSG(ECA_LOGGER::user_objects, 
		  "(audioio-jack-manager) Engine iteration did not process the requested " + 
		  kvu_numtostr(nframes) + 
		  " frames (transport_pos=" +
		  kvu_numtostr(current->transport_info_rep.frame) +
		  ", engine_pos=" +
		  kvu_numtostr(current->engine_repp->current_position_in_samples()) +
		  ".");
    }

    current->transport_info_rep.transport_state = JackTransportRolling;
    current->transport_info_rep.valid = static_cast<jack_transport_bits_t>(JackTransportState | JackTransportPosition);
    jack_set_transport_info(current->client_repp, &current->transport_info_rep);
    // DEBUG_CFLOW_STATEMENT(cerr << "eca_jack_process(): set transport position to " << current->transport_info_rep.frame << "." << endl);
  }
  /* 2. engine is not running; update transport and mute */
  else {
    current->transport_info_rep.transport_state = JackTransportStopped;
    current->transport_info_rep.frame = current->engine_repp->current_position_in_samples();
    current->transport_info_rep.valid = static_cast<jack_transport_bits_t>(JackTransportState | JackTransportPosition);
    jack_set_transport_info(current->client_repp, &current->transport_info_rep);
    eca_jack_process_mute(nframes, arg);
  }
#endif
}

/**
 * Helper routine. Only called by eca_jack_process() function.
 */
static void eca_jack_process_timebase_slave(jack_nframes_t nframes, void *arg)
{
#if ECA_JACK_TRANSPORT_API >= 2
  AUDIO_IO_JACK_MANAGER* current = static_cast<AUDIO_IO_JACK_MANAGER*>(arg);
  bool need_mute = true;

  jack_get_transport_info(current->client_repp, &current->transport_info_rep);

  SAMPLE_SPECS::sample_pos_t enginepos = current->engine_repp->current_position_in_samples();

  /* 1. engine locked for editing, do not touch! */
  if (current->engine_repp->is_locked_for_editing() == true) {
    DEBUG_CFLOW_STATEMENT(cerr << "current->engine_repp->is_locked_for_editing() == true\n");
  }

  /* 2. transport stopped */
  else if (current->transport_info_rep.transport_state == JackTransportStopped &&
	   current->transport_info_rep.valid & JackTransportState) {

    DEBUG_CFLOW_STATEMENT(cerr << "eca_jack_process_timebase_slave(): JACK stopped" << endl);
    if (current->is_running() == true) {
      // DEBUG_CFLOW_STATEMENT(cerr << "eca_jack_process(): running" << endl);
      current->engine_repp->command(ECA_ENGINE::ep_stop, 0.0f);
      current->engine_repp->command(ECA_ENGINE::ep_prepare, 0.0f);
    }

    int seekoffset = current->jackslave_seekahead_rep;
    if (current->engine_repp->is_prepared() == true) seekoffset = 1;

    if (current->transport_info_rep.valid & JackTransportPosition) {
      /* prepare for the next start by seeking to the correct position */
      if (enginepos != current->transport_info_rep.frame + seekoffset * current->buffersize()) {
	current->jackslave_seekahead_target_rep = 
	  current->transport_info_rep.frame + seekoffset * current->buffersize();
	current->engine_repp->command(ECA_ENGINE::ep_setpos_live_samples, 
				      current->jackslave_seekahead_target_rep);
      }
    }
  }

  /* 3. transport rolling (or looping, both states are fine to us, as 
        is the case where state info is not available at all) */
  else {
    if (current->is_running() != true) {

      /* transport rolling: engine not started; start it now */
      if (current->engine_repp->status() != ECA_ENGINE::engine_status_finished &&
	  ((current->transport_info_rep.frame <=
	    current->engine_repp->connected_chainsetup()->length_in_samples()) ||
	   (current->engine_repp->is_finite_length() != true) ||
	   !(current->transport_info_rep.valid & JackTransportPosition))) {

	/* conditions when we should start the engine 
	 *  a. engine status not finished, AND...
	 *   a.1. transport position not beyond csetup length, OR...
	 *   a.2. csetup has infinite length
	 */

	if (current->engine_repp->is_prepared() == true && 
	    (enginepos == current->transport_info_rep.frame ||
	     !(current->transport_info_rep.valid & JackTransportPosition))) {
	  current->engine_repp->start_operation();
	  DEBUG_CFLOW_STATEMENT(cerr << "eca_jack_process_timebase_slave(): Starting engine (direct)\n");
	}
	else {
	  current->engine_repp->command(ECA_ENGINE::ep_start, 0.0f);
	  DEBUG_CFLOW_STATEMENT(cerr << "eca_jack_process_timebase_slave(): Starting engine (cmdpipe)\n");
	  enginepos = -1;
	}
      }
    }

    if (enginepos == current->transport_info_rep.frame ||
	!(current->transport_info_rep.valid & JackTransportPosition)) {
      DEBUG_CFLOW_STATEMENT(cerr << "eca_jack_process_timebase_slave(): JACK running; correct position\n");
      /* execute engine iteration */
      eca_jack_process_engine_iteration(nframes, current);
      current->jackslave_seekahead_target_rep = -1;
      need_mute = false;
    }
    else {
      DEBUG_CFLOW_STATEMENT(cerr << endl << "eca_jack_process_timebase_slave():  engine curpos '" << 
			    current->engine_repp->current_position_in_samples() << 
			    "' doesn't match JACK curpos '" << 
			    current->transport_info_rep.frame << "'!" << endl);

      if ((current->transport_info_rep.frame >=
	   current->engine_repp->connected_chainsetup()->length_in_samples() &&
	   (current->engine_repp->is_finite_length() == true))) {
	DEBUG_CFLOW_STATEMENT(cerr << endl <<
			      "eca_jack_process_timebase_slave(): over max length" << endl);
	current->engine_repp->command(ECA_ENGINE::ep_stop, 0.0f);
      }
      else if (current->jackslave_seekahead_target_rep == -1 ||
	       current->jackslave_seekahead_target_rep < 
	       static_cast<long int>(current->transport_info_rep.frame + current->buffersize()) ||
	       current->jackslave_seekahead_target_rep - current->jackslave_seekahead_rep * current->buffersize() >
	       static_cast<long int>(current->transport_info_rep.frame + current->buffersize())) {

	/* note: we use seek-ahead to give time for the disk
	   i/o subsystem to catch up for the next round, seek-ahead 
	   must be re-initialized if...
	     a) seek-ahead target not set,
	     b) we have missed the current seek-ahead target, or
	     c) transport position has been rewinded (current seek-ahead target
	        too far in the fututre)
	*/

	if (current->jackslave_seekahead_target_rep != -1) {
	  /* previous seek has failed; try again with a longer look-ahead */
	  current->jackslave_seekahead_rep = 
	    (current->jackslave_seekahead_rep < (65536 / current->buffersize()) ?
	     current->jackslave_seekahead_rep * 2 : (65536 / current->buffersize()));
	  DEBUG_CFLOW_STATEMENT(cerr << "eca_jack_process(): seek-ahead request failed; increasing seek-ahead to "
				<< current->jackslave_seekahead_rep << endl);

	}

	current->jackslave_seekahead_target_rep = current->transport_info_rep.frame + 
	                                          current->jackslave_seekahead_rep * current->buffersize();

	current->engine_repp->command(ECA_ENGINE::ep_setpos_live_samples, 
				      current->jackslave_seekahead_target_rep);
	DEBUG_CFLOW_STATEMENT(cerr << "eca_jack_process(): seek-ahead request sent; seeking to "
			      << current->jackslave_seekahead_target_rep << endl);
      }
      else {
	/* engine is already seeking to a new pos */
	DEBUG_CFLOW_STATEMENT(cerr << endl 
			      << "eca_jack_process_timebase_slave():  seek to new pos underway; " 
			      << current->transport_info_rep.frame
			      << " is transport-curpos." <<  endl);
      }
    }
  }

  if (need_mute == true) {
    eca_jack_process_mute(nframes, current);
  }
#endif
}

#ifdef PROFILE_CALLBACK_EXECUTION
static void eca_jack_process_profile_pre(void)
{
  profile_callback_timer.start();
  DEBUG_CFLOW_STATEMENT(cerr << endl << "eca_jack_process(): entry ----> ");
}

static void eca_jack_process_profile_post(void)
{
  profile_callback_timer.stop();
  DEBUG_CFLOW_STATEMENT(cerr << endl << "eca_jack_process(): process out" << endl);
  
  if (profile_callback_timer.last_duration_seconds() > 0.005f) {
    cerr << "(audioio-jack-manager) event " << profile_callback_timer.event_count();
    cerr << ", process() took " << profile_callback_timer.last_duration_seconds() * 1000;

    cerr << " msecs." << endl;
  }
  else {
    if (profile_callback_timer.event_count() < 5) {
      cerr << "(audioio-jack-manager) event " << profile_callback_timer.event_count();
      cerr << ", process() took " << profile_callback_timer.last_duration_seconds() * 1000;
      cerr << " msecs." << endl;
      
    }
  }
}
#endif /* PROFILE_CALLBACK_EXECUTION */

/**
 * Changes current sampling rate. Callback function registered
 * to the JACK framework.
 */
static int eca_jack_srate (jack_nframes_t nframes, void *arg)
{
  AUDIO_IO_JACK_MANAGER* current = static_cast<AUDIO_IO_JACK_MANAGER*>(arg);

  ECA_LOG_MSG(ECA_LOGGER::user_objects, 
	      "(audioio-jack-manager) [callback] " + current->jackname_rep + 
	      ": setting srate to " + kvu_numtostr(nframes));

  if (static_cast<long int>(nframes) != current->srate_rep) {
    current->shutdown_request_rep = true;
    ECA_LOG_MSG(ECA_LOGGER::info, 
		"(audioio-jack-manager) Invalid new samplerate, shutting down.");
  }

  return(0);
}

/**
 * Shuts down the callback context. Callback function registered
 * to the JACK framework.
 */
static void eca_jack_shutdown (void *arg)
{
  AUDIO_IO_JACK_MANAGER* current = static_cast<AUDIO_IO_JACK_MANAGER*>(arg);
  ECA_LOG_MSG(ECA_LOGGER::user_objects, 
	      "(audioio-jack-manager) " + current->jackname_rep + 
	      ": [callback] jackd shutdown, stopping processing");
  current->shutdown_request_rep = true;
}

/**
 * Implementations of non-static functions
 */
AUDIO_IO_JACK_MANAGER::AUDIO_IO_JACK_MANAGER(void)
{
  ECA_LOG_MSG(ECA_LOGGER::system_objects, "(audioio-jack-manager) constructor");

  open_rep = false;
  activated_rep = false;

  last_node_id_rep = 1;
  jackslave_seekahead_rep = 2;
  jackslave_seekahead_target_rep = -1;

  engine_repp = 0;
  jackname_rep = "ecasound";

  pthread_cond_init(&exit_cond_rep, NULL);
  pthread_mutex_init(&exit_mutex_rep, NULL);
  pthread_mutex_init(&engine_mod_lock_rep, NULL);
  
  mode_rep = AUDIO_IO_JACK_MANAGER::Streaming;

  cb_allocated_frames_rep = 0;
  buffersize_rep = 0;
}

AUDIO_IO_JACK_MANAGER::~AUDIO_IO_JACK_MANAGER(void)
{
  ECA_LOG_MSG(ECA_LOGGER::system_objects, "(audioio-jack-manager) destructor");

  /* 1. close JACK connection */
  if (is_open() == true) close_server_connection();

  /* 2. clear input ports */
  vector<eca_jack_port_data_t*>::iterator q = inports_rep.begin();
  while(q != inports_rep.end()) {
    if ((*q)->cb_buffer != 0) {
      delete[] (*q)->cb_buffer;
      (*q)->cb_buffer = 0;
    }
    delete *q;
    ++q;
  }

  /* 3. clear output ports */
  q = inports_rep.begin();
  while(q != inports_rep.end()) {
    if ((*q)->cb_buffer != 0) {
      delete[] (*q)->cb_buffer;
      (*q)->cb_buffer = 0;
    }
    delete *q;
    ++q;
  }

  /* 4. clear objects */
  list<eca_jack_node_t*>::iterator p = node_list_rep.begin();
  while(p != node_list_rep.end()) {
    delete *p;
    ++p;
  }
}

bool AUDIO_IO_JACK_MANAGER::is_managed_type(const AUDIO_IO* aobj) const
{
  // ---
  DBC_REQUIRE(aobj != 0);
  // ---

  if (aobj->name() == "JACK interface") {
    DBC_CHECK(dynamic_cast<const AUDIO_IO_JACK*>(aobj) != 0);
    return(true);
  }

  return(false);
}

void AUDIO_IO_JACK_MANAGER::register_object(AUDIO_IO* aobj)
{
  // ---
  DBC_REQUIRE(aobj != 0);
  DBC_REQUIRE(is_managed_type(aobj) == true);
  // ---

  ECA_LOG_MSG(ECA_LOGGER::system_objects, 
		"(audioio-jack-manager) register object " + aobj->label());  

  AUDIO_IO_JACK* jobj = static_cast<AUDIO_IO_JACK*>(aobj);

  eca_jack_node_t* tmp = new eca_jack_node_t;
  tmp->aobj = jobj;
  tmp->origptr = aobj;
  tmp->client_id = last_node_id_rep;
  node_list_rep.push_back(tmp);

  jobj->set_manager(this, tmp->client_id);

  ++last_node_id_rep;

  // ---
  DBC_ENSURE(is_managed_type(aobj) == true);
  // ---
}

int AUDIO_IO_JACK_MANAGER::get_object_id(const AUDIO_IO* aobj) const
{
  // ---
  DBC_REQUIRE(is_managed_type(aobj) == true);
  // ---

  list<eca_jack_node_t*>::const_iterator p = node_list_rep.begin();
  while(p != node_list_rep.end()) {
    if ((*p)->origptr == aobj) {
      ECA_LOG_MSG(ECA_LOGGER::system_objects, 
		  "(audioio-jack-manager) found object id for aobj " +
		  aobj->name() + ": " + kvu_numtostr((*p)->client_id));
      return((*p)->client_id);
    }
    ++p;
  }
  return(-1);
}

list<int> AUDIO_IO_JACK_MANAGER::get_object_list(void) const
{
  list<int> object_list;
  list<eca_jack_node_t*>::const_iterator p = node_list_rep.begin();
  while(p != node_list_rep.end()) {
    object_list.push_back((*p)->client_id);
    ++p;
  }
  return(object_list);
}

/**
 * Unregisters object previously registered with register_object()
 * from the manager.
 *
 * @param id unique identifier for managed objects; @see
 *        get_object_id
 *
 */
void AUDIO_IO_JACK_MANAGER::unregister_object(int id)
{
  // ---
  DBC_DECLARE(unsigned int old_total_nodes = node_list_rep.size());
  // ---

  ECA_LOG_MSG(ECA_LOGGER::system_objects, 
		"(audioio-jack-manager) unregister object ");

  list<eca_jack_node_t*>::iterator p = node_list_rep.begin();
  while(p != node_list_rep.end()) {
    if ((*p)->client_id == id) {
      ECA_LOG_MSG(ECA_LOGGER::system_objects,
		  "(audioio-jack-manager) removing object " + (*p)->aobj->label());
      (*p)->aobj->set_manager(0, -1);

      delete *p;
      node_list_rep.erase(p);

      break;
    }
    ++p;
  }

  // ---
  DBC_ENSURE(node_list_rep.size() == old_total_nodes - 1);
  DBC_DECLARE(list<int> ol = get_object_list());
  DBC_ENSURE(std::count(ol.begin(), ol.end(), id) == 1);
  // ---
}

void AUDIO_IO_JACK_MANAGER::set_parameter(int param, std::string value)
{
  switch(param) 
    {
    case 1: 
      {
	jackname_rep = value;
	ECA_LOG_MSG(ECA_LOGGER::user_objects, 
		    "(audioio-jack-manager) client name set to '" +
		    value + "'.");
	break;
      }

    case 2: 
      {
	if (value == "streaming") {
	  mode_rep = AUDIO_IO_JACK_MANAGER::Streaming;
	  ECA_LOG_MSG(ECA_LOGGER::user_objects, 
		      "(audioio-jack-manager) 'streaming' mode selected.");
	}
	else if (value == "master") {
	  mode_rep = AUDIO_IO_JACK_MANAGER::Master;
	  ECA_LOG_MSG(ECA_LOGGER::user_objects, 
		      "(audioio-jack-manager) 'master' mode selected.");
	}
	else {
	  mode_rep = AUDIO_IO_JACK_MANAGER::Slave;
	  ECA_LOG_MSG(ECA_LOGGER::user_objects, 
		      "(audioio-jack-manager) 'slave' mode selected.");
	}
	break;
      }
    }
}

std::string AUDIO_IO_JACK_MANAGER::get_parameter(int param) const
{
  switch(param) 
    {
    case 1:
      {
	return(jackname_rep);
      }

    case 2: 
      { 
	switch(mode_rep) {
	case AUDIO_IO_JACK_MANAGER::Master: return("master");
	case AUDIO_IO_JACK_MANAGER::Slave: return("slave");
	default: return("streaming");
	}
	break;
      }
    }
  return("");
}

void AUDIO_IO_JACK_MANAGER::exec(ECA_ENGINE* engine, ECA_CHAINSETUP* csetup)
{
  ECA_LOG_MSG(ECA_LOGGER::system_objects, "(audioio-jack-manager) driver exec");

  engine_repp = engine;
  engine->init_engine_state();

#if ECA_JACK_TRANSPORT_API >= 2
  transport_info_rep.frame = engine_repp->current_position_in_samples();
#endif

  shutdown_request_rep = false;
  exit_request_rep = false;

  activate_server_connection();
  if (is_connection_active() != true) {
    signal_exit();
  }
  
  while(true) {

    DEBUG_CFLOW_STATEMENT(cerr << "jack_exec: wait for commands" << endl);

    engine_repp->wait_for_commands();

    DEBUG_CFLOW_STATEMENT(cerr << "jack_exec: wakes up; commands available" << endl);

    /* we must take the lock to ensure that 
     * process callback does not run at the same time */
    pthread_mutex_lock(&engine_mod_lock_rep);
    engine_repp->check_command_queue();
    pthread_mutex_unlock(&engine_mod_lock_rep);

    DEBUG_CFLOW_STATEMENT(cerr << "jack_exec: check_commands finished" << endl);

    /* case 1: external exit request */
    if (exit_request_rep == true) {
      ECA_LOG_MSG(ECA_LOGGER::system_objects, "(audioio-jack-manager) exit request in exec");
      break;
    }

    /* case 2: engine finished and in batch mode -> exit */
    if ((engine_repp->status() == ECA_ENGINE::engine_status_finished ||
	 engine_repp->status() == ECA_ENGINE::engine_status_error) &&
	engine->batch_mode() == true) {

      /* batch operation finished (or error occured) */
      ECA_LOG_MSG(ECA_LOGGER::system_objects, "(audioio-jack-manager) batch finished in exec");
      break;
    }

    /* case 3: problems with jack callbacks -> exit */
    if (shutdown_request_rep == true) {
      ECA_LOG_MSG(ECA_LOGGER::system_objects, "(audioio-jack-manager) problems with JACK callbacks");
      break;
    }
  }

  if (is_connection_active() == true) {
    deactivate_server_connection();
  }

  engine_repp = 0;

  /* signal exit() that we are done */
  signal_exit();
}

/**
 * Activate connection to the JACK server.
 *
 * context: Can be called at the same time with
 *          JACK callbacks, so proper locking 
 *          must be ensured.
 *
 * @pre is_running() != true
 * @post is_running() == true
 */
void AUDIO_IO_JACK_MANAGER::start(void)
{
  // --
  DBC_REQUIRE(is_running() != true);
  // --

  ECA_LOG_MSG(ECA_LOGGER::system_objects, "(audioio-jack-manager) driver start");

  if (engine_repp->is_prepared() != true) engine_repp->prepare_operation();
  engine_repp->start_operation();

  // --
  DBC_ENSURE(is_running() == true);
  // --
}

/**
 * Signals that driver should stop operation.
 * Once stopped, driver must not call
 * any ECA_ENGINE functions.
 *
 * Caller must ensure that JACK process callback
 * does not rung at the same time.
 */
void AUDIO_IO_JACK_MANAGER::stop(void)
{
  ECA_LOG_MSG(ECA_LOGGER::system_objects, "(audioio-jack-manager) driver stop");

  if (engine_repp->is_prepared() == true) engine_repp->stop_operation();
}

/**
 * Activates connection to server.
 *
 * @pre is_connection_active() != true
 */
void AUDIO_IO_JACK_MANAGER::activate_server_connection(void)
{
  // --
  DBC_REQUIRE(is_connection_active() != true);
  // --

  if (engine_repp->is_prepared() != true) engine_repp->prepare_operation();

  ECA_LOG_MSG(ECA_LOGGER::system_objects, "(audioio-jack-manager) jack_activate()");
  if (jack_activate (client_repp)) {
    ECA_LOG_MSG(ECA_LOGGER::info, "(audioio-jack-manager) Error! Cannot active client!");
    activated_rep = false;
  }
  else {
    connect_all_nodes();
  
    /* update port-specific latency values */
    engine_repp->update_cache_latency_values();

    activated_rep = true;
  }
}

/**
 * Disconnects all connected ports and then
 * deactives the client.
 *
 * Caller must ensure that JACK process callback
 * does not rung at the same time.
 *
 * @pre is_connection_active() == true
 * @post is_connection_active() != true
 */
void AUDIO_IO_JACK_MANAGER::deactivate_server_connection(void)
{
  // --
  DBC_REQUIRE(is_connection_active() == true);
  // --

  if (shutdown_request_rep != true) {
    /* disconnect all clients */
    disconnect_all_nodes();

    ECA_LOG_MSG(ECA_LOGGER::system_objects, "(audioio-jack-manager) jack_deactivate() ");
    if (jack_deactivate (client_repp)) {
      ECA_LOG_MSG(ECA_LOGGER::info, "(audioio-jack-manager) Error! Cannot deactive client!");
    }
  }
 
  if (engine_repp->is_prepared() == true) engine_repp->stop_operation();

  activated_rep = false;

  signal_stop();

  // --
  DBC_ENSURE(is_connection_active() != true);
  // --
}

/**
 * Signals that driver should stop operation 
 * and return from its exec() method.
 *
 * context: Can be called at the same time with
 *          JACK callbacks, so proper locking 
 *          must be ensured.
 */
void AUDIO_IO_JACK_MANAGER::exit(void)
{
  ECA_LOG_MSG(ECA_LOGGER::system_objects, "(audioio-jack-manager) driver exit");

  exit_request_rep = true;
  if (engine_repp->is_prepared() == true) engine_repp->stop_operation();
}

/**
 * Returns a pointer to a 'eca_jack_node_t' structure 
 * matching client 'client_id'.
 *
 * @pre list<int> l = get_object_list(); std::count(l.begin(), l.end(), client_id) == 1
 * @return non-zero pointer
 */
AUDIO_IO_JACK_MANAGER::eca_jack_node_t* AUDIO_IO_JACK_MANAGER::get_node(int client_id)
{
  // --
  DBC_DECLARE(list<int> ol = get_object_list());
  DBC_REQUIRE(std::count(ol.begin(), ol.end(), client_id) == 1);
  // --

  eca_jack_node_t* node = 0;
  list<eca_jack_node_t*>::iterator p = node_list_rep.begin();
  while(p != node_list_rep.end()) {
    if ((*p)->client_id == client_id) { 
      node = *p;
      break;
    }
    ++p;
  }

  // --
  DBC_ENSURE(node != 0);
  // --

  return(node);
}

/**
 * Sets up automatic port connection for client_id's port
 * 'portnum'. When jack client is activated, this port
 * is automatically connected to port 'portname'. The 
 * direction of the connection is based on audio objects I/O mode 
 * (@see AUDIO_IO::io_mode()).
 *
 * @pre list<int> l = get_object_list(); std::count(l.begin(), l.end(), client_id) == 1
 * @pre is_open() == true
 @ @pre portnum > 0
 */
void AUDIO_IO_JACK_MANAGER::auto_connect_jack_port(int client_id, int portnum, const string& portname)
{
  // ---
  DBC_DECLARE(list<int> ol = get_object_list());
  DBC_REQUIRE(std::count(ol.begin(), ol.end(), client_id) == 1);
  DBC_REQUIRE(is_open() == true);
  DBC_REQUIRE(portnum > 0);
  // ---

  ECA_LOG_MSG(ECA_LOGGER::system_objects, 
		"(audioio-jack-manager) auto-connect jack ports for client " + kvu_numtostr(client_id));

  eca_jack_node_t* node = get_node(client_id);

  list<eca_jack_port_data*>::const_iterator p = node->ports.begin();
  int n = 1;
  while(p != node->ports.end()) {
    if (n == portnum) {
      (*p)->autoconnect_string = portname;
      break;
    }
    ++n;
    ++p;
  }
}

static std::string eca_get_jack_port_item(const char **ports, int item)
{
  int n = 0;
  while(ports != 0 && ports[n] != 0) {
    if (n + 1 == item) return(string(ports[n]));
    n++;
  }
  return string("");
}

/**
 * Sets up automatic port connections to matching ports of
 * client 'dst'.
 *
 * @pre list<int> l = get_object_list(); std::count(l.begin(), l.end(), client_id) == 1
 * @pre is_open() == true
 @ @pre portnum > 0
 */
void AUDIO_IO_JACK_MANAGER::auto_connect_jack_port_client(int client_id, const string& dst, int channels)
{
  // ---
  DBC_DECLARE(list<int> ol = get_object_list());
  DBC_REQUIRE(std::count(ol.begin(), ol.end(), client_id) == 1);
  DBC_REQUIRE(is_open() == true);
  DBC_REQUIRE(channels > 0);
  // ---

  const char** ports;
  
  eca_jack_node_t* node = get_node(client_id);
  list<eca_jack_port_data*>::const_iterator p = node->ports.begin();
  int n = 1;
  while(p != node->ports.end()) {
    if (n <= channels) {
      ports = 0;
      if (node->aobj->io_mode() == AUDIO_IO::io_read) {
	ports = jack_get_ports (client_repp, dst.c_str(), NULL, JackPortIsOutput);
      }
      else {
	ports = jack_get_ports (client_repp, dst.c_str(), NULL, JackPortIsInput);
      }
      (*p)->autoconnect_string = eca_get_jack_port_item(ports, n);
      ECA_LOG_MSG(ECA_LOGGER::user_objects,
		  "(audioio-jack-manager) Making autoconnection to terminal port: " + 
		  (*p)->autoconnect_string);
      if (ports != NULL) free(ports);
    }
    else {
      break;
    }
    ++n;
    ++p;
  }
}

/**
 * Returns the total latency for ports of client 
 * 'client_id'. If client ports have different latency
 * values, the worst-case latency is reported.
 */
long int AUDIO_IO_JACK_MANAGER::client_latency(int client_id)
{
  eca_jack_node_t* node = get_node(client_id);
  long int latency = -1;

  list<eca_jack_port_data*>::const_iterator p = node->ports.begin();
  while(p != node->ports.end()) {
    if (latency == -1) {
      latency = (*p)->total_latency;
    }
    else {
      if (static_cast<long int>((*p)->total_latency) > latency) {
	ECA_LOG_MSG(ECA_LOGGER::info,
		    "(audioio-jack-manager) warning! port latencies don't match for client " + kvu_numtostr(client_id));
	latency = (*p)->total_latency;
      }
    }
    ++p;
  }
 
  return(latency);
}

/**
 * Registers new JACK port for client 'client_id'. The direction of
 * the port is based on audio objects I/O mode (@see
 * AUDIO_IO::io_mode()). If 'portname' is a non-empty string, 
 * the port will be automatically connected to the 'portname' 
 * port once JACK client is activated.
 *
 * The final port names are of the form 'clientname:portprefix_N', 
 * where N is 1...max_port.
 *
 * @pre list<int> l = get_object_list(); std::count(l.begin(), l.end(), client_id) == 1
 * @pre is_open() == true
 */
void AUDIO_IO_JACK_MANAGER::register_jack_ports(int client_id, int ports, const string& portprefix)
{
  // ---
  DBC_DECLARE(list<int> ol = get_object_list());
  DBC_REQUIRE(std::count(ol.begin(), ol.end(), client_id) == 1);
  DBC_REQUIRE(is_open() == true);
  DBC_DECLARE(unsigned int old_port_count_vectors = inports_rep.size() + outports_rep.size());
  // ---

  ECA_LOG_MSG(ECA_LOGGER::system_objects, 
	      "(audioio-jack-manager) register jack ports for client " + kvu_numtostr(client_id));

  eca_jack_node_t* node = get_node(client_id);

  for(int n = 0; n < ports; n++) {
    eca_jack_port_data_t* portdata = new eca_jack_port_data_t;

    portdata->jackport = 0;
    portdata->autoconnect_string = "";
    portdata->total_latency = 0;
    portdata->cb_buffer = new jack_default_audio_sample_t [cb_allocated_frames_rep];

    if (node->aobj->io_mode() == AUDIO_IO::io_read) {
      string tport = portprefix + "_" + kvu_numtostr(inports_rep.size() + 1);
      portdata->jackport = jack_port_register(client_repp, 
					      tport.c_str(), 
					      JACK_DEFAULT_AUDIO_TYPE, 
					      JackPortIsInput, 
					      0);
      inports_rep.push_back(portdata);
    }
    else {
      string tport = portprefix + "_" + kvu_numtostr(outports_rep.size() + 1);
      portdata->jackport = jack_port_register(client_repp, 
					      tport.c_str(), 
					      JACK_DEFAULT_AUDIO_TYPE, 
					      JackPortIsOutput, 
					      0);
      outports_rep.push_back(portdata);
    }

    node->ports.push_back(portdata);
  }

  // ---
  DBC_ENSURE(inports_rep.size() + outports_rep.size() == 
	     old_port_count_vectors + ports);
  // ---
}

/**
 * Unregisters all JACK ports for client 'client_id'.
 *
 * @pre list<int> l = get_object_list(); std::count(l.begin(), l.end(), client_id) == 1
 * @pre is_open() == true
 * @post node->in_ports == 0 && node->out_ports == 0
 */
void AUDIO_IO_JACK_MANAGER::unregister_jack_ports(int client_id)
{
  // ---
  DBC_DECLARE(list<int> ol = get_object_list());
  DBC_REQUIRE(std::count(ol.begin(), ol.end(), client_id) == 1);
  DBC_REQUIRE(is_open() == true);
  DBC_DECLARE(unsigned int old_node_port_count = get_node(client_id)->ports.size());
  DBC_DECLARE(unsigned int old_port_count_vectors = inports_rep.size() + outports_rep.size());
  // ---

  ECA_LOG_MSG(ECA_LOGGER::system_objects, 
		"(audioio-jack-manager) unregister all jack ports for client " + kvu_numtostr(client_id));

  eca_jack_node_t* node = get_node(client_id);

  list<eca_jack_port_data_t*>::iterator p = node->ports.begin();
  while(p != node->ports.end()) {
    /* 1. unregister port from JACK */
    if (open_rep == true) {
      jack_port_unregister(client_repp, (*p)->jackport);
    }

    /* 2. delete the port from inports and outports vectors */
    vector<eca_jack_port_data_t*>::iterator q = inports_rep.begin();
    while(q != inports_rep.end()) {
      if (*p == *q) {
	inports_rep.erase(q);
	break;
      }
      ++q;
    }
    
    q = outports_rep.begin();
    while(q != outports_rep.end()) {
      if (*p == *q) {
	outports_rep.erase(q);
	break;
      }
      ++q;
    }
    
    /* 3. delete sub-structures */
      
    delete[] (*p)->cb_buffer;
    (*p)->cb_buffer = 0;

    /* 4. delete the actual port_data object */
    delete *p;

    ++p;
  }

  /* 5. clear the whole node port list */
  node->ports.clear();

  // ---
  DBC_ENSURE(node->ports.size() == 0);
  DBC_ENSURE(inports_rep.size() + outports_rep.size() == 
	     old_port_count_vectors - old_node_port_count);
  // ---
}

void AUDIO_IO_JACK_MANAGER::open(int client_id)
{
  ECA_LOG_MSG(ECA_LOGGER::system_objects, 
		"(audioio-jack-manager) open for client " + kvu_numtostr(client_id));

  DBC_CHECK(shutdown_request_rep != true);

  /* only for the first client */
  if (is_open() != true) {
    open_server_connection();
  }

  ++open_clients_rep;
}

void AUDIO_IO_JACK_MANAGER::close(int client_id)
{
  ECA_LOG_MSG(ECA_LOGGER::system_objects, 
		"(audioio-jack-manager) close for client " + kvu_numtostr(client_id));

  DBC_CHECK(open_clients_rep > 0);

  /* only for the last client */
  if (open_clients_rep == 1) {
    if (is_open() == true) close_server_connection();
  }

  --open_clients_rep;
}

/**
 * Returns current buffersize in sample frames. 
 * Always returns 0 if manager is not connected.
 */
long int AUDIO_IO_JACK_MANAGER::buffersize(void) const
{
  if (is_open() != true) return(0);

  return(buffersize_rep);
}

bool AUDIO_IO_JACK_MANAGER::is_running(void) const
{ 
  if (engine_repp != 0) {
    return(engine_repp->is_running());
  }
  return(false);
}

/**
 * Returns the current JACK engine sample rate.
 * Always returns 0 if manager is not connected.
 */
SAMPLE_SPECS::sample_rate_t AUDIO_IO_JACK_MANAGER::samples_per_second(void) const
{
  if (is_open() != true) return(0);

  return(srate_rep);
}

long int AUDIO_IO_JACK_MANAGER::read_samples(int client_id, void* target_buffer, long int samples)
{
  // DEBUG_CFLOW_STATEMENT(cerr << endl << "read_samples:" << client_id);

  jack_default_audio_sample_t* ptr = 
    static_cast<jack_default_audio_sample_t*>(target_buffer);
  eca_jack_node_t* node = get_node(client_id);

  list<eca_jack_port_data*>::const_iterator p = node->ports.begin();
  while(p != node->ports.end()) {
    if ((*p)->cb_buffer != 0) {
      memcpy(ptr, (*p)->cb_buffer, buffersize_rep * sizeof(jack_default_audio_sample_t));
      ptr += buffersize_rep;
    }
    ++p;
  }

  return(buffersize_rep);
}

void AUDIO_IO_JACK_MANAGER::write_samples(int client_id, void* target_buffer, long int samples)
{
  // DEBUG_CFLOW_STATEMENT(cerr << endl << "write_samples:" << client_id);

  long int writesamples = (samples <= buffersize_rep) ? samples : buffersize_rep;
  jack_default_audio_sample_t* ptr =
    static_cast<jack_default_audio_sample_t*>(target_buffer);

  eca_jack_node_t* node = get_node(client_id);
  list<eca_jack_port_data*>::const_iterator p = node->ports.begin();
  while(p != node->ports.end()) {
    if ((*p)->cb_buffer != 0) {
      memcpy((*p)->cb_buffer, ptr, writesamples * sizeof(jack_default_audio_sample_t));
      ptr += writesamples;
    }
    ++p;
  }
}

/**
 * Opens connection to the JACK server. Sets
 * is_open() to 'true' if connection is 
 * succesfully opened.
 *
 * @pre is_open() != true
 */
void AUDIO_IO_JACK_MANAGER::open_server_connection(void)
{
  // --
  DBC_REQUIRE(is_open() != true);
  // --

  string client_name (jackname_rep);
  int n;

  for(n = 0; n < AUDIO_IO_JACK_MANAGER::instance_limit; n++) {
    client_repp = jack_client_new (client_name.c_str());
    if (client_repp != 0) break;
    client_name = jackname_rep + "_" + kvu_numtostr(n + 2);
  }

  if (n != AUDIO_IO_JACK_MANAGER::instance_limit) {
    srate_rep = static_cast<long int>(jack_get_sample_rate(client_repp));
    /* FIXME: add better control of allocated memory */
    cb_allocated_frames_rep = buffersize_rep = static_cast<long int>(jack_get_buffer_size(client_repp));
    shutdown_request_rep = false;
    jackslave_seekahead_rep = 4096 / buffersize_rep + 1;

    /* set callbacks */
    jack_set_process_callback(client_repp, eca_jack_process, static_cast<void*>(this));
    jack_set_sample_rate_callback(client_repp, eca_jack_srate, static_cast<void*>(this));
    jack_on_shutdown(client_repp, eca_jack_shutdown, static_cast<void*>(this));

#if ECA_JACK_TRANSPORT_API >= 2
    /* become the timebase master if requested */
    if (mode_rep == AUDIO_IO_JACK_MANAGER::Master) {
      ECA_LOG_MSG(ECA_LOGGER::user_objects, 
		  "(audioio-jack-manager) registering as the timebase master");
      jack_engine_takeover_timebase(client_repp);

      last_transport_state_rep = JackTransportStopped;
      transport_info_rep.transport_state = JackTransportStopped;
      if (engine_repp != 0) 
	transport_info_rep.frame = engine_repp->current_position_in_samples();
      else
	transport_info_rep.frame = 0;
      transport_info_rep.valid = static_cast<jack_transport_bits_t>(JackTransportState | JackTransportPosition);

      jack_set_transport_info(client_repp, &transport_info_rep);
    }
#endif

    open_rep = true;

#ifdef PROFILE_CALLBACK_EXECUTION
    profile_callback_timer.set_lower_bound_seconds(0.001f);
    profile_callback_timer.set_upper_bound_seconds(0.005f);
#endif

  }
  else {
    ECA_LOG_MSG(ECA_LOGGER::info, "(audioio-jack-manager) Error! Cannot connect to JACK server!");
    open_rep = false;
  }
}

/**
 * Closes connection to the JACK server.
 *
 * @pre is_open() == true
 * @post is_open() != true
 */
void AUDIO_IO_JACK_MANAGER::close_server_connection(void)
{
  // --
  DBC_REQUIRE(is_open() == true);
  // --

  // FIXME: add proper unregistration
  // iterate over cids: unregister_jack_ports()

  if (shutdown_request_rep != true) {
    jack_client_close (client_repp);
  }
  else {
    shutdown_request_rep = false;
  }

  open_rep = false;

  ECA_LOG_MSG(ECA_LOGGER::info, 
		"(audioio-jack-manager) Connection closed!");

#ifdef PROFILE_CALLBACK_EXECUTION
  cerr << profile_callback_timer.to_string() << endl;
#endif 

  // --
  DBC_ENSURE(is_open() != true);
  DBC_REQUIRE(shutdown_request_rep != true);
  // --
}

/**
 * Fetches total port latency information.
 */
void AUDIO_IO_JACK_MANAGER::get_total_port_latency(jack_client_t* client, eca_jack_port_data_t* ports)
{
  ports->total_latency = jack_port_get_total_latency(client, ports->jackport);
  ECA_LOG_MSG(ECA_LOGGER::user_objects, 
	      "(audioio-jack-manager) Total latency for port '" +
	      string(jack_port_name(ports->jackport)) +
	      "' is " + kvu_numtostr(ports->total_latency) + ".");
}

/**
 * Connects ports of node 'node'. 
 *
 * @param node pointers to a node object
 * @param connect whether to connect (true) or disconnect (false)
 */
void AUDIO_IO_JACK_MANAGER::set_node_connection(eca_jack_node_t* node, bool connect)
{
  list<eca_jack_port_data*>::iterator p = node->ports.begin();
  while(p != node->ports.end()) {
    if ((*p)->cb_buffer != 0) {
      string ecaport = (*p)->autoconnect_string;
      if (ecaport.size() > 0) {
	string jackport (jack_port_name((*p)->jackport));
	const string* fromport = &ecaport;
	const string* toport = &jackport;
	if (node->aobj->io_mode() != AUDIO_IO::io_read) {
	  /* output object -> switch direction */
	  fromport = &jackport;
	  toport = &ecaport;
	}
	
	if (connect == true) {
	  ECA_LOG_MSG(ECA_LOGGER::system_objects, "(audioio-jack-manager) jack_port_connect() ");
	  if (jack_connect (client_repp,
			    fromport->c_str(), 
			    toport->c_str())) {
	    ECA_LOG_MSG(ECA_LOGGER::info, 
			"(audioio-jack-manager) Error! Cannot make connection " + 
			*fromport + " -> " + *toport + ".");
	  }
	  else {
	    AUDIO_IO_JACK_MANAGER::get_total_port_latency(client_repp, *p);
	  }
	}
	else {
	  ECA_LOG_MSG(ECA_LOGGER::system_objects, "(audioio-jack-manager) jack_port_disconnect()");
	  /* don't call jack_disconnect() if engine has shut down */
	  if (jack_disconnect(client_repp, 
			      fromport->c_str(),
			      toport->c_str())) {
	    ECA_LOG_MSG(ECA_LOGGER::info, 
			"(audioio-jack-manager) Error! Cannot disconnect " + 
			*fromport + " -> " + *toport + ".");
	  }
	}
      }
    }
    
    ++p;
  }
}

/**
 * Connects ports of all registered nodes.
 *
 * @see set_node_connection()
 */
void AUDIO_IO_JACK_MANAGER::connect_all_nodes(void)
{ 
  if (shutdown_request_rep != true) {
    list<eca_jack_node_t*>::iterator p = node_list_rep.begin();
    while(p != node_list_rep.end()) {
      set_node_connection(*p, true);
      ++p;
    }
  }
  else {
    if (is_open() == true) close_server_connection();
  }
}

/**
 * Disconnects all ports of registered nodes.
 *
 * @see set_node_connection()
 */
void AUDIO_IO_JACK_MANAGER::disconnect_all_nodes(void)
{
  list<eca_jack_node_t*>::iterator p = node_list_rep.begin();
  while(p != node_list_rep.end()) {
    set_node_connection(*p, false);
    ++p;
  }
}

/**
 * Signals that exec() has exited.
 *
 * @see wait_for_exit();
 */
void AUDIO_IO_JACK_MANAGER::signal_exit(void)
{
  pthread_mutex_lock(&exit_mutex_rep);
  pthread_cond_signal(&exit_cond_rep);
  pthread_mutex_unlock(&exit_mutex_rep);
}

/**
 * Waits until exec() has exited.
 */
void AUDIO_IO_JACK_MANAGER::wait_for_exit(void)
{
  int ret = kvu_pthread_timed_wait(&exit_mutex_rep, &exit_cond_rep, 5);
  ECA_LOG_MSG(ECA_LOGGER::info, 
		kvu_pthread_timed_wait_result(ret, "(audioio_jack_manager) wait_for_exit"));
}

/**
 * Signals that client has stopped.
 *
 * @see wait_for_stop()
 */
void AUDIO_IO_JACK_MANAGER::signal_stop(void)
{
  pthread_mutex_lock(&exit_mutex_rep);
  pthread_cond_signal(&exit_cond_rep);
  pthread_mutex_unlock(&exit_mutex_rep);
}

/**
 * Waits until client has stopped (no more
 * callbacks).
 */
void AUDIO_IO_JACK_MANAGER::wait_for_stop(void)
{
  int ret = kvu_pthread_timed_wait(&stop_mutex_rep, &stop_cond_rep, 5);
  ECA_LOG_MSG(ECA_LOGGER::info, 
		kvu_pthread_timed_wait_result(ret, "(audioio_jack_manager) wait_for_stop"));
}
