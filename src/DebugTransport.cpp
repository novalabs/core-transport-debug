/* COPYRIGHT (c) 2016-2018 Nova Labs SRL
 *
 * All rights reserved. All use of this software and documentation is
 * subject to the License Agreement located in the file LICENSE.
 */

#include <core/mw/namespace.hpp>
#include <core/mw/transport/DebugTransport.hpp>
#include <core/mw/transport/DebugPublisher.hpp>
#include <core/mw/transport/DebugSubscriber.hpp>
#include <core/mw/Middleware.hpp>
#include <core/os/ScopedLock.hpp>
#include <core/mw/Checksummer.hpp>


#include <cstring>
#include <locale>

size_t strnlen(const char* str, size_t n)
{
    const char* start = str;

    while (n-- > 0 && *str) {
        str++;
    }

    return str - start;
}

NAMESPACE_CORE_MW_BEGIN

// Adds a delay after the starting '@', to allow full buffering of the message
#define RECV_DELAY_MS   0//5


bool
DebugTransport::skip_after_char(
    char      c,
    systime_t timeout
)
{
    register msg_t value;

    do {
        value = chnGetTimeout(channelp, timeout);

        if ((value & ~0xFF) != 0) {
            return false;
        }
    } while (static_cast<char>(value) != c);

    return true;
}

bool
DebugTransport::recv_byte(
    uint8_t&  byte,
    systime_t timeout
)
{
    msg_t c;

    c = chnGetTimeout(channelp, timeout);

    if (((c >= '0') && (c <= '9'))) {
        byte = static_cast<uint8_t>(c - '0') << 4;
    } else if (((c >= 'A') && (c <= 'F'))) {
        byte = static_cast<uint8_t>(c + (10 - 'A')) << 4;
    } else if (((c >= 'a') && (c <= 'f'))) {
        byte = static_cast<uint8_t>(c + (10 - 'a')) << 4;
    } else {
        return false;
    }

    c = chnGetTimeout(channelp, timeout);

    if (((c >= '0') && (c <= '9'))) {
        byte |= static_cast<uint8_t>(c - '0');
    } else if (((c >= 'A') && (c <= 'F'))) {
        byte |= static_cast<uint8_t>(c + (10 - 'A'));
    } else if (((c >= 'a') && (c <= 'f'))) {
        byte |= static_cast<uint8_t>(c + (10 - 'a'));
    } else {
        return false;
    }

    return true;
} // DebugTransport::recv_byte

bool
DebugTransport::send_chunk(
    const void* chunkp,
    size_t      length,
    systime_t   timeout
)
{
    const uint8_t* p = reinterpret_cast<const uint8_t*>(chunkp);

    while (length-- > 0) {
        if (!send_byte(*p++, timeout)) {
            return false;
        }
    }

    return true;
}

bool
DebugTransport::recv_chunk(
    void*     chunkp,
    size_t    length,
    systime_t timeout
)
{
    uint8_t* p = reinterpret_cast<uint8_t*>(chunkp);

    while (length-- > 0) {
        if (!recv_byte(*p++, timeout)) {
            return false;
        }
    }

    return true;
}

bool
DebugTransport::send_chunk_rev(
    const void* chunkp,
    size_t      length,
    systime_t   timeout
)
{
    const uint8_t* p = reinterpret_cast<const uint8_t*>(chunkp) + length - 1;

    while (length-- > 0) {
        if (!send_byte(*p--, timeout)) {
            return false;
        }
    }

    return true;
}

bool
DebugTransport::recv_chunk_rev(
    void*     chunkp,
    size_t    length,
    systime_t timeout
)
{
    uint8_t* p = reinterpret_cast<uint8_t*>(chunkp) + length - 1;

    while (length-- > 0) {
        if (!recv_byte(*p--, timeout)) {
            return false;
        }
    }

    return true;
}

bool
DebugTransport::send_string(
    const char* stringp,
    size_t      length,
    systime_t   timeout
)
{
    for (; length-- > 0; ++stringp) {
        if (!send_char(*stringp, timeout)) {
            return false;
        }
    }

    return true;
}

bool
DebugTransport::recv_string(
    char*     stringp,
    size_t    length,
    systime_t timeout
)
{
    for (; length-- > 0; ++stringp) {
        if (!recv_char(*stringp, timeout)) {
            return false;
        }
    }

    return true;
}

