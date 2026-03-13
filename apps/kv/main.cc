#include <iostream>
#include <ranges>
#include <any>

#include "pump/sender/just.hh"
#include "pump/sender/concurrent.hh"
#include "pump/sender/reduce.hh"
#include "pump/sender/sequential.hh"
#include "pump/sender/when_all.hh"
#include "pump/sender/pop_context.hh"
#include "pump/coro/coro.hh"
#include "senders/start_db.hh"
#include "senders/start_batch.hh"
#include "senders/put.hh"
#include "senders/make_kv.hh"
#include "senders/apply.hh"
#include "senders/scan.hh"
#include "senders/stop_db.hh"
#include "senders/get.hh"



#include "ycsb/statistic.hh"
#include "ycsb/ycsb.hh"

using namespace pump::coro;
using namespace pump::sender;
using namespace apps::kv;
using namespace apps::kv::ycsb;

uint64_t max_key = 100;

int
main(int argc, char **argv){
    start_db(argc, argv)([](){
        return with_context(statistic_helper(new statistic_data()), logger()) (load(max_key))
            >> with_context(statistic_helper(new statistic_data()), logger()) (updt(max_key))
            >> with_context(statistic_helper(new statistic_data()), logger()) (read(max_key))
            >> stop_db();
    });
    return 0;
}
