
#ifndef AISAQ_IO_READ_NODE_HH
#define AISAQ_IO_READ_NODE_HH
#include <cstdint>

#include "pump/core/meta.hh"
#include "env/scheduler/nvme/ssd.hh"
#include "env/scheduler/nvme/sender.hh"
#include "env/scheduler/nvme/scheduler.hh"
#include "pump/sender/concurrent.hh"
#include "pump/sender/generate.hh"
#include "pump/sender/reduce.hh"

#include "../common/common.hh"

namespace aisaq::io {
    using namespace pump::sender;
    using namespace pump::scheduler::nvme;
    using namespace aisaq::common;

    inline auto
    read_node(scheduler<page>* sche, page* p) {
        return get_page(p, sche);
    }
}

#endif //AISAQ_IO_READ_NODE_HH