bool
DebugTransport::send_msg(
    const Message&        msg,
    size_t                msg_size,
    const char*           topicp,
    const core::os::Time& deadline
)
{
#if CORE_USE_BRIDGE_MODE
    CORE_ASSERT(msg.get_source() != this);
#endif

    Checksummer cs;

    // Send the deadline
    if (!send_char('@')) {
        return false;
    }

    if (!send_value(deadline.raw)) {
        return false;
    }

    if (!send_char(':')) {
        return false;
    }

    cs.add(deadline.raw);

    // Send the topic name
    uint8_t namelen = static_cast<uint8_t>(
        strnlen(topicp, NamingTraits<Topic>::MAX_LENGTH)
                      );

    if (!send_value(namelen)) {
        return false;
    }

    if (!send_string(topicp, namelen)) {
        return false;
    }

    cs.add(namelen);
    cs.add(topicp, namelen);

    // Send the payload length and data
    if (!send_char(':')) {
        return false;
    }

    namelen = static_cast<uint8_t>(msg_size);

    if (!send_value(namelen)) {
        return false;
    }

    if (!send_chunk(msg.get_raw_data(), msg_size)) {
        return false;
    }

    cs.add(namelen);
    cs.add(msg.get_raw_data(), msg_size);

    // Send the checksum
    if (!send_char(':')) {
        return false;
    }

    if (!send_value(cs.compute_checksum())) {
        return false;
    }

    // End of packet
    if (!send_char('\r') || !send_char('\n')) {
        return false;
    }

    return true;
} // DebugTransport::send_msg

void
DebugTransport::initialize(
    void*                      rx_stackp,
    size_t                     rx_stacklen,
    core::os::Thread::Priority rx_priority,
    void*                      tx_stackp,
    size_t                     tx_stacklen,
    core::os::Thread::Priority tx_priority
)
{
    subp_sem.initialize();
    send_lock.initialize();

    // Create the transmission pump thread
    tx_threadp = core::os::Thread::create_static(tx_stackp, tx_stacklen, tx_priority, tx_threadf, this, "DEBUG_TX");
    CORE_ASSERT(tx_threadp != NULL);

    // Create the reception pump thread
    rx_threadp = core::os::Thread::create_static(rx_stackp, rx_stacklen, rx_priority, rx_threadf, this, "DEBUG_RX");
    CORE_ASSERT(rx_threadp != NULL);

    // Clear any previous crap caused by serial port initialization
    bool success;
    (void)success;
    send_lock.acquire();
    success = send_string("\r\n\r\n", 4);
    CORE_ASSERT(success); // DAVIDE ChibioOS 16 checks for USB_ACTIVE... :-|
    send_lock.release();

    // Register remote publisher and subscriber for the management thread
   success = advertise(mgmt_rpub, MANAGEMENT_TOPIC_NAME, core::os::Time::INFINITE, sizeof(MgmtMsg));
    CORE_ASSERT(success);
   success = subscribe(mgmt_rsub, MANAGEMENT_TOPIC_NAME, mgmt_msgbuf, MGMT_BUFFER_LENGTH);
    CORE_ASSERT(success);

#if CORE_IS_BOOTLOADER_BRIDGE
    // Register remote publisher and subscriber for the bootloader
    success = advertise(boot_rpub, BOOTLOADER_TOPIC_NAME, core::os::Time::INFINITE, sizeof(bootloader::BootMsg));
    CORE_ASSERT(success);
    success = subscribe(boot_rsub, BOOTLOADER_TOPIC_NAME, boot_msgbuf, BOOT_BUFFER_LENGTH);
    CORE_ASSERT(success);
    success = subscribe(bootmaster_rsub, BOOTLOADER_MASTER_TOPIC_NAME, bootmaster_msgbuf, BOOT_BUFFER_LENGTH);
    CORE_ASSERT(success);
#endif

    Middleware::instance().add(*this);
} // DebugTransport::initialize

bool
DebugTransport::spin_tx()
{
    Message*       msgp;
    core::os::Time timestamp;
    const BaseSubscriberQueue::Link* subp_linkp;

    core::os::SysLock::acquire();

    subp_sem.wait_unsafe();
    subp_queue.peek_unsafe(subp_linkp);
    CORE_ASSERT(subp_linkp->itemp != NULL);
    DebugSubscriber& sub = static_cast<DebugSubscriber&>(*subp_linkp->itemp);
    size_t           queue_length = sub.get_queue_count();
    CORE_ASSERT(queue_length > 0);
    core::os::SysLock::release();

    bool requeued = false;

    while (queue_length-- > 0) {
        core::os::SysLock::acquire();
        sub.fetch_unsafe(msgp, timestamp);

        if (queue_length == 0) {
            subp_queue.skip_unsafe();

            // Check if there are more messages than the ones expected
            if (!requeued && (sub.get_queue_count() > 0)) {
                requeued = true;
                notify_first_sub_unsafe(sub); // Put this sub into the queue again
            }
        }

        core::os::SysLock::release();

        send_lock.acquire();
        const Topic& topic = *sub.get_topic();

        if (!send_msg(*msgp, topic.get_payload_size(), topic.get_name(),
                      timestamp + topic.get_publish_timeout())) {
            sub.release(*msgp);
            send_char('\r');
            send_char('\n');
            send_lock.release();
            return false;
        }

        sub.release(*msgp);
        send_lock.release();
    }

    return true;
} // DebugTransport::spin_tx


