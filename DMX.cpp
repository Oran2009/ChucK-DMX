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
#include <atomic>
#include <thread>
#include <map>
#include <vector>

extern "C" {
#include "artnet/artnet.h"
}

#ifdef _WIN32
#include <windows.h>
static void dmx_usleep(unsigned int us) {
    // Sleep() has millisecond granularity; for sub-ms DMX break timing
    // we need a busy-wait loop using QueryPerformanceCounter
    if (us == 0) return;
    LARGE_INTEGER freq, start, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    double target = start.QuadPart + (freq.QuadPart * us / 1000000.0);
    do {
        QueryPerformanceCounter(&now);
    } while (now.QuadPart < target);
}
#else
#include <unistd.h>
static void dmx_usleep(unsigned int us) { usleep(us); }
#endif

static inline int clamp_dmx(int val) {
    return val < 0 ? 0 : (val > 255 ? 255 : val);
}

CK_DLL_CTOR(dmx_ctor);
CK_DLL_DTOR(dmx_dtor);

CK_DLL_MFUN(dmx_get_protocol);
CK_DLL_MFUN(dmx_protocol);
CK_DLL_MFUN(dmx_get_channel);
CK_DLL_MFUN(dmx_channel);
CK_DLL_MFUN(dmx_channels);
CK_DLL_MFUN(dmx_get_rate);
CK_DLL_MFUN(dmx_rate);

CK_DLL_MFUN(dmx_init);
CK_DLL_MFUN(dmx_send);
CK_DLL_MFUN(dmx_blackout);
CK_DLL_MFUN(dmx_connected);

// serial
CK_DLL_MFUN(dmx_get_port);
CK_DLL_MFUN(dmx_port);
CK_DLL_SFUN(dmx_list_ports);

// sACN and ArtNet
CK_DLL_MFUN(dmx_get_universe);
CK_DLL_MFUN(dmx_universe);
CK_DLL_MFUN(dmx_add_universe);
CK_DLL_MFUN(dmx_remove_universe);

// sACN priority
CK_DLL_MFUN(dmx_get_priority);
CK_DLL_MFUN(dmx_priority);

// source name
CK_DLL_MFUN(dmx_get_name);
CK_DLL_MFUN(dmx_name);

// background thread
CK_DLL_MFUN(dmx_start);
CK_DLL_MFUN(dmx_stop);

// fade
CK_DLL_MFUN(dmx_fade);

// internal data offset for C++ class pointer storage
t_CKINT dmx_data_offset = 0;

// static protocol constants exposed to ChucK
static t_CKINT dmx_SERIAL_RAW = 0;
static t_CKINT dmx_SERIAL = 1;
static t_CKINT dmx_SACN = 2;
static t_CKINT dmx_ARTNET = 3;

// sACN global init reference count (shared across all DMX instances)
static std::mutex sacn_global_mutex;
static int sacn_ref_count = 0;

static void sacn_global_init() {
    std::lock_guard<std::mutex> lock(sacn_global_mutex);
    if (sacn_ref_count == 0)
        sacn::Init();
    sacn_ref_count++;
}

static void sacn_global_deinit() {
    std::lock_guard<std::mutex> lock(sacn_global_mutex);
    if (sacn_ref_count > 0) {
        sacn_ref_count--;
        if (sacn_ref_count == 0)
            sacn::Deinit();
    }
}

class DMX {
public:
    enum class Protocol { Serial_Raw, Serial, sACN, ArtNet };

    // Enttec DMX USB Pro protocol constants
    static constexpr uint8_t ENTTEC_START_MSG = 0x7E;
    static constexpr uint8_t ENTTEC_SEND_DMX  = 0x06;
    static constexpr uint8_t ENTTEC_END_MSG   = 0xE7;
    static constexpr uint16_t DMX_PAYLOAD_LEN = 513; // start code + 512 channels

    // Reconnect backoff
    static constexpr int RECONNECT_COOLDOWN_MS = 5000;

    struct FadeState {
        bool active;
        unsigned char start_value;
        unsigned char target_value;
        std::chrono::steady_clock::time_point start_time;
        std::chrono::milliseconds duration;
    };

    struct UniverseData {
        unsigned char dmx_data[513];
        FadeState fades[513];
        UniverseData() {
            memset(dmx_data, 0, sizeof(dmx_data));
            memset(fades, 0, sizeof(fades));
        }
    };

    DMX() {
        _universes[1]; // default universe 1
    }

    ~DMX() {
        stop();
        deinit_current();
    }

    Protocol protocol() {
        std::lock_guard<std::mutex> lock(state_mutex);
        return _protocol;
    }
    void protocol(Protocol p) {
        std::lock_guard<std::mutex> lock(state_mutex);
        _protocol = p;
    }

    int get_channel(int ch) {
        if (ch < 1 || ch > 512) return 0;
        int uni = _active_universe.load();
        std::lock_guard<std::mutex> lock(dmx_mutex);
        auto it = _universes.find(uni);
        if (it == _universes.end()) return 0;
        return it->second.dmx_data[ch];
    }

    void channel(int ch, int value) {
        if (ch < 1 || ch > 512) {
            std::cerr << "DMX Warning: channel() index " << ch << " out of range (1-512), ignored." << std::endl;
            return;
        }
        if (value < 0 || value > 255) {
            std::cerr << "DMX Warning: channel() value " << value << " clamped to 0-255." << std::endl;
            value = clamp_dmx(value);
        }
        int uni = _active_universe.load();
        // Cancel any active fade on this channel
        {
            std::lock_guard<std::mutex> flock(fade_mutex);
            auto it = _universes.find(uni);
            if (it != _universes.end())
                it->second.fades[ch].active = false;
        }
        std::lock_guard<std::mutex> lock(dmx_mutex);
        auto it = _universes.find(uni);
        if (it != _universes.end())
            it->second.dmx_data[ch] = static_cast<unsigned char>(value);
    }

