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

	void expect_ready_publish(int message_id) {
		ready_message_id_ = message_id;
		ready_publish_expected_ = message_id > 0;
		ready_publish_acknowledged_ = false;
	}

	bool acknowledge_publish(int message_id) {
		if (!ready_publish_expected_ || message_id != ready_message_id_) return false;
		ready_publish_expected_ = false;
		ready_publish_acknowledged_ = true;
		return true;
	}

	bool ready_publish_acknowledged() const {
		return !failed_ && ready_publish_acknowledged_;
	}

	bool ready() const {
		return subscriptions_ready() && ready_publish_acknowledged();
    }

    bool failed() const {
        return failed_;
    }

private:
    std::unordered_set<int> pending_subscription_ids_;
    std::size_t expected_subscription_count_ = 0;
	int ready_message_id_ = 0;
	bool ready_publish_expected_ = false;
	bool ready_publish_acknowledged_ = false;
    bool failed_ = false;
};
