
#ifndef ENV_SCHEDULER_TCP_COMMON_ERROR_HH
#define ENV_SCHEDULER_TCP_COMMON_ERROR_HH

#include "env/scheduler/net/common/errors.hh"

namespace pump::scheduler::tcp::common {

    // Import common errors into TCP namespace for backward compatibility
    using pump::scheduler::net::net_error;
    using pump::scheduler::net::session_closed_error;
    using pump::scheduler::net::enqueue_failed_error;

    struct duplicate_recv_error : net_error {
        duplicate_recv_error()
            : net_error("duplicate recv on session") {}
    };

    struct join_failed_error : net_error {
        join_failed_error()
            : net_error("join session failed") {}
    };

    struct stop_failed_error : net_error {
        stop_failed_error()
            : net_error("stop session failed") {}
    };

}

#endif //ENV_SCHEDULER_TCP_COMMON_ERROR_HH