    void channels(int startCh, const unsigned char* values, int count) {
        int uni = _active_universe.load();
        // Cancel fades for affected channels
        {
            std::lock_guard<std::mutex> flock(fade_mutex);
            auto it = _universes.find(uni);
            if (it != _universes.end()) {
                for (int i = 0; i < count; i++) {
                    int ch = startCh + i;
                    if (ch >= 1 && ch <= 512)
                        it->second.fades[ch].active = false;
                }
            }
        }
        std::lock_guard<std::mutex> lock(dmx_mutex);
        auto it = _universes.find(uni);
        if (it != _universes.end()) {
            for (int i = 0; i < count; i++) {
                int ch = startCh + i;
                if (ch < 1 || ch > 512) continue;
                it->second.dmx_data[ch] = values[i];
            }
        }
    }

    void blackout() {
        std::lock_guard<std::mutex> flock(fade_mutex);
        std::lock_guard<std::mutex> dlock(dmx_mutex);
        for (auto& [uni, udata] : _universes) {
            for (int i = 1; i <= 512; i++)
                udata.fades[i].active = false;
            memset(udata.dmx_data + 1, 0, 512);
        }
    }

    int rate() {
        std::lock_guard<std::mutex> lock(state_mutex);
        if (update_interval_ms <= 0.0) return 0;
        return static_cast<int>(std::round(1000.0 / update_interval_ms));
    }
    bool rate(int hz) {
        if (hz < 1 || hz > 44) {
            std::cerr << "DMX Warning: rate() must be 1-44 Hz, got " << hz << "." << std::endl;
            return false;
        }
        std::lock_guard<std::mutex> lock(state_mutex);
        update_interval_ms = 1000.0 / hz;
        return true;
    }

    bool init() {
        // Snapshot universe keys under dmx_mutex (before acquiring state_mutex)
        std::vector<int> uni_keys;
        {
            std::lock_guard<std::mutex> lock(dmx_mutex);
            for (auto& [k, v] : _universes)
                uni_keys.push_back(k);
        }

        std::lock_guard<std::mutex> lock(state_mutex);

        deinit_all_unlocked();

        bool ok = false;
        switch (_protocol) {
        case Protocol::Serial_Raw:
        case Protocol::Serial:
            ok = init_Serial();
            break;
        case Protocol::sACN:
            ok = init_sACN(uni_keys);
            break;
        case Protocol::ArtNet:
            ok = init_ArtNet(uni_keys);
            break;
        }

        if (ok)
            last_send_time = std::chrono::steady_clock::now() - std::chrono::milliseconds(static_cast<int>(update_interval_ms));

        return ok;
    }

    void send() {
        // Snapshot all universe data under dmx_mutex
        struct Snapshot { int universe; unsigned char data[513]; };
        std::vector<Snapshot> snapshots;
        {
            std::lock_guard<std::mutex> lock(dmx_mutex);
            snapshots.reserve(_universes.size());
            for (auto& [uni, udata] : _universes) {
                snapshots.emplace_back();
                snapshots.back().universe = uni;
                memcpy(snapshots.back().data, udata.dmx_data, 513);
            }
        }

        // Snapshot state
        Protocol current_protocol;
        std::map<int, int> artnet_ports;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            current_protocol = _protocol;
            artnet_ports = _artnet_port_map;

            // Rate limiting: check and update last_send_time atomically
            auto now = std::chrono::steady_clock::now();
            auto interval = std::chrono::milliseconds(static_cast<int>(std::round(update_interval_ms)));
            if (now - last_send_time < interval)
                return;
            last_send_time = now;
        }

