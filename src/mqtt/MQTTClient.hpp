#pragma once
#include <MQTTClient.h>
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace orbis {

/**
 * @brief Thread-safe MQTT client wrapping the Eclipse Paho C library.
 *
 * Supports TLS via OpenSSL, automatic exponential-backoff reconnection,
 * QoS-1 publish, subscribe, and a single message-arrived callback
 * dispatched to the MessageQueue.
 */
class MQTTClientWrapper {
public:
    using MessageCallback = std::function<void(const std::string& topic,
                                               const std::string& payload)>;

    /**
     * @param broker_uri   Full URI, e.g. "ssl://localhost:8883" or "tcp://localhost:1883"
     * @param client_id    Unique MQTT client ID (device_id or "register-<uuid>")
     * @param ca_cert_path Path to CA certificate for TLS. Empty = skip verify.
     * @param reconnect_min_sec  Minimum reconnect backoff in seconds
     * @param reconnect_max_sec  Maximum reconnect backoff in seconds
     */
    MQTTClientWrapper(const std::string& broker_uri,
                      const std::string& client_id,
                      const std::string& ca_cert_path,
                      int reconnect_min_sec,
                      int reconnect_max_sec);

    ~MQTTClientWrapper();

    // Non-copyable
    MQTTClientWrapper(const MQTTClientWrapper&) = delete;
    MQTTClientWrapper& operator=(const MQTTClientWrapper&) = delete;

    /**
     * @brief Connect to the broker synchronously.
     * @param username        MQTT username
     * @param password        MQTT password
     * @param will_topic      Last Will topic (empty = no will)
     * @param will_payload    Last Will payload JSON
     * @param keepalive_sec   MQTT keepalive in seconds
     * @return true on success
     */
    bool connect(const std::string& username,
                 const std::string& password,
                 const std::string& will_topic   = "",
                 const std::string& will_payload = "",
                 int keepalive_sec = 60);

    void disconnect();
    bool isConnected() const;

    /**
     * @brief Publish a message (thread-safe).
     * @param topic     MQTT topic
     * @param payload   Message payload (UTF-8 JSON)
     * @param qos       QoS level (default 1)
     * @param retained  Retained flag
     * @return true on success
     */
    bool publish(const std::string& topic,
                 const std::string& payload,
                 int qos = 1,
                 bool retained = false);

    /**
     * @brief Subscribe to a topic.
     * @param topic  MQTT topic (wildcards # and + supported)
     * @param qos    QoS level (default 1)
     */
    bool subscribe(const std::string& topic, int qos = 1);

    /// Register the callback invoked for every incoming message.
    /// Called from Paho's internal delivery thread — implementations must be thread-safe.
    void setMessageCallback(MessageCallback cb);

    /// Register a callback invoked when the connection is established (or re-established).
    void setConnectedCallback(std::function<void()> cb);

    /// Register a callback invoked when the connection is lost.
    void setConnectionLostCallback(std::function<void(std::string)> cb);

private:
    // Paho C callbacks (must be free functions or static methods)
    static void  onConnectionLost(void* context, char* cause);
    static int   onMessageArrived(void* context, char* topic, int topicLen,
                                  MQTTClient_message* message);
    static void  onDeliveryComplete(void* context, int token);

    void reconnectLoop();

    std::string broker_uri_;
    std::string client_id_;
    std::string ca_cert_path_;
    int reconnect_min_sec_;
    int reconnect_max_sec_;

    // Credentials stored at connect() time for use in reconnectLoop()
    std::string last_username_;
    std::string last_password_;
    std::string last_will_topic_;
    std::string last_will_payload_;
    int         last_keepalive_sec_{60};

    MQTTClient client_{nullptr};  // Paho handle (opaque pointer)

    mutable std::mutex       pub_mutex_;
    std::atomic<bool>        connected_{false};
    std::atomic<bool>        shutdown_{false};

    MessageCallback              msg_callback_;
    std::function<void()>        connected_callback_;
    std::function<void(std::string)> conn_lost_callback_;

    // Topics to re-subscribe after reconnection
    std::mutex                   sub_mutex_;
    std::vector<std::pair<std::string,int>> subscriptions_;
};

} // namespace orbis
