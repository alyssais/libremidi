#pragma once
#include <libremidi/backends/alsa_seq/config.hpp>
#include <libremidi/backends/alsa_seq/helpers.hpp>
#include <libremidi/detail/midi_out.hpp>

namespace libremidi
{

class midi_out_alsa final
    : public midi_out_api
    , private alsa_data
    , private error_handler
{
public:
  struct
      : output_configuration
      , alsa_sequencer_output_configuration
  {
  } configuration;

  midi_out_alsa(output_configuration&& conf, alsa_sequencer_output_configuration&& apiconf)
      : configuration{std::move(conf), std::move(apiconf)}
  {
    // Set up the ALSA sequencer client.
    snd_seq_t* seq{};
    if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_OUTPUT, SND_SEQ_NONBLOCK) < 0)
    {
      error<driver_error>(
          "midi_out_alsa::initialize: error creating ALSA sequencer client "
          "object.");
      return;
    }

    // Set client name.
    snd_seq_set_client_name(seq, configuration.client_name.c_str());

    // Save our api-specific connection information.
    this->seq = seq;
    this->vport = -1;
    this->bufferSize = 32;
    this->coder = nullptr;
    int result = snd_midi_event_new(this->bufferSize, &this->coder);
    if (result < 0)
    {
      error<driver_error>(
          "midi_out_alsa::initialize: error initializing MIDI event "
          "parser!\n\n");
      return;
    }
    snd_midi_event_init(this->coder);
  }

  ~midi_out_alsa() override
  {
    // Close a connection if it exists.
    midi_out_alsa::close_port();

    // Cleanup.
    if (this->vport >= 0)
      snd_seq_delete_port(this->seq, this->vport);
    if (this->coder)
      snd_midi_event_free(this->coder);
    snd_seq_close(this->seq);
  }

  libremidi::API get_current_api() const noexcept override { return libremidi::API::LINUX_ALSA; }

  void open_port(unsigned int portNumber, std::string_view portName) override
  {
    if (connected_)
    {
      warning("midi_out_alsa::open_port: a valid connection already exists!");
      return;
    }

    unsigned int nSrc = this->get_port_count();
    if (nSrc < 1)
    {
      error<no_devices_found_error>("midi_out_alsa::open_port: no MIDI output sources found!");
      return;
    }

    snd_seq_port_info_t* pinfo{};
    snd_seq_port_info_alloca(&pinfo);
    if (alsa_seq::port_info(
            this->seq, pinfo, SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
            (int)portNumber)
        == 0)
    {
      error<invalid_parameter_error>(
          "midi_out_alsa::open_port: invalid 'portNumber' argument: "
          + std::to_string(portNumber));
      return;
    }

    snd_seq_addr_t sender{}, receiver{};
    receiver.client = snd_seq_port_info_get_client(pinfo);
    receiver.port = snd_seq_port_info_get_port(pinfo);
    sender.client = snd_seq_client_id(this->seq);

    if (this->vport < 0)
    {
      this->vport = snd_seq_create_simple_port(
          this->seq, portName.data(), SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
          SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
      if (this->vport < 0)
      {
        error<driver_error>("midi_out_alsa::open_port: ALSA error creating output port.");
        return;
      }
    }

    sender.port = this->vport;

    // Make subscription
    if (snd_seq_port_subscribe_malloc(&this->subscription) < 0)
    {
      snd_seq_port_subscribe_free(this->subscription);
      error<driver_error>("midi_out_alsa::open_port: error allocating port subscription.");
      return;
    }
    snd_seq_port_subscribe_set_sender(this->subscription, &sender);
    snd_seq_port_subscribe_set_dest(this->subscription, &receiver);
    snd_seq_port_subscribe_set_time_update(this->subscription, 1);
    snd_seq_port_subscribe_set_time_real(this->subscription, 1);
    if (snd_seq_subscribe_port(this->seq, this->subscription))
    {
      snd_seq_port_subscribe_free(this->subscription);
      error<driver_error>("midi_out_alsa::open_port: ALSA error making port connection.");
      return;
    }

    connected_ = true;
  }

  void open_virtual_port(std::string_view portName) override
  {
    if (this->vport < 0)
    {
      this->vport = snd_seq_create_simple_port(
          this->seq, portName.data(), SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
          SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);

      if (this->vport < 0)
      {
        error<driver_error>("midi_out_alsa::open_virtual_port: ALSA error creating virtual port.");
      }
    }
  }

  void close_port() override
  {
    if (connected_)
    {
      snd_seq_unsubscribe_port(this->seq, this->subscription);
      snd_seq_port_subscribe_free(this->subscription);
      this->subscription = nullptr;
      connected_ = false;
    }
  }

  void set_client_name(std::string_view clientName) override { this->set_client_name(clientName); }

  void set_port_name(std::string_view portName) override { this->set_port_name(portName); }

  unsigned int get_port_count() const override
  {
    return alsa_data::get_port_count(SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE);
  }

  std::string get_port_name(unsigned int portNumber) const override
  {
    return alsa_data::get_port_name(
        portNumber, SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE);
  }

  void send_message(const unsigned char* message, std::size_t size) override
  {
    int64_t result{};
    if (size > this->bufferSize)
    {
      this->bufferSize = size;
      result = snd_midi_event_resize_buffer(this->coder, size);
      if (result != 0)
      {
        error<driver_error>(
            "midi_out_alsa::send_message: ALSA error resizing MIDI event "
            "buffer.");
        return;
      }
    }

    std::size_t offset = 0;
    while (offset < size)
    {
      snd_seq_event_t ev;
      snd_seq_ev_clear(&ev);
      snd_seq_ev_set_source(&ev, this->vport);
      snd_seq_ev_set_subs(&ev);
      // FIXME direct is set but snd_seq_event_output_direct is not used...
      snd_seq_ev_set_direct(&ev);

      const int64_t nBytes = size; // signed to avoir potential overflow with size - offset below
      result = snd_midi_event_encode(this->coder, message + offset, (long)(nBytes - offset), &ev);
      if (result < 0)
      {
        warning("midi_out_alsa::send_message: event parsing error!");
        return;
      }

      if (ev.type == SND_SEQ_EVENT_NONE)
      {
        warning("midi_out_alsa::send_message: incomplete message!");
        return;
      }

      offset += result;

      result = snd_seq_event_output(this->seq, &ev);
      if (result < 0)
      {
        warning("midi_out_alsa::send_message: error sending MIDI message to port.");
        return;
      }
    }
    snd_seq_drain_output(this->seq);
  }

private:
  unsigned int bufferSize{};
};
}
