
#ifndef ENV_SCHEDULER_TCP_COMMON_ERROR_HH
#define ENV_SCHEDULER_TCP_COMMON_ERROR_HH

#include <stdexcept>
#include <string>

namespace pump::scheduler::tcp::common {

    struct net_error : std::runtime_error {
        using std::runtime_error::runtime_error;
    };

    struct session_closed_error : net_error {
        session_closed_error()
            : net_error("session closed") {}
    };

    struct duplicate_recv_error : net_error {
        duplicate_recv_error()
            : net_error("duplicate recv on session") {}
    };

    struct enqueue_failed_error : net_error {
        enqueue_failed_error()
            : net_error("enqueue failed") {}
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
