#ifndef INCLUDED_AUDIOFX_ENVELOPE_MODULATION_H
#define INCLUDED_AUDIOFX_ENVELOPE_MODULATION_H

#include <vector>

#include "samplebuffer_iterators.h"
#include "audiofx.h"

/**
 * Virtual base for envelope modulation effects.
 * @author Rob Coker
 */
class EFFECT_ENV_MOD : public EFFECT_BASE {

 public:
  virtual ~EFFECT_ENV_MOD(void) { }
};

/**
 * Pulse shaped gate
 * @author Rob Coker
 */
class EFFECT_PULSE_GATE: public EFFECT_ENV_MOD {

  SAMPLE_ITERATOR_INTERLEAVED i;
  parameter_type period;
  parameter_type stopTime;
  parameter_type currentTime;
  parameter_type incrTime;

 public:

  virtual string name(void) const { return("Pulse Gate"); }
  virtual string parameter_names(void) const  { return("freq-Hz,on-time-%"); }

  virtual void set_parameter(int param, parameter_type value);
  virtual parameter_type get_parameter(int param) const;

  virtual void init(SAMPLE_BUFFER *insample);
  virtual void process(void);

  EFFECT_PULSE_GATE (parameter_type freq_Hz = 1.0, parameter_type onTime_percent = 50.0);
  virtual ~EFFECT_PULSE_GATE(void) { }
  EFFECT_PULSE_GATE* clone(void)  { return new EFFECT_PULSE_GATE(*this); }
  EFFECT_PULSE_GATE* new_expr(void)  { return new EFFECT_PULSE_GATE(); }
};

/**
 * Wrapper class for pulse shaped gate providing 
 * a beats-per-minute (bpm) based parameters.
 *
 * @author Kai Vehmanen
 */
class EFFECT_PULSE_GATE_BPM : public EFFECT_ENV_MOD {

  EFFECT_PULSE_GATE pulsegate_rep;

 public:

  virtual string name(void) const { return("Pulse gate BPM"); }
  virtual string parameter_names(void) const  { return("bpm,on-time-msec"); }

  virtual void set_parameter(int param, parameter_type value);
  virtual parameter_type get_parameter(int param) const;

  virtual void init(SAMPLE_BUFFER *insample);
  virtual void process(void);

  EFFECT_PULSE_GATE_BPM (parameter_type bpm = 120.0, parameter_type ontime_percent = 5.0);
  virtual ~EFFECT_PULSE_GATE_BPM(void) { }
  EFFECT_PULSE_GATE_BPM* clone(void)  { return new EFFECT_PULSE_GATE_BPM(*this); }
  EFFECT_PULSE_GATE_BPM* new_expr(void)  { return new EFFECT_PULSE_GATE_BPM(); }
};

/**
 * Tremolo
 * @author Rob Coker
 */
class EFFECT_TREMOLO: public EFFECT_ENV_MOD {

  SAMPLE_ITERATOR_INTERLEAVED i;
  parameter_type freq;
  parameter_type depth;
  parameter_type currentTime;
  parameter_type incrTime;

 public:

  virtual string name(void) const { return("Tremolo"); }
  virtual string parameter_names(void) const  { return("bpm,depth-%"); }

  virtual void set_parameter(int param, parameter_type value);
  virtual parameter_type get_parameter(int param) const;

  virtual void init(SAMPLE_BUFFER *insample);
  virtual void process(void);

  EFFECT_TREMOLO (parameter_type freq_bpm = 60.0, parameter_type depth_percent = 100.0);
  virtual ~EFFECT_TREMOLO(void) { }
  EFFECT_TREMOLO* clone(void)  { return new EFFECT_TREMOLO(*this); }
  EFFECT_TREMOLO* new_expr(void)  { return new EFFECT_TREMOLO(); }
};

#endif
