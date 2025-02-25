#pragma once

#include <map>
#include <string>
#include <vector>

#include "storage_EXPORTS.h"

#include "common.h"
#include "http_base.h"
#include "storage_account.h"
#include "storage_request_base.h"

namespace microsoft_azure {
    namespace storage {

        class list_containers_request_base : public blob_request_base {
        public:
            virtual std::string prefix() const { return std::string(); }
            virtual std::string marker() const { return std::string(); }
            virtual int maxresults() const { return 0; }
            virtual bool include_metadata() const { return false; }

            AZURE_STORAGE_API void build_request(const storage_account &a, http_base &h, const storage_client_key &k) const override;
        };

        class list_containers_item {
        public:
            std::string name;
            std::string last_modified;
            std::string etag;
            lease_status status;
            lease_state state;
            lease_duration duration;
        };

        class list_containers_response {
        public:
            std::string ms_request_id;
            std::vector<list_containers_item> containers;
            std::string next_marker;
        };

    }
}

