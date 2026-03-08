
#ifndef ENV_COMMON_ERRORS_HH
#define ENV_COMMON_ERRORS_HH

#include <stdexcept>

namespace pump::scheduler::net {

    struct net_error : std::runtime_error {
        using std::runtime_error::runtime_error;
    };

    struct session_closed_error : net_error {
        session_closed_error()
            : net_error("session closed") {}
    };

    struct enqueue_failed_error : net_error {
        enqueue_failed_error()
            : net_error("enqueue failed") {}
    };

}

#endif //ENV_COMMON_ERRORS_HH
