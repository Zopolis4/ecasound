#ifndef INCLUDED_AUDIOIO_PROXY_BUFFER_H
#define INCLUDED_AUDIOIO_PROXY_BUFFER_H

#include <kvutils/locks.h>
#include "audioio.h"
#include "samplebuffer.h"

/**
 * Buffer used between proxy server and client
 */
class AUDIO_IO_PROXY_BUFFER {

 public:

  ATOMIC_INTEGER readptr_rep;
  ATOMIC_INTEGER writeptr_rep;
  ATOMIC_INTEGER finished_rep;
  vector<SAMPLE_BUFFER> sbufs_rep;
  AUDIO_IO::Io_mode io_mode_rep;

  void reset(void);
  int read_space(void);
  int write_space(void);
  void advance_read_pointer(void);
  void advance_write_pointer(void);

  AUDIO_IO_PROXY_BUFFER(int number_of_buffers,
			long int buffersize,
			int channels,
			long int sample_rate);
};

#endif
