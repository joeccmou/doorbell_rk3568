#include "device/mqtt_session_handshake.h"

#include <cassert>

int main() {
    MqttSessionHandshake handshake;
    handshake.expect_subscription(11);
    handshake.expect_subscription(12);
    assert(!handshake.subscriptions_ready());

    handshake.acknowledge_subscription(11, true);
    assert(!handshake.subscriptions_ready());
    handshake.acknowledge_subscription(12, true);
    assert(handshake.subscriptions_ready());

	handshake.expect_ready_publish(21);
	assert(!handshake.ready_publish_acknowledged());
	assert(!handshake.acknowledge_publish(20));
	assert(!handshake.ready_publish_acknowledged());
	assert(handshake.acknowledge_publish(21));
	assert(handshake.ready_publish_acknowledged());
    assert(handshake.ready());

    MqttSessionHandshake rejected;
    rejected.expect_subscription(31);
    rejected.acknowledge_subscription(31, false);
    assert(rejected.failed());
    assert(!rejected.subscriptions_ready());
    assert(!rejected.ready());
    return 0;
}
