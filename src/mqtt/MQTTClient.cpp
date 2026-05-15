#include "MQTTClient.hpp"
#include "constants.hpp"

#include <MQTTClient.h>   // Paho C library
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <thread>

namespace orbis {

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

MQTTClientWrapper::MQTTClientWrapper(const std::string& broker_uri,
                                     const std::string& client_id,
                                     const std::string& ca_cert_path,
                                     int reconnect_min_sec,
                                     int reconnect_max_sec)
    : broker_uri_(broker_uri)
    , client_id_(client_id)
    , ca_cert_path_(ca_cert_path)
    , reconnect_min_sec_(reconnect_min_sec)
    , reconnect_max_sec_(reconnect_max_sec)
{
    int rc = MQTTClient_create(&client_,
                               broker_uri_.c_str(),
                               client_id_.c_str(),
                               MQTTCLIENT_PERSISTENCE_NONE,
                               nullptr);
    if (rc != MQTTCLIENT_SUCCESS) {
        throw std::runtime_error("MQTTClient_create failed rc=" + std::to_string(rc));
    }

    MQTTClient_setCallbacks(client_, this,
                            &MQTTClientWrapper::onConnectionLost,
                            &MQTTClientWrapper::onMessageArrived,
                            &MQTTClientWrapper::onDeliveryComplete);
}

MQTTClientWrapper::~MQTTClientWrapper() {
    shutdown_ = true;
    if (connected_) {
        try { disconnect(); } catch (...) {}
    }
    MQTTClient_destroy(&client_);
}

// ---------------------------------------------------------------------------
// Connect
// ---------------------------------------------------------------------------

bool MQTTClientWrapper::connect(const std::string& username,
                                const std::string& password,
                                const std::string& will_topic,
                                const std::string& will_payload,
                                int keepalive_sec)
{
    // Store credentials for automatic reconnection
    last_username_      = username;
    last_password_      = password;
    last_will_topic_    = will_topic;
    last_will_payload_  = will_payload;
    last_keepalive_sec_ = keepalive_sec;

    MQTTClient_connectOptions conn_opts = MQTTClient_connectOptions_initializer;
    conn_opts.keepAliveInterval = keepalive_sec;
    conn_opts.cleansession      = 1;
    conn_opts.username          = username.empty() ? nullptr : username.c_str();
    conn_opts.password          = password.empty() ? nullptr : password.c_str();

    // Last Will
    MQTTClient_willOptions will_opts = MQTTClient_willOptions_initializer;
    if (!will_topic.empty()) {
        will_opts.topicName = will_topic.c_str();
        will_opts.message   = will_payload.c_str();
        will_opts.qos       = constants::MQTT_DEFAULT_QOS;
        will_opts.retained  = 1;  // Section 3: status retained=true
        conn_opts.will      = &will_opts;
    }

    // TLS
    MQTTClient_SSLOptions ssl_opts = MQTTClient_SSLOptions_initializer;
    if (broker_uri_.find("ssl://") == 0 || broker_uri_.find("tls://") == 0) {
        ssl_opts.enableServerCertAuth = ca_cert_path_.empty() ? 0 : 1;
        if (!ca_cert_path_.empty()) {
            ssl_opts.trustStore = ca_cert_path_.c_str();
        }
        conn_opts.ssl = &ssl_opts;
    }

    spdlog::info("MQTT connecting to {} as '{}'", broker_uri_, username);

    int rc = MQTTClient_connect(client_, &conn_opts);
    if (rc != MQTTCLIENT_SUCCESS) {
        spdlog::error("MQTT connect failed rc={}", rc);
        return false;
    }

    connected_ = true;
    spdlog::info("MQTT connected");

    // Re-subscribe to all registered topics
    {
        std::lock_guard<std::mutex> lock(sub_mutex_);
        for (auto& [topic, qos] : subscriptions_) {
            MQTTClient_subscribe(client_, topic.c_str(), qos);
        }
    }

    if (connected_callback_) connected_callback_();
    return true;
}

void MQTTClientWrapper::disconnect() {
    if (!connected_) return;
    MQTTClient_disconnect(client_, 5000);
    connected_ = false;
    spdlog::info("MQTT disconnected");
}

bool MQTTClientWrapper::isConnected() const {
    return connected_ && MQTTClient_isConnected(client_);
}

// ---------------------------------------------------------------------------
// Publish (thread-safe)
// ---------------------------------------------------------------------------

bool MQTTClientWrapper::publish(const std::string& topic,
                                const std::string& payload,
                                int qos,
                                bool retained)
{
    std::lock_guard<std::mutex> lock(pub_mutex_);
    if (!isConnected()) {
        spdlog::warn("Publish skipped — not connected. topic={}", topic);
        return false;
    }

    MQTTClient_message msg = MQTTClient_message_initializer;
    msg.payload    = const_cast<char*>(payload.c_str());
    msg.payloadlen = static_cast<int>(payload.size());
    msg.qos        = qos;
    msg.retained   = retained ? 1 : 0;

    MQTTClient_deliveryToken token;
    int rc = MQTTClient_publishMessage(client_, topic.c_str(), &msg, &token);
    if (rc != MQTTCLIENT_SUCCESS) {
        spdlog::error("Publish failed rc={} topic={}", rc, topic);
        return false;
    }

    // NOTE: We intentionally do NOT call MQTTClient_waitForCompletion here.
    // Paho synchronous client uses a background receive thread for PUBACKs.
    // Calling waitForCompletion from any application thread creates a potential
    // deadlock: the app thread holds Paho's internal lock waiting for the signal,
    // while the receive thread needs that same lock to deliver the PUBACK signal.
    // Delivery confirmation is handled via the onDeliveryComplete callback.
    return true;
}

// ---------------------------------------------------------------------------
// Subscribe
// ---------------------------------------------------------------------------

bool MQTTClientWrapper::subscribe(const std::string& topic, int qos) {
    {
        std::lock_guard<std::mutex> lock(sub_mutex_);
        // Avoid duplicate registrations
        auto it = std::find_if(subscriptions_.begin(), subscriptions_.end(),
                               [&](auto& p){ return p.first == topic; });
        if (it == subscriptions_.end()) {
            subscriptions_.emplace_back(topic, qos);
        }
    }

    if (!isConnected()) {
        spdlog::warn("Subscribe deferred (not connected): {}", topic);
        return false;
    }

    int rc = MQTTClient_subscribe(client_, topic.c_str(), qos);
    if (rc != MQTTCLIENT_SUCCESS) {
        spdlog::error("Subscribe failed rc={} topic={}", rc, topic);
        return false;
    }
    spdlog::info("Subscribed to {}", topic);
    return true;
}

// ---------------------------------------------------------------------------
// Callback setters
// ---------------------------------------------------------------------------

void MQTTClientWrapper::setMessageCallback(MessageCallback cb) {
    msg_callback_ = std::move(cb);
}

void MQTTClientWrapper::setConnectedCallback(std::function<void()> cb) {
    connected_callback_ = std::move(cb);
}

void MQTTClientWrapper::setConnectionLostCallback(std::function<void(std::string)> cb) {
    conn_lost_callback_ = std::move(cb);
}

// ---------------------------------------------------------------------------
// Static Paho callbacks
// ---------------------------------------------------------------------------

void MQTTClientWrapper::onConnectionLost(void* context, char* cause) {
    auto* self = static_cast<MQTTClientWrapper*>(context);
    std::string reason = cause ? cause : "unknown";
    spdlog::warn("MQTT connection lost: {}", reason);
    self->connected_ = false;
    if (self->conn_lost_callback_) self->conn_lost_callback_(reason);

    // Reconnect loop in a detached thread
    if (!self->shutdown_) {
        std::thread([self]() {
            self->reconnectLoop();
        }).detach();
    }
}

int MQTTClientWrapper::onMessageArrived(void* context, char* topicName, int /*topicLen*/,
                                         MQTTClient_message* message)
{
    auto* self = static_cast<MQTTClientWrapper*>(context);
    std::string topic(topicName);
    std::string payload(static_cast<char*>(message->payload),
                        static_cast<size_t>(message->payloadlen));
    MQTTClient_freeMessage(&message);
    MQTTClient_free(topicName);

    if (self->msg_callback_) {
        try {
            self->msg_callback_(topic, payload);
        } catch (const std::exception& e) {
            spdlog::error("Message callback exception: {}", e.what());
        }
    }
    return 1; // 1 = message handled, Paho will free it
}

void MQTTClientWrapper::onDeliveryComplete(void* /*context*/, int /*token*/) {
    // No-op: fire-and-forget delivery. PUBACK receipt is confirmed here by Paho's
    // background receive thread without any application-side blocking.
}

// ---------------------------------------------------------------------------
// Exponential-backoff reconnection
// ---------------------------------------------------------------------------

void MQTTClientWrapper::reconnectLoop() {
#if defined(PLATFORM_LINUX)
    pthread_setname_np(pthread_self(), "mqtt-reconnect");
#endif

    int attempt = 0;
    while (!shutdown_ && !connected_) {
        int delay = std::min(
            reconnect_max_sec_,
            reconnect_min_sec_ * (1 << std::min(attempt, 10)));

        spdlog::info("MQTT reconnect attempt {} in {}s", attempt + 1, delay);
        std::this_thread::sleep_for(std::chrono::seconds(delay));

        if (shutdown_) break;

        // Reconnect with the same credentials used in the last connect() call
        MQTTClient_connectOptions conn_opts = MQTTClient_connectOptions_initializer;
        conn_opts.keepAliveInterval = last_keepalive_sec_;
        conn_opts.cleansession      = 1;
        conn_opts.username = last_username_.empty() ? nullptr : last_username_.c_str();
        conn_opts.password = last_password_.empty() ? nullptr : last_password_.c_str();

        // Re-attach Last Will if set
        MQTTClient_willOptions will_opts = MQTTClient_willOptions_initializer;
        if (!last_will_topic_.empty()) {
            will_opts.topicName = last_will_topic_.c_str();
            will_opts.message   = last_will_payload_.c_str();
            will_opts.qos       = constants::MQTT_DEFAULT_QOS;
            will_opts.retained  = 1;
            conn_opts.will      = &will_opts;
        }

        // Re-attach TLS if needed
        MQTTClient_SSLOptions ssl_opts = MQTTClient_SSLOptions_initializer;
        if (broker_uri_.find("ssl://") == 0 || broker_uri_.find("tls://") == 0) {
            ssl_opts.enableServerCertAuth = ca_cert_path_.empty() ? 0 : 1;
            if (!ca_cert_path_.empty()) ssl_opts.trustStore = ca_cert_path_.c_str();
            conn_opts.ssl = &ssl_opts;
        }

        int rc = MQTTClient_connect(client_, &conn_opts);
        if (rc == MQTTCLIENT_SUCCESS) {
            connected_ = true;
            spdlog::info("MQTT reconnected after {} attempts", attempt + 1);

            // Re-subscribe
            {
                std::lock_guard<std::mutex> lock(sub_mutex_);
                for (auto& [topic, qos] : subscriptions_) {
                    MQTTClient_subscribe(client_, topic.c_str(), qos);
                }
            }
            if (connected_callback_) connected_callback_();
            return;
        }
        ++attempt;
    }
}

} // namespace orbis
