/*----------------------------------------------------------------------------
* ChucK-DMX: A plugin for ChucK that enables the sending of DMX over serial or over ethernet via the ArtNet and sACN network protocols.
* 
* author: Ben Hoang (https://ccrma.stanford.edu/~hoangben/)
* date: Fall 2025
-----------------------------------------------------------------------------*/
#include "chugin.h"
#include "serial/serial.h" // serial
#include "sacn/cpp/source.h" //sACN

#include <string>
#include <iostream>
#include <cstring>
#include <cmath>
#include <mutex>

extern "C" {
#include "artnet/artnet.h"
}

#ifdef _WIN32
#include <windows.h>
#define usleep(x) Sleep((x)/1000)
#else
#include <unistd.h>
#endif

CK_DLL_CTOR(dmx_ctor);
CK_DLL_DTOR(dmx_dtor);

CK_DLL_MFUN(dmx_get_protocol);
CK_DLL_MFUN(dmx_protocol);
CK_DLL_MFUN(dmx_get_channel);
CK_DLL_MFUN(dmx_channel);
CK_DLL_MFUN(dmx_get_rate);
CK_DLL_MFUN(dmx_rate);

CK_DLL_MFUN(dmx_init);
CK_DLL_MFUN(dmx_send);

// serial
CK_DLL_MFUN(dmx_get_port);
CK_DLL_MFUN(dmx_port);

// sACN and ArtNet
CK_DLL_MFUN(dmx_get_universe);
CK_DLL_MFUN(dmx_universe);

// internal data offset for C++ class pointer storage
t_CKINT dmx_data_offset = 0;

class DMX {
public:
    enum class Protocol { Serial_Raw, Serial, sACN, ArtNet };

    DMX(t_CKFLOAT fs) {
        std::lock_guard<std::mutex> lock(dmx_mutex);
        memset(dmx_data, 0, sizeof(dmx_data));
        dmx_data[0] = 0; // DMX start code
    }

    ~DMX() {
        deinit_Serial();
        deinit_sACN();
        deinit_ArtNet();
    }

    Protocol protocol() {
        std::lock_guard<std::mutex> lock(state_mutex);
        return _protocol;
    }
    void protocol(Protocol protocol) {
        std::lock_guard<std::mutex> lock(state_mutex);
        _protocol = protocol;
    }

    void channel(int channel, unsigned char value) {
        if (channel < 1 || channel > 512)
            return;
        std::lock_guard<std::mutex> lock(dmx_mutex);
        dmx_data[channel] = value;
    }

    int rate() {
        std::lock_guard<std::mutex> lock(state_mutex);
        if (update_interval_ms <= 0.0) return 0;
        return static_cast<int>(std::round(1000.0 / update_interval_ms));
    }
    void rate(int hz) {
        if (hz < 1 || hz > 44) {
            throw std::invalid_argument("Update rate must be between 1 and 44 Hz");
        }
        std::lock_guard<std::mutex> lock(state_mutex);
        update_interval_ms = 1000.0 / hz;
    }

    void init() {
        std::lock_guard<std::mutex> lock(state_mutex);

        deinit_Serial();
        deinit_sACN();
        deinit_ArtNet();

        switch (_protocol) {
        case Protocol::Serial_Raw:
            init_Serial();
            break;
        case Protocol::Serial:
            init_Serial();
            break;
        case Protocol::sACN:
            init_sACN();
            break;
        case Protocol::ArtNet:
            init_ArtNet();
            break;
        }
        last_send_time = std::chrono::steady_clock::now() - std::chrono::milliseconds(static_cast<int>(update_interval_ms));
    }

    void send() {
        // Snapshot DMX data under lock
        unsigned char snapshot[513];
        {
            std::lock_guard<std::mutex> lock(dmx_mutex);
            memcpy(snapshot, dmx_data, sizeof(dmx_data));
        }

        Protocol current_protocol;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            current_protocol = _protocol;
        }

        switch (current_protocol) {
        case Protocol::Serial_Raw:
        case Protocol::Serial:
            send_Serial(snapshot);
            break;
        case Protocol::sACN:
            send_sACN(snapshot);
            break;
        case Protocol::ArtNet:
            send_ArtNet(snapshot);
            break;
        }
    }

    std::string port() {
        std::lock_guard<std::mutex> lock(state_mutex);
        return serial_port;
    }
    void port(const std::string& name) {
        std::lock_guard<std::mutex> lock(state_mutex);
        serial_port = name;
    }

    int universe() {
        std::lock_guard<std::mutex> lock(state_mutex);
        return _universe;
    }
    void universe(int universe) {
        std::lock_guard<std::mutex> lock(state_mutex);
        _universe = universe;
    }