        switch (current_protocol) {
        case Protocol::Serial_Raw:
        case Protocol::Serial: {
            // Serial only sends the active universe
            int active = _active_universe.load();
            for (auto& snap : snapshots) {
                if (snap.universe == active) {
                    send_Serial(snap.data, current_protocol);
                    break;
                }
            }
            break;
        }
        case Protocol::sACN: {
            bool any_failed = false;
            for (auto& snap : snapshots) {
                try {
                    source.UpdateLevels(static_cast<uint16_t>(snap.universe), snap.data + 1, 512);
                }
                catch (const std::exception& e) {
                    std::cerr << "DMX Warning: sACN UpdateLevels exception on universe "
                              << snap.universe << ": " << e.what() << std::endl;
                    any_failed = true;
                }
            }
            if (any_failed && can_attempt_reconnect()) {
                std::vector<int> uni_keys;
                for (auto& snap : snapshots)
                    uni_keys.push_back(snap.universe);
                std::lock_guard<std::mutex> lock(state_mutex);
                deinit_sACN();
                if (init_sACN(uni_keys))
                    std::cerr << "DMX Info: sACN reinitialized." << std::endl;
                else
                    std::cerr << "DMX Warning: sACN reconnect failed." << std::endl;
            }
            break;
        }
        case Protocol::ArtNet: {
            bool any_failed = false;
            for (auto& snap : snapshots) {
                auto it = artnet_ports.find(snap.universe);
                if (it == artnet_ports.end()) continue;
                int res = artnet_send_dmx(artnet_node_obj, it->second, 512, snap.data + 1);
                if (res < 0) any_failed = true;
            }
            if (any_failed) {
                std::cerr << "DMX Warning: libartnet failed to send DMX." << std::endl;
                if (can_attempt_reconnect()) {
                    std::vector<int> uni_keys;
                    for (auto& snap : snapshots)
                        uni_keys.push_back(snap.universe);
                    std::lock_guard<std::mutex> lock(state_mutex);
                    deinit_ArtNet();
                    if (init_ArtNet(uni_keys))
                        std::cerr << "DMX Info: ArtNet reinitialized." << std::endl;
                    else
                        std::cerr << "DMX Warning: ArtNet reconnect failed." << std::endl;
                }
            }
            break;
        }
        }
    }

    bool connected() {
        std::lock_guard<std::mutex> lock(state_mutex);
        switch (_protocol) {
        case Protocol::Serial_Raw:
        case Protocol::Serial:
            return _serial_initialized;
        case Protocol::sACN:
            return _sacn_initialized;
        case Protocol::ArtNet:
            return _artnet_initialized;
        }
        return false;
    }

    std::string port() {
        std::lock_guard<std::mutex> lock(state_mutex);
        return serial_port;
    }
    void port(const std::string& p) {
        std::lock_guard<std::mutex> lock(state_mutex);
        serial_port = p;
    }

    static std::string ports() {
        std::vector<serial::PortInfo> ports = serial::list_ports();
        std::string result;
        for (size_t i = 0; i < ports.size(); i++) {
            if (i > 0) result += ",";
            result += ports[i].port;
        }
        return result;
    }

    int universe() {
        return _active_universe.load();
    }
    bool universe(int u) {
        if (u < 1 || u > 63999) {
            std::cerr << "DMX Warning: universe() must be 1-63999, got " << u << "." << std::endl;
            return false;
        }
        // Auto-create universe data if it doesn't exist
        {
            std::lock_guard<std::mutex> flock(fade_mutex);
            std::lock_guard<std::mutex> dlock(dmx_mutex);
            if (_universes.find(u) == _universes.end())
                _universes[u]; // default construct
        }
        _active_universe.store(u);
        return true;
    }

    bool addUniverse(int uni) {
        if (uni < 1 || uni > 63999) {
            std::cerr << "DMX Warning: addUniverse() must be 1-63999, got " << uni << "." << std::endl;
            return false;
        }
        {
            std::lock_guard<std::mutex> flock(fade_mutex);
            std::lock_guard<std::mutex> dlock(dmx_mutex);
            if (_universes.count(uni)) return true; // already exists
            _universes[uni]; // create
        }
        // If sACN is already running, add the universe live
        std::lock_guard<std::mutex> lock(state_mutex);
        if (_sacn_initialized) {
            sacn::Source::UniverseSettings settings{ static_cast<uint16_t>(uni) };
            settings.priority = static_cast<uint8_t>(_sacn_priority);
            etcpal::Error err = source.AddUniverse(settings);
            if (!err.IsOk()) {
                std::cerr << "DMX Warning: sACN AddUniverse(" << uni << ") failed: " << err.ToString() << std::endl;
                return false;
            }
        }
        if (_artnet_initialized) {
            std::cerr << "DMX Warning: ArtNet requires re-initialization to add universes. Call init() again." << std::endl;
        }
        return true;
    }

    bool removeUniverse(int uni) {
        {
            std::lock_guard<std::mutex> flock(fade_mutex);
            std::lock_guard<std::mutex> dlock(dmx_mutex);
            auto it = _universes.find(uni);
            if (it == _universes.end()) return true; // doesn't exist
            if (_universes.size() <= 1) {
                std::cerr << "DMX Warning: Cannot remove the last universe." << std::endl;
                return false;
            }
            _universes.erase(it);
            // If we removed the active universe, switch to the first remaining
            if (_active_universe.load() == uni)
                _active_universe.store(_universes.begin()->first);
        }
        // If protocols are running, remove from live source
        std::lock_guard<std::mutex> lock(state_mutex);
        if (_sacn_initialized) {
            source.RemoveUniverse(static_cast<uint16_t>(uni));
        }
        if (_artnet_initialized) {
            _artnet_port_map.erase(uni);
        }
        return true;
    }

    int priority() {
        std::lock_guard<std::mutex> lock(state_mutex);
        return _sacn_priority;
    }
    bool priority(int p) {
        if (p < 0 || p > 200) {
            std::cerr << "DMX Warning: priority() must be 0-200, got " << p << "." << std::endl;
            return false;
        }
        // Get universe keys before acquiring state_mutex (lock ordering)
        std::vector<int> uni_keys;
        {
            std::lock_guard<std::mutex> lock(dmx_mutex);
            for (auto& [k, v] : _universes)
                uni_keys.push_back(k);
        }
        std::lock_guard<std::mutex> lock(state_mutex);
        _sacn_priority = p;
        // If sACN is already running, update priority on all universes
        if (_sacn_initialized) {
            for (int uni : uni_keys) {
                etcpal::Error err = source.ChangePriority(static_cast<uint16_t>(uni), static_cast<uint8_t>(p));
                if (!err.IsOk()) {
                    std::cerr << "DMX Warning: Failed to change sACN priority on universe "
                              << uni << ": " << err.ToString() << std::endl;
                    return false;
                }
            }
        }
        return true;
    }

    std::string name() {
        std::lock_guard<std::mutex> lock(state_mutex);
        return _source_name;
    }
    bool name(const std::string& n) {
        std::lock_guard<std::mutex> lock(state_mutex);
        _source_name = n;
        // If sACN is already running, update name live
        if (_sacn_initialized) {
            etcpal::Error err = source.ChangeName(n);
            if (!err.IsOk()) {
                std::cerr << "DMX Warning: Failed to change sACN source name: " << err.ToString() << std::endl;
                return false;
            }
        }
        if (_artnet_initialized) {
            std::cerr << "DMX Warning: ArtNet requires re-initialization to change the source name. Call init() again." << std::endl;
        }
        return true;
    }

    void fade(int ch, int target, int durationMs) {
        if (ch < 1 || ch > 512) {
            std::cerr << "DMX Warning: fade() channel " << ch << " out of range (1-512), ignored." << std::endl;
            return;
        }
        target = clamp_dmx(target);
        if (durationMs <= 0) {
            // Instant set
            channel(ch, target);
            return;
        }

        int uni = _active_universe.load();
        std::lock_guard<std::mutex> flock(fade_mutex);
        auto it = _universes.find(uni);
        if (it == _universes.end()) return;
        UniverseData& udata = it->second;
        FadeState& f = udata.fades[ch];
        {
            std::lock_guard<std::mutex> dlock(dmx_mutex);
            f.start_value = udata.dmx_data[ch];
        }
        f.target_value = static_cast<unsigned char>(target);
        f.start_time = std::chrono::steady_clock::now();
        f.duration = std::chrono::milliseconds(durationMs);
        f.active = true;
    }

    void start() {
        std::lock_guard<std::mutex> lock(thread_mutex);
        if (_thread_running.load()) return;
        _thread_running.store(true);
        _send_thread = std::thread(&DMX::thread_loop, this);
    }

    void stop() {
        std::lock_guard<std::mutex> lock(thread_mutex);
        if (!_thread_running.load()) return;
        _thread_running.store(false);
        if (_send_thread.joinable())
            _send_thread.join();
    }

