
#ifndef ENV_COMMON_SESSION_TAGS_HH
#define ENV_COMMON_SESSION_TAGS_HH

namespace pump::scheduler::net {

    // Common tags for session composition (transport-agnostic)
    struct on_frame{};  constexpr inline on_frame on_frame{};
    struct on_error{};  constexpr inline on_error on_error{};
    struct do_close{};  constexpr inline do_close do_close{};
    struct do_recv{};   constexpr inline do_recv do_recv{};
    struct do_send{};   constexpr inline do_send do_send{};

}

#endif //ENV_COMMON_SESSION_TAGS_HH