private:
    Protocol _protocol{ Protocol::Serial };
    int _universe{ 1 };
    unsigned char dmx_data[513];

    std::mutex dmx_mutex;     // protects dmx_data
    std::mutex state_mutex;   // protects protocol and universe

    double update_interval_ms{ 1000.0 / 44.0 }; // default to 44 Hz
    std::chrono::steady_clock::time_point last_send_time = std::chrono::steady_clock::now();

    // Serial
    serial::Serial serial;
    std::string serial_port;
    std::mutex serial_mutex;

    // sACN
    sacn::Source source;

    // ArtNet
    artnet_node artnet_node_obj = nullptr;

    void openPort() {
        if (serial.isOpen())
            serial.close();

        serial.setPort(serial_port);
        serial.setBaudrate(250000);
        serial.setBytesize(serial::eightbits);
        serial.setParity(serial::parity_none);
        serial.setStopbits(serial::stopbits_two);
        serial.setFlowcontrol(serial::flowcontrol_none);
        serial::Timeout timeout = serial::Timeout::simpleTimeout(1000);
        serial.setTimeout(timeout);
        serial.open();
    }

    void closePort() {
        try {
            if (serial.isOpen())
                serial.close();
        }
        catch (...) {
            // ignore
        }
    }

    void init_Serial() {
        if (serial_port.empty()) {
            throw std::runtime_error("Serial port name not set. Call port() before init().");
        }
        openPort();
    }

    void deinit_Serial() {
        closePort();
    }

    void init_sACN() {
        sacn::Init();
        etcpal::Uuid cid = etcpal::Uuid::OsPreferred();
        if (cid.IsNull()) {
            throw std::runtime_error("Failed to generate UUID for sACN Source CID");
        }

        sacn::Source::Settings settings{ cid, "ChucK DMX" };
        etcpal::Error error = source.Startup(settings);
        if (!error.IsOk()) {
            throw std::runtime_error(error.ToString());
        }

        sacn::Source::UniverseSettings universe_settings{ static_cast<uint16_t>(_universe) };
        error = source.AddUniverse(universe_settings);
        if (!error.IsOk()) {
            throw std::runtime_error(error.ToString());
        }
    }

    void deinit_sACN() {
        source.Shutdown();
        sacn::Deinit();
    }

    void init_ArtNet() {
        artnet_node_obj = artnet_new(nullptr, 0);
        if (!artnet_node_obj) {
            throw std::runtime_error("Failed to create libartnet node");
        }

        artnet_set_short_name(artnet_node_obj, "ChucK DMX");
        artnet_set_long_name(artnet_node_obj, "ChucK DMX ArtNet Node");

        artnet_set_node_type(artnet_node_obj, ARTNET_NODE);

        artnet_set_port_type(artnet_node_obj, 0, ARTNET_ENABLE_INPUT, ARTNET_PORT_DMX);

        // Configure universe
        uint8_t subnet = (_universe >> 4) & 0x0F;
        uint8_t uni = _universe & 0x0F;
        artnet_set_subnet_addr(artnet_node_obj, subnet);
        artnet_set_port_addr(artnet_node_obj, 0, ARTNET_INPUT_PORT, uni);

        if (artnet_start(artnet_node_obj) < 0) {
            artnet_destroy(artnet_node_obj);
            artnet_node_obj = nullptr;
            throw std::runtime_error("Failed to start libartnet node");
        }
    }

    void deinit_ArtNet() {
        artnet_destroy(artnet_node_obj);
        artnet_node_obj = nullptr;
    }

    void send_Serial(const unsigned char* snapshot)
    {
        std::lock_guard<std::mutex> lock(serial_mutex);

        if (!serial.isOpen()) {
            try {
                openPort();
            }
            catch (const std::exception& e) {
                std::cerr << "DMX Warning: Failed to open serial port: " << e.what() << std::endl;
                return;
            }
        }

        try {
            auto ms = static_cast<int>(std::round(update_interval_ms));
            auto now = std::chrono::steady_clock::now();
            {
                std::lock_guard<std::mutex> state_lock(state_mutex);
                if (now - last_send_time < std::chrono::milliseconds(ms))
                    return;
                last_send_time = now;
            }

            // --- Break condition only for "raw" FTDI/RS485 interfaces (OpenDMX style) ---
            if (_protocol == Protocol::Serial_Raw) {
                serial.setBreak(true);
                usleep(120);      // 88+ μs break low
                serial.setBreak(false);
                usleep(12);       // 8+ μs Mark After Break high
                serial.write(snapshot, 513); // 1 start + 512 DMX channels
                return;
            }

            // --- For buffered interfaces (e.g., Enttec DMX USB Pro, DMXking, DSD Tech): ---
            if (_protocol == Protocol::Serial) {
                const uint16_t payloadLen = 513;     // Start code + 512 DMX slots
                unsigned char buf[518];
                buf[0] = 0x7E;                       // Start of message
                buf[1] = 0x06;                       // SEND_DMX_PACKET command
                buf[2] = payloadLen & 0xFF;          // Data length LSB
                buf[3] = (payloadLen >> 8) & 0xFF;   // Data length MSB
                memcpy(&buf[4], snapshot, payloadLen);
                buf[4 + payloadLen] = 0xE7;          // End of message
                serial.write(buf, 5 + payloadLen);   // 0x7E .. data .. 0xE7
                return;
            }
        }
        catch (const std::exception& e) {
            std::cerr << "DMX Error: Serial write error: " << e.what() << std::endl;
            try { if (serial.isOpen()) serial.close(); }
            catch (...) { /* ignore */ }
        }
    }

    void send_sACN(const unsigned char* snapshot) {
        auto ms = static_cast<int>(std::round(update_interval_ms));
        auto now = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> state_lock(state_mutex);
            if (now - last_send_time < std::chrono::milliseconds(ms))
                return;
            last_send_time = now;
        }

        try {
            source.UpdateLevels(static_cast<uint16_t>(_universe), snapshot + 1, 512);
        }
        catch (const std::exception& e) {
            std::cerr << "DMX Warning: sACN UpdateLevels exception: " << e.what() << std::endl;
            deinit_sACN();
            try {
                init_sACN();
                std::cerr << "DMX Info: sACN reinitialized." << std::endl;
            }
            catch (const std::exception& e2) {
                std::cerr << "DMX Warning: sACN reconnect failed: " << e2.what() << std::endl;
            }
        }
    }

    void send_ArtNet(const unsigned char* snapshot) {
        auto ms = static_cast<int>(std::round(update_interval_ms));
        auto now = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> state_lock(state_mutex);
            if (now - last_send_time < std::chrono::milliseconds(ms))
                return;
            last_send_time = now;
        }

        int res = artnet_send_dmx(artnet_node_obj, 0, 512, snapshot + 1);
        if (res < 0) {
            std::cerr << "DMX Warning: libartnet failed to send DMX, attempting reconnect..." << std::endl;
            deinit_ArtNet();
            try {
                init_ArtNet();
                std::cerr << "DMX Info: ArtNet reinitialized." << std::endl;
            }
            catch (const std::exception& e) {
                std::cerr << "DMX Warning: ArtNet reconnect failed: " << e.what() << std::endl;
            }
        }
    }
};