private:
    Protocol _protocol{ Protocol::Serial };
    std::string _source_name{ "ChucK DMX" };
    int _sacn_priority{ 100 };

    // Multi-universe data: maps universe number -> per-universe DMX + fade state
    // Protected by fade_mutex (fades) and dmx_mutex (dmx_data); map structure
    // modifications require both locks (fade_mutex then dmx_mutex)
    std::map<int, UniverseData> _universes;
    std::atomic<int> _active_universe{ 1 };

    // Lock ordering: fade_mutex -> dmx_mutex -> state_mutex
    std::mutex dmx_mutex;     // protects dmx_data within _universes
    std::mutex state_mutex;   // protects protocol, rate, last_send_time, init state, source name
    std::mutex fade_mutex;    // protects fades within _universes

    double update_interval_ms{ 1000.0 / 44.0 }; // default to 44 Hz
    std::chrono::steady_clock::time_point last_send_time = std::chrono::steady_clock::now();

    // Initialization tracking
    bool _serial_initialized{ false };
    bool _sacn_initialized{ false };
    bool _artnet_initialized{ false };

    // Reconnect backoff tracking
    std::chrono::steady_clock::time_point last_reconnect_attempt{};

    // Serial
    serial::Serial serial_obj;
    std::string serial_port;
    std::mutex serial_mutex; // protects serial_obj

    // sACN
    sacn::Source source;

    // ArtNet
    artnet_node artnet_node_obj = nullptr;
    std::map<int, int> _artnet_port_map; // universe -> ArtNet port index

    // Background thread
    std::mutex thread_mutex;
    std::thread _send_thread;
    std::atomic<bool> _thread_running{ false };

    void thread_loop() {
        while (_thread_running.load()) {
            update_fades();
            send();

            double interval;
            {
                std::lock_guard<std::mutex> lock(state_mutex);
                interval = update_interval_ms;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(interval)));
        }
    }

    void update_fades() {
        auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> flock(fade_mutex);
        std::lock_guard<std::mutex> dlock(dmx_mutex);

        for (auto& [uni, udata] : _universes) {
            for (int ch = 1; ch <= 512; ch++) {
                FadeState& f = udata.fades[ch];
                if (!f.active) continue;

                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - f.start_time);
                if (elapsed >= f.duration) {
                    udata.dmx_data[ch] = f.target_value;
                    f.active = false;
                } else {
                    double t = static_cast<double>(elapsed.count()) / static_cast<double>(f.duration.count());
                    udata.dmx_data[ch] = static_cast<unsigned char>(
                        f.start_value + t * (static_cast<int>(f.target_value) - static_cast<int>(f.start_value)));
                }
            }
        }
    }

    void openPort() {
        if (serial_obj.isOpen())
            serial_obj.close();

        serial_obj.setPort(serial_port);
        serial_obj.setBaudrate(250000);
        serial_obj.setBytesize(serial::eightbits);
        serial_obj.setParity(serial::parity_none);
        serial_obj.setStopbits(serial::stopbits_two);
        serial_obj.setFlowcontrol(serial::flowcontrol_none);
        serial::Timeout timeout = serial::Timeout::simpleTimeout(1000);
        serial_obj.setTimeout(timeout);
        serial_obj.open();
    }

    void closePort() {
        try {
            if (serial_obj.isOpen())
                serial_obj.close();
        }
        catch (...) {}
    }

    bool init_Serial() {
        if (serial_port.empty()) {
            std::cerr << "DMX Error: Serial port name not set. Call port() before init()." << std::endl;
            return false;
        }
        try {
            std::lock_guard<std::mutex> lock(serial_mutex);
            openPort();
            _serial_initialized = true;
            return true;
        }
        catch (const std::exception& e) {
            std::cerr << "DMX Error: Failed to open serial port: " << e.what() << std::endl;
            return false;
        }
    }

    void deinit_Serial() {
        if (!_serial_initialized) return;
        std::lock_guard<std::mutex> lock(serial_mutex);
        closePort();
        _serial_initialized = false;
    }

    bool init_sACN(const std::vector<int>& uni_keys) {
        try {
            sacn_global_init();

            etcpal::Uuid cid = etcpal::Uuid::OsPreferred();
            if (cid.IsNull()) {
                std::cerr << "DMX Error: Failed to generate UUID for sACN Source CID." << std::endl;
                sacn_global_deinit();
                return false;
            }

            sacn::Source::Settings settings{ cid, _source_name };
            etcpal::Error error = source.Startup(settings);
            if (!error.IsOk()) {
                std::cerr << "DMX Error: sACN Startup failed: " << error.ToString() << std::endl;
                sacn_global_deinit();
                return false;
            }

            for (int uni : uni_keys) {
                sacn::Source::UniverseSettings universe_settings{ static_cast<uint16_t>(uni) };
                universe_settings.priority = static_cast<uint8_t>(_sacn_priority);
                error = source.AddUniverse(universe_settings);
                if (!error.IsOk()) {
                    std::cerr << "DMX Error: sACN AddUniverse(" << uni << ") failed: " << error.ToString() << std::endl;
                    source.Shutdown();
                    sacn_global_deinit();
                    return false;
                }
            }

            _sacn_initialized = true;
            return true;
        }
        catch (const std::exception& e) {
            std::cerr << "DMX Error: sACN init exception: " << e.what() << std::endl;
            sacn_global_deinit();
            return false;
        }
    }

    void deinit_sACN() {
        if (!_sacn_initialized) return;
        source.Shutdown();
        sacn_global_deinit();
        _sacn_initialized = false;
    }

    bool init_ArtNet(const std::vector<int>& uni_keys) {
        if (uni_keys.empty()) {
            std::cerr << "DMX Error: No universes configured for ArtNet." << std::endl;
            return false;
        }
        if (uni_keys.size() > 4) {
            std::cerr << "DMX Warning: ArtNet supports at most 4 ports per node. "
                      << "Only the first 4 universes will be used." << std::endl;
        }

        artnet_node_obj = artnet_new(nullptr, 0);
        if (!artnet_node_obj) {
            std::cerr << "DMX Error: Failed to create libartnet node." << std::endl;
            return false;
        }

        artnet_set_short_name(artnet_node_obj, _source_name.c_str());
        artnet_set_long_name(artnet_node_obj, _source_name.c_str());
        artnet_set_node_type(artnet_node_obj, ARTNET_NODE);

        // Use subnet from first universe; all ports share the same subnet
        uint8_t subnet = (uni_keys[0] >> 4) & 0x0F;
        artnet_set_subnet_addr(artnet_node_obj, subnet);

        _artnet_port_map.clear();
        int port_idx = 0;
        for (int uni : uni_keys) {
            if (port_idx >= 4) break;

            uint8_t uni_subnet = (uni >> 4) & 0x0F;
            if (uni_subnet != subnet) {
                std::cerr << "DMX Warning: ArtNet universe " << uni
                          << " is in a different subnet, skipped. "
                          << "All universes must share the same ArtNet subnet." << std::endl;
                continue;
            }

            uint8_t uni_addr = uni & 0x0F;
            artnet_set_port_type(artnet_node_obj, port_idx, ARTNET_ENABLE_INPUT, ARTNET_PORT_DMX);
            artnet_set_port_addr(artnet_node_obj, port_idx, ARTNET_INPUT_PORT, uni_addr);
            _artnet_port_map[uni] = port_idx;
            port_idx++;
        }

        if (artnet_start(artnet_node_obj) < 0) {
            artnet_destroy(artnet_node_obj);
            artnet_node_obj = nullptr;
            _artnet_port_map.clear();
            std::cerr << "DMX Error: Failed to start libartnet node." << std::endl;
            return false;
        }

        _artnet_initialized = true;
        return true;
    }

    void deinit_ArtNet() {
        if (!_artnet_initialized) return;
        if (artnet_node_obj) {
            artnet_destroy(artnet_node_obj);
            artnet_node_obj = nullptr;
        }
        _artnet_port_map.clear();
        _artnet_initialized = false;
    }

    // Deinit all protocols (called under state_mutex)
    void deinit_all_unlocked() {
        deinit_Serial();
        deinit_sACN();
        deinit_ArtNet();
    }

    // Deinit only the currently-active protocol
    void deinit_current() {
        deinit_Serial();
        deinit_sACN();
        deinit_ArtNet();
    }

    bool can_attempt_reconnect() {
        auto now = std::chrono::steady_clock::now();
        if (now - last_reconnect_attempt < std::chrono::milliseconds(RECONNECT_COOLDOWN_MS))
            return false;
        last_reconnect_attempt = now;
        return true;
    }

    void send_Serial(const unsigned char* snapshot, Protocol proto)
    {
        std::lock_guard<std::mutex> lock(serial_mutex);

        if (!serial_obj.isOpen()) {
            if (!can_attempt_reconnect()) return;
            try {
                openPort();
            }
            catch (const std::exception& e) {
                std::cerr << "DMX Warning: Failed to open serial port: " << e.what() << std::endl;
                return;
            }
        }

        try {
            if (proto == Protocol::Serial_Raw) {
                // Break condition for "raw" FTDI/RS485 interfaces (OpenDMX style)
                serial_obj.setBreak(true);
                dmx_usleep(120);      // 88+ us break low
                serial_obj.setBreak(false);
                dmx_usleep(12);       // 8+ us Mark After Break high
                serial_obj.write(snapshot, 513); // 1 start + 512 DMX channels
                return;
            }

            if (proto == Protocol::Serial) {
                // Buffered interfaces (e.g., Enttec DMX USB Pro, DMXking, DSD Tech)
                unsigned char buf[5 + DMX_PAYLOAD_LEN];
                buf[0] = ENTTEC_START_MSG;
                buf[1] = ENTTEC_SEND_DMX;
                buf[2] = DMX_PAYLOAD_LEN & 0xFF;          // Data length LSB
                buf[3] = (DMX_PAYLOAD_LEN >> 8) & 0xFF;   // Data length MSB
                memcpy(&buf[4], snapshot, DMX_PAYLOAD_LEN);
                buf[4 + DMX_PAYLOAD_LEN] = ENTTEC_END_MSG;
                serial_obj.write(buf, sizeof(buf));
                return;
            }
        }
        catch (const std::exception& e) {
            std::cerr << "DMX Error: Serial write error: " << e.what() << std::endl;
            try { if (serial_obj.isOpen()) serial_obj.close(); }
            catch (...) {}
        }
    }
};