bool
DebugTransport::spin_rx()
{
    Checksummer cs;

    // Skip the deadline
    if (!skip_after_char('@')) {
        return false;
    }

#if RECV_DELAY_MS
    Thread::sleep(Time::ms(100));
#endif
    {
        core::os::Time deadline;

        if (!recv_value(deadline.raw)) {
            return false;
        }

        cs.add(deadline.raw);
    }

    // Receive the topic name length and data
    if (!expect_char(':')) {
        return false;
    }

    uint8_t length;

    if (!recv_value(length)) {
        return false;
    }

    if ((length == 0) || (length > NamingTraits<Topic>::MAX_LENGTH)) {
        return false;
    }

    memset(namebufp, 0, NamingTraits<Topic>::MAX_LENGTH);

    if (!recv_string(namebufp, length)) {
        return false;
    }

    if (!expect_char(':')) {
        return false;
    }

    cs.add(length);
    cs.add(namebufp, length);

    // Check if the topic is known
    Topic* topicp = Middleware::instance().find_topic(namebufp);

    if (topicp == NULL) {
        return false;
    }

    RemotePublisher* pubp;
    pubp = publishers.find_first(BasePublisher::has_topic, topicp->get_name());

    if (pubp == NULL) {
        return false;
    }

    // Get the payload length
    if (!recv_value(length)) {
        return false;
    }

    if (length != topicp->get_payload_size()) {
        return false;
    }

    cs.add(length);

    // Get the payload data
    Message* msgp;

    if (!pubp->alloc(msgp)) {
        return false;
    }

#if CORE_USE_BRIDGE_MODE
    MessageGuard guard(*msgp, *topicp);
    msgp->set_source(this);
#endif

    if (!recv_chunk(const_cast<uint8_t*>(msgp->get_raw_data()), length)) {
        return false;
    }

    cs.add(msgp->get_raw_data(), length);

    // Get the checksum
    if (!expect_char(':') || !recv_value(length) || !cs.check(length)) {
        return false;
    }

    bool success = pubp->publish_locally(*msgp, false);

#if CORE_USE_BRIDGE_MODE
    // Forward the message locally
    if (pubp->get_topic()->is_forwarding()) {
        success = success && pubp->publish_remotely(*msgp);
    }
#endif

    return success;
} // DebugTransport::spin_rx

void
DebugTransport::rx_threadf(
    core::os::Thread::Argument arg
)
{
    CORE_ASSERT(arg != static_cast<core::os::Thread::Argument>(NULL));

    // Reception pump
    for (;;) {
        register bool ok;
        ok = reinterpret_cast<DebugTransport*>(arg)->spin_rx();
        (void)ok;
    }

    chThdExitS(core::os::Thread::OK);
}

void
DebugTransport::tx_threadf(
    core::os::Thread::Argument arg
)
{
    CORE_ASSERT(arg != static_cast<core::os::Thread::Argument>(NULL));

    // Transmission pump
    for (;;) {
        reinterpret_cast<DebugTransport*>(arg)->spin_tx();
    }

    chThdExitS(core::os::Thread::OK);
}

DebugTransport::DebugTransport(
    const char*  namep,
    BaseChannel* channelp,
    char         namebuf[]
)
    :
    Transport(namep),
    rx_threadp(NULL),
    tx_threadp(NULL),
    channelp(channelp),
    namebufp(namebuf),
    subp_sem(false),
    send_lock(false),
    mgmt_rsub(*this, mgmt_msgqueue_buf, MGMT_BUFFER_LENGTH),
    mgmt_rpub(*this)
#if CORE_IS_BOOTLOADER_BRIDGE
    ,
    boot_rsub(*this, boot_msgqueue_buf, BOOT_BUFFER_LENGTH),
    boot_rpub(*this),
    bootmaster_rsub(*this, bootmaster_msgqueue_buf, BOOT_BUFFER_LENGTH)
#endif
{
    CORE_ASSERT(channelp != NULL);
    CORE_ASSERT(namebuf != NULL);
}

DebugTransport::~DebugTransport() {}


NAMESPACE_CORE_MW_END