// ChucK interface implementations

CK_DLL_CTOR(dmx_ctor) {
    OBJ_MEMBER_INT(SELF, dmx_data_offset) = 0;
    DMX* dmx_obj = new DMX(API->vm->srate(VM));
    OBJ_MEMBER_INT(SELF, dmx_data_offset) = (t_CKINT)dmx_obj;
}

CK_DLL_DTOR(dmx_dtor) {
    DMX* dmx_obj = (DMX*)OBJ_MEMBER_INT(SELF, dmx_data_offset);
    CK_SAFE_DELETE(dmx_obj);
    OBJ_MEMBER_INT(SELF, dmx_data_offset) = 0;
}

CK_DLL_MFUN(dmx_get_protocol) {
    DMX* dmx_obj = (DMX*)OBJ_MEMBER_INT(SELF, dmx_data_offset);
    if (!dmx_obj) { RETURN->v_int = -1; return; }
    RETURN->v_int = static_cast<int>(dmx_obj->protocol());
}
CK_DLL_MFUN(dmx_protocol) {
    DMX* dmx_obj = (DMX*)OBJ_MEMBER_INT(SELF, dmx_data_offset);
    if (!dmx_obj) return;

    int mode_int = GET_NEXT_INT(ARGS);
    try {
        if (mode_int == 0) dmx_obj->protocol(DMX::Protocol::Serial_Raw);
        else if (mode_int == 1) dmx_obj->protocol(DMX::Protocol::Serial);
        else if (mode_int == 2) dmx_obj->protocol(DMX::Protocol::sACN);
        else if (mode_int == 3) dmx_obj->protocol(DMX::Protocol::ArtNet);
        else
            throw std::runtime_error("Invalid protocol set, valid values: 0=Serial_Raw, 1=Serial, 2=sACN, 3=ArtNet");
    }
    catch (const std::exception& e) {
        std::cerr << "DMX Error in protocol(): " << e.what() << std::endl;
        throw e;
    }
}