// ChucK interface implementations

CK_DLL_CTOR(dmx_ctor) {
    OBJ_MEMBER_INT(SELF, dmx_data_offset) = 0;
    DMX* dmx_obj = new DMX();
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
    t_CKINT mode_int = GET_NEXT_INT(ARGS);
    if (!dmx_obj) { RETURN->v_int = mode_int; return; }

    if (mode_int >= 0 && mode_int <= 3) {
        dmx_obj->protocol(static_cast<DMX::Protocol>(mode_int));
    } else {
        std::cerr << "DMX Error: Invalid protocol " << mode_int
                  << ", valid values: 0=Serial_Raw, 1=Serial, 2=sACN, 3=ArtNet" << std::endl;
    }
    RETURN->v_int = mode_int;
}

CK_DLL_MFUN(dmx_get_channel) {
    DMX* dmx_obj = (DMX*)OBJ_MEMBER_INT(SELF, dmx_data_offset);
    t_CKINT ch = GET_NEXT_INT(ARGS);
    if (!dmx_obj) { RETURN->v_int = 0; return; }
    RETURN->v_int = dmx_obj->get_channel(static_cast<int>(ch));
}
CK_DLL_MFUN(dmx_channel) {
    DMX* dmx_obj = (DMX*)OBJ_MEMBER_INT(SELF, dmx_data_offset);
    if (!dmx_obj) return;

    t_CKINT ch = GET_NEXT_INT(ARGS);
    t_CKINT value = GET_NEXT_INT(ARGS);
    dmx_obj->channel(static_cast<int>(ch), static_cast<int>(value));
}
CK_DLL_MFUN(dmx_channels) {
    DMX* dmx_obj = (DMX*)OBJ_MEMBER_INT(SELF, dmx_data_offset);
    if (!dmx_obj) return;

    t_CKINT startCh = GET_NEXT_INT(ARGS);
    Chuck_ArrayInt* arr = (Chuck_ArrayInt*)GET_NEXT_OBJECT(ARGS);
    if (!arr) return;

    t_CKINT count = API->object->array_int_size(arr);
    if (count <= 0) return;
    if (count > 512) count = 512;

    unsigned char values[512];
    for (t_CKINT i = 0; i < count; i++) {
        values[i] = static_cast<unsigned char>(clamp_dmx(static_cast<int>(API->object->array_int_get_idx(arr, i))));
    }

    dmx_obj->channels(static_cast<int>(startCh), values, static_cast<int>(count));
}

