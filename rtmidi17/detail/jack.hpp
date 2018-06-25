#pragma once
#if __has_include(<weak_libjack.h>)
#include <weak_libjack.h>
#else
#include <jack/jack.h>
#include <jack/midiport.h>
#include <jack/ringbuffer.h>
#endif
#include <rtmidi17/detail/midi_api.hpp>
#include <rtmidi17/detail/semaphore.hpp>
#include <rtmidi17/rtmidi17.hpp>

//*********************************************************************//
//  API: UNIX JACK
//
//  Written primarily by Alexander Svetalkin, with updates for delta
//  time by Gary Scavone, April 2011.
//
//  *********************************************************************//

namespace rtmidi
{
struct jack_data
{
  static const constexpr auto ringbuffer_size = 16384;
  jack_client_t* client{};
  jack_port_t* port{};
  jack_ringbuffer_t* buffSize{};
  jack_ringbuffer_t* buffMessage{};
  jack_time_t lastTime{};

  rtmidi::semaphore sem_cleanup;
  rtmidi::semaphore sem_needpost{};

  midi_in_api::in_data* rtMidiIn{};
};

class observer_jack final : public observer_api
{
public:
  observer_jack(observer::callbacks&& c) : observer_api{std::move(c)}
  {
  }

  ~observer_jack()
  {
  }
};

class midi_in_jack final : public midi_in_api
{
public:
  midi_in_jack(std::string_view cname, unsigned int queueSizeLimit)
    : midi_in_api{&data, queueSizeLimit}
  {
    // TODO do like the others
    data.rtMidiIn = &inputData_;
    data.port = nullptr;
    data.client = nullptr;
    this->clientName = cname;

    connect();
  }

  ~midi_in_jack() override
  {
    midi_in_jack::close_port();

    if (data.client)
      jack_client_close(data.client);
  }

  rtmidi::API get_current_api() const noexcept override
  {
    return rtmidi::API::UNIX_JACK;
  }

  void open_port(unsigned int portNumber, std::string_view portName) override
  {
    connect();

    // Creating new port
    if (data.port == nullptr)
      data.port = jack_port_register(
          data.client, portName.data(), JACK_DEFAULT_MIDI_TYPE, JackPortIsInput, 0);

    if (data.port == nullptr)
    {
      error<driver_error>("MidiInJack::openPort: JACK error creating port");
      return;
    }

    // Connecting to the output
    std::string name = get_port_name(portNumber);
    jack_connect(data.client, name.c_str(), jack_port_name(data.port));
  }

  void open_virtual_port(std::string_view portName) override
  {
    connect();
    if (!data.port)
      data.port = jack_port_register(
          data.client, portName.data(), JACK_DEFAULT_MIDI_TYPE, JackPortIsInput, 0);

    if (!data.port)
    {
      error<driver_error>("MidiInJack::openVirtualPort: JACK error creating virtual port");
    }
  }

  void close_port() override
  {
    if (data.port == nullptr)
      return;
    jack_port_unregister(data.client, data.port);
    data.port = nullptr;
  }

  void set_client_name(std::string_view clientName) override
  {
    warning(
        "MidiInJack::setClientName: this function is not implemented for the "
        "UNIX_JACK API!");
  }

  void set_port_name(std::string_view portName) override
  {
#if defined(RTMIDI17_JACK_HAS_PORT_RENAME)
    jack_port_rename(data.client, data.port, portName.c_str());
#else
    jack_port_set_name(data.port, portName.data());
#endif
  }

  unsigned int get_port_count() override
  {
    int count = 0;
    connect();
    if (!data.client)
      return 0;

    // List of available ports
    auto ports = jack_get_ports(data.client, nullptr, JACK_DEFAULT_MIDI_TYPE, JackPortIsOutput);

    if (!ports)
      return 0;

    while (ports[count] != nullptr)
      count++;

    jack_free(ports);

    return count;
  }

