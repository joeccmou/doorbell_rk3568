#pragma once

#include <cstddef>
#include <unordered_set>

class MqttSessionHandshake {
public:
    void expect_subscription(int message_id) {
        if (message_id <= 0) return;
        pending_subscription_ids_.insert(message_id);
        ++expected_subscription_count_;
    }

    void acknowledge_subscription(int message_id, bool accepted) {
        if (pending_subscription_ids_.erase(message_id) == 0) return;
        if (!accepted) failed_ = true;
    }

    bool subscriptions_ready() const {
        return !failed_ && expected_subscription_count_ > 0 && pending_subscription_ids_.empty();
    }

    void expect_online_publish(int message_id) {
        online_message_id_ = message_id;
        online_publish_expected_ = message_id > 0;
        online_publish_acknowledged_ = false;
    }

    bool acknowledge_publish(int message_id) {
        if (!online_publish_expected_ || message_id != online_message_id_) return false;
        online_publish_expected_ = false;
        online_publish_acknowledged_ = true;
        return true;
    }

    bool online_ready() const {
        return !failed_ && online_publish_acknowledged_;
    }

    bool ready() const {
        return subscriptions_ready() && online_ready();
    }

    bool failed() const {
        return failed_;
    }

private:
    std::unordered_set<int> pending_subscription_ids_;
    std::size_t expected_subscription_count_ = 0;
    int online_message_id_ = 0;
    bool online_publish_expected_ = false;
    bool online_publish_acknowledged_ = false;
    bool failed_ = false;
};
