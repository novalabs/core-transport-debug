/* COPYRIGHT (c) 2016 Nova Labs SRL
 *
 * All rights reserved. All use of this software and documentation is
 * subject to the License Agreement located in the file LICENSE.
 */

#pragma once

#include <core/mw/namespace.hpp>
#include <core/common.hpp>
#include <core/mw/RemotePublisher.hpp>
#include <core/mw/StaticList.hpp>

NAMESPACE_CORE_MW_BEGIN


class DebugPublisher:
   public RemotePublisher
{
public:
   DebugPublisher(
      Transport& transport
   );
   ~DebugPublisher();
};


NAMESPACE_CORE_MW_END
