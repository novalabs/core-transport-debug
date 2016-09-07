/* COPYRIGHT (c) 2016 Nova Labs SRL
 *
 * All rights reserved. All use of this software and documentation is
 * subject to the License Agreement located in the file LICENSE.
 */

#pragma once

#include <core/mw/namespace.hpp>
#include <core/common.hpp>
#include <core/mw/Transport.hpp>
#include <core/mw/BaseSubscriberQueue.hpp>
#include <core/mw/TimestampedMsgPtrQueue.hpp>
#include <core/os/Mutex.hpp>
#include <core/mw/MgmtMsg.hpp>
#include <core/mw/BootMsg.hpp>
#include <core/os/Semaphore.hpp>

//#include <ch.h>
#include <hal_channels.h>

#include "DebugPublisher.hpp"
#include "DebugSubscriber.hpp"
#include <core/os/Thread.hpp>

NAMESPACE_CORE_MW_BEGIN


class DebugTransport:
   public Transport
{
private:
   core::os::Thread* rx_threadp;
   core::os::Thread* tx_threadp;

   BaseChannel* channelp;
   char*        namebufp;

   core::os::Semaphore subp_sem;
   BaseSubscriberQueue subp_queue;

   core::os::Mutex send_lock;

   enum {
      MGMT_BUFFER_LENGTH = 4
   };

   TimestampedMsgPtrQueue::Entry mgmt_msgqueue_buf[MGMT_BUFFER_LENGTH];
   MgmtMsg         mgmt_msgbuf[MGMT_BUFFER_LENGTH];
   DebugSubscriber mgmt_rsub;
   DebugPublisher  mgmt_rpub;

#if CORE_IS_BOOTLOADER_BRIDGE
   enum {
      BOOT_BUFFER_LENGTH = 8
   };

   TimestampedMsgPtrQueue::Entry boot_msgqueue_buf[BOOT_BUFFER_LENGTH];
   BootMsg         boot_msgbuf[BOOT_BUFFER_LENGTH];
   DebugSubscriber boot_rsub;
   DebugPublisher  boot_rpub;
#endif

public:
   bool
   send_stop();

   bool
   send_reboot();

   bool
   send_bootload();

   void
   initialize(
      void*                      rx_stackp,
      size_t                     rx_stacklen,
      core::os::Thread::Priority rx_priority,
      void*                      tx_stackp,
      size_t                     tx_stacklen,
      core::os::Thread::Priority tx_priority
   );

   void
   notify_first_sub_unsafe(
      const DebugSubscriber& sub
   );


private:
   RemotePublisher*
   create_publisher(
      Topic&        topic,
      const uint8_t raw_params[] = NULL
   ) const;

   RemoteSubscriber*
   create_subscriber(
      Topic&                        topic,
      TimestampedMsgPtrQueue::Entry queue_buf[],
      size_t                        queue_length
   ) const;

   bool
   spin_tx();

   bool
   spin_rx();


private:
   bool
   send_char(
      char      c,
      systime_t timeout = TIME_INFINITE
   );

   bool
   expect_char(
      char      c,
      systime_t timeout = TIME_INFINITE
   );

   bool
   skip_after_char(
      char      c,
      systime_t timeout = TIME_INFINITE
   );

   bool
   recv_char(
      char&     c,
      systime_t timeout = TIME_INFINITE
   );

   bool
   send_byte(
      uint8_t   byte,
      systime_t timeout = TIME_INFINITE
   );

   bool
   recv_byte(
      uint8_t&  byte,
      systime_t timeout = TIME_INFINITE
   );

   bool
   send_chunk(
      const void* chunkp,
      size_t      length,
      systime_t   timeout = TIME_INFINITE
   );

   bool
   recv_chunk(
      void*     chunkp,
      size_t    length,
      systime_t timeout = TIME_INFINITE
   );

   bool
   send_chunk_rev(
      const void* chunkp,
      size_t      length,
      systime_t   timeout = TIME_INFINITE
   );

   bool
   recv_chunk_rev(
      void*     chunkp,
      size_t    length,
      systime_t timeout = TIME_INFINITE
   );

   bool
   send_string(
      const char* stringp,
      size_t      length,
      systime_t   timeout = TIME_INFINITE
   );

   bool
   recv_string(
      char*     stringp,
      size_t    length,
      systime_t timeout = TIME_INFINITE
   );


   template <typename T>
   bool
   send_value(
      const T&  value,
      systime_t timeout = TIME_INFINITE
   );


   template <typename T>
   inline
   bool
   recv_value(
      T&        value,
      systime_t timeout = TIME_INFINITE
   );

   bool
   send_msg(
      const Message&        msg,
      size_t                msg_size,
      const char*           topicp,
      const core::os::Time& deadline
   );

   bool
   send_signal_msg(
      char id
   );


public:
   DebugTransport(
      const char*  namep,
      BaseChannel* channelp,
      char         namebuf[]
   );
   ~DebugTransport();

private:
   static void
   rx_threadf(
      core::os::Thread::Argument arg
   );

   static void
   tx_threadf(
      core::os::Thread::Argument arg
   );
};