CK_DLL_MFUN(dmx_channel) {
    DMX* dmx_obj = (DMX*)OBJ_MEMBER_INT(SELF, dmx_data_offset);
    if (!dmx_obj) return;

    int channel = GET_NEXT_INT(ARGS);
    int value = GET_NEXT_INT(ARGS);
    dmx_obj->channel(channel, (unsigned char)value);
}

CK_DLL_MFUN(dmx_get_rate) {
    DMX* dmx_obj = (DMX*)OBJ_MEMBER_INT(SELF, dmx_data_offset);
    if (!dmx_obj) { RETURN->v_int = -1; return; }
    RETURN->v_int = dmx_obj->rate();
}
CK_DLL_MFUN(dmx_rate) {
    DMX* dmx_obj = (DMX*)OBJ_MEMBER_INT(SELF, dmx_data_offset);
    if (!dmx_obj) return;

    int hz = GET_NEXT_INT(ARGS);
    try {
        dmx_obj->rate(hz);
    }
    catch (const std::exception& e) {
        std::cerr << "DMX Error in rate(): " << e.what() << std::endl;
        throw e;
    }
}

CK_DLL_MFUN(dmx_init) {
    DMX* dmx_obj = (DMX*)OBJ_MEMBER_INT(SELF, dmx_data_offset);
    if (!dmx_obj) return;

    try {
        dmx_obj->init();
    }
    catch (const std::exception& e) {
        std::cerr << "DMX Error in init(): " << e.what() << std::endl;
        throw e;
    }
}

CK_DLL_MFUN(dmx_send) {
    DMX* dmx_obj = (DMX*)OBJ_MEMBER_INT(SELF, dmx_data_offset);
    if (!dmx_obj) return;

    try {
        dmx_obj->send();
    }
    catch (const std::exception& e) {
        std::cerr << "DMX Error in send(): " << e.what() << std::endl;
        throw e;
    }
}

// Serial

CK_DLL_MFUN(dmx_get_port) {
    DMX* dmx_obj = (DMX*)OBJ_MEMBER_INT(SELF, dmx_data_offset);
    if (!dmx_obj) { RETURN->v_string = API->object->create_string(VM, "", 0); return; }
    const std::string& port = dmx_obj->port();
    RETURN->v_string = API->object->create_string(VM, port.c_str(), (t_CKUINT)port.length());
}
CK_DLL_MFUN(dmx_port) {
    DMX* dmx_obj = (DMX*)OBJ_MEMBER_INT(SELF, dmx_data_offset);
    if (!dmx_obj) return;

    std::string name = GET_NEXT_STRING_SAFE(ARGS);
    try {
        dmx_obj->port(name);
    }
    catch (const std::exception& e) {
        std::cerr << "DMX Error in port(): " << e.what() << std::endl;
        throw e;
    }
}

// sACN

CK_DLL_MFUN(dmx_get_universe) {
    DMX* dmx_obj = (DMX*)OBJ_MEMBER_INT(SELF, dmx_data_offset);
    if (!dmx_obj) { RETURN->v_int = -1; return; }
    RETURN->v_int = dmx_obj->universe();
}
CK_DLL_MFUN(dmx_universe) {
    DMX* dmx_obj = (DMX*)OBJ_MEMBER_INT(SELF, dmx_data_offset);
    if (!dmx_obj) return;

    int universe = GET_NEXT_INT(ARGS);
    try {
        dmx_obj->universe(universe);
    }
    catch (const std::exception& e) {
        std::cerr << "DMX Error in universe(): " << e.what() << std::endl;
        throw e;
    }
}

CK_DLL_INFO(DMX)
{
    // the version string of this chugin, e.g., "v1.2.1"
    QUERY->setinfo(QUERY, CHUGIN_INFO_CHUGIN_VERSION, "v0.1.0");
    // the author(s) of this chugin, e.g., "Alice Baker & Carl Donut"
    QUERY->setinfo(QUERY, CHUGIN_INFO_AUTHORS, "Ben Hoang");
    // text description of this chugin; what is it? what does it do? who is it for?
    QUERY->setinfo(QUERY, CHUGIN_INFO_DESCRIPTION, 
        "ChucK-DMX: A plugin for ChucK that enables the sending of DMX "
        "over serial or over ethernet via the ArtNet and sACN network protocols.");
    // (optional) URL of the homepage for this chugin
    QUERY->setinfo(QUERY, CHUGIN_INFO_URL, "https://ccrma.stanford.edu/~hoangben/ChucK-DMX/");
    // (optional) contact email
    QUERY->setinfo(QUERY, CHUGIN_INFO_EMAIL, "hoangben@ccrma.stanford.edu");
}