CK_DLL_MFUN(dmx_get_rate) {
    DMX* dmx_obj = (DMX*)OBJ_MEMBER_INT(SELF, dmx_data_offset);
    if (!dmx_obj) { RETURN->v_int = -1; return; }
    RETURN->v_int = dmx_obj->rate();
}
CK_DLL_MFUN(dmx_rate) {
    DMX* dmx_obj = (DMX*)OBJ_MEMBER_INT(SELF, dmx_data_offset);
    t_CKINT hz = GET_NEXT_INT(ARGS);
    if (!dmx_obj) { RETURN->v_int = hz; return; }

    dmx_obj->rate(static_cast<int>(hz));
    RETURN->v_int = hz;
}

CK_DLL_MFUN(dmx_init) {
    DMX* dmx_obj = (DMX*)OBJ_MEMBER_INT(SELF, dmx_data_offset);
    if (!dmx_obj) { RETURN->v_int = 0; return; }

    RETURN->v_int = dmx_obj->init() ? 1 : 0;
}

CK_DLL_MFUN(dmx_send) {
    DMX* dmx_obj = (DMX*)OBJ_MEMBER_INT(SELF, dmx_data_offset);
    if (!dmx_obj) return;

    dmx_obj->send();
}

CK_DLL_MFUN(dmx_blackout) {
    DMX* dmx_obj = (DMX*)OBJ_MEMBER_INT(SELF, dmx_data_offset);
    if (!dmx_obj) return;

    dmx_obj->blackout();
}

CK_DLL_MFUN(dmx_connected) {
    DMX* dmx_obj = (DMX*)OBJ_MEMBER_INT(SELF, dmx_data_offset);
    if (!dmx_obj) { RETURN->v_int = 0; return; }
    RETURN->v_int = dmx_obj->connected() ? 1 : 0;
}

// Serial

CK_DLL_MFUN(dmx_get_port) {
    DMX* dmx_obj = (DMX*)OBJ_MEMBER_INT(SELF, dmx_data_offset);
    if (!dmx_obj) { RETURN->v_string = API->object->create_string(VM, "", 0); return; }
    const std::string& p = dmx_obj->port();
    RETURN->v_string = API->object->create_string(VM, p.c_str(), (t_CKUINT)p.length());
}
CK_DLL_MFUN(dmx_port) {
    DMX* dmx_obj = (DMX*)OBJ_MEMBER_INT(SELF, dmx_data_offset);
    std::string n = GET_NEXT_STRING_SAFE(ARGS);
    if (!dmx_obj) { RETURN->v_string = API->object->create_string(VM, n.c_str(), (t_CKUINT)n.length()); return; }

    dmx_obj->port(n);
    RETURN->v_string = API->object->create_string(VM, n.c_str(), (t_CKUINT)n.length());
}

CK_DLL_SFUN(dmx_list_ports) {
    std::string p = DMX::ports();
    RETURN->v_string = API->object->create_string(VM, p.c_str(), (t_CKUINT)p.length());
}

// sACN and ArtNet

CK_DLL_MFUN(dmx_get_universe) {
    DMX* dmx_obj = (DMX*)OBJ_MEMBER_INT(SELF, dmx_data_offset);
    if (!dmx_obj) { RETURN->v_int = -1; return; }
    RETURN->v_int = dmx_obj->universe();
}
CK_DLL_MFUN(dmx_universe) {
    DMX* dmx_obj = (DMX*)OBJ_MEMBER_INT(SELF, dmx_data_offset);
    t_CKINT u = GET_NEXT_INT(ARGS);
    if (!dmx_obj) { RETURN->v_int = u; return; }

    dmx_obj->universe(static_cast<int>(u));
    RETURN->v_int = u;
}

CK_DLL_MFUN(dmx_add_universe) {
    DMX* dmx_obj = (DMX*)OBJ_MEMBER_INT(SELF, dmx_data_offset);
    t_CKINT uni = GET_NEXT_INT(ARGS);
    if (!dmx_obj) { RETURN->v_int = 0; return; }
    RETURN->v_int = dmx_obj->addUniverse(static_cast<int>(uni)) ? 1 : 0;
}

CK_DLL_MFUN(dmx_remove_universe) {
    DMX* dmx_obj = (DMX*)OBJ_MEMBER_INT(SELF, dmx_data_offset);
    t_CKINT uni = GET_NEXT_INT(ARGS);
    if (!dmx_obj) { RETURN->v_int = 0; return; }
    RETURN->v_int = dmx_obj->removeUniverse(static_cast<int>(uni)) ? 1 : 0;
}

// sACN priority

CK_DLL_MFUN(dmx_get_priority) {
    DMX* dmx_obj = (DMX*)OBJ_MEMBER_INT(SELF, dmx_data_offset);
    if (!dmx_obj) { RETURN->v_int = -1; return; }
    RETURN->v_int = dmx_obj->priority();
}
CK_DLL_MFUN(dmx_priority) {
    DMX* dmx_obj = (DMX*)OBJ_MEMBER_INT(SELF, dmx_data_offset);
    t_CKINT p = GET_NEXT_INT(ARGS);
    if (!dmx_obj) { RETURN->v_int = p; return; }

    dmx_obj->priority(static_cast<int>(p));
    RETURN->v_int = p;
}

// Source name