  std::string get_port_name(unsigned int portNumber) override
  {
    std::string retStr;

    connect();

    // List of available ports
    auto ports = jack_get_ports(data.client, nullptr, JACK_DEFAULT_MIDI_TYPE, JackPortIsOutput);

    // Check port validity
    if (ports == nullptr)
    {
      warning("MidiInJack::getPortName: no ports available!");
      return retStr;
    }

    if (ports[portNumber] == nullptr)
    {
      std::ostringstream ost;
      ost << "MidiInJack::getPortName: the 'portNumber' argument (" << portNumber
          << ") is invalid.";
      warning(ost.str());
    }
    else
      retStr.assign(ports[portNumber]);

    jack_free(ports);
    return retStr;
  }

private:
  void connect()
  {
    if (data.client)
      return;

    // Initialize JACK client
    data.client = jack_client_open(clientName.c_str(), JackNoStartServer, nullptr);
    if (data.client == nullptr)
    {
      warning("MidiInJack::initialize: JACK server not running?");
      return;
    }

    jack_set_process_callback(data.client, jackProcessIn, &data);
    jack_activate(data.client);
  }

  static int jackProcessIn(jack_nframes_t nframes, void* arg)
  {
    jack_data& jData = *(jack_data*)arg;
    midi_in_api::in_data& rtData = *jData.rtMidiIn;
    jack_midi_event_t event;
    jack_time_t time;

    // Is port created?
    if (jData.port == nullptr)
      return 0;
    void* buff = jack_port_get_buffer(jData.port, nframes);

    // We have midi events in buffer
    uint32_t evCount = jack_midi_get_event_count(buff);
    for (uint32_t j = 0; j < evCount; j++)
    {
      message m;

      jack_midi_event_get(&event, buff, j);

      m.bytes.assign(event.buffer, event.buffer + event.size);

      // Compute the delta time.
      time = jack_get_time();
      if (rtData.firstMessage == true)
        rtData.firstMessage = false;
      else
        m.timestamp = (time - jData.lastTime) * 0.000001;

      jData.lastTime = time;

      if (!rtData.continueSysex)
      {
        if (rtData.userCallback)
        {
          rtData.userCallback(std::move(m));
        }
        else
        {
          // As long as we haven't reached our queue size limit, push the
          // message.
          if (!rtData.queue.push(std::move(m)))
            std::cerr << "\nMidiInJack: message queue limit reached!!\n\n";
        }
      }
    }

    return 0;
  }

  std::string clientName;
  jack_data data;
};

class midi_out_jack final : public midi_out_api
{
public:
  midi_out_jack(std::string_view cname)
  {
    data.port = nullptr;
    data.client = nullptr;
    this->clientName = cname;

    connect();
  }

  ~midi_out_jack() override
  {
    midi_out_jack::close_port();

    // Cleanup
    jack_ringbuffer_free(data.buffSize);
    jack_ringbuffer_free(data.buffMessage);
    if (data.client)
    {
      jack_client_close(data.client);
    }
  }

  rtmidi::API get_current_api() const noexcept override
  {
    return rtmidi::API::UNIX_JACK;
  }

  void open_port(unsigned int portNumber, std::string_view portName) override
  {
    connect();

    // Creating new port
    if (!data.port)
      data.port = jack_port_register(
          data.client, portName.data(), JACK_DEFAULT_MIDI_TYPE, JackPortIsOutput, 0);

    if (!data.port)
    {
      error<driver_error>("MidiOutJack::openPort: JACK error creating port");
      return;
    }

    // Connecting to the output
    std::string name = get_port_name(portNumber);
    jack_connect(data.client, jack_port_name(data.port), name.c_str());
  }

  void open_virtual_port(std::string_view portName) override
  {
    connect();
    if (data.port == nullptr)
      data.port = jack_port_register(
          data.client, portName.data(), JACK_DEFAULT_MIDI_TYPE, JackPortIsOutput, 0);

    if (data.port == nullptr)
    {
      error<driver_error>("MidiOutJack::openVirtualPort: JACK error creating virtual port");
    }
  }

