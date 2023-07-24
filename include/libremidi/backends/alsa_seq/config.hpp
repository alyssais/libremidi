#pragma once
#include <libremidi/backends/alsa_raw/config.hpp>

extern "C" typedef struct _snd_seq snd_seq_t;

namespace libremidi
{

// Possible parameters:
// - Direct / non-direct output
// - Timer source used (high resolution, etc)
// - Timestamping
// - Tempo, ppq?

struct alsa_sequencer_input_configuration
{
  std::string client_name;
  std::function<bool(const manual_poll_parameters&)> manual_poll;

  snd_seq_t* context{};
};

struct alsa_sequencer_output_configuration
{
  std::string client_name;

  snd_seq_t* context{};
};

}
