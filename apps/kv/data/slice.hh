//

//

#ifndef APPS_KV_DATA_SLICE_HH
#define APPS_KV_DATA_SLICE_HH

namespace apps::kv::data {
    struct
    slice {
        uint64_t    len;
        char        ptr[];

        uint64_t
        ptr_size() const {
            return len - sizeof(uint64_t);
        }

        inline friend
        auto
        operator == (const slice& s, const char* k) {
            if (s.ptr_size() != std::strlen(k))
                return false;
            for (uint64_t i = 0; i < s.ptr_size(); ++i) {
                if (s.ptr[i] != k[i])
                    return false;
            }

            return true;
        }

        inline
        auto
        equal(const char* k) const {
            return (*this) == k;
        }
    };



    inline auto
    make_slice(const char* s) {
        uint64_t l = std::strlen(s);
        slice *res = (slice *) new char[l + sizeof(uint64_t)];
        res->len = l + sizeof(uint64_t);
        memcpy(&res->ptr, s, l);
        return res;
    }
}

#endif //APPS_KV_DATA_SLICE_HH