CK_DLL_MFUN(dmx_get_name) {
    DMX* dmx_obj = (DMX*)OBJ_MEMBER_INT(SELF, dmx_data_offset);
    if (!dmx_obj) { RETURN->v_string = API->object->create_string(VM, "", 0); return; }
    const std::string& n = dmx_obj->name();
    RETURN->v_string = API->object->create_string(VM, n.c_str(), (t_CKUINT)n.length());
}
CK_DLL_MFUN(dmx_name) {
    DMX* dmx_obj = (DMX*)OBJ_MEMBER_INT(SELF, dmx_data_offset);
    std::string n = GET_NEXT_STRING_SAFE(ARGS);
    if (!dmx_obj) { RETURN->v_string = API->object->create_string(VM, n.c_str(), (t_CKUINT)n.length()); return; }

    dmx_obj->name(n);
    RETURN->v_string = API->object->create_string(VM, n.c_str(), (t_CKUINT)n.length());
}

// Background thread

CK_DLL_MFUN(dmx_start) {
    DMX* dmx_obj = (DMX*)OBJ_MEMBER_INT(SELF, dmx_data_offset);
    if (!dmx_obj) return;
    dmx_obj->start();
}

CK_DLL_MFUN(dmx_stop) {
    DMX* dmx_obj = (DMX*)OBJ_MEMBER_INT(SELF, dmx_data_offset);
    if (!dmx_obj) return;
    dmx_obj->stop();
}

// Fade

CK_DLL_MFUN(dmx_fade) {
    DMX* dmx_obj = (DMX*)OBJ_MEMBER_INT(SELF, dmx_data_offset);
    if (!dmx_obj) return;

    t_CKINT ch = GET_NEXT_INT(ARGS);
    t_CKINT target = GET_NEXT_INT(ARGS);
    t_CKINT durationMs = GET_NEXT_INT(ARGS);
    dmx_obj->fade(static_cast<int>(ch), static_cast<int>(target), static_cast<int>(durationMs));
}