  void close_port() override
  {
    using namespace std::literals;
    if (data.port == nullptr)
      return;

    data.sem_needpost.notify();
    data.sem_cleanup.wait_for(1s);

    jack_port_unregister(data.client, data.port);
    data.port = nullptr;
  }

  void set_client_name(std::string_view clientName) override
  {
    warning(
        "MidiOutJack::setClientName: this function is not implemented for the "
        "UNIX_JACK API!");
  }

  void set_port_name(std::string_view portName) override
  {
#if defined(RTMIDI17_JACK_HAS_PORT_RENAME)
    jack_port_rename(data.client, data.port, portName.c_str());
#else
    jack_port_set_name(data.port, portName.data());
#endif
  }

  unsigned int get_port_count() override
  {
    int count = 0;
    connect();
    if (!data.client)
      return 0;

    // List of available ports
    const char** ports
        = jack_get_ports(data.client, nullptr, JACK_DEFAULT_MIDI_TYPE, JackPortIsInput);

    if (ports == nullptr)
      return 0;
    while (ports[count] != nullptr)
      count++;

    jack_free(ports);

    return count;
  }

  std::string get_port_name(unsigned int portNumber) override
  {
    std::string retStr("");

    connect();

    // List of available ports
    const char** ports
        = jack_get_ports(data.client, nullptr, JACK_DEFAULT_MIDI_TYPE, JackPortIsInput);

    // Check port validity
    if (ports == nullptr)
    {
      warning("MidiOutJack::getPortName: no ports available!");
      return retStr;
    }

    if (ports[portNumber] == nullptr)
    {
      std::ostringstream ost;
      ost << "MidiOutJack::getPortName: the 'portNumber' argument (" << portNumber
          << ") is invalid.";
      warning(ost.str());
    }
    else
      retStr.assign(ports[portNumber]);

    jack_free(ports);
    return retStr;
  }

  void send_message(const unsigned char* message, size_t size) override
  {
    int nBytes = static_cast<int>(size);

    // Write full message to buffer
    jack_ringbuffer_write(data.buffMessage, (const char*)message, nBytes);
    jack_ringbuffer_write(data.buffSize, (char*)&nBytes, sizeof(nBytes));
  }

private:
  std::string clientName;

  void connect()
  {
    if (data.client)
      return;

    // Initialize output ringbuffers
    data.buffSize = jack_ringbuffer_create(jack_data::ringbuffer_size);
    data.buffMessage = jack_ringbuffer_create(jack_data::ringbuffer_size);

    // Initialize JACK client
    data.client = jack_client_open(clientName.c_str(), JackNoStartServer, nullptr);
    if (data.client == nullptr)
    {
      warning("MidiOutJack::initialize: JACK server not running?");
      return;
    }

    jack_set_process_callback(data.client, jackProcessOut, &data);
    jack_activate(data.client);
  }

  static int jackProcessOut(jack_nframes_t nframes, void* arg)
  {
    auto& data = *(jack_data*)arg;

    // Is port created?
    if (data.port == nullptr)
      return 0;

    void* buff = jack_port_get_buffer(data.port, nframes);
    jack_midi_clear_buffer(buff);

    while (jack_ringbuffer_read_space(data.buffSize) > 0)
    {
      int space{};
      jack_ringbuffer_read(data.buffSize, (char*)&space, sizeof(int));
      auto midiData = jack_midi_event_reserve(buff, 0, space);

      jack_ringbuffer_read(data.buffMessage, (char*)midiData, space);
    }

    if (!data.sem_needpost.try_wait())
      data.sem_cleanup.notify();

    return 0;
  }

  jack_data data;
};

struct jack_backend
{
    using midi_in = midi_in_jack;
    using midi_out = midi_out_jack;
    using midi_observer = observer_jack;
    static const constexpr auto API = rtmidi::API::UNIX_JACK;
};
}