inline
void
DebugTransport::notify_first_sub_unsafe(
   const DebugSubscriber& sub
)
{
   subp_queue.post_unsafe(sub.by_transport_notify);
   subp_sem.signal_unsafe();
}

inline
RemotePublisher*
DebugTransport::create_publisher(
   Topic&        topic,
   const uint8_t raw_params[]
)
 const
{
   (void)topic;
   (void)raw_params;
   return new DebugPublisher(*const_cast<DebugTransport*>(this));
}

inline
RemoteSubscriber*
DebugTransport::create_subscriber(
   Topic&                        topic,
   TimestampedMsgPtrQueue::Entry queue_buf[],
   size_t                        queue_length
) const
{
   (void)topic;
   return new DebugSubscriber(*const_cast<DebugTransport*>(this),
                              queue_buf, queue_length);
}

inline
bool
DebugTransport::send_char(
   char      c,
   systime_t timeout
)
{
   return chnPutTimeout(channelp, c, timeout) == Q_OK;
}

inline
bool
DebugTransport::expect_char(
   char      c,
   systime_t timeout
)
{
   return chnGetTimeout(channelp, timeout) == c;
}

inline
bool
DebugTransport::recv_char(
   char&     c,
   systime_t timeout
)
{
   register msg_t value = chnGetTimeout(channelp, timeout);

   if ((value & ~0xFF) == 0) {
      c = static_cast<char>(value);
      return true;
   }

   return false;
}

inline
bool
DebugTransport::send_byte(
   uint8_t   byte,
   systime_t timeout
)
{
   static const char hex[16] = {
      '0', '1', '2', '3', '4', '5', '6', '7',
      '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'
   };

   if (chnPutTimeout(channelp, hex[byte >> 4], timeout) != Q_OK) {
      return false;
   }

   if (chnPutTimeout(channelp, hex[byte & 15], timeout) != Q_OK) {
      return false;
   }

   return true;
}

template <typename T>
inline
bool
DebugTransport::send_value(
   const T&  value,
   systime_t timeout
)
{
   return send_chunk_rev(reinterpret_cast<const void*>(&value),
                         sizeof(T), timeout);
}

template <>
inline
bool
DebugTransport::send_value(
   const uint8_t& value,
   systime_t      timeout
)
{
   return send_byte(value, timeout);
}

template <>
inline
bool
DebugTransport::send_value(
   const char& value,
   systime_t   timeout
)
{
   return send_byte(static_cast<const uint8_t>(value), timeout);
}

template <typename T>
inline
bool
DebugTransport::recv_value(
   T&        value,
   systime_t timeout
)
{
   return recv_chunk_rev(reinterpret_cast<void*>(&value),
                         sizeof(T), timeout);
}

template <>
inline
bool
DebugTransport::recv_value(
   uint8_t&  value,
   systime_t timeout
)
{
   return recv_byte(value, timeout);
}

template <>
inline
bool
DebugTransport::recv_value(
   char&     value,
   systime_t timeout
)
{
   return recv_byte(reinterpret_cast<uint8_t&>(value), timeout);
}

NAMESPACE_CORE_MW_END