CK_DLL_INFO(DMX)
{
    QUERY->setinfo(QUERY, CHUGIN_INFO_CHUGIN_VERSION, "v0.1.1");
    QUERY->setinfo(QUERY, CHUGIN_INFO_AUTHORS, "Ben Hoang");
    QUERY->setinfo(QUERY, CHUGIN_INFO_DESCRIPTION,
        "ChucK-DMX: A plugin for ChucK that enables the sending of DMX "
        "over serial or over ethernet via the ArtNet and sACN network protocols.");
    QUERY->setinfo(QUERY, CHUGIN_INFO_URL, "https://ccrma.stanford.edu/~hoangben/ChucK-DMX/");
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
        "Configure protocol, port/universe, and rate, then call init() to instantiate the connection. "
        "Use start() to enable a background send thread with automatic fade interpolation. "
        "Multiple universes are supported: use addUniverse() to add universes, and universe() to "
        "select the active universe for channel and fade operations."
    );

    QUERY->add_ctor(QUERY, dmx_ctor);
    QUERY->add_dtor(QUERY, dmx_dtor);

    // --- Static protocol constants ---

    QUERY->add_svar(QUERY, "int", "SERIAL_RAW", TRUE, &dmx_SERIAL_RAW);
    QUERY->doc_var(QUERY, "Protocol constant for raw serial (OpenDMX-style break timing).");

    QUERY->add_svar(QUERY, "int", "SERIAL", TRUE, &dmx_SERIAL);
    QUERY->doc_var(QUERY, "Protocol constant for buffered serial (Enttec DMX USB Pro, DMXking, DSD Tech).");

    QUERY->add_svar(QUERY, "int", "SACN", TRUE, &dmx_SACN);
    QUERY->doc_var(QUERY, "Protocol constant for sACN (E1.31 streaming ACN over Ethernet).");

    QUERY->add_svar(QUERY, "int", "ARTNET", TRUE, &dmx_ARTNET);
    QUERY->doc_var(QUERY, "Protocol constant for Art-Net (DMX over Ethernet).");

    // --- Protocol ---

    QUERY->add_mfun(QUERY, dmx_get_protocol, "int", "protocol");
    QUERY->doc_func(QUERY,
        "Get the current DMX protocol as an integer: 0=SERIAL_RAW, 1=SERIAL, 2=SACN, 3=ARTNET."
    );

    QUERY->add_mfun(QUERY, dmx_protocol, "int", "protocol");
    QUERY->add_arg(QUERY, "int", "protocol");
    QUERY->doc_func(QUERY,
        "Set the DMX protocol. Use DMX.SERIAL_RAW, DMX.SERIAL, DMX.SACN, or DMX.ARTNET. "
        "Configure this before init()."
    );

    // --- Channel ---

    QUERY->add_mfun(QUERY, dmx_get_channel, "int", "channel");
    QUERY->add_arg(QUERY, "int", "channel");
    QUERY->doc_func(QUERY,
        "Get the current value (0-255) of a DMX channel (1-512) on the active universe. "
        "Returns 0 if out of range."
    );

    QUERY->add_mfun(QUERY, dmx_channel, "void", "channel");
    QUERY->add_arg(QUERY, "int", "channel");
    QUERY->add_arg(QUERY, "int", "value");
    QUERY->doc_func(QUERY,
        "Set a DMX channel (1-512) to a value (0-255) on the active universe. "
        "Values outside 0-255 are clamped. "
        "Cancels any active fade on this channel. "
        "Changes are staged in the buffer and take effect upon next send()."
    );

    QUERY->add_mfun(QUERY, dmx_channels, "void", "channels");
    QUERY->add_arg(QUERY, "int", "startChannel");
    QUERY->add_arg(QUERY, "int[]", "values");
    QUERY->doc_func(QUERY,
        "Set multiple consecutive DMX channels starting at startChannel on the active universe. "
        "Values are clamped to 0-255. Cancels any active fades on affected channels. "
        "All channels are updated atomically under a single lock."
    );

    // --- Rate ---

    QUERY->add_mfun(QUERY, dmx_get_rate, "int", "rate");
    QUERY->doc_func(QUERY,
        "Get the current DMX update rate in Hz (frames per second)."
    );

    QUERY->add_mfun(QUERY, dmx_rate, "int", "rate");
    QUERY->add_arg(QUERY, "int", "rate");
    QUERY->doc_func(QUERY,
        "Set the DMX update rate (Hz). Acceptable range is 1-44 Hz. "
        "This controls the minimum interval between consecutive sends."
    );

    // --- Init / Send / Lifecycle ---

    QUERY->add_mfun(QUERY, dmx_init, "int", "init");
    QUERY->doc_func(QUERY,
        "Initialize the DMX connection using the configured protocol, port, universe(s), "
        "priority, and source name. All configured universes are initialized. "
        "Returns 1 on success, 0 on failure. "
        "Call this after adjusting configuration parameters."
    );

    QUERY->add_mfun(QUERY, dmx_send, "void", "send");
    QUERY->doc_func(QUERY,
        "Transmit the current DMX buffer for all configured universes over the active protocol. "
        "For Serial, only the active universe is sent. "
        "Enforces the minimum interval set by rate(). "
        "Not needed if start() is used for background sending."
    );

    QUERY->add_mfun(QUERY, dmx_blackout, "void", "blackout");
    QUERY->doc_func(QUERY,
        "Set all 512 DMX channels to 0 on all universes and cancel all active fades. "
        "Does not automatically send; call send() afterwards to transmit."
    );

    QUERY->add_mfun(QUERY, dmx_connected, "int", "connected");
    QUERY->doc_func(QUERY,
        "Returns 1 if the current protocol is initialized and ready to send, 0 otherwise."
    );

    // --- Background Thread ---

    QUERY->add_mfun(QUERY, dmx_start, "void", "start");
    QUERY->doc_func(QUERY,
        "Start a background thread that automatically sends DMX at the configured rate. "
        "Also processes fade() transitions. Call after init()."
    );

    QUERY->add_mfun(QUERY, dmx_stop, "void", "stop");
    QUERY->doc_func(QUERY,
        "Stop the background send thread. The thread is also stopped automatically on destruction."
    );

    // --- Fade ---

    QUERY->add_mfun(QUERY, dmx_fade, "void", "fade");
    QUERY->add_arg(QUERY, "int", "channel");
    QUERY->add_arg(QUERY, "int", "target");
    QUERY->add_arg(QUERY, "int", "durationMs");
    QUERY->doc_func(QUERY,
        "Fade a DMX channel (1-512) to a target value (0-255) over durationMs milliseconds "
        "on the active universe. "
        "Requires start() for the background thread to process fades. "
        "If durationMs <= 0, the value is set immediately. "
        "Setting a channel directly via channel() cancels any active fade on that channel."
    );

    // --- Serial ---

    QUERY->add_mfun(QUERY, dmx_get_port, "string", "port");
    QUERY->doc_func(QUERY,
        "Get the currently configured serial port string (e.g., '/dev/ttyUSB0' or 'COM3')."
    );

    QUERY->add_mfun(QUERY, dmx_port, "string", "port");
    QUERY->add_arg(QUERY, "string", "port");
    QUERY->doc_func(QUERY,
        "Set the serial port name used when protocol is SERIAL or SERIAL_RAW. "
        "Configure this before init()."
    );

    QUERY->add_sfun(QUERY, dmx_list_ports, "string", "ports");
    QUERY->doc_func(QUERY,
        "Returns a comma-separated string of available serial port names (e.g., 'COM3,COM5'). "
        "Use this to discover connected DMX interfaces."
    );

    // --- sACN and ArtNet ---

    QUERY->add_mfun(QUERY, dmx_get_universe, "int", "universe");
    QUERY->doc_func(QUERY,
        "Get the active DMX universe number. Channel and fade operations target this universe."
    );

    QUERY->add_mfun(QUERY, dmx_universe, "int", "universe");
    QUERY->add_arg(QUERY, "int", "universe");
    QUERY->doc_func(QUERY,
        "Set the active DMX universe (1-63999). Channel and fade operations target this universe. "
        "If the universe does not exist yet, it is automatically created. "
        "Use addUniverse() to add universes without switching."
    );

    QUERY->add_mfun(QUERY, dmx_add_universe, "int", "addUniverse");
    QUERY->add_arg(QUERY, "int", "universe");
    QUERY->doc_func(QUERY,
        "Add a universe (1-63999) to this DMX instance without switching the active universe. "
        "Returns 1 on success, 0 on failure. "
        "If sACN is already initialized, the universe is added live. "
        "ArtNet requires re-initialization via init() to add universes."
    );

    QUERY->add_mfun(QUERY, dmx_remove_universe, "int", "removeUniverse");
    QUERY->add_arg(QUERY, "int", "universe");
    QUERY->doc_func(QUERY,
        "Remove a universe from this DMX instance. Returns 1 on success, 0 on failure. "
        "Cannot remove the last remaining universe. "
        "If the removed universe was the active one, the active universe switches to "
        "the lowest remaining universe."
    );

    QUERY->add_mfun(QUERY, dmx_get_priority, "int", "priority");
    QUERY->doc_func(QUERY,
        "Get the current sACN priority (0-200, default 100)."
    );

    QUERY->add_mfun(QUERY, dmx_priority, "int", "priority");
    QUERY->add_arg(QUERY, "int", "priority");
    QUERY->doc_func(QUERY,
        "Set the sACN priority (0-200, default 100). Higher priority sources take precedence "
        "when multiple sources send to the same universe. "
        "Can be changed before or after init(); if sACN is already running, the priority "
        "updates live on all configured universes."
    );

    // --- Source Name ---

    QUERY->add_mfun(QUERY, dmx_get_name, "string", "name");
    QUERY->doc_func(QUERY,
        "Get the source name used in sACN and ArtNet (default: 'ChucK DMX')."
    );

    QUERY->add_mfun(QUERY, dmx_name, "string", "name");
    QUERY->add_arg(QUERY, "string", "name");
    QUERY->doc_func(QUERY,
        "Set the source name used in sACN and ArtNet (default: 'ChucK DMX'). "
        "If sACN is already running, the name updates live. "
        "ArtNet requires re-initialization via init() to change the name."
    );

    dmx_data_offset = QUERY->add_mvar(QUERY, "int", "@dmx_data", false);

    QUERY->end_class(QUERY);

    return TRUE;
}
