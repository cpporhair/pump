
#ifndef ENV_COMMON_SESSION_LIFECYCLE_HH
#define ENV_COMMON_SESSION_LIFECYCLE_HH

#include "session_tags.hh"

namespace pump::scheduler::net {

    // Lifecycle tags
    struct pipeline_end{};  constexpr inline pipeline_end pipeline_end{};
    struct read_end{};      constexpr inline read_end read_end{};

    // Session lifecycle layer: tracks two independent users of a session
    // (application pipeline + scheduler read chain). Deletes the session
    // when both are done. Add as the last layer in session_t composition.
    //
    // Flags default to true — a session is created with the intent of
    // having both a pipeline and a read chain active.
    struct session_lifecycle {
        bool pipeline_active = true;
        bool read_active = true;

        template<typename owner_t>
        void invoke(const struct pipeline_end&, owner_t& self) {
            if (!pipeline_active) return;
            pipeline_active = false;
            if (!read_active) {
                self.invoke(::pump::scheduler::net::do_close);
                delete &self;
            }
        }

        template<typename owner_t>
        void invoke(const struct read_end&, owner_t& self) {
            if (!read_active) return;
            read_active = false;
            if (!pipeline_active) {
                self.invoke(::pump::scheduler::net::do_close);
                delete &self;
            }
        }
    };

}

#endif //ENV_COMMON_SESSION_LIFECYCLE_HH