// ChucK Query function

CK_DLL_QUERY(DMX) {
    QUERY->setname(QUERY, "DMX");

    QUERY->begin_class(QUERY, "DMX", "Object");
    QUERY->doc_class(QUERY,
        "The DMX class provides control over DMX512 lighting data and protocol selection for ChucK. "
        "It supports sending DMX using Serial (including Enttec-style USB and OpenDMX), sACN (E1.31), "
        "and Art-Net output, with runtime selection of protocol, port, universe, and refresh rate. "
        "Configure protocol, port/universe, and rate, then call init() to instantiate the connection."
    );

    QUERY->add_ctor(QUERY, dmx_ctor);
    QUERY->add_dtor(QUERY, dmx_dtor);

    QUERY->add_mfun(QUERY, dmx_channel, "void", "channel");
    QUERY->add_arg(QUERY, "int", "channel");
    QUERY->add_arg(QUERY, "int", "value");
    QUERY->doc_func(QUERY,
        "Set a DMX channel (1–512) to a value (0–255). Changes are staged in the transmission buffer and take effect upon next send()."
    );

    QUERY->add_mfun(QUERY, dmx_get_rate, "int", "rate");
    QUERY->doc_func(QUERY,
        "Get the current DMX update rate in Hz (frames per second)."
    );

    QUERY->add_mfun(QUERY, dmx_rate, "void", "rate");
    QUERY->add_arg(QUERY, "int", "rate");
    QUERY->doc_func(QUERY,
        "Set the DMX update rate (Hz). Acceptable range is 1–44 Hz. "
        "This controls the minimum interval between consecutive sends."
    );

    QUERY->add_mfun(QUERY, dmx_init, "void", "init");
    QUERY->doc_func(QUERY,
        "Initialize the DMX connection using the last-set protocol, port (for serial), and universe (for sACN/Art-Net). "
        "Call this after adjusting configuration parameters to reconfigure the DMX device."
    );

    QUERY->add_mfun(QUERY, dmx_send, "void", "send");
    QUERY->doc_func(QUERY,
        "Transmit the currently-set DMX buffer over the active protocol. Will enforce the minimum rate set by rate()."
    );

    // Serial

    QUERY->add_mfun(QUERY, dmx_get_port, "string", "port");
    QUERY->doc_func(QUERY,
        "Get the currently configured serial port string (e.g., '/dev/ttyUSB0' or 'COM3'). "
        "This should be used for Serial and Serial_Raw protocols only."
    );

    QUERY->add_mfun(QUERY, dmx_port, "void", "port");
    QUERY->add_arg(QUERY, "string", "port");
    QUERY->doc_func(QUERY,
        "Set the serial port name used when protocol is Serial or Serial_Raw. "
        "This should be used for Serial and Serial_Raw protocols only. "
        "Configure this before init()."
    );

    // sACN and ArtNet

    QUERY->add_mfun(QUERY, dmx_get_protocol, "int", "protocol");
    QUERY->doc_func(QUERY,
        "Get the current DMX protocol as an integer: 0=Serial_Raw, 1=Serial, 2=sACN, 3=ArtNet."
    );

    QUERY->add_mfun(QUERY, dmx_protocol, "void", "protocol");
    QUERY->add_arg(QUERY, "int", "protocol");
    QUERY->doc_func(QUERY,
        "Set the DMX protocol to use. 0=Serial_Raw (break timing), 1=Serial (Enttec DMX USB Pro, etc), "
        "2=sACN (E1.31 streaming ACN), 3=ArtNet. "
        "Configure this before init()."
    );

    QUERY->add_mfun(QUERY, dmx_get_universe, "int", "universe");
    QUERY->doc_func(QUERY,
        "Get the current DMX universe used for sACN or ArtNet output. "
        "Irrelevant for Serial or Serial_Raw protocols."
    );

    QUERY->add_mfun(QUERY, dmx_universe, "void", "universe");
    QUERY->add_arg(QUERY, "int", "universe");
    QUERY->doc_func(QUERY,
        "Set the DMX universe for sACN or ArtNet output. "
        "This should be used for sACN and ArtNet protocols only. "
        "Configure this before init()."
    );

    dmx_data_offset = QUERY->add_mvar(QUERY, "int", "@dmx_data", false);

    QUERY->end_class(QUERY);

    return TRUE;
}
